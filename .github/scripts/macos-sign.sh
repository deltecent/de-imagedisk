#!/bin/sh
# Sign and notarize the macOS tools in the package directory $1.
#
# With the Developer ID secrets set, the tools are signed with the hardened
# runtime and a secure timestamp, then submitted to Apple's notary service.
# Without them, the tools get an ad-hoc signature, and users have to clear the
# quarantine flag after downloading.
#
#   MACOS_CERTIFICATE_P12       base64 of the exported Developer ID Application .p12
#   MACOS_CERTIFICATE_PASSWORD  password of that .p12
#   APPLE_API_KEY_P8            App Store Connect API key (contents of AuthKey_XXXX.p8)
#   APPLE_API_KEY_ID            its key ID
#   APPLE_API_ISSUER_ID         its issuer ID
set -eu

dir=$1
tools="$dir/imdu $dir/bin2imd $dir/td02imd"

missing() {
    echo "::warning::$1 is not set; $2"
}

if [ -z "${MACOS_CERTIFICATE_P12:-}" ]; then
    missing MACOS_CERTIFICATE_P12 "signing ad hoc"
    codesign --force --sign - $tools
    codesign --verify --strict $tools
    exit 0
fi

work=${RUNNER_TEMP:-/tmp}/macos-signing
keychain=$work/signing.keychain-db
mkdir -p "$work"
cleanup() {
    security delete-keychain "$keychain" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

# A throwaway keychain holds the certificate for this job only.
kcpass=$(uuidgen)
printf '%s' "$MACOS_CERTIFICATE_P12" | base64 --decode > "$work/cert.p12"
security create-keychain -p "$kcpass" "$keychain"
security set-keychain-settings -lut 3600 "$keychain"
security unlock-keychain -p "$kcpass" "$keychain"
security import "$work/cert.p12" -k "$keychain" -P "$MACOS_CERTIFICATE_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple: -s -k "$kcpass" "$keychain" >/dev/null
# shellcheck disable=SC2046
security list-keychains -d user -s "$keychain" $(security list-keychains -d user | tr -d '"')

identity=$(security find-identity -v -p codesigning "$keychain" |
    sed -n 's/.*\([0-9A-F]\{40\}\) "Developer ID Application:.*/\1/p' | head -n 1)
if [ -z "$identity" ]; then
    echo "::error::MACOS_CERTIFICATE_P12 has no Developer ID Application identity"
    exit 1
fi

codesign --force --options runtime --timestamp --keychain "$keychain" --sign "$identity" $tools
codesign --verify --strict --verbose=2 $tools
codesign --display --verbose=2 "$dir/imdu" 2>&1 | grep -E '^(Authority|TeamIdentifier|Timestamp)='

if [ -z "${APPLE_API_KEY_P8:-}" ]; then
    missing APPLE_API_KEY_P8 "skipping notarization"
    exit 0
fi

# Bare executables can't have a notarization ticket stapled to them. Gatekeeper
# looks the ticket up online the first time a downloaded tool is run.
printf '%s\n' "$APPLE_API_KEY_P8" > "$work/AuthKey.p8"
ditto -c -k --keepParent "$dir" "$work/notarize.zip"
xcrun notarytool submit "$work/notarize.zip" \
    --key "$work/AuthKey.p8" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID" \
    --wait --timeout 60m --output-format json > "$work/notary.json"
cat "$work/notary.json"; echo
status=$(plutil -extract status raw "$work/notary.json")
if [ "$status" != Accepted ]; then
    id=$(plutil -extract id raw "$work/notary.json")
    xcrun notarytool log "$id" \
        --key "$work/AuthKey.p8" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID" || true
    echo "::error::Notarization finished with status $status"
    exit 1
fi
