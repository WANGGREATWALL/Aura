#include "mm/registry.h"

#include <limits>
#include "log/xerror.h"

au::mm::Registry& au::mm::Registry::instance() noexcept
{
    // No heap allocation is needed to obtain the registry itself. SDK owners
    // drain native work and release handles before shared-object unload.
    static au::mm::Registry instance;
    return instance;
}

int au::mm::Registry::insert(const au::mm::MemBlock& block) noexcept
{
    if (!block.ptr) return au::err::kErrorNullPointer;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(block.ptr);
    if (!block.size || block.size > std::numeric_limits<std::uintptr_t>::max() - begin)
        return au::err::kErrorInvalidSize;
    std::uint64_t id = mNextId.load(std::memory_order_relaxed); // Identity only, not storage publication.
    do {
        if (id == UINT64_MAX) return au::err::kErrorOverflow;
    } while (!mNextId.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
    try {
        const au::mm::detail::MemoryIdentity identity{reinterpret_cast<std::uintptr_t>(this), id, id};
        // Shared across owner retirement and asynchronous leases.
        std::shared_ptr<au::mm::detail::ReleaseStatus> report = std::make_shared<au::mm::detail::ReleaseStatus>();
        std::shared_ptr<au::mm::detail::MemoryControl> control =
            std::make_shared<au::mm::detail::MemoryControl>(block, identity, std::move(report));
        decltype(mTable) prepared;
        prepared.emplace(begin, control); // Prepare map storage before the publication lock.
        auto entry = prepared.extract(prepared.begin());
        {
            std::lock_guard<std::mutex> lock(mMutex);
            auto after = mTable.lower_bound(begin);
            if (after != mTable.end() && after->first - begin < block.size) return au::err::kErrorAlreadyExists;
            if (after != mTable.begin()) {
                auto before = after; --before;
                if (begin - before->first < before->second->mBlock.size) return au::err::kErrorAlreadyExists;
            }
            auto inserted = mTable.insert(std::move(entry));
            if (!inserted.inserted) return au::err::kErrorAlreadyExists;
            control->mOwnsStorage = true; // Last step before publication through mutex unlock.
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
    out = found->second->mBlock;
    return 0;
}

int au::mm::Registry::acquire(const void* ptr, std::size_t size,
                             au::mm::detail::MemoryLease& out) const noexcept
{
    if (!ptr) return au::err::kErrorNullPointer;
    if (!size) return au::err::kErrorInvalidSize;
    std::shared_ptr<au::mm::detail::MemoryControl> control;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
        auto after = mTable.upper_bound(address);
        if (after == mTable.begin()) return au::err::kErrorInvalidAddr;
        --after;
        const std::size_t offset = address - after->first;
        if (offset >= after->second->mBlock.size) return au::err::kErrorInvalidAddr;
        if (size > after->second->mBlock.size - offset) return au::err::kErrorOutOfRange;
        control = after->second;
    }
    out = au::mm::detail::MemoryLease(std::move(control)); // Old lease cleanup outside registry lock.
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
        removed = mTable.extract(found); // Retirement is the free() linearization point.
    }
    std::shared_ptr<au::mm::detail::ReleaseStatus> report = removed.mapped()->mReport;
    removed.mapped().reset(); // Last asynchronous lease may release backing later.
    return report->mDone.load(std::memory_order_acquire) ? report->mCode : 0;
}
