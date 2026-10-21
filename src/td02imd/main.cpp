// td02imd - convert Teledisk .TD0 images to ImageDisk .IMD format.
#include "cli.hpp"
#include "imd_image.hpp"
#include "td0_reader.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* const kHelp =
    "TD0-2-IMageDisk " IMD_VERSION " / " __DATE__ "\n\n"
    "Use:    td02imd filename[.TD0] [options]\n\n"
    "Opts:   /Dx,l,h - enable Debug options\n"
    "        /F      - don't assume whole disk is FM if file FM indicator set\n"
    "        /L      - don't convert 300kbps (LD in HD drive) to 250 (standard LD)\n"
    "        /Q      - Quiet: less informational output.\n"
    "        /RF     - remove Repeated (duplicate) sectors, keep First\n"
    "        /RL     - remove Repeated (duplicate) sectors, keep Last\n"
    "        F=ACDIN - remove sectors based on status Flags         [none]\n"
    "                  A=deletedaddressmark C=crcerror D=dos I=noid N=nodata\n"
    "        M=n     - Maximum sector number (remove any higher)    [255]\n"
    "        O=file  - specify Output filename                      [filename.IMD]\n"
    "        W=CDHIN - inhibit Warnings:\n"
    "                C: CRC error (bad sector)\n"
    "                D: Duplicate sector numbers\n"
    "                H: Head not 0 or 1\n"
    "                I: No ID field - possible bogus sector\n"
    "                N: No DATA field\n";

// Debug flags (/D)
constexpr unsigned kDumpHeader = 0x80;
constexpr unsigned kDumpTracks = 0x40;
constexpr unsigned kDumpSectors = 0x20;
constexpr unsigned kShowSectors = 0x02;
constexpr unsigned kOneTrack = 0x01;

// Warning kinds; each is reported at most once per track.
constexpr unsigned kWarnHead = 0x01;
constexpr unsigned kWarnDuplicate = 0x02;
constexpr unsigned kWarnNoId = 0x04;
constexpr unsigned kWarnNoData = 0x08;
constexpr unsigned kWarnCrc = 0x10;

// Options
unsigned debugFlags = 0, debugLow = 0, debugHigh = 255;
bool assumeFm = true;
bool convertLowDensity = true;
bool verbose = true;
uint8_t removeFlags = 0;
enum class Duplicates { Keep, First, Last } duplicates = Duplicates::Keep;
unsigned maxSector = 0xFFFF;
unsigned warningsOff = 0;

// Current track, for messages
unsigned curCyl = 0, curHead = 0;
unsigned warnedThisTrack = 0;

[[noreturn]] void help()
{
    std::fputs(kHelp, stdout);
    std::exit(1);
}

void warning(unsigned kind, const char* msg)
{
    if (warnedThisTrack & kind)
        return;
    std::printf("%02u/%u: %s\n", curCyl, curHead, msg);
    warnedThisTrack |= kind;
}

void debug(unsigned flag, const char* fmt, ...)
{
    if (!(debugFlags & flag) || curCyl < debugLow || curCyl > debugHigh)
        return;
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
}

// Number in `base`; a leading '$' selects hex and '.' decimal. Ends at ',' or the end.
unsigned getNumber(const char*& p, unsigned base)
{
    if (*p == '$') {
        base = 16;
        ++p;
    } else if (*p == '.') {
        base = 10;
        ++p;
    }
    unsigned long v;
    if (!imd::scanNumber(p, base, v) || (*p && *p != ','))
        imd::fatal("Bad numeric argument");
    return static_cast<unsigned>(v);
}

bool isKnownSwitch(const std::string& s)
{
    if (s == "F" || s == "L" || s == "Q" || s == "RF" || s == "RL")
        return true;
    return s.size() > 1 && s[0] == 'D' &&
           s.find_first_not_of("0123456789ABCDEF$.,", 1) == std::string::npos;
}

const char* tableText(unsigned v, const std::vector<const char*>& table)
{
    static char buf[20];
    if (v >= table.size()) {
        std::snprintf(buf, sizeof buf, "<unknown %02x>", v);
        return buf;
    }
    return table[v];
}

void append(std::vector<uint8_t>& out, const std::string& s)
{
    out.insert(out.end(), s.begin(), s.end());
}

// A sector as kept for output.
struct Sector {
    uint8_t id, cyl, head, sizeCode, flags;
    uint8_t type;  // IMD sector type without compression (0 = no data)
    std::vector<uint8_t> data;
};

void removeSectors(std::vector<Sector>& sectors, unsigned& removed)
{
    std::vector<Sector> kept;
    int firstIndex[256];
    std::fill(std::begin(firstIndex), std::end(firstIndex), -1);
    for (auto& s : sectors) {
        if ((s.flags & removeFlags) || s.id > maxSector) {
            ++removed;
            continue;
        }
        if (duplicates != Duplicates::Keep) {
            int& first = firstIndex[s.id];
            if (first >= 0) {
                if (duplicates == Duplicates::Last) {
                    // The later copy replaces the one already kept, in its position.
                    uint8_t id = kept[static_cast<size_t>(first)].id;
                    kept[static_cast<size_t>(first)] = std::move(s);
                    kept[static_cast<size_t>(first)].id = id;
                }
                ++removed;
                continue;
            }
            first = static_cast<int>(kept.size());
        }
        kept.push_back(std::move(s));
    }
    sectors = std::move(kept);
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string file, outName;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string body, key, value;
        if (imd::switchBody(arg, isKnownSwitch, body)) {
            std::string s = imd::toUpper(body);
            if (s[0] == 'D' && s.size() > 1) {
                const char* p = body.c_str() + 1;
                debugFlags = getNumber(p, 16);
                if (*p == ',') {
                    ++p;
                    debugLow = debugHigh = getNumber(p, 10);
                }
                if (*p == ',') {
                    ++p;
                    debugHigh = getNumber(p, 10);
                }
            } else if (s == "F") {
                assumeFm = false;
            } else if (s == "L") {
                convertLowDensity = false;
            } else if (s == "Q") {
                verbose = false;
            } else if (s == "RF") {
                duplicates = Duplicates::First;
            } else if (s == "RL") {
                duplicates = Duplicates::Last;
            } else {
                help();
            }
            continue;
        }
        if (imd::keyValue(arg, {"F", "M", "O", "W"}, key, value)) {
            if (key == "M") {
                const char* p = value.c_str();
                maxSector = getNumber(p, 10);
            } else if (key == "O") {
                outName = value;
            } else if (key == "F") {
                static const std::string letters = "CADNI";
                static const uint8_t bits[] = {imd::kTd0CrcError, imd::kTd0Deleted, imd::kTd0DosSkipped,
                                               imd::kTd0NoData, imd::kTd0NoId};
                for (char c : imd::toUpper(value)) {
                    auto n = letters.find(c);
                    if (n == std::string::npos)
                        help();
                    removeFlags |= bits[n];
                }
            } else {
                static const std::string letters = "HDINC";
                if (value.empty())
                    help();
                for (char c : imd::toUpper(value)) {
                    auto n = letters.find(c);
                    if (n == std::string::npos)
                        help();
                    warningsOff |= 1u << n;
                }
            }
            continue;
        }
        if (!file.empty() || (arg.size() > 1 && arg[0] == '-'))
            help();
        file = arg;
    }
    if (file.empty())
        help();

    std::string inName = imd::inputName(file, ".TD0");
    if (outName.empty()) {
        // Input name without its extension, plus .imd
        auto slash = file.find_last_of('/');
        auto dot = file.find_last_of('.');
        std::string base = (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                               ? file.substr(0, dot)
                               : file;
        outName = base + ".imd";
    } else {
        outName = imd::outputName(outName, ".IMD");
    }

    try {
        imd::Td0Reader td0(imd::readFile(inName));
        const imd::Td0Header& h = td0.header();
        debug(kDumpHeader,
              "sig:%04x seq:%02x cseq:%02x ver:%02x Den:%02x Dtyp:%02x Tden:%02x DOS:%02x Side:%02x Crc:%04x\n",
              h.advanced ? 0x6474 : 0x4454, h.sequence, h.checkSequence, h.version, h.dataRate, h.driveType,
              h.stepping, h.dosMode, h.sides, h.crc);

        static const std::vector<const char*> drives = {"5.25-96/48", "5.25", "5.25-96", "3.5", "3.5", "8\"", "3.5"};
        static const std::vector<const char*> rates = {"LD", "LD", "HD"};
        static const std::vector<const char*> steps = {"S", "D", "E"};
        char line[160];
        int n = std::snprintf(line, sizeof line, "TD %u.%u %s", h.version >> 4, h.version & 15,
                              tableText(h.driveType, drives));
        n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " %s %sFM",
                           tableText(h.dataRate & 0x7F, rates), (h.dataRate & 0x80) ? "" : "M");
        n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " %s-step, %u sides",
                           tableText(h.stepping & 0x7F, steps), h.sides);
        if (h.advanced)
            n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " ADV");
        if (h.dosMode)
            std::snprintf(line + n, sizeof line - static_cast<size_t>(n), ", DOS");
        if (verbose)
            std::fputs(line, stdout);

        std::vector<uint8_t> out;
        append(out, std::string("IMD ") + line);
        if (h.hasComment()) {
            const imd::Td0Comment& c = td0.comment();
            char date[64];
            std::snprintf(date, sizeof date, " %u/%02u/%04u %u:%02u:%02u", c.day, c.month + 1, c.year + 1900,
                          c.hour, c.minute, c.second);
            std::printf("%s\n", date);
            append(out, date);
            append(out, "\r\n");
            // NULs end lines; trailing ones are dropped.
            unsigned pendingNewlines = 0;
            for (uint8_t ch : c.text) {
                if (!ch) {
                    ++pendingNewlines;
                    continue;
                }
                for (; pendingNewlines; --pendingNewlines) {
                    std::putchar('\n');
                    append(out, "\r\n");
                }
                std::putchar(ch);
                if (ch == '\n')
                    out.push_back('\r');
                out.push_back(ch);
            }
        }
        // End the header line, or the comment, which is shown even with /Q.
        if (verbose || h.hasComment())
            std::putchar('\n');
        append(out, "\r\n");
        out.push_back(imd::kCommentEnd);

        if ((h.dataRate & 0x7F) > 2)
            imd::fatal("Unknown data rate");

        unsigned tracks = 0, sectorsRead = 0, removed = 0;
        imd::Td0Track t;
        while (td0.nextTrack(t)) {
            curCyl = t.cyl;
            curHead = t.side & 1;
            debug(kDumpTracks, "T#sec:%02x cyl:%02x side:%02x crc:%02x\n", t.sectorCount, t.cyl, t.side, t.crc);
            if (t.side & 0x7E)
                imd::fatal("Unknown track-side indicator");

            // IMD mode: rate 0/1/2 (250/300/500) -> 5/4/3 MFM, minus 3 for FM.
            unsigned mode = (2 - (h.dataRate & 0x03u)) + 3;
            if (mode == 4 && convertLowDensity)
                mode = 5;
            if ((t.side & 0x80) || ((h.dataRate & 0x80) && assumeFm))
                mode -= 3;

            warnedThisTrack = warningsOff;
            ++tracks;

            std::vector<Sector> sectors;
            for (auto& ts : t.sectors) {
                ++sectorsRead;
                debug(kDumpSectors, "Scyl:%02x side:%02x num:%02x size:%02x flag:%02x crc:%02x", ts.cyl, ts.head,
                      ts.id, ts.sizeCode, ts.flags, ts.crc);
                if (ts.hasData)
                    debug(kDumpSectors, " offset:%04x method:%02x", ts.dataLength, ts.method);
                debug(kDumpSectors, "\n");
                if (ts.head & 0xFE)
                    warning(kWarnHead, "Head not 0 or 1");

                Sector s{ts.id, ts.cyl, ts.head, ts.sizeCode, ts.flags, 0, {}};
                s.type = (ts.flags & imd::kTd0Deleted) ? 3 : 1;
                if (ts.flags & imd::kTd0CrcError)
                    s.type += 4;
                if (ts.flags & imd::kTd0NoData)
                    s.type = 0;
                else if (ts.flags & imd::kTd0DosSkipped)
                    s.data.assign(imd::sectorBytes(ts.sizeCode), 0xE5);
                else
                    s.data = std::move(ts.data);
                if (s.data.empty())
                    s.data.assign(imd::sectorBytes(ts.sizeCode), 0);
                debug(kShowSectors, " %zu %x %zu\n", sectors.size(), s.type,
                      (sectors.size() + 1) * imd::sectorBytes(ts.sizeCode));
                sectors.push_back(std::move(s));
            }

            uint8_t sizeCode = sectors.empty() ? 0 : sectors[0].sizeCode;
            bool cylMap = false, headMap = false;
            for (const auto& s : sectors) {
                if (s.sizeCode != sizeCode)
                    imd::fatal("Cannot do mixed sector size within track");
                cylMap = cylMap || s.cyl != curCyl;
                headMap = headMap || s.head != curHead;
            }

            removeSectors(sectors, removed);

            for (size_t i = 0; i < sectors.size(); ++i) {
                uint8_t f = sectors[i].flags;
                if (f & imd::kTd0NoId)
                    warning(kWarnNoId, "No ID field - possible bogus sector");
                if (f & imd::kTd0NoData)
                    warning(kWarnNoData, "No DATA field");
                if (f & imd::kTd0CrcError)
                    warning(kWarnCrc, "CRC error (bad sector)");
                for (size_t j = i + 1; j < sectors.size(); ++j) {
                    if (sectors[i].id == sectors[j].id) {
                        warning(kWarnDuplicate, "Duplicate sector numbers");
                        break;
                    }
                }
            }

            if (sectors.empty())
                continue;
            imd::Track track;
            track.mode = static_cast<uint8_t>(mode);
            track.cyl = static_cast<uint8_t>(curCyl);
            track.head = static_cast<uint8_t>(curHead);
            track.cylMap = cylMap;
            track.headMap = headMap;
            track.sizeCode = sizeCode;
            for (auto& s : sectors) {
                imd::Sector is;
                is.id = s.id;
                is.cyl = s.cyl;
                is.head = s.head;
                is.available = s.type != 0;
                is.deleted = is.available && ((s.type - 1) & 2);
                is.bad = is.available && ((s.type - 1) & 4);
                is.data = std::move(s.data);
                track.sectors.push_back(std::move(is));
            }
            imd::appendTrack(out, track, imd::Compression::Compress, track.mode);
            if (debugFlags & kOneTrack)
                break;
        }

        imd::writeFile(outName, out);
        if (verbose) {
            std::printf("%u tracks, %u sectors converted", tracks, sectorsRead);
            if (removed)
                std::printf(", %u sectors removed", removed);
            std::printf(".\n");
        }
    } catch (const imd::Td0Error& e) {
        imd::fatal("%s", e.what());
    }
    return 0;
}
