// Decode the body of an advanced-compression TD0 file and compare it with the expected
// uncompressed bytes. Usage: td0_stream_check file.td0 expected.raw
#include "cli.hpp"
#include "td0_reader.hpp"

#include <cstdio>

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: td0_stream_check file.td0 expected.raw\n");
        return 2;
    }
    auto td0 = imd::readFile(argv[1]);
    auto expected = imd::readFile(argv[2]);
    imd::Td0Stream stream(td0, 12, true);
    for (size_t i = 0; i < expected.size(); ++i) {
        int c = stream.get();
        if (c != expected[i]) {
            std::fprintf(stderr, "mismatch at byte %zu: got %d, expected %d\n", i, c, expected[i]);
            return 1;
        }
    }
    std::printf("%zu bytes decoded correctly\n", expected.size());
    return 0;
}
