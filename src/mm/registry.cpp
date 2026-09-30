#include "mm/registry.h"

#include "log/xerror.h"

namespace au {
namespace mm {

Registry& Registry::instance() noexcept
{
    // Heap-allocated singleton that's intentionally never destroyed.
    // This avoids the "static destruction order fiasco" - other global
    // destructors or late-running threads may still call instance() during
    // process shutdown, after a stack-allocated singleton's mutex has been
    // destroyed. The OS reclaims all memory at exit anyway.
    static Registry* sInstance = new Registry();
    return *sInstance;
}

int Registry::insert(const MemBlock& block) noexcept
{
    if (block.ptr == nullptr) {
        return au::err::kErrorNullPointer;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    auto result = mTable.emplace(block.ptr, block);
    return result.second ? au::err::kSuccess : au::err::kErrorAlreadyExists;
}

int Registry::find(void* ptr, MemBlock& out) const noexcept
{
    if (ptr == nullptr) {
        return au::err::kErrorNullPointer;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mTable.find(ptr);
    if (it == mTable.end()) {
        return au::err::kErrorInvalidAddr;
    }
    out = it->second;
    return au::err::kSuccess;
}

int Registry::take(void* ptr, MemBlock& out) noexcept
{
    if (ptr == nullptr) {
        return au::err::kErrorNullPointer;
    }
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mTable.find(ptr);
    if (it == mTable.end()) {
        return au::err::kErrorInvalidAddr;
    }
    out = it->second;
    mTable.erase(it);
    return au::err::kSuccess;
}

}  // namespace mm
}  // namespace au
