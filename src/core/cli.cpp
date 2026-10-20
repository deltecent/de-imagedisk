#include "cli.hpp"

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace imd {

void fatal(const char* fmt, ...)
{
    std::fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::exit(1);
}

void warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::putchar('\n');
}

std::string toUpper(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool iequals(const std::string& a, const std::string& b)
{
    return toUpper(a) == toUpper(b);
}

bool switchBody(const std::string& arg, const std::function<bool(const std::string&)>& isKnown,
                std::string& body)
{
    if (arg.size() < 2)
        return false;
    if (arg[0] == '-') {
        body = arg.substr(1);
        return true;
    }
    if (arg[0] == '/' && isKnown(toUpper(arg.substr(1)))) {
        body = arg.substr(1);
        return true;
    }
    return false;
}

bool keyValue(const std::string& arg, const std::vector<std::string>& keys, std::string& key,
              std::string& value)
{
    auto eq = arg.find('=');
    if (eq == std::string::npos || eq == 0)
        return false;
    std::string k = toUpper(arg.substr(0, eq));
    for (const auto& known : keys) {
        if (k == known) {
            key = k;
            value = arg.substr(eq + 1);
            return true;
        }
    }
    return false;
}

int digitValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool scanNumber(const char*& p, unsigned base, unsigned long& value)
{
    value = 0;
    bool any = false;
    for (;;) {
        int d = digitValue(*p);
        if (d < 0)
            return any;
        if (static_cast<unsigned>(d) >= base)
            return false;
        value = value * base + static_cast<unsigned>(d);
        if (value > 0xFFFFFFFFul)
            value = 0xFFFFFFFFul;
        any = true;
        ++p;
    }
}

static bool hasExtension(const std::string& name)
{
    auto slash = name.find_last_of('/');
    std::string last = slash == std::string::npos ? name : name.substr(slash + 1);
    return last.find('.') != std::string::npos;
}

static std::string lowerExt(const std::string& ext)
{
    std::string e = ext;
    for (char& c : e)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

std::string defaultExt(const std::string& name, const std::string& ext)
{
    return hasExtension(name) ? name : name + ext;
}

std::string inputName(const std::string& name, const std::string& ext)
{
    if (hasExtension(name))
        return name;
    std::string upper = name + ext;
    if (fileExists(upper))
        return upper;
    std::string lower = name + lowerExt(ext);
    return fileExists(lower) ? lower : upper;
}

std::string outputName(const std::string& name, const std::string& ext)
{
    return defaultExt(name, lowerExt(ext));
}

bool fileExists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        fatal("Cannot open %s: %s", path.c_str(), std::strerror(errno));
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        fatal("Cannot create %s: %s", path.c_str(), std::strerror(errno));
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out)
        fatal("Error writing %s", path.c_str());
}

bool stdinIsTerminal()
{
    return ::isatty(STDIN_FILENO) != 0;
}

int readKey()
{
    std::fflush(stdout);
    termios saved;
    bool raw = ::tcgetattr(STDIN_FILENO, &saved) == 0;
    if (raw) {
        termios t = saved;
        t.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSANOW, &t);
    }
    unsigned char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (raw)
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return n == 1 ? c : -1;
}

}  // namespace imd
