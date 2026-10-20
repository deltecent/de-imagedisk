// Command-line helpers shared by the ImageDisk tools.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace imd {

// Print a printf-style message to stderr (a newline is added) and exit with status 1.
[[noreturn]] void fatal(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Print a printf-style message to stdout followed by a newline.
void warn(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

std::string toUpper(std::string s);
bool iequals(const std::string& a, const std::string& b);

// Recognize a switch argument. "-X..." is always a switch. "/X..." is a switch only when
// `isKnown` accepts the upper-cased text after the '/', so absolute paths are not mistaken
// for options. On success `body` receives the text after the prefix (case preserved).
bool switchBody(const std::string& arg, const std::function<bool(const std::string&)>& isKnown,
                std::string& body);

// Recognize a KEY=value argument whose key (case-insensitive) is one of `keys`.
// `key` receives the upper-cased key and `value` the text after '='.
bool keyValue(const std::string& arg, const std::vector<std::string>& keys, std::string& key,
              std::string& value);

// Value of a hexadecimal digit, or -1.
int digitValue(char c);

// Read digits in `base` starting at `p`, advancing past them. Returns false when no digit is
// present or a digit is not valid in `base`; `p` is then left at the offending character.
bool scanNumber(const char*& p, unsigned base, unsigned long& value);

// Append `ext` (e.g. ".IMD") when the last path component has no '.'.
std::string defaultExt(const std::string& name, const std::string& ext);

// Name of an input file: adds `ext` if needed; when that file does not exist, the same name
// with a lowercase extension is tried.
std::string inputName(const std::string& name, const std::string& ext);

// Name of an output file: adds the lowercase form of `ext` if needed.
std::string outputName(const std::string& name, const std::string& ext);

bool fileExists(const std::string& path);

// Whole-file I/O; failures are fatal.
std::vector<uint8_t> readFile(const std::string& path);
void writeFile(const std::string& path, const std::vector<uint8_t>& data);

bool stdinIsTerminal();

// Read a single key from the terminal without waiting for Enter. Returns -1 on EOF.
int readKey();

}  // namespace imd
