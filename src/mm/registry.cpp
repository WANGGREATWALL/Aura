#include "mm/registry.h"

#include <limits>
#include "log/xerror.h"
#include "mm/native_backend.h"
#include "mm/pool_backend.h"

namespace {
struct Registration {
    au::mm::Registry* mOwner;
    std::uintptr_t mBase;
    std::uint64_t mId;
};
}

au::mm::Registry& au::mm::Registry::instance() noexcept
{
    // Backends/sys allocator state outlive registry-owned controls, including
    // deferred storage cleanup during normal module destruction.
    (void)au::mm::nativeBackend();
    (void)au::mm::poolBackend();
    static au::mm::Registry instance;
    return instance;
}
int au::mm::Registry::nextId(std::uint64_t& out) noexcept
{
    std::uint64_t id = mNextId.load(std::memory_order_relaxed); // Identity only, not publication.
    do {
        if (id == UINT64_MAX) return au::err::kErrorOverflow;
    } while (!mNextId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
    out = id;
    return 0;
}
bool au::mm::Registry::overlaps(std::uintptr_t begin, std::size_t size) const noexcept
{
    auto after = mTable.lower_bound(begin);
    if (after != mTable.end() && after->first - begin < size) return true;
    if (after == mTable.begin()) return false;
    --after;
    return begin - after->first < after->second.mSize;
}
int au::mm::Registry::insert(const au::mm::MemBlock& block) noexcept
{
    if (!block.ptr) return au::err::kErrorNullPointer;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(block.ptr);
    if (!block.size || block.size > std::numeric_limits<std::uintptr_t>::max() - begin)
        return au::err::kErrorInvalidSize;
    std::uint64_t id;
    const int code = nextId(id);
    if (code) return code;
    try {
        const au::mm::detail::MemoryIdentity identity{reinterpret_cast<std::uintptr_t>(this), id, id};
        // Shared across owner retirement, CPU access and asynchronous device work.
        auto report = std::make_shared<au::mm::detail::ReleaseStatus>();
        auto control = std::make_shared<au::mm::detail::MemoryControl>(block, identity, std::move(report));
        if (block.fd >= 0) {
            int duplicate = -1;
            const int fdCode = au::sys::memory::duplicateFd(block.fd, block.size, duplicate, control->mFdIdentity);
            if (fdCode) return fdCode;
            au::sys::memory::closeFd(duplicate);
            control->mHasFdIdentity = true;
        }
        decltype(mTable) prepared;
        prepared.emplace(begin, au::mm::Registry::View{control, nullptr, block.size, 0});
        auto entry = prepared.extract(prepared.begin());
        decltype(mFds) preparedFd;
        if (control->mHasFdIdentity) preparedFd.emplace(control->mFdIdentity, control);
        auto fdEntry = preparedFd.empty() ? decltype(mFds)::node_type{} : preparedFd.extract(preparedFd.begin());
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (overlaps(begin, block.size)) return au::err::kErrorAlreadyExists;
            if (control->mHasFdIdentity) {
                auto previous = mFds.find(control->mFdIdentity);
                if (previous != mFds.end()) {
                    if (!previous->second.expired()) return au::err::kErrorAlreadyExists;
                    previous->second = control;
                } else mFds.insert(std::move(fdEntry));
            }
            mTable.insert(std::move(entry)); // Prepared node: no allocation under lock.
            control->mOwnsStorage = true;
        }
        return 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
int au::mm::Registry::find(void* ptr, au::mm::MemBlock& out) const noexcept
{
    if (!ptr) return au::err::kErrorNullPointer;
    std::lock_guard<std::mutex> lock(mMutex);
    auto found = mTable.find(reinterpret_cast<std::uintptr_t>(ptr));
    if (found == mTable.end()) return au::err::kErrorInvalidAddr;
    out = found->second.mControl->mBlock;
    out.ptr = ptr; out.size = found->second.mSize;
    return 0;
}
int au::mm::Registry::acquire(const void* ptr, std::size_t size,
                             au::mm::detail::MemoryLease& out) const noexcept
{
    if (!ptr) return au::err::kErrorNullPointer;
    if (!size) return au::err::kErrorInvalidSize;
    au::mm::detail::MemoryLease lease;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
        auto after = mTable.upper_bound(address);
        if (after == mTable.begin()) return au::err::kErrorInvalidAddr;
        --after;
        const std::size_t offset = address - after->first;
        if (offset >= after->second.mSize) return au::err::kErrorInvalidAddr;
        if (size > after->second.mSize - offset) return au::err::kErrorOutOfRange;
        lease.mControl = after->second.mControl;
        lease.mViewOwner = after->second.mOwner;
        lease.mOffset = offset; lease.mSize = size;
    }
    out = std::move(lease); // Old leases, including user callbacks, release outside registry lock.
    return 0;
}
int au::mm::Registry::retire(void* ptr) noexcept
{
    if (!ptr) return au::err::kErrorNullPointer;
    decltype(mTable)::node_type removed;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto found = mTable.find(reinterpret_cast<std::uintptr_t>(ptr));
        if (found == mTable.end()) return au::err::kErrorInvalidAddr;
        if (found->second.mRegistration) return au::err::kErrorStateInvalid;
        removed = mTable.extract(found);
    }
    auto report = removed.mapped().mControl->mReport;
    removed.mapped().mControl.reset();
    return report->mDone.load(std::memory_order_acquire) ? report->mCode : 0;
}
int au::mm::Registry::registerExternal(const ::au_mm_external_memory& memory, void** out) noexcept
{
    if (!out) return au::err::kErrorNullPointer;
    *out = nullptr;
    if (memory.mStructSize != sizeof(memory)) return au::err::kErrorVersionMismatch;
    if (!memory.mData) return au::err::kErrorNullPointer;
    const auto begin = reinterpret_cast<std::uintptr_t>(memory.mData);
    if (!memory.mSize || memory.mSize > UINTPTR_MAX - begin) return au::err::kErrorInvalidSize;
    if (memory.mType > 2 || ((memory.mType == 0) != (memory.mFd == -1)) ||
        ((memory.mRetain == nullptr) != (memory.mRelease == nullptr))) return au::err::kErrorInvalidParam;
    std::uint64_t id;
    int code = nextId(id);
    if (code) return code;
    int owned = -1;
    au::sys::memory::FdIdentity fdIdentity;
    if (memory.mFd >= 0) {
        code = au::sys::memory::duplicateFd(memory.mFd, memory.mSize, owned, fdIdentity);
        if (code) return code;
    }
    // Keep all RAII owners outside the publication lock, even on rollback.
    struct FdOwner {
        int mFd;
        ~FdOwner() noexcept { au::sys::memory::closeFd(mFd); }
    } fdOwner{owned};
    try {
        auto owner = std::make_shared<au::mm::detail::ExternalOwner>();
        if (memory.mRetain) {
            try { code = memory.mRetain(memory.mOwner); }
            catch (...) { code = au::err::kErrorStateInvalid; }
            if (code) return code;
            owner->mPayload = memory.mOwner; owner->mRelease = memory.mRelease;
        }
        const au::mm::MemBlock block{memory.mData, memory.mSize, owned,
            static_cast<au::mm::MemType>(memory.mType), au::mm::BackendId::Native};
        const au::mm::detail::MemoryIdentity identity{reinterpret_cast<std::uintptr_t>(this), id, id};
        auto report = std::make_shared<au::mm::detail::ReleaseStatus>();
        auto control = std::make_shared<au::mm::detail::MemoryControl>(block, identity, std::move(report));
        control->mOwnedFd = owned; fdOwner.mFd = -1;
        control->mExternalOwner = owner;
        control->mFdIdentity = fdIdentity; control->mHasFdIdentity = owned >= 0;
        std::unique_ptr<Registration> registration(new Registration{this, begin, id});
        decltype(mTable) prepared;
        prepared.emplace(begin, au::mm::Registry::View{control, owner, memory.mSize, id});
        auto entry = prepared.extract(prepared.begin());
        decltype(mFds) preparedFd;
        preparedFd.emplace(fdIdentity, control);
        auto fdEntry = preparedFd.extract(preparedFd.begin());
        std::shared_ptr<au::mm::detail::MemoryControl> canonical; // Destruction outside mMutex on all paths.
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (overlaps(begin, memory.mSize)) return au::err::kErrorAlreadyExists;
            // Expired weak records carry no native resources. Bound metadata
            // growth across repeated registration of distinct allocations.
            for (auto it = mFds.begin(); it != mFds.end();) {
                if (it->second.expired()) it = mFds.erase(it); else ++it;
            }
            if (owned >= 0) {
                auto previous = mFds.find(fdIdentity);
                if (previous != mFds.end()) {
                    canonical = previous->second.lock();
                    if (canonical) {
                        if (canonical->mBlock.size != memory.mSize || canonical->mBlock.type != block.type)
                            return au::err::kErrorSizeMismatch;
                        entry.mapped().mControl = std::move(canonical);
                    } else previous->second = control;
                } else mFds.insert(std::move(fdEntry));
            }
            mTable.insert(std::move(entry));
        }
        *out = registration.release();
        return 0;
    } catch (...) { return au::err::kErrorNoMemory; }
}
int au::mm::Registry::unregisterExternal(void* handle) noexcept
{
    if (!handle) return au::err::kErrorInvalidHandle;
    auto* registration = static_cast<Registration*>(handle);
    if (registration->mOwner != this) return au::err::kErrorInvalidHandle;
    std::unique_ptr<Registration> owner(registration);
    decltype(mTable)::node_type removed;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto found = mTable.find(registration->mBase);
        if (found == mTable.end() || found->second.mRegistration != registration->mId)
            return au::err::kErrorInvalidHandle;
        removed = mTable.extract(found);
    }
    return 0; // removed (and possible user release) destructs outside the lock.
}

extern "C" int au_mm_register(const ::au_mm_external_memory* memory, void** out) noexcept
{
    if (!memory) return au::err::kErrorNullPointer;
    return au::mm::Registry::instance().registerExternal(*memory, out);
}
extern "C" int au_mm_unregister(void* handle) noexcept
{ return au::mm::Registry::instance().unregisterExternal(handle); }
