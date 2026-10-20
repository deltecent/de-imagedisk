// Reader for Teledisk (.TD0) disk images.
//
// Layout: a 12-byte header ("TD" normal, "td" advanced compression), then - LZHUF
// compressed when advanced - an optional comment block, then track records (4-byte header
// followed by sector records) ending with a track header whose sector count is 255.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace imd {

class Td0Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// CRC-16, polynomial 0xA097, as used throughout TD0 files.
uint16_t td0Crc(const uint8_t* p, size_t n, uint16_t crc = 0);

// Byte stream over the part of a TD0 file after the header, decoding LZHUF when the image
// uses advanced compression.
class Td0Stream {
public:
    Td0Stream(const std::vector<uint8_t>& file, size_t start, bool compressed);
    ~Td0Stream();
    Td0Stream(const Td0Stream&) = delete;
    Td0Stream& operator=(const Td0Stream&) = delete;

    // Next byte, or -1 at the end of the data.
    int get();
    // Bytes delivered so far.
    size_t count() const { return count_; }

private:
    int rawByte();
    unsigned bit();
    unsigned byte8();
    unsigned decodeChar();
    unsigned decodePosition();
    void update(unsigned c);
    void rebuild();

    const std::vector<uint8_t>& file_;
    size_t pos_;
    bool compressed_;
    size_t count_ = 0;
    bool eof_ = false;
    // Bit input
    unsigned bitBuf_ = 0, bitsLeft_ = 0;
    // Huffman tree and LZSS ring
    struct Tree;
    Tree* tree_ = nullptr;
    std::vector<uint8_t> ring_;
    unsigned ringPos_ = 0;
    unsigned copyFrom_ = 0, copyLeft_ = 0;
};

// Teledisk sector flags
constexpr uint8_t kTd0Duplicate = 0x01;
constexpr uint8_t kTd0CrcError = 0x02;
constexpr uint8_t kTd0Deleted = 0x04;
constexpr uint8_t kTd0DosSkipped = 0x10;
constexpr uint8_t kTd0NoData = 0x20;
constexpr uint8_t kTd0NoId = 0x40;

struct Td0Header {
    bool advanced = false;  // "td" signature: LZHUF compressed
    uint8_t sequence = 0, checkSequence = 0, version = 0, dataRate = 0, driveType = 0, stepping = 0,
            dosMode = 0, sides = 0;
    uint16_t crc = 0;
    bool hasComment() const { return (stepping & 0x80) != 0; }
};

struct Td0Comment {
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;  // as stored
    std::vector<uint8_t> text;  // NUL-separated lines
};

struct Td0Sector {
    uint8_t cyl = 0, head = 0, id = 0, sizeCode = 0, flags = 0, crc = 0;
    bool hasData = false;          // a data block followed the header
    uint8_t method = 0;            // data encoding method, when hasData
    std::vector<uint8_t> data;     // decoded data (sector size), empty when no data block
};

struct Td0Track {
    uint8_t sectorCount = 0, cyl = 0, side = 0, crc = 0;
    std::vector<Td0Sector> sectors;
};

class Td0Reader {
public:
    // Parse the header (and comment) of a TD0 file; throws Td0Error.
    explicit Td0Reader(std::vector<uint8_t> file);
    ~Td0Reader();

    const Td0Header& header() const { return header_; }
    const Td0Comment& comment() const { return comment_; }

    // Read the next track. Returns false at the end of the image.
    bool nextTrack(Td0Track& track);

private:
    int get();
    uint8_t need(const char* what);
    void readSector(Td0Sector& s);

    std::vector<uint8_t> file_;
    Td0Header header_;
    Td0Comment comment_;
    Td0Stream* stream_ = nullptr;
};

}  // namespace imd
