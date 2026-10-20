// Unit tests for the imdcore library.
#include "cli.hpp"

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

int main()
{
    testSwitches();
    testKeyValue();
    testNumbers();
    testFilenames();
    if (failures)
        std::fprintf(stderr, "%d check(s) failed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
