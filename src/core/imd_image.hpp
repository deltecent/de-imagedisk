// In-memory model of an ImageDisk (.IMD) image, with reader and writer.
//
// File layout: ASCII comment terminated by 0x1A, then for each track:
//   mode, cylinder, head (+0x80 cylinder map, +0x40 head map), sector count, size code,
//   sector numbering map, optional cylinder map, optional head map, then one record per
//   sector: a type byte (0 = unavailable, else 1 + bit0 compressed, bit1 deleted, bit2 error)
//   followed by the sector data, or by a single fill byte when compressed.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace imd {

constexpr unsigned kMaxMode = 5;
constexpr unsigned kMaxSizeCode = 6;
constexpr uint8_t kCommentEnd = 0x1A;
constexpr uint8_t kCylMapFlag = 0x80;
constexpr uint8_t kHeadMapFlag = 0x40;

inline unsigned sectorBytes(unsigned sizeCode) { return 128u << sizeCode; }

// Data rate in kbps for an IMD mode (0-2 FM, 3-5 MFM; 500/300/250).
inline unsigned modeRate(unsigned mode) { return mode % 3 == 0 ? 500 : mode % 3 == 1 ? 300 : 250; }
inline bool modeIsMfm(unsigned mode) { return mode > 2; }

struct Sector {
    uint8_t id = 0;
    uint8_t cyl = 0;   // cylinder in the sector ID field
    uint8_t head = 0;  // head in the sector ID field
    bool available = false;
    bool compressed = false;  // stored compressed in the file it came from
    bool deleted = false;     // deleted-data address mark
    bool bad = false;         // read with a data error
    std::vector<uint8_t> data;  // always the full sector size, even when unavailable

    // True when every byte of `data` is the same value.
    bool allSame() const;
};

struct Track {
    uint8_t mode = 0;
    uint8_t cyl = 0;
    uint8_t head = 0;
    bool cylMap = false;   // per-sector cylinder map present
    bool headMap = false;  // per-sector head map present
    uint8_t sizeCode = 0;
    std::vector<Sector> sectors;

    unsigned sectorSize() const { return sectorBytes(sizeCode); }
};

struct Image {
    std::vector<uint8_t> comment;  // text before the 0x1A terminator
    std::vector<Track> tracks;
};

// Malformed image. `cyl`/`head` are the physical track being read, or -1 if none yet.
class FormatError : public std::runtime_error {
public:
    FormatError(const std::string& msg, int c = -1, int h = -1)
        : std::runtime_error(msg), cyl(c), head(h) {}
    int cyl, head;
};

enum class Compression {
    Keep,      // compress sectors that were compressed in the input
    Compress,  // compress every all-same sector
    Expand,    // never compress
};

// Parse an image; throws FormatError.
Image parseImage(const std::vector<uint8_t>& bytes);

// Load an image file; I/O failures are fatal, format errors throw FormatError.
Image readImage(const std::string& path);

// Serialize one track record. Unavailable sectors are written as type 0 with no data.
void appendTrack(std::vector<uint8_t>& out, const Track& track, Compression compression,
                 uint8_t mode);

// Serialize a whole image.
std::vector<uint8_t> serializeImage(const Image& image, Compression compression = Compression::Keep);

}  // namespace imd
