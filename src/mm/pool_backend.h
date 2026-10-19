#ifndef AURA_MM_POOL_BACKEND_H_
#define AURA_MM_POOL_BACKEND_H_

#include "mm/backend.h"

namespace au {
namespace mm {

/// Pool backend entry point. The vendor libvivo.mempool.so integration is not
/// implemented yet: available() is false and all operations report
/// kErrorNotSupported. Auto selection therefore falls back to Native.
IBackend& poolBackend() noexcept;

}  // namespace mm
}  // namespace au

#endif  // AURA_MM_POOL_BACKEND_H_
