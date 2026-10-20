// Unit tests for the imdcore library.
#include "cli.hpp"
#include "imd_image.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void testSwitches()
{
    auto known = [](const std::string& b) { return b == "B" || b == "ND"; };
    std::string body;
    CHECK(imd::switchBody("-b", known, body) && body == "b");
    CHECK(imd::switchBody("/b", known, body) && body == "b");
    CHECK(imd::switchBody("/nd", known, body) && body == "nd");
    CHECK(!imd::switchBody("/Users/me/disk.imd", known, body));
    CHECK(!imd::switchBody("disk.imd", known, body));
    CHECK(!imd::switchBody("-", known, body));
    CHECK(imd::switchBody("-zz", known, body) && body == "zz");
}

static void testKeyValue()
{
    std::string k, v;
    CHECK(imd::keyValue("il=3", {"IL", "F"}, k, v) && k == "IL" && v == "3");
    CHECK(imd::keyValue("IL=", {"IL"}, k, v) && k == "IL" && v.empty());
    CHECK(!imd::keyValue("XX=3", {"IL"}, k, v));
    CHECK(!imd::keyValue("disk.imd", {"IL"}, k, v));
    CHECK(!imd::keyValue("=3", {"IL"}, k, v));
}

static void testNumbers()
{
    unsigned long v;
    const char* p = "123,";
    CHECK(imd::scanNumber(p, 10, v) && v == 123 && *p == ',');
    p = "fF";
    CHECK(imd::scanNumber(p, 16, v) && v == 255 && *p == 0);
    p = "12a";
    CHECK(!imd::scanNumber(p, 10, v) && *p == 'a');
    p = "";
    CHECK(!imd::scanNumber(p, 10, v));
    p = "101";
    CHECK(imd::scanNumber(p, 2, v) && v == 5);
    p = "18";
    CHECK(!imd::scanNumber(p, 8, v));
    CHECK(imd::digitValue('g') == -1);
}

static void testFilenames()
{
    CHECK(imd::defaultExt("disk", ".IMD") == "disk.IMD");
    CHECK(imd::defaultExt("disk.img", ".IMD") == "disk.img");
    CHECK(imd::defaultExt("dir.d/disk", ".IMD") == "dir.d/disk.IMD");
    CHECK(imd::outputName("out", ".BIN") == "out.bin");
    CHECK(imd::outputName("out.X", ".BIN") == "out.X");
    CHECK(imd::inputName("/nonexistent/zz", ".IMD") == "/nonexistent/zz.IMD");
    CHECK(imd::toUpper("aBc1") == "ABC1");
    CHECK(imd::iequals("t2", "T2"));
}

static imd::Image sampleImage()
{
    imd::Image img;
    std::string c = "IMD 1.20: test\r\nhello\r\n";
    img.comment.assign(c.begin(), c.end());
    for (unsigned cyl = 0; cyl < 2; ++cyl) {
        for (unsigned head = 0; head < 2; ++head) {
            imd::Track t;
            t.mode = 5;
            t.cyl = static_cast<uint8_t>(cyl);
            t.head = static_cast<uint8_t>(head);
            t.sizeCode = 1;
            t.cylMap = cyl == 1;
            t.headMap = head == 1;
            for (unsigned i = 0; i < 4; ++i) {
                imd::Sector s;
                s.id = static_cast<uint8_t>(i + 1);
                s.cyl = static_cast<uint8_t>(cyl + 10);
                s.head = static_cast<uint8_t>(head);
                s.available = i != 3;
                s.deleted = i == 1;
                s.bad = i == 2;
                s.data.assign(t.sectorSize(), static_cast<uint8_t>(i));
                if (i == 0)
                    for (unsigned b = 0; b < s.data.size(); ++b)
                        s.data[b] = static_cast<uint8_t>(b * 7 + cyl);
                else if (i != 3)
                    s.compressed = true;
                t.sectors.push_back(s);
            }
            img.tracks.push_back(t);
        }
    }
    return img;
}

static void testImageRoundTrip()
{
    imd::Image img = sampleImage();
    auto bytes = imd::serializeImage(img);
    imd::Image back = imd::parseImage(bytes);
    CHECK(back.comment == img.comment);
    CHECK(back.tracks.size() == 4);
    CHECK(imd::serializeImage(back) == bytes);
    const imd::Track& t = back.tracks[3];
    CHECK(t.cylMap && t.headMap && t.cyl == 1 && t.head == 1);
    CHECK(t.sectors[0].cyl == 11 && t.sectors[1].deleted && t.sectors[2].bad);
    CHECK(t.sectors[1].compressed && !t.sectors[0].compressed);
    CHECK(!t.sectors[3].available && t.sectors[3].data.size() == 256);
    // Track 0 has no maps, so sector cylinders come from the track.
    CHECK(back.tracks[0].sectors[0].cyl == 0);

    auto expanded = imd::serializeImage(back, imd::Compression::Expand);
    imd::Image e = imd::parseImage(expanded);
    CHECK(!e.tracks[0].sectors[1].compressed);
    CHECK(expanded.size() > bytes.size());
    auto recompressed = imd::serializeImage(e, imd::Compression::Compress);
    CHECK(recompressed == bytes);
}

static void testImageErrors()
{
    auto bytes = imd::serializeImage(sampleImage());
    auto expectError = [&](std::vector<uint8_t> b, const std::string& msg) {
        try {
            imd::parseImage(b);
            CHECK(!"no error");
        } catch (const imd::FormatError& e) {
            if (std::string(e.what()).find(msg) == std::string::npos) {
                std::fprintf(stderr, "unexpected error: %s\n", e.what());
                CHECK(!"wrong message");
            }
        }
    };
    expectError({'a', 'b'}, "EOF in comment");
    auto truncated = bytes;
    truncated.resize(truncated.size() - 1);
    expectError(truncated, "EOF at sector 3/4 flag");
    auto truncData = bytes;
    truncData.resize(truncData.size() - 6);
    expectError(truncData, "EOF in sector 0/1 data");
    auto badMode = bytes;
    size_t trackStart = sampleImage().comment.size() + 1;
    badMode[trackStart] = 6;
    expectError(badMode, "Mode value 6 out of range 0-5");
    auto badSize = bytes;
    badSize[trackStart + 4] = 7;
    expectError(badSize, "Size value 7 out of range 0-6");
}

int main()
{
    testImageRoundTrip();
    testImageErrors();
    testSwitches();
    testKeyValue();
    testNumbers();
    testFilenames();
    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
