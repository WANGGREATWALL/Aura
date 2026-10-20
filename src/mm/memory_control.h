#ifndef AURA_MM_MEMORY_CONTROL_H_
#define AURA_MM_MEMORY_CONTROL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include "mm/xmemory.h"
#include "mm/xaccess_api.h"
#include "sys/memory_runtime.h"

namespace au {
namespace mm {
class Registry;
namespace detail {

struct FenceStatus {
    int mCode = 0;
    int mNativeCode = 0;
    std::uint64_t mCauseTaskId = 0;
    bool mDeviceError = false;
};
// Device-neutral owning completion capability. Implementations include their
// transfer/readback commands in wait(), not the enclosing Task's final result.
struct DeviceFence {
    void* mPayload = nullptr;
    au::mm::detail::FenceStatus (*mWait)(void*) noexcept = nullptr;
    bool (*mReady)(void*) noexcept = nullptr; // Cached observation only; no driver call or blocking.
    void (*mDestroy)(void*) noexcept = nullptr;
    std::uintptr_t mContext = 0;
    void* mNativeEvent = nullptr; // Optional provider-specific immutable dependency handle; mm never dereferences it.
    std::shared_ptr<void> mNativeOwner; // Owns only native completion, never storage leases back to MemoryControl.
    bool mHostCompletion = false; // A host copyback/cleanup must complete before a native successor can submit.
    ~DeviceFence() noexcept { if (mDestroy) mDestroy(mPayload); }
    DeviceFence() noexcept = default;
    DeviceFence(const au::mm::detail::DeviceFence&) = delete;
    au::mm::detail::DeviceFence& operator=(const au::mm::detail::DeviceFence&) = delete;
    DeviceFence(au::mm::detail::DeviceFence&&) = delete;
    au::mm::detail::DeviceFence& operator=(au::mm::detail::DeviceFence&&) = delete;
    au::mm::detail::FenceStatus wait() const noexcept
    { return mWait ? mWait(mPayload) : au::mm::detail::FenceStatus{}; }
    bool ready() const noexcept { return mReady && mReady(mPayload); }
};
struct ExternalOwner {
    ExternalOwner() noexcept = default;
    ExternalOwner(const au::mm::detail::ExternalOwner&) = delete;
    au::mm::detail::ExternalOwner& operator=(const au::mm::detail::ExternalOwner&) = delete;
    ExternalOwner(au::mm::detail::ExternalOwner&&) = delete;
    au::mm::detail::ExternalOwner& operator=(au::mm::detail::ExternalOwner&&) = delete;
    void* mPayload = nullptr;
    ::au_mm_owner_release mRelease = nullptr;
    ~ExternalOwner() noexcept;
};
enum class CpuPhase { Idle, Starting, Active, Ending };

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
                  std::shared_ptr<au::mm::detail::ReleaseStatus> report)
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
    int mOwnedFd = -1; // External duplicate only; managed fd belongs to its backend.
    std::shared_ptr<au::mm::detail::ExternalOwner> mExternalOwner;
    au::sys::memory::FdIdentity mFdIdentity;
    bool mHasFdIdentity = false;

    std::mutex mAccessMutex;
    au::mm::detail::CpuPhase mCpuPhase = au::mm::detail::CpuPhase::Idle;
    unsigned int mCpuMode = 0;
    std::size_t mReaders = 0;
    bool mDevicePreparing = false;
    std::uint64_t mAccessVersion = 0;
    int mAccessError = 0; // A failed END/fence makes contents untrustworthy until allocation retirement.
    int mAccessNativeError = 0;
    // Shared because CPU waits, successive device commands and completion
    // cleanup may concurrently hold the same device-neutral fence.
    std::shared_ptr<au::mm::detail::DeviceFence> mFence;
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
    std::size_t offset() const noexcept { return mOffset; }
    std::size_t size() const noexcept { return mSize; }
    au::mm::detail::MemoryIdentity identity() const noexcept
    { return mControl ? mControl->mIdentity : au::mm::detail::MemoryIdentity{}; }
    std::shared_ptr<au::mm::detail::ReleaseStatus> releaseStatus() const noexcept
    { return mControl ? mControl->mReport : nullptr; }
    void reset() noexcept { mViewOwner.reset(); mControl.reset(); mOffset = 0; mSize = 0; }
    au::mm::detail::MemoryControl* control() const noexcept { return mControl.get(); }
    std::weak_ptr<au::mm::detail::MemoryControl> weakControl() const noexcept { return mControl; }
private:
    friend class au::mm::Registry;
    // One control is shared by owner registry, CPU scopes, imported CL objects and
    // in-flight fences; any one of them can be the last storage owner.
    std::shared_ptr<au::mm::detail::MemoryControl> mControl;
    // An alias mapping can have a different external owner from the canonical
    // mapping. Both remain pinned while its CPU pointer is being accessed.
    std::shared_ptr<au::mm::detail::ExternalOwner> mViewOwner;
    std::size_t mOffset = 0;
    std::size_t mSize = 0;
};

} // namespace detail
} // namespace mm
} // namespace au
#endif
