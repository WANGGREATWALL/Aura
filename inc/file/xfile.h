#ifndef AURA_XFILE_H_
#define AURA_XFILE_H_

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

#include "file/xpath.h"
#include "sys/xsystem.h"  // for AU_API

namespace au {
namespace file {

// =============================================================================
// Existence checks
// =============================================================================

/** @brief Returns true if path refers to a regular file (not a directory). */
AU_API bool existFile(const std::string& path);

/** @brief Returns true if path refers to an existing directory. */
AU_API bool existDir(const std::string& path);

/** @brief Returns true if path exists as any filesystem entry (file, dir, etc.). */
AU_API bool exists(const std::string& path);


// =============================================================================
// File attributes
// =============================================================================

/**
 * @brief Returns the file size in bytes.
 *        Returns 0 if the file does not exist or an error occurs.
 */
AU_API size_t sizeOf(const std::string& path);


// =============================================================================
// Directory operations
// =============================================================================

/**
 * @brief Creates a single directory; the parent must already exist.
 *        Succeeds silently if the directory already exists.
 *        Returns 0 on success, negative on failure.
 */
AU_API int createDir(const std::string& path);

/**
 * @brief Creates a directory and all missing parent directories (mkdir -p).
 *        Succeeds if the path already exists.
 *        Returns 0 on success, negative on failure.
 */
AU_API int createDirs(const std::string& path);


// =============================================================================
// File create / remove
// =============================================================================

/**
 * @brief Creates an empty file, truncating it if it already exists.
 *        The parent directory must exist.
 *        Returns 0 on success, negative on failure.
 */
AU_API int createFile(const std::string& path);

/**
 * @brief Deletes a file.
 *        Returns 0 on success, negative on failure (including when the file does not exist).
 */
AU_API int removeFile(const std::string& path);


// =============================================================================
// Read
// =============================================================================

/**
 * @brief Reads the entire file into a std::string (binary-safe).
 *        buffer is cleared and resized to match the file size.
 *        Returns 0 on success, negative on failure.
 */
AU_API int read(const std::string& path, std::string& buffer);

/**
 * @brief Reads the entire file into a caller-provided memory block.
 *        sizeInByte must equal the file size exactly.
 *        Returns 0 on success, negative on failure.
 */
AU_API int read(const std::string& path, void* data, size_t sizeInByte);

/**
 * @brief Reads sizeInByte bytes starting at offset into a caller-provided buffer.
 *        Fails if offset + sizeInByte exceeds the file size.
 *        Returns 0 on success, negative on failure.
 */
AU_API int readAt(const std::string& path, size_t offset, void* data, size_t sizeInByte);


// =============================================================================
// Write
// =============================================================================

/**
 * @brief Writes content to a file, overwriting any existing content.
 *        Creates the file if it does not exist.
 *        Returns 0 on success, negative on failure.
 */
AU_API int write(const std::string& content, const std::string& path);

/**
 * @brief Writes a raw memory block to a file, overwriting any existing content.
 *        Returns error if data is nullptr.
 *        Returns 0 on success, negative on failure.
 */
AU_API int write(const void* data, size_t sizeInByte, const std::string& path);

/**
 * @brief Appends content to a file.
 *        Creates the file if it does not exist.
 *        Returns 0 on success, negative on failure.
 */
AU_API int append(const std::string& content, const std::string& path);

/**
 * @brief Appends a raw memory block to a file.
 *        Creates the file if it does not exist.
 *        Returns error if data is nullptr.
 *        Returns 0 on success, negative on failure.
 */
AU_API int append(const void* data, size_t sizeInByte, const std::string& path);


// =============================================================================
// Directory listing
// =============================================================================

/**
 * @brief Lists regular files in dir (non-recursive).
 *        regex filters by filename; pass "" to disable filtering.
 *        absolute=true  -> returns full paths  (e.g. "dir/a.txt")
 *        absolute=false -> returns names only  (e.g. "a.txt")
 */
AU_API std::vector<XPath> listFiles(
    const std::string& dir,
    const std::string& regex = "",
    bool absolute = true);

/**
 * @brief Lists immediate subdirectories of dir (non-recursive).
 *        "." and ".." are always excluded.
 *        regex filters by directory name; pass "" to disable filtering.
 *        absolute=true  -> returns full paths  (e.g. "dir/sub")
 *        absolute=false -> returns names only  (e.g. "sub")
 */
AU_API std::vector<XPath> listDirs(
    const std::string& dir,
    const std::string& regex = "",
    bool absolute = true);

}  // namespace file
}  // namespace au

#endif  // AURA_XFILE_H_
