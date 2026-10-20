#ifndef AURA_MM_MEMORY_CONTROL_H_
#define AURA_MM_MEMORY_CONTROL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include "mm/xmemory.h"

namespace au {
namespace mm {
class Registry;
namespace detail {

struct MemoryIdentity {
    std::uintptr_t mOwner = 0;
    std::uint64_t mAllocationId = 0;
    std::uint64_t mGeneration = 0;
    bool operator==(const au::mm::detail::MemoryIdentity& other) const noexcept
    {
        return mOwner == other.mOwner && mAllocationId == other.mAllocationId && mGeneration == other.mGeneration;
    }
};
// Shared because free() and the last asynchronous device lease can run on
// different threads. This report does not itself keep the allocation alive.
struct ReleaseStatus {
    std::atomic<bool> mDone{false};
    int mCode = 0; // Read only after acquiring mDone.
};

class MemoryControl {
public:
    MemoryControl(const au::mm::MemBlock& block, const au::mm::detail::MemoryIdentity& identity,
                  std::shared_ptr<au::mm::detail::ReleaseStatus> report) noexcept
        : mBlock(block), mIdentity(identity), mReport(std::move(report)) {}
    ~MemoryControl() noexcept;
    MemoryControl(const au::mm::detail::MemoryControl&) = delete;
    au::mm::detail::MemoryControl& operator=(const au::mm::detail::MemoryControl&) = delete;
    // Control records have stable identity and are never moved; leases move instead.
    MemoryControl(au::mm::detail::MemoryControl&&) = delete;
    au::mm::detail::MemoryControl& operator=(au::mm::detail::MemoryControl&&) = delete;

    const au::mm::MemBlock mBlock;
    const au::mm::detail::MemoryIdentity mIdentity;
    const std::shared_ptr<au::mm::detail::ReleaseStatus> mReport;
    bool mOwnsStorage = false; // Set once before publication; failed insertion leaves ownership with alloc().
};

// Internal owning capability for asynchronous backing storage. Copying this
// capability is deliberate sharing; it never changes XImage's borrowed copies.
class MemoryLease {
public:
    MemoryLease() noexcept = default;
    explicit MemoryLease(std::shared_ptr<au::mm::detail::MemoryControl> control) noexcept
        : mControl(std::move(control)) {}
    MemoryLease(const au::mm::detail::MemoryLease&) noexcept = default;
    au::mm::detail::MemoryLease& operator=(const au::mm::detail::MemoryLease&) noexcept = default;
    MemoryLease(au::mm::detail::MemoryLease&&) noexcept = default;
    au::mm::detail::MemoryLease& operator=(au::mm::detail::MemoryLease&&) noexcept = default;
    bool valid() const noexcept { return static_cast<bool>(mControl); }
    const au::mm::MemBlock& block() const noexcept;
    au::mm::detail::MemoryIdentity identity() const noexcept
    { return mControl ? mControl->mIdentity : au::mm::detail::MemoryIdentity{}; }
    std::shared_ptr<au::mm::detail::ReleaseStatus> releaseStatus() const noexcept
    { return mControl ? mControl->mReport : nullptr; }
    void reset() noexcept { mControl.reset(); }
private:
    friend class au::mm::Registry;
    // One control is shared by owner registry, CPU scopes, imported CL objects and
    // in-flight fences; any one of them can be the last storage owner.
    std::shared_ptr<au::mm::detail::MemoryControl> mControl;
};

} // namespace detail
} // namespace mm
} // namespace au
#endif
