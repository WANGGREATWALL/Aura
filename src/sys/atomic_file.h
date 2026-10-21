#ifndef AURA_SYS_ATOMIC_FILE_H_
#define AURA_SYS_ATOMIC_FILE_H_
#include <cstddef>
#include <string>
namespace au {
namespace sys {
// Best-effort cache publication in the same directory. Readers see either old
// or complete new bytes; no persistence/fsync guarantee is claimed.
int replaceFile(const std::string& path, const void* bytes, std::size_t size) noexcept;
} // namespace sys
} // namespace au
#endif
