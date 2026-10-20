// bin2imd - convert a raw binary disk image into ImageDisk (.IMD) format.
#include "cli.hpp"
#include "imd_image.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* const kHelp =
    "\nUse: bin2imd binary-input-file IMD-output-file [option-file] [options]\n\n"
    "opts:  /1                  - 1-sided output\n"
    "       /2                  - 2-sided output\n"
    "       /C                  - write Compressed sectors\n"
    "       /U                  - write Uncompressed sectors\n"
    "       /V[0|1]             - Verbose output\n"
    "       C=text | @file      - image Comment\n"
    "       N=#cylinders        - set Number of output cylinders\n"
    "       DM[s]=0-5           - track Data Mode\n"
    "       SS[s]=128-8192      - track Sector Size\n"
    "       SM[s]=n[,n-n][n.#]  - track Sector numbering Map\n"
    "       CM[s]=n[,n-n][n.#]  - track/sector Cylinder Map\n"
    "       HM[s]=n[,n-n][n.#]  - track/sector Head Map\n"
    "\nNumbers are decimal, or $hex, @octal, %binary.\n";

constexpr size_t kMaxComment = 32767;
constexpr size_t kMaxMap = 255;

struct Side {
    unsigned mode = 255;  // 255 = not defined
    unsigned sizeCode = 0;
    unsigned sizeBytes = 0;  // 0 = not defined
    std::vector<uint8_t> smap, cmap, hmap;
};

Side sides[2];
bool twoSided = false;
unsigned verbose = 0;  // 0 = off, 0x0F = summary, 0xFF = summary and maps
uint8_t fillByte = 0;
bool compress = true;
std::vector<uint8_t> comment;
long cylinders = -1;

// Text naming the option (or side) being processed, used as an error prefix.
std::string context;

[[noreturn]] void error(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (context.empty())
        imd::fatal("%s", buf);
    imd::fatal("%s: %s", context.c_str(), buf);
}

// Parse one number at `p`: decimal, or $hex, @octal, %binary. Stops at a delimiter
// (',', '-', '.') or the end of the text, which is returned (0 at the end).
char getNumber(const char*& p, unsigned& value, unsigned lo, unsigned hi)
{
    unsigned base = 10;
    switch (*p) {
    case '$': base = 16; ++p; break;
    case '@': base = 8; ++p; break;
    case '%': base = 2; ++p; break;
    }
    unsigned long v;
    if (!imd::scanNumber(p, base, v) || (*p && *p != ',' && *p != '-' && *p != '.'))
        error("Bad digit '%c'", *p ? *p : ' ');
    if (v < lo || v > hi)
        error("Value %lu out of range %u-%u", v, lo, hi);
    value = static_cast<unsigned>(v);
    char delim = *p;
    if (delim)
        ++p;
    return delim;
}

unsigned getSingle(const std::string& text, unsigned lo, unsigned hi)
{
    const char* p = text.c_str();
    unsigned v;
    if (getNumber(p, v, lo, hi))
        error("Bad digit '%c'", p[-1]);
    return v;
}

void addMapEntry(std::vector<uint8_t>& map, unsigned v)
{
    if (map.size() >= kMaxMap)
        error("Too large (exceeds %zu entries)", kMaxMap);
    map.push_back(static_cast<uint8_t>(v));
}

// Map syntax: n[,n...] where n-m adds an ascending or descending range and n.count
// repeats the last value until it appears `count` times in a row.
std::vector<uint8_t> getMap(const std::string& text, unsigned lo, unsigned hi)
{
    std::vector<uint8_t> map;
    const char* p = text.c_str();
    unsigned v;
    char c = getNumber(p, v, lo, hi);
    addMapEntry(map, v);
    while (c) {
        unsigned j;
        switch (c) {
        case ',':
            c = getNumber(p, v, lo, hi);
            addMapEntry(map, v);
            break;
        case '-':
            c = getNumber(p, j, lo, hi);
            while (v < j)
                addMapEntry(map, ++v);
            while (v > j)
                addMapEntry(map, --v);
            break;
        case '.':
            c = getNumber(p, j, 1, 256);
            while (--j)
                addMapEntry(map, v);
            break;
        }
    }
    return map;
}

void readComment(const std::string& text)
{
    if (!text.empty() && text[0] == '@') {
        auto data = imd::readFile(imd::inputName(text.substr(1), ".TXT"));
        if (comment.size() + data.size() > kMaxComment)
            error("Too large (exceeds %zu bytes)", kMaxComment);
        comment.insert(comment.end(), data.begin(), data.end());
        return;
    }
    if (comment.size() + text.size() + 2 > kMaxComment)
        error("Too large (exceeds %zu bytes)", kMaxComment);
    for (char ch : text)
        comment.push_back(static_cast<uint8_t>(ch == '~' ? ' ' : ch));
    comment.push_back('\r');
    comment.push_back('\n');
}

bool isKnownSwitch(const std::string& s)
{
    return s == "1" || s == "2" || s == "C" || s == "U" || s == "V" || s == "V0" || s == "V1";
}

// Apply one option. Returns false when `arg` is not an option. `inTrackRecord` is set for
// option-file records after cylinder 0, where C= and N= are not allowed.
bool option(const std::string& arg, bool inTrackRecord)
{
    context = arg;
    std::string body;
    if (imd::switchBody(arg, isKnownSwitch, body)) {
        std::string s = imd::toUpper(body);
        if (s == "1")
            twoSided = false;
        else if (s == "2")
            twoSided = true;
        else if (s == "C")
            compress = true;
        else if (s == "U")
            compress = false;
        else if (s == "V")
            verbose = 0x0F;
        else if (s == "V0")
            verbose = 0;
        else if (s == "V1")
            verbose = 0xFF;
        else
            error("Bad option");
        context.clear();
        return true;
    }

    std::string key, value;
    static const std::vector<std::string> keys = {
        "F", "N", "C", "DM", "DM0", "DM1", "SS", "SS0", "SS1", "SM", "SM0", "SM1",
        "CM", "CM0", "CM1", "HM", "HM0", "HM1"};
    if (!imd::keyValue(arg, keys, key, value)) {
        context.clear();
        return false;
    }

    if (key == "F") {
        fillByte = static_cast<uint8_t>(getSingle(value, 0, 255));
    } else if (key == "N" || key == "C") {
        if (inTrackRecord)
            error("Not allowed in track records");
        if (key == "N")
            cylinders = getSingle(value, 1, 256);
        else
            readComment(value);
    } else {
        // Two-letter key with an optional side digit. No digit applies to both sides.
        bool both = key.size() == 2;
        Side& s = sides[key.size() == 3 && key[2] == '1' ? 1 : 0];
        if (key.size() == 3 && key[2] == '1')
            twoSided = true;
        std::string base = key.substr(0, 2);
        if (base == "DM") {
            s.mode = getSingle(value, 0, 5);
        } else if (base == "SS") {
            unsigned bytes = getSingle(value, 0, 8192);
            unsigned code = 0;
            while (code <= imd::kMaxSizeCode && imd::sectorBytes(code) != bytes)
                ++code;
            if (code > imd::kMaxSizeCode)
                error("Bad sector size");
            s.sizeCode = code;
            s.sizeBytes = bytes;
        } else if (base == "SM") {
            s.smap = getMap(value, 0, 255);
        } else if (base == "CM") {
            s.cmap = getMap(value, 0, 255);
        } else if (base == "HM") {
            s.hmap = getMap(value, 0, 1);
        }
        if (both)
            sides[1] = sides[0];
    }
    context.clear();
    return true;
}

void showMap(const char* title, const std::vector<uint8_t>& map)
{
    std::printf("%5s:", title);
    unsigned col = 0;
    for (uint8_t v : map) {
        if (++col > 18) {
            std::printf("\n     ");
            col = 1;
        }
        std::printf("%4u", v);
    }
    std::putchar('\n');
}

void validate(unsigned cyl, unsigned side)
{
    const Side& s = sides[side];
    context = side ? "Side1" : "Side0";
    if (verbose) {
        std::printf("%3u/%u:  Mode:%u  Sectors:%zu  Ssize:%u/%u\n", cyl, side, s.mode, s.smap.size(),
                    s.sizeCode, s.sizeBytes);
        if (verbose & 0xF0) {
            showMap("Smap", s.smap);
            if (!s.hmap.empty())
                showMap("Hmap", s.hmap);
            if (!s.cmap.empty())
                showMap("Cmap", s.cmap);
        }
    }
    if (s.mode > imd::kMaxMode)
        error("Data Mode must be defined.");
    if (!s.sizeBytes)
        error("Sector Size must be defined");
    if (s.smap.empty())
        error("At least 1 sector must be defined");
    if ((!s.cmap.empty() && s.cmap.size() != s.smap.size()) ||
        (!s.hmap.empty() && s.hmap.size() != s.smap.size()))
        error("Cylinder/Head maps must match sector map size");
    bool seen[256] = {};
    for (uint8_t id : s.smap) {
        if (seen[id])
            error("Duplicate sector map entry %u", id);
        seen[id] = true;
    }
    context.clear();
}

// Option file: lines of "<cylinder> options...", with ';' comment lines.
class OptionFile {
public:
    void open(const std::string& path)
    {
        in_.open(path);
        if (!in_)
            imd::fatal("Cannot open %s", path.c_str());
        isOpen_ = true;
        next();
    }

    // Cylinder at which the pending record applies, or -1 when there are no more.
    long cylinder() const { return cylinder_; }
    const std::vector<std::string>& options() const { return options_; }

    void next()
    {
        cylinder_ = -1;
        options_.clear();
        std::string line;
        while (isOpen_ && std::getline(in_, line)) {
            std::vector<std::string> tokens;
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                    ++i;
                size_t start = i;
                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
                    ++i;
                if (i > start)
                    tokens.push_back(line.substr(start, i - start));
            }
            if (tokens.empty() || tokens[0][0] == ';')
                continue;
            context = tokens[0];
            const char* p = tokens[0].c_str();
            unsigned cyl;
            if (getNumber(p, cyl, 0, 65535))
                error("Bad digit '%c'", p[-1]);
            context.clear();
            cylinder_ = cyl;
            options_.assign(tokens.begin() + 1, tokens.end());
            return;
        }
    }

private:
    std::ifstream in_;
    bool isOpen_ = false;
    long cylinder_ = -1;
    std::vector<std::string> options_;
};

void applyRecord(const OptionFile& f, bool inTrackRecord)
{
    for (const auto& opt : f.options()) {
        if (!option(opt, inTrackRecord)) {
            context = opt;
            error("Bad option");
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    std::fputs("BINary-2-IMageDisk " IMD_VERSION " / " __DATE__ "\n", stdout);

    std::string inFile, outFile, optFile;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (option(arg, false))
            continue;
        if (arg.size() > 1 && arg[0] == '-') {
            context = arg;
            error("Bad option");
        }
        if (inFile.empty())
            inFile = imd::inputName(arg, ".BIN");
        else if (outFile.empty())
            outFile = imd::outputName(arg, ".IMD");
        else if (optFile.empty())
            optFile = imd::inputName(arg, ".B2I");
        else {
            context = arg;
            error("Bad option");
        }
    }
    if (inFile.empty() || outFile.empty()) {
        std::fputs(kHelp, stdout);
        return 1;
    }

    OptionFile records;
    if (!optFile.empty()) {
        records.open(optFile);
        if (records.cylinder() == 0) {
            applyRecord(records, false);
            records.next();
        }
    }

    validate(0, 0);
    if (twoSided)
        validate(0, 1);
    if (cylinders < 0)
        imd::fatal("N= (cylinders) not specified");

    std::vector<uint8_t> input = imd::readFile(inFile);
    size_t inPos = 0;
    bool inputEnded = false;

    std::time_t now = std::time(nullptr);
    std::tm tm = *std::localtime(&now);
    char header[64];
    std::snprintf(header, sizeof header, "IMD " IMD_VERSION ": %2u/%02u/%04u %2u:%02u:%02u\r\n",
                  static_cast<unsigned>(tm.tm_mday), static_cast<unsigned>(tm.tm_mon + 1),
                  static_cast<unsigned>(tm.tm_year + 1900), static_cast<unsigned>(tm.tm_hour),
                  static_cast<unsigned>(tm.tm_min), static_cast<unsigned>(tm.tm_sec));
    std::vector<uint8_t> out(header, header + std::char_traits<char>::length(header));
    out.insert(out.end(), comment.begin(), comment.end());
    out.push_back(imd::kCommentEnd);

    for (unsigned cyl = 0; cyl < static_cast<unsigned>(cylinders); ++cyl) {
        if (records.cylinder() == static_cast<long>(cyl)) {
            applyRecord(records, true);
            validate(cyl, 0);
            if (twoSided)
                validate(cyl, 1);
            records.next();
        }
        for (unsigned head = 0; head < (twoSided ? 2u : 1u); ++head) {
            const Side& s = sides[head];
            imd::Track t;
            t.mode = static_cast<uint8_t>(s.mode);
            t.cyl = static_cast<uint8_t>(cyl);
            t.head = static_cast<uint8_t>(head);
            t.cylMap = !s.cmap.empty();
            t.headMap = !s.hmap.empty();
            t.sizeCode = static_cast<uint8_t>(s.sizeCode);
            for (size_t i = 0; i < s.smap.size(); ++i) {
                imd::Sector sec;
                sec.id = s.smap[i];
                sec.cyl = t.cylMap ? s.cmap[i] : t.cyl;
                sec.head = t.headMap ? s.hmap[i] : t.head;
                sec.available = true;
                sec.data.assign(s.sizeBytes, fillByte);
                if (!inputEnded) {
                    size_t n = std::min<size_t>(s.sizeBytes, input.size() - inPos);
                    std::copy_n(input.begin() + static_cast<long>(inPos), n, sec.data.begin());
                    inPos += n;
                    if (n != s.sizeBytes) {
                        std::printf("Input file is smaller than output image (ends at CHS %u/%u/%zu)\n",
                                    cyl, head, i);
                        inputEnded = true;
                    }
                }
                t.sectors.push_back(std::move(sec));
            }
            imd::appendTrack(out, t, compress ? imd::Compression::Compress : imd::Compression::Expand,
                             t.mode);
        }
    }

    imd::writeFile(outFile, out);
    if (!inputEnded && inPos < input.size())
        std::printf("Input file is larger than output image.\n");
    return 0;
}
