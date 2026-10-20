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

private:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    mutable std::mutex mMutex;
    std::map<std::uintptr_t, std::shared_ptr<au::mm::detail::MemoryControl>> mTable;
    std::atomic<std::uint64_t> mNextId{1};
};

}  // namespace mm
}  // namespace au

#endif  // AURA_MM_REGISTRY_H_
