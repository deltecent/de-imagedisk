// imdu - ImageDisk utility: display, convert and merge .IMD images.
#include "cli.hpp"
#include "imd_image.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* const kHelp =
    "\nUse: imdu image [[merge-image] [output-image]] [options]\n\n"
    "opts:  /B                  - output Binary image\n"
    "       /C                  - Compress \"all-same\" sectors\n"
    "       /D                  - display track/sector Detail\n"
    "       /E                  - Expand compressed sectors to full data\n"
    "       /Q                  - Quiet: suppress warnings\n"
    "       /Y                  - auto-Yes (no overwrite prompt)\n"
    "       AC=file[.TXT]       - Append Comment from file        [none]\n"
    "       EC=file[.TXT]       - Extract Comment to file         [none]\n"
    "       RC=file[.TXT]       - Replace Comment from file       [none]\n";

// Options
bool binaryOut = false;
bool detail = false;
bool verbose = true;
bool autoYes = false;
imd::Compression compression = imd::Compression::Keep;
std::string appendCommentFile, extractCommentFile;
bool replaceComment = false;

// Statistics
unsigned head0Tracks = 0, head1Tracks = 0;
unsigned totalSectors = 0, compressedCount = 0, deletedCount = 0, badCount = 0, unavailCount = 0;

// Physical track currently being processed, used to prefix messages.
int curCyl = -1, curHead = -1;

// Print "cc/h " when the current track differs from the last one shown, else blanks.
void trackPrefix()
{
    static int shownCyl = -1, shownHead = -1;
    if (curCyl != shownCyl || curHead != shownHead) {
        shownCyl = curCyl;
        shownHead = curHead;
        std::printf("%2d/%d ", curCyl, curHead);
    } else {
        std::printf("     ");
    }
}

[[maybe_unused]] void warning(const char* fmt, ...)
{
    if (!verbose)
        return;
    trackPrefix();
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::putchar('\n');
}

[[noreturn]] void help()
{
    std::fputs(kHelp, stdout);
    std::exit(1);
}

bool isKnownSwitch(const std::string& s)
{
    return s == "B" || s == "C" || s == "D" || s == "E" || s == "Q" || s == "Y";
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
        values.clear();
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
            std::vector<unsigned> values(ids.begin(), ids.end());
            printMapLine(values);
        }
    }
    if (detail)
        showDetail(t);
}

// Write one track to the output buffer.
void outputTrack(std::vector<uint8_t>& out, const imd::Track& t)
{
    if (binaryOut) {
        for (const auto& s : t.sectors)
            out.insert(out.end(), s.data.begin(), s.data.end());
        return;
    }
    imd::appendTrack(out, t, compression, t.mode);
}

void printComment(const std::vector<uint8_t>& comment)
{
    for (size_t i = 0; i < comment.size(); ++i) {
        if (comment[i] == '\r' && i + 1 < comment.size() && comment[i + 1] == '\n')
            continue;
        std::putchar(comment[i]);
    }
}

// Ask before overwriting `path`. Never blocks when stdin is not a terminal.
void confirmOverwrite(const std::string& path)
{
    if (autoYes || !imd::fileExists(path))
        return;
    if (!imd::stdinIsTerminal())
        imd::fatal("%s already exists - use /Y to overwrite", path.c_str());
    std::printf("%s already exists - proceed(Y/N)?", path.c_str());
    for (;;) {
        int k = imd::readKey();
        if (k == 'y' || k == 'Y') {
            std::printf("Y\n");
            return;
        }
        if (k == 'n' || k == 'N' || k == 0x1B || k == 0x03 || k < 0) {
            std::printf("N\n");
            std::exit(1);
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string inFile, secondFile, thirdFile;
    bool converting = false;  // an option was given that changes the image

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string body, key, value;
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
            } else if (s == "D") {
                detail = true;
            } else if (s == "Q") {
                verbose = false;
            } else if (s == "Y") {
                autoYes = true;
            } else {
                help();
            }
            continue;
        }
        if (imd::keyValue(arg, {"AC", "EC", "RC"}, key, value)) {
            if (key == "EC") {
                extractCommentFile = value;
            } else {
                appendCommentFile = value;
                replaceComment = key == "RC";
            }
            continue;
        }
        if (inFile.empty())
            inFile = imd::inputName(arg, ".IMD");
        else if (secondFile.empty())
            secondFile = arg;
        else if (thirdFile.empty())
            thirdFile = arg;
        else
            help();
    }

    std::fputs("IMageDisk Utility " IMD_VERSION " / " __DATE__ "\n", stdout);
    if (inFile.empty())
        help();

    imd::Image image;
    try {
        image = imd::readImage(inFile);
    } catch (const imd::FormatError& e) {
        if (e.cyl >= 0)
            imd::fatal("%2d/%d %s", e.cyl, e.head < 0 ? 0 : e.head, e.what());
        imd::fatal("%s", e.what());
    }

    printComment(image.comment);
    std::vector<uint8_t> comment = image.comment;
    if (!extractCommentFile.empty())
        imd::writeFile(imd::outputName(extractCommentFile, ".TXT"), comment);
    if (replaceComment)
        comment.clear();
    if (!appendCommentFile.empty()) {
        converting = true;
        auto text = imd::readFile(imd::inputName(appendCommentFile, ".TXT"));
        comment.insert(comment.end(), text.begin(), text.end());
    }

    // The second file is the merge image when a third file is given or nothing is being
    // converted; otherwise it is the output.
    std::string mergeFile, outFile = thirdFile;
    if (!secondFile.empty()) {
        if (!thirdFile.empty() || !converting)
            mergeFile = secondFile;
        else
            outFile = secondFile;
    }
    if (!mergeFile.empty())
        imd::fatal("Merging is not supported yet");

    if (!outFile.empty()) {
        outFile = imd::outputName(outFile, binaryOut ? ".BIN" : ".IMD");
        confirmOverwrite(outFile);
    } else if (converting) {
        imd::fatal("No output file.");
    }

    std::vector<uint8_t> out;
    if (!binaryOut) {
        out = comment;
        out.push_back(imd::kCommentEnd);
    }

    Display display;
    for (const auto& t : image.tracks) {
        curCyl = t.cyl;
        curHead = t.head;
        updateStats(t);
        displayTrack(display, t);
        outputTrack(out, t);
    }

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

    if (!outFile.empty())
        imd::writeFile(outFile, out);
    return 0;
}
