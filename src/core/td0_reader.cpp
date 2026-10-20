#include "td0_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace imd {

uint16_t td0Crc(const uint8_t* p, size_t n, uint16_t crc)
{
    while (n--) {
        crc = static_cast<uint16_t>(crc ^ (*p++ << 8));
        for (int i = 0; i < 8; ++i)
            crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0xA097 : crc << 1);
    }
    return crc;
}

// ---------------------------------------------------------------- LZHUF

namespace {

constexpr unsigned kRingSize = 4096;
constexpr unsigned kLookahead = 60;
constexpr unsigned kThreshold = 2;
constexpr unsigned kChars = 256 - kThreshold + kLookahead;  // literal bytes + match lengths
constexpr unsigned kTable = kChars * 2 - 1;                 // tree nodes
constexpr unsigned kRoot = kTable - 1;
constexpr unsigned kMaxFreq = 0x8000;

// Upper six bits of a match position are Huffman coded with a fixed code. Indexed by the
// next 8 input bits, these give the decoded value and the code length.
struct PositionTables {
    uint8_t code[256];
    uint8_t bits[256];
    PositionTables()
    {
        // (number of position values, code length): shorter codes for nearer positions.
        static const unsigned groups[][2] = {{1, 3}, {3, 4}, {8, 5}, {12, 6}, {24, 7}, {16, 8}};
        unsigned index = 0, value = 0;
        for (const auto& g : groups) {
            for (unsigned n = 0; n < g[0]; ++n, ++value) {
                for (unsigned k = 0; k < (256u >> g[1]); ++k, ++index) {
                    code[index] = static_cast<uint8_t>(value);
                    bits[index] = static_cast<uint8_t>(g[1]);
                }
            }
        }
    }
};

const PositionTables& positionTables()
{
    static const PositionTables tables;
    return tables;
}

}  // namespace

// Adaptive Huffman tree (Okumura's LZHUF). Leaves are stored as son[] values >= kTable.
struct Td0Stream::Tree {
    unsigned freq[kTable + 1];
    unsigned parent[kTable + kChars];
    unsigned son[kTable];

    Tree()
    {
        for (unsigned i = 0; i < kChars; ++i) {
            freq[i] = 1;
            son[i] = i + kTable;
            parent[i + kTable] = i;
        }
        for (unsigned i = 0, j = kChars; j <= kRoot; i += 2, ++j) {
            freq[j] = freq[i] + freq[i + 1];
            son[j] = i;
            parent[i] = parent[i + 1] = j;
        }
        freq[kTable] = 0xFFFF;  // sentinel
        parent[kRoot] = 0;
    }
};

Td0Stream::Td0Stream(const std::vector<uint8_t>& file, size_t start, bool compressed)
    : file_(file), pos_(start), compressed_(compressed)
{
    if (compressed_) {
        tree_ = new Tree;
        ring_.assign(kRingSize, ' ');
        ringPos_ = kRingSize - kLookahead;
    }
}

Td0Stream::~Td0Stream()
{
    delete tree_;
}

int Td0Stream::rawByte()
{
    if (pos_ >= file_.size()) {
        eof_ = true;
        return -1;
    }
    return file_[pos_++];
}

unsigned Td0Stream::bit()
{
    if (bitsLeft_ == 0) {
        int c = rawByte();
        bitBuf_ = c < 0 ? 0 : static_cast<unsigned>(c);
        bitsLeft_ = 8;
    }
    --bitsLeft_;
    return (bitBuf_ >> bitsLeft_) & 1;
}

unsigned Td0Stream::byte8()
{
    unsigned v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 1) | bit();
    return v;
}

// Halve the leaf frequencies and rebuild the tree.
void Td0Stream::rebuild()
{
    Tree& t = *tree_;
    unsigned j = 0;
    for (unsigned i = 0; i < kTable; ++i) {
        if (t.son[i] >= kTable) {
            t.freq[j] = (t.freq[i] + 1) / 2;
            t.son[j] = t.son[i];
            ++j;
        }
    }
    for (unsigned i = 0, n = kChars; n < kTable; i += 2, ++n) {
        unsigned f = t.freq[i] + t.freq[i + 1];
        unsigned k = n;
        while (f < t.freq[k - 1])
            --k;
        std::memmove(&t.freq[k + 1], &t.freq[k], (n - k) * sizeof t.freq[0]);
        std::memmove(&t.son[k + 1], &t.son[k], (n - k) * sizeof t.son[0]);
        t.freq[k] = f;
        t.son[k] = i;
    }
    for (unsigned i = 0; i < kTable; ++i) {
        unsigned k = t.son[i];
        if (k >= kTable)
            t.parent[k] = i;
        else
            t.parent[k] = t.parent[k + 1] = i;
    }
}

// Count one more occurrence of symbol `c`, keeping nodes ordered by frequency.
void Td0Stream::update(unsigned c)
{
    Tree& t = *tree_;
    if (t.freq[kRoot] == kMaxFreq)
        rebuild();
    c = t.parent[c + kTable];
    do {
        unsigned k = ++t.freq[c];
        unsigned l = c + 1;
        if (k > t.freq[l]) {
            while (k > t.freq[l + 1])
                ++l;
            t.freq[c] = t.freq[l];
            t.freq[l] = k;

            unsigned i = t.son[c];
            t.parent[i] = l;
            if (i < kTable)
                t.parent[i + 1] = l;
            unsigned j = t.son[l];
            t.son[l] = i;
            t.parent[j] = c;
            if (j < kTable)
                t.parent[j + 1] = c;
            t.son[c] = j;
            c = l;
        }
        c = t.parent[c];
    } while (c != 0);
}

unsigned Td0Stream::decodeChar()
{
    unsigned c = tree_->son[kRoot];
    while (c < kTable)
        c = tree_->son[c + bit()];
    c -= kTable;
    update(c);
    return c;
}

unsigned Td0Stream::decodePosition()
{
    const PositionTables& pt = positionTables();
    unsigned i = byte8();
    unsigned upper = static_cast<unsigned>(pt.code[i]) << 6;
    for (unsigned n = pt.bits[i] - 2u; n; --n)
        i = (i << 1) | bit();
    return upper | (i & 0x3F);
}

int Td0Stream::get()
{
    if (!compressed_) {
        int c = rawByte();
        if (c >= 0)
            ++count_;
        return c;
    }
    if (eof_)
        return -1;
    if (!copyLeft_) {
        unsigned c = decodeChar();
        if (c < 256) {
            ring_[ringPos_] = static_cast<uint8_t>(c);
            ringPos_ = (ringPos_ + 1) & (kRingSize - 1);
            ++count_;
            return static_cast<int>(c);
        }
        copyFrom_ = (ringPos_ - decodePosition() - 1) & (kRingSize - 1);
        copyLeft_ = c - 255 + kThreshold;
        if (eof_)
            return -1;
    }
    uint8_t b = ring_[copyFrom_];
    copyFrom_ = (copyFrom_ + 1) & (kRingSize - 1);
    ring_[ringPos_] = b;
    ringPos_ = (ringPos_ + 1) & (kRingSize - 1);
    --copyLeft_;
    ++count_;
    return b;
}

// ---------------------------------------------------------------- reader

namespace {

std::string fmt(const char* f, unsigned a)
{
    char buf[96];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}

}  // namespace

Td0Reader::Td0Reader(std::vector<uint8_t> file) : file_(std::move(file))
{
    if (file_.size() < 12)
        throw Td0Error("File too short for a TD0 header");
    const uint8_t* h = file_.data();
    bool badCrc = td0Crc(h, 10) != static_cast<uint16_t>(h[10] | (h[11] << 8));
    bool advanced = h[0] == 't' && h[1] == 'd';
    bool normal = h[0] == 'T' && h[1] == 'D';
    if (badCrc && !advanced && !normal)
        throw Td0Error("Header CRC failure; No TD signature");
    if (badCrc)
        throw Td0Error("Header CRC failure");
    if (!advanced && !normal)
        throw Td0Error("No TD signature");

    header_.advanced = advanced;
    header_.sequence = h[2];
    header_.checkSequence = h[3];
    header_.version = h[4];
    header_.dataRate = h[5];
    header_.driveType = h[6];
    header_.stepping = h[7];
    header_.dosMode = h[8];
    header_.sides = h[9];
    header_.crc = static_cast<uint16_t>(h[10] | (h[11] << 8));
    stream_ = new Td0Stream(file_, 12, advanced);

    if (header_.hasComment()) {
        uint8_t block[10];
        for (auto& b : block)
            b = need("comment header");
        uint16_t crc = static_cast<uint16_t>(block[0] | (block[1] << 8));
        unsigned len = block[2] | (static_cast<unsigned>(block[3]) << 8);
        comment_.year = block[4];
        comment_.month = block[5];
        comment_.day = block[6];
        comment_.hour = block[7];
        comment_.minute = block[8];
        comment_.second = block[9];
        for (unsigned i = 0; i < len; ++i)
            comment_.text.push_back(need("comment"));
        uint16_t c = td0Crc(block + 2, 8);
        c = td0Crc(comment_.text.data(), comment_.text.size(), c);
        if (c != crc)
            throw Td0Error("Comment CRC error!");
    }
}

Td0Reader::~Td0Reader()
{
    delete stream_;
}

int Td0Reader::get()
{
    return stream_->get();
}

uint8_t Td0Reader::need(const char* what)
{
    int c = get();
    if (c < 0)
        throw Td0Error(std::string("EOF reading ") + what);
    return static_cast<uint8_t>(c);
}

bool Td0Reader::nextTrack(Td0Track& t)
{
    uint8_t h[4];
    for (int i = 0; i < 4; ++i) {
        int c = get();
        if (c < 0)
            return false;  // running out here is a normal end of image
        h[i] = static_cast<uint8_t>(c);
        if (i == 0 && c == 0xFF)
            return false;
    }
    t = Td0Track();
    t.sectorCount = h[0];
    t.cyl = h[1];
    t.side = h[2];
    t.crc = h[3];
    if ((td0Crc(h, 3) & 0xFF) != t.crc)
        throw Td0Error("Track header CRC error");
    t.sectors.resize(t.sectorCount);
    for (auto& s : t.sectors)
        readSector(s);
    return true;
}

void Td0Reader::readSector(Td0Sector& s)
{
    s.cyl = need("sector header");
    s.head = need("sector header");
    s.id = need("sector header");
    s.sizeCode = need("sector header");
    s.flags = need("sector header");
    s.crc = need("sector header");
    if (s.sizeCode > 6)
        throw Td0Error("Unknown sector size");
    if (s.flags & (kTd0DosSkipped | kTd0NoData))
        return;  // no data block follows

    unsigned length = need("data header");
    length |= static_cast<unsigned>(need("data header")) << 8;
    s.dataLength = static_cast<uint16_t>(length);
    s.method = need("data header");
    s.hasData = true;
    size_t start = stream_->count();

    // Data beyond the sector size is discarded.
    const size_t size = 128u << s.sizeCode;
    auto put = [&](uint8_t b) {
        if (s.data.size() < size)
            s.data.push_back(b);
    };
    while (s.data.size() < size) {
        switch (s.method) {
        case 0:  // raw
            while (s.data.size() < size)
                put(need("sector data"));
            break;
        case 1: {  // count, then a two-byte pattern repeated count times
            unsigned count = need("sector data");
            count |= static_cast<unsigned>(need("sector data")) << 8;
            uint8_t a = need("sector data"), b = need("sector data");
            while (count--) {
                put(a);
                put(b);
            }
            break;
        }
        case 2: {  // run-length blocks
            unsigned n = need("sector data");
            if (n == 0) {  // literal block
                unsigned count = need("sector data");
                while (count--)
                    put(need("sector data"));
            } else {  // pattern of 1<<n bytes, repeated count times
                if (n > 13)
                    throw Td0Error(fmt("Bad RLE pattern length %u", n));
                unsigned count = need("sector data");
                std::vector<uint8_t> pattern(1u << n);
                for (auto& b : pattern)
                    b = need("sector data");
                while (count--)
                    for (uint8_t b : pattern)
                        put(b);
            }
            break;
        }
        default:
            throw Td0Error(fmt("Unknown data mode: %02x", s.method));
        }
    }
    if (stream_->count() - start != length - 1u)
        throw Td0Error("Sector data size/offset mismatch");
}

}  // namespace imd
