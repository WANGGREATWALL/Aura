#ifndef AURA_MM_ACCESS_H_
#define AURA_MM_ACCESS_H_
#include <vector>
#include "mm/memory_control.h"
#include "mm/xaccess_api.h"

namespace au {
namespace mm {
namespace detail {

struct AccessEntry {
    au::mm::detail::MemoryControl* mControl = nullptr; // Pinned by mLeases.
    unsigned int mMode = 0;
    bool mStarted = false;
    bool mReserved = false;
    std::uint64_t mVersion = 0;
    std::shared_ptr<au::mm::detail::DeviceFence> mPrevious;
};
class AccessBatch {
public:
    int resolve(const ::au_mm_access_set* sets, std::size_t count) noexcept;
    const std::vector<au::mm::detail::MemoryLease>& leases() const noexcept { return mLeases; }
protected:
    // Allocation happens while resolving descriptors, before reservation or any
    // native enqueue. Lock/unlock never allocates and never invokes callbacks.
    void lock() noexcept;
    void unlock() noexcept;
    int releaseResources() noexcept;
    std::vector<au::mm::detail::MemoryLease> mLeases;
    std::vector<au::mm::detail::AccessEntry> mEntries;
    std::vector<std::unique_lock<std::mutex>> mLocks;
    std::vector<std::shared_ptr<au::mm::detail::ReleaseStatus>> mReports;
    int mNativeError = 0;
    std::uint64_t mCauseTaskId = 0;
    bool mDeviceError = false;
};
class CpuSession final : private au::mm::detail::AccessBatch {
public:
    CpuSession() noexcept;
    ~CpuSession() noexcept { (void)finish(0); }
    CpuSession(const au::mm::detail::CpuSession&) = delete;
    au::mm::detail::CpuSession& operator=(const au::mm::detail::CpuSession&) = delete;
    CpuSession(au::mm::detail::CpuSession&&) = delete;
    au::mm::detail::CpuSession& operator=(au::mm::detail::CpuSession&&) = delete;
    int begin(const ::au_mm_access_set* sets, std::size_t count) noexcept;
    int finish(int bodyCode) noexcept;
    bool ownedHere() const noexcept;
private:
    int reserve() noexcept;
    bool mActive = false;
    const void* mOwner = nullptr;
};
// A provider reserves all resources before native submission. Existing fences
// are returned for queue wait-lists or host bridges. commit publishes an already
// allocated final fence and cannot allocate; partial submissions commit their
// last valid event too. A fence must not own leases back to these controls.
class DeviceAccess final : private au::mm::detail::AccessBatch {
public:
    DeviceAccess() noexcept = default;
    ~DeviceAccess() noexcept { (void)finish(); }
    DeviceAccess(const au::mm::detail::DeviceAccess&) = delete;
    au::mm::detail::DeviceAccess& operator=(const au::mm::detail::DeviceAccess&) = delete;
    DeviceAccess(au::mm::detail::DeviceAccess&&) = delete;
    au::mm::detail::DeviceAccess& operator=(au::mm::detail::DeviceAccess&&) = delete;
    int begin(const ::au_mm_access_set* sets, std::size_t count, std::uintptr_t context = 0,
              bool nativeDependencies = false, bool hostAccess = false) noexcept;
    int commit(const std::shared_ptr<au::mm::detail::DeviceFence>& fence) noexcept;
    void abort() noexcept;
    // May block until native completion. A failed query alone must not make a
    // DeviceFence::wait return before the backing storage is safe to release.
    int finish() noexcept;
    // Provider-only: native termination AND required host transfer cleanup have
    // already been proven. Avoid recursively waiting on the fence being finalized.
    int releaseCompleted(const au::mm::detail::FenceStatus& status) noexcept;
    const std::vector<au::mm::detail::AccessEntry>& entries() const noexcept { return mEntries; }
    const std::vector<au::mm::detail::MemoryLease>& leases() const noexcept { return mLeases; }
    int nativeCode() const noexcept { return mNativeError; }
    bool deviceError() const noexcept { return mDeviceError; }
private:
    bool mActive = false;
    std::shared_ptr<au::mm::detail::DeviceFence> mCompletion;
};
int syncCpuAccess(au::mm::detail::MemoryControl* control, unsigned int mode, bool start, int* native) noexcept;
int legacySync(void* pointer, bool start) noexcept;

} // namespace detail
} // namespace mm
} // namespace au
#endif
