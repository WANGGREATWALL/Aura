#ifndef AURA_MM_NATIVE_BACKEND_H_
#define AURA_MM_NATIVE_BACKEND_H_

#include "mm/backend.h"

namespace au {
namespace mm {

/// Native backend delegates physical allocation to au::sys. PSS is portable;
/// individual MemType requests may still fail (e.g. uncached heap missing).
IBackend& nativeBackend() noexcept;

}  // namespace mm
}  // namespace au

#endif  // AURA_MM_NATIVE_BACKEND_H_
