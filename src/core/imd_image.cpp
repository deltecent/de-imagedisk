#include "imd_image.hpp"

#include "cli.hpp"

#include <algorithm>
#include <cstdio>

namespace imd {

bool Sector::allSame() const
{
    return std::all_of(data.begin(), data.end(), [&](uint8_t b) { return b == data.front(); });
}

namespace {

std::string format(const char* fmt, unsigned a = 0, unsigned b = 0)
{
    char buf[128];
    std::snprintf(buf, sizeof buf, fmt, a, b);
    return buf;
}

class Cursor {
public:
    explicit Cursor(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    bool atEnd() const { return pos_ >= bytes_.size(); }
    size_t remaining() const { return bytes_.size() - pos_; }
    bool get(uint8_t& b)
    {
        if (atEnd())
            return false;
        b = bytes_[pos_++];
        return true;
    }
    bool get(std::vector<uint8_t>& out, size_t n)
    {
        if (remaining() < n)
            return false;
        out.assign(bytes_.begin() + static_cast<long>(pos_), bytes_.begin() + static_cast<long>(pos_ + n));
        pos_ += n;
        return true;
    }

private:
    const std::vector<uint8_t>& bytes_;
    size_t pos_ = 0;
};

}  // namespace

Image parseImage(const std::vector<uint8_t>& bytes)
{
    Image image;
    auto end = std::find(bytes.begin(), bytes.end(), kCommentEnd);
    if (end == bytes.end())
        throw FormatError("EOF in comment");
    image.comment.assign(bytes.begin(), end);

    std::vector<uint8_t> rest(end + 1, bytes.end());
    Cursor in(rest);
    while (!in.atEnd()) {
        Track t;
        uint8_t mode, cyl, head, nsec, size;
        in.get(mode);
        if (!in.get(cyl))
            throw FormatError("EOF at Cylinder");
        int c = cyl;
        if (!in.get(head))
            throw FormatError("EOF at Head", c);
        int h = head & 0x0F;
        if (!in.get(nsec))
            throw FormatError("EOF at Nsec", c, h);
        if (!in.get(size))
            throw FormatError("EOF at Size", c, h);
        if (mode > kMaxMode)
            throw FormatError(format("Mode value %u out of range 0-5", mode), c, h);
        if ((head & 0x0F) > 1)
            throw FormatError(format("Head value %u out of range 0-1", head), c, h);
        if (size > kMaxSizeCode)
            throw FormatError(format("Size value %u out of range 0-6", size), c, h);

        t.mode = mode;
        t.cyl = cyl;
        t.head = head & 0x0F;
        t.cylMap = (head & kCylMapFlag) != 0;
        t.headMap = (head & kHeadMapFlag) != 0;
        t.sizeCode = size;
        t.sectors.resize(nsec);

        std::vector<uint8_t> map;
        if (!in.get(map, nsec))
            throw FormatError("EOF in Sector Map", c, h);
        for (unsigned i = 0; i < nsec; ++i) {
            t.sectors[i].id = map[i];
            t.sectors[i].cyl = t.cyl;
            t.sectors[i].head = t.head;
        }
        if (t.cylMap) {
            if (!in.get(map, nsec))
                throw FormatError("EOF in Cylinder Map", c, h);
            for (unsigned i = 0; i < nsec; ++i)
                t.sectors[i].cyl = map[i];
        }
        if (t.headMap) {
            if (!in.get(map, nsec))
                throw FormatError("EOF in Head Map", c, h);
            for (unsigned i = 0; i < nsec; ++i)
                t.sectors[i].head = map[i];
        }

        unsigned bytesPerSector = t.sectorSize();
        for (unsigned i = 0; i < nsec; ++i) {
            Sector& s = t.sectors[i];
            uint8_t type;
            if (!in.get(type))
                throw FormatError(format("EOF at sector %u/%u flag", i, s.id), c, h);
            if (type > 8)
                throw FormatError(format("Sector %u/%u type out of range 0-8", i, s.id), c, h);
            if (type == 0) {
                s.data.assign(bytesPerSector, 0);
                continue;
            }
            unsigned bits = type - 1u;
            s.available = true;
            s.compressed = (bits & 1) != 0;
            s.deleted = (bits & 2) != 0;
            s.bad = (bits & 4) != 0;
            if (s.compressed) {
                uint8_t fill;
                if (!in.get(fill))
                    throw FormatError(format("EOF in sector %u/%u compress data", i, s.id), c, h);
                s.data.assign(bytesPerSector, fill);
            } else if (!in.get(s.data, bytesPerSector)) {
                throw FormatError(format("EOF in sector %u/%u data", i, s.id), c, h);
            }
        }
        image.tracks.push_back(std::move(t));
    }
    return image;
}

Image readImage(const std::string& path)
{
    return parseImage(readFile(path));
}

void appendTrack(std::vector<uint8_t>& out, const Track& t, Compression compression, uint8_t mode)
{
    out.push_back(mode);
    out.push_back(t.cyl);
    out.push_back(static_cast<uint8_t>(t.head | (t.cylMap ? kCylMapFlag : 0) | (t.headMap ? kHeadMapFlag : 0)));
    out.push_back(static_cast<uint8_t>(t.sectors.size()));
    out.push_back(t.sizeCode);
    for (const auto& s : t.sectors)
        out.push_back(s.id);
    if (t.cylMap)
        for (const auto& s : t.sectors)
            out.push_back(s.cyl);
    if (t.headMap)
        for (const auto& s : t.sectors)
            out.push_back(s.head);

    for (const auto& s : t.sectors) {
        if (!s.available) {
            out.push_back(0);
            continue;
        }
        bool compress = false;
        if (compression == Compression::Compress)
            compress = s.allSame();
        else if (compression == Compression::Keep)
            compress = s.compressed && s.allSame();
        unsigned bits = (compress ? 1u : 0u) | (s.deleted ? 2u : 0u) | (s.bad ? 4u : 0u);
        out.push_back(static_cast<uint8_t>(bits + 1));
        if (compress)
            out.push_back(s.data.empty() ? 0 : s.data.front());
        else
            out.insert(out.end(), s.data.begin(), s.data.end());
    }
}

std::vector<uint8_t> serializeImage(const Image& image, Compression compression)
{
    std::vector<uint8_t> out(image.comment);
    out.push_back(kCommentEnd);
    for (const auto& t : image.tracks)
        appendTrack(out, t, compression, t.mode);
    return out;
}

}  // namespace imd
