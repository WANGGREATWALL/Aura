#ifndef AURA_XPATH_H_
#define AURA_XPATH_H_

/**
 * @file xpath.h
 * @brief Header-only C++ wrapper around the XPath C ABI (xpath_api.h).
 *
 * Hourglass Pattern: all heavy logic lives in the compiled C functions
 * (xpath_api.h / xpath_impl.cpp). This header is compiled consumer-side
 * and has no AU_API decoration: it carries no DLL-exported symbols.
 *
 * This wrapper provides string-path operations through the C ABI.
 */

#include <string>
#include <cstdint>
#include <cstddef>

#include "file/xpath_api.h"  // AU_PATH_BUF_SIZE + all au_xpath_* declarations

namespace au {
namespace file {

/**
 * @brief Cross-platform path string manipulation class (no filesystem access).
 *
 * - Value semantics: all mutating operations return a new XPath.
 * - Pure string layer: filesystem operations belong to XFile.
 * - Separator: '\' on Windows, '/' elsewhere.
 * - Implicitly constructible from std::string / const char*.
 *
 * Convention: a path ending with a separator (e.g. "dir/") denotes a
 * directory intent; otherwise it is treated as a file path.
 * No syntax validation is performed.
 */
class XPath {
public:
    // -------------------------------------------------------------------------
    // Construction / assignment
    // -------------------------------------------------------------------------

    XPath() = default;

    /** Construct from std::string; normalizes separators on construction. */
    XPath(const std::string& pathname) {  // NOLINT(google-explicit-constructor)
        char buf[AU_PATH_BUF_SIZE] = {};
        if (au_xpath_normalize(pathname.c_str(), buf, AU_PATH_BUF_SIZE) >= 0) {
            mPathname = buf;
        }
    }

    /** Construct from C-string; nullptr is treated as an empty path. */
    XPath(const char* pathname) {  // NOLINT(google-explicit-constructor)
        char buf[AU_PATH_BUF_SIZE] = {};
        if (au_xpath_normalize(pathname ? pathname : "", buf, AU_PATH_BUF_SIZE) >= 0) {
            mPathname = buf;
        }
    }

    XPath(const XPath&) = default;
    XPath(XPath&&) noexcept = default;
    XPath& operator=(const XPath&) = default;
    XPath& operator=(XPath&&) noexcept = default;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /** Returns the underlying path string. */
    const std::string& string() const { return mPathname; }

    /** Returns the underlying path as a null-terminated C-string. */
    const char* c_str() const { return mPathname.c_str(); }

    /** Implicit conversion to std::string (value copy). */
    operator std::string() const { return mPathname; }  // NOLINT(google-explicit-constructor)

    // -------------------------------------------------------------------------
    // Predicates
    // -------------------------------------------------------------------------

    /** Returns true if the path is an empty string. */
    bool isEmpty() const { return mPathname.empty(); }

    /**
     * @brief Returns true if the path ends with a separator character.
     *        Does NOT check whether the directory actually exists.
     */
    bool isDirectory() const {
        return au_xpath_is_directory(mPathname.c_str()) != 0;
    }

    /**
     * @brief Returns true if the path is absolute.
     *        Linux: starts with '/'. Windows: starts with "X:\".
     */
    bool isAbsolute() const {
        return au_xpath_is_absolute(mPathname.c_str()) != 0;
    }

    /**
     * @brief Returns true if the path is a root directory.
     *        Linux: "/". Windows: "C:\".
     */
    bool isRoot() const {
        return au_xpath_is_root(mPathname.c_str()) != 0;
    }

    // -------------------------------------------------------------------------
    // Decomposition
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the parent directory path (always ends with a separator).
     *        "path/to/file" -> "path/to/"
     *        "just_a_file" -> "./"
     *        "path/to/dir/" -> "path/to/dir/" (directory path returned as-is)
     */
    XPath parent() const {
        return make_([this](char* b, int n) {
            return au_xpath_parent(mPathname.c_str(), b, n);
        });
    }

    /**
     * @brief Returns the last path component (filename or innermost dir name).
     *        "path/to/file.txt" -> "file.txt"
     *        "path/to/dir/" -> "dir"
     */
    XPath filename() const {
        return make_([this](char* b, int n) {
            return au_xpath_filename(mPathname.c_str(), b, n);
        });
    }

    /**
     * @brief Returns the extension of the filename, including the leading dot.
     *        "file.txt" -> ".txt"
     *        "archive.tar.gz" -> ".gz"
     *        "noext" -> ""
     *        ".hidden" -> ""
     */
    XPath extension() const {
        return make_([this](char* b, int n) {
            return au_xpath_extension(mPathname.c_str(), b, n);
        });
    }

    /**
     * @brief Returns the filename without its extension (the stem).
     *        "path/to/file.txt" -> "file"
     *        "archive.tar.gz" -> "archive.tar"
     *        "noext" -> "noext"
     */
    XPath stem() const {
        return make_([this](char* b, int n) {
            return au_xpath_stem(mPathname.c_str(), b, n);
        });
    }

    // -------------------------------------------------------------------------
    // Transformation (returns a new XPath)
    // -------------------------------------------------------------------------

    /** Returns a copy with the trailing separator removed. */
    XPath withoutTrailingSeparator() const {
        return make_([this](char* b, int n) {
            return au_xpath_without_trailing_sep(mPathname.c_str(), b, n);
        });
    }

    /** Returns a copy with a trailing separator appended. */
    XPath withTrailingSeparator() const {
        return make_([this](char* b, int n) {
            return au_xpath_with_trailing_sep(mPathname.c_str(), b, n);
        });
    }

    /**
     * @brief Returns a copy with the given extension removed (case-insensitive).
     *        The leading dot is optional: both "txt" and ".txt" are accepted.
     *        "file.TXT".withoutExtension("txt") -> "file"
     */
    XPath withoutExtension(const std::string& ext) const {
        return make_([this, &ext](char* b, int n) {
            return au_xpath_without_extension(mPathname.c_str(), ext.c_str(), b, n);
        });
    }

    /**
     * @brief Returns a copy with the extension replaced.
     *        The leading dot is optional: both "png" and ".png" are accepted.
     *        "file.txt".replaceExtension("png") -> "file.png"
     */
    XPath replaceExtension(const std::string& newExt) const {
        return make_([this, &newExt](char* b, int n) {
            return au_xpath_replace_extension(mPathname.c_str(), newExt.c_str(), b, n);
        });
    }

    // -------------------------------------------------------------------------
    // Path concatenation
    // -------------------------------------------------------------------------

    /**
     * @brief Appends a relative path, inserting the platform separator.
     *        XPath("dir") / "file.txt" -> "dir/file.txt" (Linux)
     *                                  -> "dir\file.txt" (Windows)
     */
    XPath operator/(const XPath& relative) const {
        return make_([this, &relative](char* b, int n) {
            return au_xpath_join(mPathname.c_str(), relative.mPathname.c_str(), b, n);
        });
    }

    XPath operator/(const std::string& relative) const {
        return *this / XPath(relative);
    }

    XPath operator/(const char* relative) const {
        return *this / XPath(relative);
    }

    // -------------------------------------------------------------------------
    // Comparison
    // -------------------------------------------------------------------------

    bool operator==(const XPath& rhs) const { return mPathname == rhs.mPathname; }
    bool operator!=(const XPath& rhs) const { return mPathname != rhs.mPathname; }

    // -------------------------------------------------------------------------
    // Static utilities
    // -------------------------------------------------------------------------

    /** Returns the current working directory, or an empty XPath on failure. */
    static XPath cwd() {
        return make_([](char* b, int n) {
            return au_xpath_cwd(b, n);
        });
    }

    /**
     * @brief Joins a directory and a relative path, equivalent to directory / relativePath.
     *        If directory is empty, returns relativePath unchanged.
     */
    static XPath join(const XPath& directory, const XPath& relativePath) {
        return directory / relativePath;
    }

    /**
     * @brief Builds a file path as "folder/name.ext" or "folder/name_N.ext".
     *        The numeric suffix is omitted when number == 0.
     *        makeFilename("out", "test", 0, "xml") -> "out/test.xml"
     *        makeFilename("out", "test", 3, "xml") -> "out/test_3.xml"
     */
    static XPath makeFilename(
        const XPath& folder,
        const XPath& name,
        int number,
        const char* extension) {
        return make_([&](char* b, int n) {
            return au_xpath_make_filename_n(
                folder.c_str(), name.c_str(), number, extension, b, n);
        });
    }

    /**
     * @brief Builds a file path as "folder/name.ext" (no numeric suffix).
     *        makeFilename("out", "image", "png") -> "out/image.png"
     */
    static XPath makeFilename(
        const std::string& folder,
        const std::string& name,
        const std::string& extension) {
        return make_([&](char* b, int n) {
            return au_xpath_make_filename(
                folder.c_str(), name.c_str(), extension.c_str(), b, n);
        });
    }

    // -------------------------------------------------------------------------
    // Regex helpers (operate on the full path string)
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the first substring matching regex, or "" if none.
     *        XPath("img_320x240.jpg").firstMatch("[0-9]+x[0-9]+") -> "320x240"
     */
    std::string firstMatch(const std::string& regex) const {
        return str_([this, &regex](char* b, int n) {
            return au_xpath_first_match(mPathname.c_str(), regex.c_str(), b, n);
        });
    }

    /**
     * @brief Returns the last substring matching regex, or "" if none.
     *        XPath("img_320x240_640x480.jpg").lastMatch("[0-9]+x[0-9]+") -> "640x480"
     */
    std::string lastMatch(const std::string& regex) const {
        return str_([this, &regex](char* b, int n) {
            return au_xpath_last_match(mPathname.c_str(), regex.c_str(), b, n);
        });
    }

    // -------------------------------------------------------------------------
    // Image size parsing (parses "NxM" tokens from the filename)
    // -------------------------------------------------------------------------

    struct ImageSize {
        uint32_t width{0};
        uint32_t height{0};

        ImageSize() = default;
        ImageSize(uint32_t w, uint32_t h) : width(w), height(h) {}
    };

    /**
     * @brief Parses the first "NxM" token in the filename.
     *        Returns {0, 0} if no match is found.
     */
    ImageSize firstImageSize() const {
        ImageSize sz;
        au_xpath_first_image_size(mPathname.c_str(), &sz.width, &sz.height);
        return sz;
    }

    /**
     * @brief Parses the last "NxM" token in the filename.
     *        Returns {0, 0} if no match is found.
     */
    ImageSize lastImageSize() const {
        ImageSize sz;
        au_xpath_last_image_size(mPathname.c_str(), &sz.width, &sz.height);
        return sz;
    }

    /**
     * @brief Returns the stem portion before the first "_NxM" size tag.
     *        Falls back to stem() when no size tag is found.
     *        "dir/img_640x480_result.jpg" -> "img"
     */
    std::string stemBeforeFirstSize() const {
        return str_([this](char* b, int n) {
            return au_xpath_stem_before_first_size(mPathname.c_str(), b, n);
        });
    }

    /**
     * @brief Returns the stem portion before the last "_NxM" size tag.
     *        Falls back to stem() when no size tag is found.
     *        "dir/img_128x256_640x480.jpg" -> "img_128x256"
     */
    std::string stemBeforeLastSize() const {
        return str_([this](char* b, int n) {
            return au_xpath_stem_before_last_size(mPathname.c_str(), b, n);
        });
    }

private:
    std::string mPathname;

    // Internal helpers reduce repetition for buffer-returning C calls.

    /** Call fn(buf, size) -> int; return result as an XPath (raw, no re-normalize). */
    template<typename Fn>
    static XPath make_(Fn fn) {
        char buf[AU_PATH_BUF_SIZE] = {};
        const int n = fn(buf, AU_PATH_BUF_SIZE);
        XPath r;
        if (n >= 0 && n < AU_PATH_BUF_SIZE) {
            r.mPathname.assign(buf, static_cast<std::size_t>(n));
        }
        return r;
    }

    /** Call fn(buf, size) -> int; return result as std::string. */
    template<typename Fn>
    static std::string str_(Fn fn) {
        char buf[AU_PATH_BUF_SIZE] = {};
        const int n = fn(buf, AU_PATH_BUF_SIZE);
        return (n >= 0 && n < AU_PATH_BUF_SIZE)
            ? std::string(buf, static_cast<std::size_t>(n))
            : std::string();
    }
};

}  // namespace file
}  // namespace au

#endif  // AURA_XPATH_H_
