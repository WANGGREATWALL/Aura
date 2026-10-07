#include "file/xpath_api.h"

#include "regex/xregex.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#  include <direct.h>
#  define AU_GETCWD_(buffer, size) _getcwd((buffer), (size))
#else
#  include <unistd.h>
#  define AU_GETCWD_(buffer, size) getcwd((buffer), (size))
#endif

namespace {

#ifdef _WIN32
constexpr char kSeparator = '\\';
constexpr char kAlternateSeparator = '/';
constexpr char kCurrentDir[] = ".\\";
#else
constexpr char kSeparator = '/';
constexpr char kCurrentDir[] = "./";
#endif

bool isSeparator(char ch) noexcept
{
#ifdef _WIN32
    return ch == kSeparator || ch == kAlternateSeparator;
#else
    return ch == kSeparator;
#endif
}

// A failed write leaves the caller's output untouched.
int writeOut(const std::string& value, char* out, int outSize) noexcept
{
    if (!out || outSize <= 0 || value.size() >= static_cast<size_t>(outSize) ||
        value.size() > static_cast<size_t>(INT_MAX)) {
        return -1;
    }
    std::memmove(out, value.c_str(), value.size() + 1);
    return static_cast<int>(value.size());
}

template<typename Fn>
int safeResult(Fn&& fn) noexcept
{
    try {
        return fn();
    } catch (...) {
        return -1;
    }
}

std::string normalizePath(const char* in)
{
    if (!in || !*in) return {};
    std::string result;
    result.reserve(std::strlen(in));
    for (const char* p = in; *p; ++p) {
        if (isSeparator(*p)) {
            if (result.empty() || result.back() != kSeparator) result += kSeparator;
        } else {
            result += *p;
        }
    }
    return result;
}

std::string filenameOf(const std::string& path)
{
    if (path.empty()) return {};
    const size_t end = path.find_last_not_of(kSeparator);
    if (end == std::string::npos) return path;
    const size_t separator = path.rfind(kSeparator, end);
    return path.substr(separator == std::string::npos ? 0 : separator + 1,
                       end - (separator == std::string::npos ? 0 : separator + 1) + 1);
}

std::string extensionOf(const std::string& filename)
{
    const size_t dot = filename.rfind('.');
    return dot == std::string::npos || dot == 0 ? std::string() : filename.substr(dot);
}

std::string stemOf(const std::string& filename)
{
    const size_t dot = filename.rfind('.');
    return dot == std::string::npos || dot == 0 ? filename : filename.substr(0, dot);
}

std::string dottedExtension(const char* extension)
{
    std::string result = extension ? extension : "";
    if (!result.empty() && result.front() != '.') result.insert(result.begin(), '.');
    return result;
}

bool endsWithIgnoringCase(const std::string& value, const std::string& suffix) noexcept
{
    if (suffix.size() > value.size()) return false;
    const size_t start = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char left = static_cast<unsigned char>(value[start + i]);
        const unsigned char right = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

std::string joinPath(const char* dir, const char* relative)
{
    std::string base = normalizePath(dir);
    const std::string child = normalizePath(relative);
    if (base.empty()) return child;
    if (base.back() == kSeparator) base.pop_back();
    base += kSeparator;
    base += child;
    return base;
}

bool parseSize(const std::string& token, uint32_t* outW, uint32_t* outH) noexcept
{
    const size_t delimiter = token.find('x');
    if (delimiter == std::string::npos || delimiter == 0 || delimiter + 1 == token.size()) {
        return false;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    for (size_t i = 0; i < token.size(); ++i) {
        if (i == delimiter) continue;
        if (token[i] < '0' || token[i] > '9') return false;
        uint32_t& number = i < delimiter ? width : height;
        number = number * 10 + static_cast<uint32_t>(token[i] - '0');
    }
    *outW = width;
    *outH = height;
    return true;
}

void imageSize(const char* in, uint32_t* outW, uint32_t* outH, bool first) noexcept
{
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!in || !outW || !outH) return;
    try {
        const std::string filename = filenameOf(normalizePath(in));
        const std::string pattern = "[0-9]{1,5}x[0-9]{1,5}";
        const std::string token = first
            ? au::re::getFirstMatchInString(filename, pattern)
            : au::re::getLastMatchInString(filename, pattern);
        if (!token.empty()) parseSize(token, outW, outH);
    } catch (...) {
        *outW = 0;
        *outH = 0;
    }
}

int stemBeforeSize(const char* in, char* out, int outSize, bool first)
{
    const std::string filename = filenameOf(normalizePath(in));
    const std::string pattern = "_[0-9]{1,5}x[0-9]{1,5}";
    const std::string token = first
        ? au::re::getFirstMatchInString(filename, pattern)
        : au::re::getLastMatchInString(filename, pattern);
    if (token.empty()) return writeOut(stemOf(filename), out, outSize);
    const size_t position = first ? filename.find(token) : filename.rfind(token);
    return writeOut(filename.substr(0, position), out, outSize);
}

}  // namespace

extern "C" {

AU_API int au_xpath_normalize(const char* in, char* out, int outSize)
{
    return safeResult([&] { return writeOut(normalizePath(in), out, outSize); });
}

AU_API int au_xpath_is_directory(const char* in)
{
    return in && *in && isSeparator(in[std::strlen(in) - 1]) ? 1 : 0;
}

AU_API int au_xpath_is_absolute(const char* in)
{
    if (!in || !*in) return 0;
#ifdef _WIN32
    return std::strlen(in) >= 3 &&
                   std::isalpha(static_cast<unsigned char>(in[0])) &&
                   in[1] == ':' && isSeparator(in[2]) ? 1 : 0;
#else
    return in[0] == kSeparator ? 1 : 0;
#endif
}

AU_API int au_xpath_is_root(const char* in)
{
    if (!in) return 0;
#ifdef _WIN32
    return std::strlen(in) == 3 && au_xpath_is_absolute(in) ? 1 : 0;
#else
    return in[0] == kSeparator && in[1] == '\0' ? 1 : 0;
#endif
}

AU_API int au_xpath_parent(const char* in, char* out, int outSize)
{
    return safeResult([&] {
        const std::string path = normalizePath(in);
        if (!path.empty() && path.back() == kSeparator) return writeOut(path, out, outSize);
        const size_t separator = path.rfind(kSeparator);
        return writeOut(separator == std::string::npos
                            ? std::string(kCurrentDir) : path.substr(0, separator + 1),
                        out, outSize);
    });
}

AU_API int au_xpath_filename(const char* in, char* out, int outSize)
{
    return safeResult([&] {
        return writeOut(filenameOf(normalizePath(in)), out, outSize);
    });
}

AU_API int au_xpath_extension(const char* in, char* out, int outSize)
{
    return safeResult([&] {
        return writeOut(extensionOf(filenameOf(normalizePath(in))), out, outSize);
    });
}

AU_API int au_xpath_stem(const char* in, char* out, int outSize)
{
    return safeResult([&] {
        return writeOut(stemOf(filenameOf(normalizePath(in))), out, outSize);
    });
}

AU_API int au_xpath_without_trailing_sep(const char* in, char* out, int outSize)
{
    return safeResult([&] {
        std::string path = normalizePath(in);
        if (!path.empty() && path.back() == kSeparator) path.pop_back();
        return writeOut(path, out, outSize);
    });
}

AU_API int au_xpath_with_trailing_sep(const char* in, char* out, int outSize)
{
    return safeResult([&] {
        std::string path = normalizePath(in);
        if (path.empty() || path.back() != kSeparator) path += kSeparator;
        return writeOut(path, out, outSize);
    });
}

AU_API int au_xpath_without_extension(
    const char* in, const char* ext, char* out, int outSize)
{
    return safeResult([&] {
        const std::string path = normalizePath(in);
        const std::string suffix = dottedExtension(ext);
        return writeOut(!suffix.empty() && endsWithIgnoringCase(path, suffix)
                            ? path.substr(0, path.size() - suffix.size()) : path,
                        out, outSize);
    });
}

AU_API int au_xpath_replace_extension(
    const char* in, const char* newExt, char* out, int outSize)
{
    return safeResult([&] {
        std::string path = normalizePath(in);
        const std::string oldExtension = extensionOf(filenameOf(path));
        if (!oldExtension.empty()) path.resize(path.size() - oldExtension.size());
        path += dottedExtension(newExt);
        return writeOut(path, out, outSize);
    });
}

AU_API int au_xpath_join(const char* dir, const char* rel, char* out, int outSize)
{
    return safeResult([&] { return writeOut(joinPath(dir, rel), out, outSize); });
}

AU_API int au_xpath_cwd(char* out, int outSize)
{
    return safeResult([&] {
        char buffer[AU_PATH_BUF_SIZE] = {};
        const char* result = AU_GETCWD_(buffer, static_cast<int>(sizeof(buffer)));
        return writeOut(result ? result : "", out, outSize);
    });
}

AU_API int au_xpath_make_filename_n(
    const char* folder, const char* name, int number, const char* ext,
    char* out, int outSize)
{
    return safeResult([&] {
        std::string filename = name ? name : "";
        if (number != 0) filename += "_" + std::to_string(number);
        filename += ".";
        filename += ext ? ext : "";
        return writeOut(joinPath(folder, filename.c_str()), out, outSize);
    });
}

AU_API int au_xpath_make_filename(
    const char* folder, const char* name, const char* ext, char* out, int outSize)
{
    return au_xpath_make_filename_n(folder, name, 0, ext, out, outSize);
}

AU_API int au_xpath_first_match(
    const char* in, const char* regex, char* out, int outSize)
{
    return safeResult([&] {
        return writeOut(!in || !regex ? std::string()
                                      : au::re::getFirstMatchInString(in, regex), out, outSize);
    });
}

AU_API int au_xpath_last_match(
    const char* in, const char* regex, char* out, int outSize)
{
    return safeResult([&] {
        return writeOut(!in || !regex ? std::string()
                                      : au::re::getLastMatchInString(in, regex), out, outSize);
    });
}

AU_API void au_xpath_first_image_size(const char* in, uint32_t* outW, uint32_t* outH)
{
    imageSize(in, outW, outH, true);
}

AU_API void au_xpath_last_image_size(const char* in, uint32_t* outW, uint32_t* outH)
{
    imageSize(in, outW, outH, false);
}

AU_API int au_xpath_stem_before_first_size(const char* in, char* out, int outSize)
{
    return safeResult([&] { return stemBeforeSize(in, out, outSize, true); });
}

AU_API int au_xpath_stem_before_last_size(const char* in, char* out, int outSize)
{
    return safeResult([&] { return stemBeforeSize(in, out, outSize, false); });
}

}  // extern "C"
