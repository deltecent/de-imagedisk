// imdu - ImageDisk utility: display, convert and merge .IMD images.
#include "cli.hpp"
#include "imd_image.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* const kHelp =
    "\nUse: imdu image [[merge-image] [output-image]] [options]\n\n"
    "opts:  /B                  - output Binary image\n"
    "       /C                  - Compress \"all-same\" sectors\n"
    "       /D                  - display track/sector Detail\n"
    "       /E                  - Expand compressed sectors to full data\n"
    "       /M                  - ignore Mode difference in merge/compare\n"
    "       /NB                 - force Non-Bad data\n"
    "       /ND                 - force Non-Deleted data\n"
    "       /Q                  - Quiet: suppress warnings\n"
    "       /Y                  - auto-Yes (no overwrite prompt)\n"
    "       --json              - print the image summary as JSON\n"
    "       AC=file[.TXT]       - Append Comment from file        [none]\n"
    "       EC=file[.TXT]       - Extract Comment to file         [none]\n"
    "       F=xx                - missing sector Fill value       [00]\n"
    "       IL=[1-99]           - reInterLeave(blank=BestGuess)   [As read]\n"
    "       RC=file[.TXT]       - Replace Comment from file       [none]\n"
    "       T2=250/300/500      - 250kbps Translate               [250]\n"
    "       T3=250/300/500      - 300kbps Translate               [300]\n"
    "       T5=250/300/500      - 500kbps Translate               [500]\n"
    "       X=track[,to_track]  - eXclude entire track(s)         [None]\n"
    "       X0=track[,to_track] - eXclude track(s) side 0 only    [None]\n"
    "       X1=track[,to_track] - eXclude track(s) side 1 only    [None]\n";

constexpr unsigned kBestGuess = 255;

// Options
bool binaryOut = false;
bool detail = false;
bool verbose = true;
bool autoYes = false;
bool ignoreMode = false;
bool dropBad = false;
bool dropDeleted = false;
bool json = false;
bool fillMissing = false;
uint8_t fillByte = 0;
unsigned interleave = 0;  // 0 = as read, kBestGuess = calculate, else the step
uint8_t modeMap[6] = {0, 1, 2, 3, 4, 5};
uint8_t excluded[256] = {};  // bit 0 = exclude head 0, bit 1 = exclude head 1
imd::Compression compression = imd::Compression::Keep;
std::string appendCommentFile, extractCommentFile;
bool replaceComment = false;

// Statistics
unsigned head0Tracks = 0, head1Tracks = 0;
unsigned totalSectors = 0, compressedCount = 0, deletedCount = 0, badCount = 0, unavailCount = 0;

// Physical track currently being processed, used to prefix messages.
int curCyl = -1, curHead = -1;

// Informational output; goes to stderr in JSON mode so stdout carries only JSON.
FILE* info = stdout;

// Print "cc/h " when the current track differs from the last one shown, else blanks.
void trackPrefix(FILE* f = stdout)
{
    static int shownCyl = -1, shownHead = -1;
    if (curCyl != shownCyl || curHead != shownHead) {
        shownCyl = curCyl;
        shownHead = curHead;
        std::fprintf(f, "%2d/%d ", curCyl, curHead);
    } else {
        std::fprintf(f, "     ");
    }
}

void warning(const char* fmt, ...)
{
    if (!verbose)
        return;
    trackPrefix(info);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(info, fmt, ap);
    va_end(ap);
    std::fputc('\n', info);
}

[[noreturn]] void help()
{
    std::fputs(kHelp, stdout);
    std::exit(1);
}

bool isKnownSwitch(const std::string& s)
{
    static const std::set<std::string> known = {"B", "C", "D", "E", "M", "NB", "ND", "Q", "Y"};
    return known.count(s) != 0;
}

// Parse a number terminated by ',' or the end of the text.
unsigned number(const char*& p, unsigned base, unsigned lo, unsigned hi)
{
    unsigned long v;
    if (!imd::scanNumber(p, base, v)) {
        if (imd::digitValue(*p) >= 0)
            imd::fatal("Bad numeric value");
        imd::fatal("Bad number");
    }
    if (*p && *p != ',')
        imd::fatal("Bad number");
    if (v < lo || v > hi) {
        if (base == 16)
            imd::fatal("Value (%lx) out of range: %x-%x", v, lo, hi);
        imd::fatal("Value (%lu) out of range: %u-%u", v, lo, hi);
    }
    return static_cast<unsigned>(v);
}

// Step past a ',' before another value; true if one follows.
bool nextValue(const char*& p)
{
    if (*p == ',') {
        ++p;
        return true;
    }
    if (*p)
        imd::fatal("Syntax error");
    return false;
}

// Require the end of the value text.
void endValue(const char* p)
{
    if (*p)
        imd::fatal("Syntax error");
}

void excludeTracks(const std::string& value, uint8_t heads)
{
    const char* p = value.c_str();
    unsigned first = number(p, 10, 0, 255), last = first;
    if (nextValue(p))
        last = number(p, 10, 0, 255);
    endValue(p);
    for (unsigned c = first; c <= last; ++c)
        excluded[c] |= heads;
}

void translateMode(const std::string& value, unsigned rateIndex)
{
    const char* p = value.c_str();
    unsigned rate = number(p, 10, 0, 65535);
    endValue(p);
    uint8_t to;
    switch (rate) {
    case 500: to = 0; break;
    case 300: to = 1; break;
    case 250: to = 2; break;
    default: help();
    }
    modeMap[rateIndex] = to;
    modeMap[rateIndex + 3] = static_cast<uint8_t>(to + 3);
}

void updateStats(const imd::Track& t)
{
    ++(t.head ? head1Tracks : head0Tracks);
    for (const auto& s : t.sectors) {
        ++totalSectors;
        if (!s.available) {
            ++unavailCount;
            continue;
        }
        compressedCount += s.compressed;
        deletedCount += s.deleted;
        badCount += s.bad;
    }
}

// ---------------------------------------------------------------- display

void printMapLine(const std::vector<unsigned>& values)
{
    for (unsigned v : values) {
        if (values.size() < 19)
            std::putchar(' ');
        std::printf("%-3u", v);
    }
    std::putchar('\n');
}

void showDetail(const imd::Track& t)
{
    std::vector<unsigned> values;
    if (t.cylMap) {
        for (const auto& s : t.sectors)
            values.push_back(s.cyl);
        std::printf(" CL: ");
        printMapLine(values);
    }
    if (t.headMap) {
        values.clear();
        for (const auto& s : t.sectors)
            values.push_back(s.head);
        std::printf(" HD: ");
        printMapLine(values);
    }
    trackPrefix();
    for (const auto& s : t.sectors) {
        if (t.sectors.size() < 19)
            std::putchar(' ');
        if (!s.available) {
            std::printf("U  ");
            continue;
        }
        char c = s.bad ? 'B' : 'D';
        if (s.deleted)
            c = static_cast<char>(c + ('a' - 'A'));
        if (s.compressed)
            std::printf("%c%02x", c, s.data.empty() ? 0 : s.data.front());
        else
            std::printf("%c  ", c);
    }
    std::putchar('\n');
}

// Summary display state: the last format shown.
struct Display {
    bool first = true;
    unsigned mode = 0, nsec = 0, size = 0;
    std::vector<uint8_t> ids;
};

void displayTrack(Display& d, const imd::Track& t)
{
    std::vector<uint8_t> ids;
    for (const auto& s : t.sectors)
        ids.push_back(s.id);
    bool changed = d.first || t.mode != d.mode || t.sectors.size() != d.nsec || t.sizeCode != d.size;
    d.first = false;
    if (changed) {
        trackPrefix();
        if (t.sectors.empty()) {
            std::printf("No data\n");
            return;
        }
        d.mode = t.mode;
        d.nsec = static_cast<unsigned>(t.sectors.size());
        d.size = t.sizeCode;
        std::printf("%u kbps %cD  %ux%u\n", imd::modeRate(t.mode), imd::modeIsMfm(t.mode) ? 'D' : 'S',
                    d.nsec, t.sectorSize());
    }
    if (changed || ids != d.ids) {
        d.ids = ids;
        if (detail) {
            trackPrefix();
            printMapLine(std::vector<unsigned>(ids.begin(), ids.end()));
        }
    }
    if (detail)
        showDetail(t);
}

void printComment(const std::vector<uint8_t>& comment)
{
    for (size_t i = 0; i < comment.size(); ++i) {
        if (comment[i] == '\r' && i + 1 < comment.size() && comment[i + 1] == '\n')
            continue;
        std::putchar(comment[i]);
    }
}

// ---------------------------------------------------------------- JSON

std::string jsonString(const std::vector<uint8_t>& bytes)
{
    std::string s = "\"";
    for (uint8_t b : bytes) {
        switch (b) {
        case '"': s += "\\\""; break;
        case '\\': s += "\\\\"; break;
        case '\n': s += "\\n"; break;
        case '\r': s += "\\r"; break;
        case '\t': s += "\\t"; break;
        default:
            if (b < 0x20 || b >= 0x7F) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", b);
                s += buf;
            } else {
                s += static_cast<char>(b);
            }
        }
    }
    return s + "\"";
}

const char* jsonBool(bool b) { return b ? "true" : "false"; }

void printJson(const std::string& file, const std::vector<uint8_t>& comment,
               const std::vector<imd::Track>& tracks)
{
    std::printf("{\n  \"file\": %s,\n", jsonString(std::vector<uint8_t>(file.begin(), file.end())).c_str());
    std::printf("  \"comment\": %s,\n  \"tracks\": [", jsonString(comment).c_str());
    for (size_t i = 0; i < tracks.size(); ++i) {
        const imd::Track& t = tracks[i];
        std::printf("%s\n    {\"cylinder\": %u, \"head\": %u, \"mode\": %u, \"rate_kbps\": %u, "
                    "\"encoding\": \"%s\", \"sector_size\": %u, \"cylinder_map\": %s, \"head_map\": %s,\n"
                    "     \"sectors\": [",
                    i ? "," : "", t.cyl, t.head, t.mode, imd::modeRate(t.mode),
                    imd::modeIsMfm(t.mode) ? "MFM" : "FM", t.sectorSize(), jsonBool(t.cylMap),
                    jsonBool(t.headMap));
        for (size_t j = 0; j < t.sectors.size(); ++j) {
            const imd::Sector& s = t.sectors[j];
            std::printf("%s\n       {\"id\": %u, \"cylinder\": %u, \"head\": %u, \"available\": %s",
                        j ? "," : "", s.id, s.cyl, s.head, jsonBool(s.available));
            if (s.available) {
                std::printf(", \"compressed\": %s, \"deleted\": %s, \"bad\": %s", jsonBool(s.compressed),
                            jsonBool(s.deleted), jsonBool(s.bad));
                if (s.compressed)
                    std::printf(", \"fill\": %u", s.data.empty() ? 0 : s.data.front());
            }
            std::printf("}");
        }
        std::printf("]}");
    }
    std::printf("\n  ],\n  \"totals\": {\"tracks\": %u, \"head0_tracks\": %u, \"head1_tracks\": %u, "
                "\"sectors\": %u, \"compressed\": %u, \"deleted\": %u, \"bad\": %u, \"unavailable\": %u}\n}\n",
                head0Tracks + head1Tracks, head0Tracks, head1Tracks, totalSectors, compressedCount,
                deletedCount, badCount, unavailCount);
}

// ---------------------------------------------------------------- merge

void mergeSector(imd::Track& t1, size_t i, const imd::Track& t2)
{
    imd::Sector& s1 = t1.sectors[i];
    auto it = std::find_if(t2.sectors.begin(), t2.sectors.end(),
                           [&](const imd::Sector& s) { return s.id == s1.id; });
    if (it == t2.sectors.end()) {
        warning("Sector %u not found in merge image", s1.id);
        return;
    }
    const imd::Sector& s2 = *it;
    size_t j = static_cast<size_t>(it - t2.sectors.begin());
    if (!s1.available) {
        if (s2.available) {
            warning("Filling in sector %zu/%u", i, s1.id);
            s1.available = true;
            s1.compressed = s2.compressed;
            s1.deleted = s2.deleted;
            s1.bad = s2.bad;
            s1.data = s2.data;
        } else {
            warning("Sector %zu/%u data missing in merge image", i, s1.id);
        }
        return;
    }
    if (!s2.available)
        return;
    if (s1.deleted != s2.deleted)
        warning("Deleted data status differs: %zu %zu", i, j);
    if (s1.bad != s2.bad)
        warning("Bad sector status differs: %zu %zu", i, j);
    if (s1.data != s2.data)
        warning("Merge data differs from original %zu %zu", i, j);
}

void mergeTrack(imd::Track& t1, const imd::Track& t2)
{
    if (t2.mode != t1.mode && !ignoreMode) {
        warning("Incompatible track mode");
        return;
    }
    if (t2.sizeCode != t1.sizeCode) {
        warning("Incompatible sector size");
        return;
    }
    size_t n = t1.sectors.size();
    for (size_t i = 0; i < n; ++i)
        mergeSector(t1, i, t2);

    for (const auto& s2 : t2.sectors) {
        bool found = std::any_of(t1.sectors.begin(), t1.sectors.end(),
                                 [&](const imd::Sector& s) { return s.id == s2.id; });
        if (found)
            continue;
        if (t1.sectors.size() >= 255) {
            warning("Too many sectors to add sector %u", s2.id);
            continue;
        }
        warning("Adding sector %zu/%u", t1.sectors.size(), s2.id);
        t1.sectors.push_back(s2);
        t1.cylMap = t1.cylMap || s2.cyl != t1.cyl;
        t1.headMap = t1.headMap || s2.head != t1.head;
    }
}

// ---------------------------------------------------------------- output

// Reorder sectors to the requested interleave (or a best guess at the original one).
void reinterleave(imd::Track& t)
{
    size_t n = t.sectors.size();
    if (n < 2)
        return;
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return t.sectors[a].id < t.sectors[b].id; });

    size_t step = interleave;
    if (interleave == kBestGuess) {
        // The most common physical distance between consecutive sector numbers.
        std::vector<unsigned> count(n, 0);
        for (size_t i = 0; i < n; ++i)
            ++count[(order[(i + 1) % n] + n - order[i]) % n];
        step = 0;
        for (size_t g = 0; g < n; ++g)
            if (count[g] > count[step])
                step = g;
    }

    std::vector<imd::Sector> out(n);
    std::vector<bool> used(n, false);
    size_t pos = 0;
    for (size_t i = 0; i < n; ++i) {
        while (used[pos])
            pos = (pos + 1) % n;
        out[pos] = t.sectors[order[i]];
        used[pos] = true;
        pos = (pos + step) % n;
    }
    t.sectors = std::move(out);
}

void outputTrack(std::vector<uint8_t>& out, imd::Track t)
{
    if (excluded[t.cyl] & (t.head ? 2 : 1))
        return;
    if (interleave)
        reinterleave(t);
    if (fillMissing) {
        for (auto& s : t.sectors) {
            if (!s.available) {
                s.available = true;
                s.compressed = false;
            }
        }
    }
    if (binaryOut) {
        for (const auto& s : t.sectors)
            out.insert(out.end(), s.data.begin(), s.data.end());
        return;
    }
    imd::appendTrack(out, t, compression, modeMap[t.mode]);
}

// Ask before overwriting `path`. Never blocks when stdin is not a terminal.
void confirmOverwrite(const std::string& path)
{
    if (autoYes || !imd::fileExists(path))
        return;
    if (!imd::stdinIsTerminal())
        imd::fatal("%s already exists - use /Y to overwrite", path.c_str());
    std::fprintf(info, "%s already exists - proceed(Y/N)?", path.c_str());
    for (;;) {
        int k = imd::readKey();
        if (k == 'y' || k == 'Y') {
            std::fprintf(info, "Y\n");
            return;
        }
        if (k == 'n' || k == 'N' || k == 0x1B || k == 0x03 || k < 0) {
            std::fprintf(info, "N\n");
            std::exit(1);
        }
    }
}

imd::Image load(const std::string& path)
{
    imd::Image image;
    try {
        image = imd::readImage(path);
    } catch (const imd::FormatError& e) {
        if (e.cyl >= 0)
            imd::fatal("%s: %2d/%d %s", path.c_str(), e.cyl, e.head < 0 ? 0 : e.head, e.what());
        imd::fatal("%s: %s", path.c_str(), e.what());
    }
    for (auto& t : image.tracks) {
        for (auto& s : t.sectors) {
            if (dropDeleted)
                s.deleted = false;
            if (dropBad)
                s.bad = false;
            if (!s.available)
                s.data.assign(t.sectorSize(), fillByte);
        }
    }
    return image;
}

using Key = std::pair<unsigned, unsigned>;
Key key(const imd::Track& t) { return {t.cyl, t.head}; }

}  // namespace

int main(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (imd::iequals(argv[i], "--json")) {
            json = true;
            info = stderr;
        }
    }
    if (!json)
        std::fputs("IMageDisk Utility " IMD_VERSION " / " __DATE__ "\n", stdout);

    std::string inFile, secondFile, thirdFile;
    bool converting = false;  // an option was given that changes the image

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string body, key, value;
        if (imd::iequals(arg, "--json"))
            continue;
        if (imd::switchBody(arg, isKnownSwitch, body)) {
            std::string s = imd::toUpper(body);
            if (s == "B") {
                binaryOut = true;
                converting = true;
            } else if (s == "C") {
                compression = imd::Compression::Compress;
                converting = true;
            } else if (s == "E") {
                compression = imd::Compression::Expand;
                converting = true;
            } else if (s == "ND") {
                dropDeleted = true;
                converting = true;
            } else if (s == "NB") {
                dropBad = true;
                converting = true;
            } else if (s == "D") {
                detail = true;
            } else if (s == "M") {
                ignoreMode = true;
            } else if (s == "Q") {
                verbose = false;
            } else if (s == "Y") {
                autoYes = true;
            } else {
                help();
            }
            continue;
        }
        static const std::vector<std::string> keys = {"AC", "EC", "RC", "F", "IL", "T2", "T3",
                                                      "T5", "X",  "X0", "X1"};
        if (imd::keyValue(arg, keys, key, value)) {
            const char* p = value.c_str();
            if (key == "EC") {
                extractCommentFile = value;
                continue;
            }
            converting = true;
            if (key == "AC" || key == "RC") {
                appendCommentFile = value;
                replaceComment = key == "RC";
            } else if (key == "F") {
                fillByte = static_cast<uint8_t>(number(p, 16, 0, 255));
                endValue(p);
                fillMissing = true;
            } else if (key == "IL") {
                interleave = *p ? number(p, 10, 1, 99) : kBestGuess;
                endValue(p);
            } else if (key == "T5") {
                translateMode(value, 0);
            } else if (key == "T3") {
                translateMode(value, 1);
            } else if (key == "T2") {
                translateMode(value, 2);
            } else {
                excludeTracks(value, key == "X0" ? 1 : key == "X1" ? 2 : 3);
            }
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-')
            help();
        if (inFile.empty())
            inFile = imd::inputName(arg, ".IMD");
        else if (secondFile.empty())
            secondFile = arg;
        else if (thirdFile.empty())
            thirdFile = arg;
        else
            help();
    }
    if (inFile.empty())
        help();

    imd::Image image = load(inFile);
    if (!json)
        printComment(image.comment);

    std::vector<uint8_t> comment = image.comment;
    if (!extractCommentFile.empty())
        imd::writeFile(imd::outputName(extractCommentFile, ".TXT"), comment);
    if (replaceComment)
        comment.clear();
    if (!appendCommentFile.empty()) {
        auto text = imd::readFile(imd::inputName(appendCommentFile, ".TXT"));
        comment.insert(comment.end(), text.begin(), text.end());
    }

    // The second file is the merge image when a third file is given or nothing is being
    // converted; otherwise it is the output.
    std::string mergeFile, outFile = thirdFile;
    if (!secondFile.empty()) {
        if (!thirdFile.empty() || !converting)
            mergeFile = imd::inputName(secondFile, ".IMD");
        else
            outFile = secondFile;
    }

    if (!outFile.empty()) {
        if (binaryOut && !interleave) {
            interleave = 1;
            if (verbose)
                std::fprintf(info, "Assuming 1:1 for Binary output\n");
        }
        outFile = imd::outputName(outFile, binaryOut ? ".BIN" : ".IMD");
        confirmOverwrite(outFile);
    } else if (converting) {
        imd::fatal("No output file.");
    } else if (verbose && !mergeFile.empty()) {
        std::fprintf(info, "No output file - compare only.\n");
    }

    imd::Image merge;
    bool merging = !mergeFile.empty();
    if (merging)
        merge = load(mergeFile);

    std::vector<uint8_t> out;
    if (!binaryOut) {
        out = comment;
        out.push_back(imd::kCommentEnd);
    }

    std::set<Key> inputKeys;
    for (const auto& t : image.tracks)
        inputKeys.insert(key(t));
    std::vector<bool> mergeAdded(merge.tracks.size(), false);
    std::vector<imd::Track> result;

    // Add a track found only in the merge image.
    auto addMergeTrack = [&](size_t m) {
        const imd::Track& t2 = merge.tracks[m];
        mergeAdded[m] = true;
        warning("Adding track %u/%u from mergefile", t2.cyl, t2.head);
        updateStats(t2);
        result.push_back(t2);
        outputTrack(out, t2);
    };

    Display display;
    for (auto& t : image.tracks) {
        curCyl = t.cyl;
        curHead = t.head;
        if (merging) {
            for (size_t m = 0; m < merge.tracks.size(); ++m)
                if (!mergeAdded[m] && !inputKeys.count(key(merge.tracks[m])) && key(merge.tracks[m]) < key(t))
                    addMergeTrack(m);
            auto it = std::find_if(merge.tracks.begin(), merge.tracks.end(),
                                   [&](const imd::Track& m) { return key(m) == key(t); });
            if (it == merge.tracks.end())
                warning("Track not found in merge file");
            else
                mergeTrack(t, *it);
        }
        updateStats(t);
        if (!json)
            displayTrack(display, t);
        result.push_back(t);
        outputTrack(out, t);
    }
    for (size_t m = 0; m < merge.tracks.size(); ++m)
        if (!mergeAdded[m] && !inputKeys.count(key(merge.tracks[m])))
            addMergeTrack(m);

    if (json) {
        printJson(inFile, comment, result);
    } else {
        std::printf("%u tracks(%u/%u), %u sectors", head0Tracks + head1Tracks, head0Tracks, head1Tracks,
                    totalSectors);
        const unsigned counts[] = {compressedCount, deletedCount, badCount, unavailCount};
        const char* const names[] = {"Compressed", "Deleted", "Bad", "Unavail"};
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            if (counts[i]) {
                std::printf("%s%u %s", any ? ", " : " (", counts[i], names[i]);
                any = true;
            }
        }
        std::printf("%s\n", any ? ")" : "");
    }

    if (!outFile.empty())
        imd::writeFile(outFile, out);
    return 0;
}
