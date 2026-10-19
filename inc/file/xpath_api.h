#ifndef AURA_XPATH_API_H_
#define AURA_XPATH_API_H_

/**
 * @file xpath_api.h
 * @brief Pure C ABI for cross-platform path string manipulation.
 *
 * This is the "narrow waist" of the Hourglass Pattern employed by XPath.
 * All functions accept and return null-terminated C strings via caller-
 * provided output buffers, ensuring complete ABI stability across compiler
 * versions and standard-library implementations.
 *
 * Output buffer convention:
 *   - All buffer-out functions write a NUL-terminated string to @p out.
 *   - Return value >= 0: number of bytes written (excluding the NUL).
 *   - Return value -1: output buffer too small; @p out is unmodified.
 *
 * Recommended stack-buffer size: AU_PATH_BUF_SIZE (4096 bytes).
 */

#include <cstdint>

#include "sys/xsystem.h"  // AU_API

/// Recommended stack-buffer size for all au_xpath_* output parameters.
#define AU_PATH_BUF_SIZE 4096

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Normalization
// -----------------------------------------------------------------------------

/**
 * @brief Collapse consecutive separators into one; on Windows also converts
 *        '/' to '\\'.
 *
 * @param in      Input path (nullptr is treated as "").
 * @param out     Output buffer receiving the normalized path.
 * @param outSize Capacity of @p out in bytes (must include room for NUL).
 * @return Written length, or -1 on overflow.
 */
AU_API int au_xpath_normalize(const char* in, char* out, int outSize);

// -----------------------------------------------------------------------------
// Decomposition
// -----------------------------------------------------------------------------

/**
 * @brief Returns the parent directory (always ends with a separator).
 *        "path/to/file" -> "path/to/"  "just_a_file" -> "./"
 *        "path/to/dir/" -> "path/to/dir/"  (directory path returned as-is)
 */
AU_API int au_xpath_parent(const char* in, char* out, int outSize);

/**
 * @brief Returns the last path component (filename or innermost dir name).
 *        "path/to/file.txt" -> "file.txt"  "path/to/dir/" -> "dir"
 */
AU_API int au_xpath_filename(const char* in, char* out, int outSize);

/**
 * @brief Returns the extension including the leading dot, or "" if none.
 *        "file.txt" -> ".txt"  ".hidden" -> ""  "noext" -> ""
 */
AU_API int au_xpath_extension(const char* in, char* out, int outSize);

/**
 * @brief Returns the filename without its extension.
 *        "path/to/file.txt" -> "file"  "archive.tar.gz" -> "archive.tar"
 */
AU_API int au_xpath_stem(const char* in, char* out, int outSize);

// -----------------------------------------------------------------------------
// Transformation
// -----------------------------------------------------------------------------

/** @brief Returns a copy with the trailing separator removed. */
AU_API int au_xpath_without_trailing_sep(const char* in, char* out, int outSize);

/** @brief Returns a copy with a trailing separator appended. */
AU_API int au_xpath_with_trailing_sep(const char* in, char* out, int outSize);

/**
 * @brief Returns a copy with the given extension stripped (case-insensitive).
 * @p ext may include or omit the leading '.'.
 */
AU_API int au_xpath_without_extension(const char* in, const char* ext, char* out, int outSize);

/**
 * @brief Returns a copy with the extension replaced.
 * @p newExt may include or omit the leading '.'.
 */
AU_API int au_xpath_replace_extension(const char* in, const char* newExt, char* out, int outSize);

// -----------------------------------------------------------------------------
// Joining
// -----------------------------------------------------------------------------

/**
 * @brief Joins @p dir and @p rel with the platform separator.
 *        If @p dir is empty or nullptr, returns @p rel (normalized).
 */
AU_API int au_xpath_join(const char* dir, const char* rel, char* out, int outSize);

// -----------------------------------------------------------------------------
// Static utilities
// -----------------------------------------------------------------------------

/** @brief Writes the current working directory to @p out. Returns length or -1. */
AU_API int au_xpath_cwd(char* out, int outSize);

/**
 * @brief Builds "folder/name_N.ext" (omits the numeric suffix when number == 0).
 * @param ext Extension without leading '.'.
 */
AU_API int au_xpath_make_filename_n(
    const char* folder,
    const char* name,
    int number,
    const char* ext,
    char* out,
    int outSize);

/** @brief Builds "folder/name.ext". */
AU_API int au_xpath_make_filename(
    const char* folder,
    const char* name,
    const char* ext,
    char* out,
    int outSize);

// -----------------------------------------------------------------------------
// Predicates (1 = true, 0 = false)
// -----------------------------------------------------------------------------

/** @brief True if @p in ends with a path separator. */
AU_API int au_xpath_is_directory(const char* in);

/** @brief True if @p in is an absolute path. */
AU_API int au_xpath_is_absolute(const char* in);

/** @brief True if @p in is a filesystem root ("/" on POSIX, "C:\\" on Windows). */
AU_API int au_xpath_is_root(const char* in);

// -----------------------------------------------------------------------------
// Regex helpers
// -----------------------------------------------------------------------------

/** @brief Writes the first substring matching @p regex into @p out. Returns length or -1. */
AU_API int au_xpath_first_match(const char* in, const char* regex, char* out, int outSize);

/** @brief Writes the last substring matching @p regex into @p out. Returns length or -1. */
AU_API int au_xpath_last_match(const char* in, const char* regex, char* out, int outSize);

// -----------------------------------------------------------------------------
// Image-size parsing ("NxM" tokens in the filename)
// -----------------------------------------------------------------------------

/** @brief Parses the first "NxM" token; sets outW and outH to 0 when not found. */
AU_API void au_xpath_first_image_size(const char* in, uint32_t* outW, uint32_t* outH);

/** @brief Parses the last "NxM" token; sets outW and outH to 0 when not found. */
AU_API void au_xpath_last_image_size(const char* in, uint32_t* outW, uint32_t* outH);

/** @brief Writes the stem portion before the first "_NxM" tag (or full stem if absent). */
AU_API int au_xpath_stem_before_first_size(const char* in, char* out, int outSize);

/** @brief Writes the stem portion before the last "_NxM" tag (or full stem if absent). */
AU_API int au_xpath_stem_before_last_size(const char* in, char* out, int outSize);

#ifdef __cplusplus
}
#endif

#endif  // AURA_XPATH_API_H_
