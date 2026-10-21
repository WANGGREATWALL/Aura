#ifndef AURA_MM_REGISTRY_H_
#define AURA_MM_REGISTRY_H_

#include <mutex>
#include <atomic>
#include <map>
#include <memory>

#include "mm/xmemory.h"
#include "mm/memory_control.h"

namespace au {
namespace mm {

/// Per-linked-instance owner registry. Interior views resolve to one stable
/// allocation; asynchronous leases survive removal of the owner's entry.
class Registry
{
public:
    static Registry& instance() noexcept;

    int insert(const MemBlock& block) noexcept;
    int find(void* ptr, MemBlock& out) const noexcept;
    int retire(void* ptr) noexcept;
    int acquire(const void* ptr, std::size_t size, au::mm::detail::MemoryLease& out) const noexcept;
    int registerExternal(const ::au_mm_external_memory& memory, void** out) noexcept;
    int unregisterExternal(void* handle) noexcept;

private:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    mutable std::mutex mMutex;
    struct View {
        std::shared_ptr<au::mm::detail::MemoryControl> mControl;
        std::shared_ptr<au::mm::detail::ExternalOwner> mOwner;
        std::size_t mSize = 0;
        std::uint64_t mRegistration = 0; // Zero is an Aura-owned allocation.
    };
    std::map<std::uintptr_t, au::mm::Registry::View> mTable;
    std::map<au::sys::memory::FdIdentity, std::weak_ptr<au::mm::detail::MemoryControl>> mFds;
    std::atomic<std::uint64_t> mNextId{1};
    int nextId(std::uint64_t& out) noexcept;
    bool overlaps(std::uintptr_t begin, std::size_t size) const noexcept; // mMutex held.
};

}  // namespace mm
}  // namespace au

#endif  // AURA_MM_REGISTRY_H_
