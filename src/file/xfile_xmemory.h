#ifndef AURA_XFILE_XMEMORY_H_
#define AURA_XFILE_XMEMORY_H_

/**
 * @file xfile_xmemory.h
 * @brief Internal extension of au::file with XMemory-aware overloads.
 *
 * Why a separate header:
 *   The public API in inc/file/xfile.h is restricted to STL/POD types so that
 *   downstream consumers do not transitively depend on Aura's internal memory
 *   subsystem. These overloads remain available to in-tree call sites that
 *   opt into the internal contract.
 *
 * Lifetime contract: identical to the public read/write family in xfile.h.
 */

#include <string>
#include <cstddef>

#include "file/xfile.h"  // public surface
#include "mm/xmemory.h"  // au::mm::XMemory (internal)

namespace au {
namespace file {

/**
 * @brief Reads the entire file into an XMemory buffer.
 *        Auto-allocates memory if buffer is unallocated (size == 0 or data == nullptr).
 *        If pre-allocated, its size must equal the file size exactly.
 *        Returns 0 on success, negative on failure.
 */
int read(const std::string& path, mm::XMemory& buffer);

/**
 * @brief Reads size bytes starting at offset into an XMemory buffer.
 *        Auto-allocates memory if buffer is unallocated.
 *        Fails if offset + size exceeds the file size.
 *        Returns 0 on success, negative on failure.
 */
int readAt(const std::string& path, size_t offset, size_t size, mm::XMemory& buffer);

/**
 * @brief Writes an XMemory buffer to a file, overwriting any existing content.
 *        Returns an error if content is empty or unallocated.
 *        Returns 0 on success, negative on failure.
 */
int write(const mm::XMemory& content, const std::string& path);

}  // namespace file
}  // namespace au

#endif  // AURA_XFILE_XMEMORY_H_
