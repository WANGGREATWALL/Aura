#include "mm/access.h"
#include "mm/access_context.h"
#include "mm/registry.h"
#include "log/xerror.h"
#include "log/xlogger.h"
#include <algorithm>
#include <limits>

namespace {
thread_local au::mm::detail::AccessErrorSink gAccessSink;
}
au::mm::detail::AccessErrorSink au::mm::detail::setAccessErrorSink(
    const au::mm::detail::AccessErrorSink& sink) noexcept
{ auto previous = gAccessSink; gAccessSink = sink; return previous; }
bool au::mm::detail::accessMayWait() noexcept { return gAccessSink.mMayWait; }
void au::mm::detail::reportAccessError(int code, int nativeCode, std::uint64_t causeTaskId, bool deviceError) noexcept
{
    if (!code) return;
    if (gAccessSink.mReport) gAccessSink.mReport(gAccessSink.mPayload, code, nativeCode, causeTaskId, deviceError);
    XLOG_E("au::mm access failed code=%d native=%d\n", code, nativeCode);
}
int au::mm::detail::syncCpuAccess(au::mm::detail::MemoryControl* control,
                                 unsigned int mode, bool start, int* native) noexcept
{
    if (native) *native = 0;
    if (control->mBlock.type == au::mm::MemType::Pss) return 0;
    return au::sys::memory::cpuAccess(control->mBlock.fd, mode, start, native);
}
int au::mm::detail::AccessBatch::resolve(const ::au_mm_access_set* sets, std::size_t count) noexcept
{
    if (!sets || !count) return au::err::kErrorInvalidParam;
    if (count > std::numeric_limits<std::size_t>::max() / 4) return au::err::kErrorOverflow;
    try {
        mEntries.clear(); mLeases.clear(); mLocks.clear(); mReports.clear(); mNativeError = 0;
        mCauseTaskId = 0; mDeviceError = false;
        mLeases.reserve(count * 4); mEntries.reserve(count * 4);
        for (std::size_t set = 0; set < count; ++set) {
            if (sets[set].mStructSize != sizeof(sets[set])) return au::err::kErrorVersionMismatch;
            if (sets[set].mCode) return sets[set].mCode;
            if (!sets[set].mCount || sets[set].mCount > 4) return au::err::kErrorInvalidParam;
            for (std::size_t part = 0; part < sets[set].mCount; ++part) {
                const auto& range = sets[set].mRanges[part];
                if (range.mMode < 1 || range.mMode > 3) return au::err::kErrorInvalidParam;
                au::mm::detail::MemoryLease lease;
                const int code = au::mm::Registry::instance().acquire(range.mData, range.mSize, lease);
                if (code) return code;
                mEntries.push_back({lease.control(), range.mMode, false, false, 0, nullptr});
                mLeases.push_back(std::move(lease)); // Preserve every alias mapping's owner.
            }
        }
        std::sort(mEntries.begin(), mEntries.end(), [](const auto& a, const auto& b) {
            return a.mControl->mIdentity.mAllocationId < b.mControl->mIdentity.mAllocationId;
        });
        std::size_t unique = 0;
        for (std::size_t i = 0; i < mEntries.size(); ++i) {
            if (unique && mEntries[unique - 1].mControl == mEntries[i].mControl)
                mEntries[unique - 1].mMode |= mEntries[i].mMode;
            else { if (unique != i) mEntries[unique] = std::move(mEntries[i]); ++unique; }
        }
        mEntries.resize(unique);
        mLocks.resize(unique);
        mReports.reserve(unique);
        for (const auto& entry : mEntries) mReports.push_back(entry.mControl->mReport);
        return 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
void au::mm::detail::AccessBatch::lock() noexcept
{
    for (std::size_t i = 0; i < mEntries.size(); ++i)
        mLocks[i] = std::unique_lock<std::mutex>(mEntries[i].mControl->mAccessMutex);
}
void au::mm::detail::AccessBatch::unlock() noexcept
{ for (auto it = mLocks.rbegin(); it != mLocks.rend(); ++it) it->unlock(); }
int au::mm::detail::AccessBatch::releaseResources() noexcept
{
    mEntries.clear(); mLocks.clear(); mLeases.clear(); // No resource locks; may run an external owner release.
    int code = 0;
    for (const auto& report : mReports)
        if (report->mDone.load(std::memory_order_acquire) && report->mCode && !code) code = report->mCode;
    mReports.clear();
    return code;
}

int au::mm::detail::CpuSession::reserve() noexcept
{
    // Versioned retry never holds a resource reservation while waiting for a
    // device. A racing submit can force retry, but cannot leave partial access.
    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        for (auto& entry : mEntries) entry.mPrevious.reset(); // Native destruction outside locks.
        int code = 0;
        lock();
        for (auto& entry : mEntries) {
            auto* c = entry.mControl;
            if (c->mAccessError) { code = c->mAccessError; mNativeError = c->mAccessNativeError; break; }
            if (c->mDevicePreparing || (c->mCpuPhase != au::mm::detail::CpuPhase::Idle &&
                !(c->mCpuPhase == au::mm::detail::CpuPhase::Active && c->mCpuMode == 1 && entry.mMode == 1))) {
                code = au::err::kErrorBusy; break;
            }
            if (c->mAccessVersion > UINT64_MAX - 2 || c->mReaders == SIZE_MAX) { code = au::err::kErrorOverflow; break; }
            entry.mVersion = c->mAccessVersion; entry.mPrevious = c->mFence;
        }
        unlock();
        if (code) return code;
        for (auto& entry : mEntries) if (entry.mPrevious) {
            if (!au::mm::detail::accessMayWait() && !entry.mPrevious->ready()) return au::err::kErrorBusy;
            const auto status = entry.mPrevious->wait(); // No registry/resource lock or reservation.
            if (status.mCode) {
                mNativeError = status.mNativeCode; mCauseTaskId = status.mCauseTaskId; mDeviceError = status.mDeviceError;
                return status.mCode;
            }
        }
        bool changed = false;
        lock();
        for (const auto& entry : mEntries) {
            auto* c = entry.mControl;
            if (c->mReaders == SIZE_MAX || c->mAccessVersion != entry.mVersion || c->mFence != entry.mPrevious || c->mDevicePreparing ||
                (c->mCpuPhase != au::mm::detail::CpuPhase::Idle &&
                !(c->mCpuPhase == au::mm::detail::CpuPhase::Active && c->mCpuMode == 1 && entry.mMode == 1))) {
                changed = true; break;
            }
        }
        if (!changed) for (auto& entry : mEntries) {
            auto* c = entry.mControl;
            entry.mReserved = true;
            if (c->mCpuPhase == au::mm::detail::CpuPhase::Idle) {
                c->mCpuPhase = au::mm::detail::CpuPhase::Starting;
                c->mCpuMode = entry.mMode; c->mReaders = 1; ++c->mAccessVersion;
                c->mFence.reset(); // Snapshot pins the old fence until outside all locks.
            } else { ++c->mReaders; entry.mStarted = true; }
        }
        unlock();
        if (!changed) return 0;
    }
    return au::err::kErrorBusy;
}
au::mm::detail::CpuSession::CpuSession() noexcept : mOwner(&au::mm::Registry::instance()) {}
bool au::mm::detail::CpuSession::ownedHere() const noexcept { return mOwner == &au::mm::Registry::instance(); }
int au::mm::detail::CpuSession::begin(const ::au_mm_access_set* sets, std::size_t count) noexcept
{
    int code = resolve(sets, count);
    if (!code) code = reserve();
    if (code) { au::mm::detail::reportAccessError(code, mNativeError, mCauseTaskId, mDeviceError); return code; }
    mActive = true;
    for (auto& entry : mEntries) if (!entry.mStarted) {
        int native = 0;
        code = au::mm::detail::syncCpuAccess(entry.mControl, entry.mMode, true, &native);
        if (code) {
            au::mm::detail::reportAccessError(code, native);
            return finish(code); // Roll back STARTs and joined readers in reverse order.
        }
        entry.mStarted = true;
    }
    lock();
    for (auto& entry : mEntries) entry.mControl->mCpuPhase = au::mm::detail::CpuPhase::Active;
    unlock();
    return 0;
}
int au::mm::detail::CpuSession::finish(int bodyCode) noexcept
{
    if (!mActive) return bodyCode;
    mActive = false;
    int cleanup = 0;
    for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it) {
        auto& entry = *it;
        if (!entry.mReserved) continue;
        auto* c = entry.mControl;
        bool end = false;
        {
            std::lock_guard<std::mutex> lock(c->mAccessMutex);
            if (--c->mReaders == 0) {
                c->mCpuPhase = au::mm::detail::CpuPhase::Ending; end = true;
            }
        }
        if (end) {
            int native = 0;
            const int code = entry.mStarted ? au::mm::detail::syncCpuAccess(c, entry.mMode, false, &native) : 0;
            {
                std::lock_guard<std::mutex> lock(c->mAccessMutex);
                c->mCpuPhase = au::mm::detail::CpuPhase::Idle; c->mCpuMode = 0;
                ++c->mAccessVersion;
                if (code) { c->mAccessError = code; c->mAccessNativeError = native; }
            }
            if (code) { if (!cleanup) cleanup = code; au::mm::detail::reportAccessError(code, native); }
        }
        entry.mReserved = false;
    }
    const int released = releaseResources();
    return bodyCode ? bodyCode : cleanup ? cleanup : released;
}
int au::mm::detail::DeviceAccess::begin(const ::au_mm_access_set* sets, std::size_t count,
                                        std::uintptr_t context, bool nativeDependencies, bool hostAccess) noexcept
{
    if (mActive || mCompletion) return au::err::kErrorStateInvalid;
    int code = resolve(sets, count);
    if (code) return code;
    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        for (auto& entry : mEntries) entry.mPrevious.reset();
        lock();
        for (auto& entry : mEntries) {
            auto* c = entry.mControl;
            if (c->mAccessError) { code = c->mAccessError; mNativeError = c->mAccessNativeError; break; }
            if (c->mCpuPhase != au::mm::detail::CpuPhase::Idle || c->mDevicePreparing) { code = au::err::kErrorBusy; break; }
            if (c->mAccessVersion > UINT64_MAX - 2) { code = au::err::kErrorOverflow; break; }
            entry.mPrevious = c->mFence; entry.mVersion = c->mAccessVersion;
        }
        unlock();
        if (code) return code;
        for (const auto& entry : mEntries) if (entry.mPrevious &&
            (hostAccess || entry.mPrevious->mHostCompletion || entry.mPrevious->mContext != context ||
             (nativeDependencies && !entry.mPrevious->mNativeEvent))) {
            if (!au::mm::detail::accessMayWait() && !entry.mPrevious->ready()) return au::err::kErrorBusy;
            const auto status = entry.mPrevious->wait(); // Bridge before reserving ANY resource.
            if (status.mCode) {
                mNativeError = status.mNativeCode; mCauseTaskId = status.mCauseTaskId; mDeviceError = status.mDeviceError;
                au::mm::detail::reportAccessError(status.mCode, status.mNativeCode, status.mCauseTaskId, status.mDeviceError);
                return status.mCode;
            }
        }
        bool changed = false;
        lock();
        for (const auto& entry : mEntries) {
            auto* c = entry.mControl;
            if (c->mAccessVersion != entry.mVersion || c->mFence != entry.mPrevious || c->mDevicePreparing ||
                c->mCpuPhase != au::mm::detail::CpuPhase::Idle) { changed = true; break; }
        }
        if (!changed) for (auto& entry : mEntries) {
            entry.mControl->mDevicePreparing = true; ++entry.mControl->mAccessVersion;
        }
        unlock();
        if (!changed) { mActive = true; return 0; }
    }
    return au::err::kErrorBusy;
}
int au::mm::detail::DeviceAccess::commit(const std::shared_ptr<au::mm::detail::DeviceFence>& fence) noexcept
{
    if (!mActive || !fence || !fence->mWait) return au::err::kErrorStateInvalid;
    lock();
    for (auto& entry : mEntries) {
        entry.mControl->mFence = fence; // mPrevious retains any replaced native record.
        entry.mControl->mDevicePreparing = false; ++entry.mControl->mAccessVersion;
    }
    unlock();
    mCompletion = fence;
    mActive = false;
    return 0;
}
void au::mm::detail::DeviceAccess::abort() noexcept
{
    if (!mActive) return;
    lock();
    for (auto& entry : mEntries) { entry.mControl->mDevicePreparing = false; ++entry.mControl->mAccessVersion; }
    unlock();
    mActive = false;
}
int au::mm::detail::DeviceAccess::finish() noexcept
{
    abort();
    const auto status = mCompletion ? mCompletion->wait() : au::mm::detail::FenceStatus{};
    return releaseCompleted(status);
}
int au::mm::detail::DeviceAccess::releaseCompleted(const au::mm::detail::FenceStatus& status) noexcept
{
    abort();
    mNativeError = status.mNativeCode;
    const int cleanup = releaseResources();
    mCompletion.reset();
    return status.mCode ? status.mCode : cleanup;
}

int au::mm::detail::legacySync(void* pointer, bool start) noexcept
{
    au::mm::detail::MemoryLease lease;
    int code = au::mm::Registry::instance().acquire(pointer, 1, lease);
    if (code) return code;
    auto* c = lease.control();
    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        std::shared_ptr<au::mm::detail::DeviceFence> fence;
        std::uint64_t version;
        {
            std::lock_guard<std::mutex> lock(c->mAccessMutex);
            if (c->mCpuPhase != au::mm::detail::CpuPhase::Idle || c->mDevicePreparing) return au::err::kErrorBusy;
            if (c->mAccessError) return c->mAccessError;
            if (c->mAccessVersion > UINT64_MAX - 2) return au::err::kErrorOverflow;
            fence = c->mFence; version = c->mAccessVersion;
        }
        if (fence) {
            if (!au::mm::detail::accessMayWait() && !fence->ready()) return au::err::kErrorBusy;
            const auto status = fence->wait();
            if (status.mCode) {
                au::mm::detail::reportAccessError(status.mCode, status.mNativeCode, status.mCauseTaskId, status.mDeviceError);
                return status.mCode;
            }
        }
        {
            std::lock_guard<std::mutex> lock(c->mAccessMutex);
            if (c->mAccessVersion != version || c->mCpuPhase != au::mm::detail::CpuPhase::Idle || c->mDevicePreparing)
                continue;
            c->mDevicePreparing = true; ++c->mAccessVersion;
        }
        int native = 0;
        code = au::mm::detail::syncCpuAccess(c, 3, start, &native);
        {
            std::lock_guard<std::mutex> lock(c->mAccessMutex);
            c->mDevicePreparing = false; ++c->mAccessVersion;
        }
        au::mm::detail::reportAccessError(code, native);
        return code;
    }
    return au::err::kErrorBusy;
}

extern "C" int au_mm_cpu_begin(const ::au_mm_access_set* sets, std::size_t count, void** out) noexcept
{
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    try {
        std::unique_ptr<au::mm::detail::CpuSession> session(new au::mm::detail::CpuSession());
        const int code = session->begin(sets, count);
        if (code) return code;
        *out = session.release(); return 0;
    } catch (...) { au::mm::detail::reportAccessError(au::err::kErrorNoMemory); return au::err::kErrorNoMemory; }
}
extern "C" int au_mm_cpu_finish(void* scope, int bodyCode) noexcept
{
    if (!scope) return bodyCode ? bodyCode : au::err::kErrorNotInitialized;
    if (!static_cast<au::mm::detail::CpuSession*>(scope)->ownedHere()) return au::err::kErrorInvalidHandle;
    std::unique_ptr<au::mm::detail::CpuSession> session(static_cast<au::mm::detail::CpuSession*>(scope));
    return session->finish(bodyCode);
}
extern "C" std::uint32_t au_mm_access_version(void) noexcept { return AURA_MM_ACCESS_ABI_VERSION; }
