#ifndef AURA_FLOW_SLAB_H_
#define AURA_FLOW_SLAB_H_

#include <array>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace au {
namespace flow {
namespace detail {

// Stable-address pages. Growth is a creation/submission slow path. Reclamation
// never allocates; callers destroy user payloads before returning the record.
template <class T, std::size_t Count> class Slab {
    struct Slot {
        alignas(T) unsigned char mStorage[sizeof(T)];
        Slot* mNext;
    };
    struct Page {
        std::array<Slot, Count> mSlots;
        std::unique_ptr<Page> mNext;
    };
public:
    template <class... A> T* create(A&&... args)
    {
        Slot* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mFree) { slot = mFree; mFree = slot->mNext; }
        }
        if (!slot) {
            std::unique_ptr<Page> page(new Page()); // Cold growth outside the pool lock.
            for (std::size_t i = 0; i < Count; ++i)
                page->mSlots[i].mNext = i + 1 < Count ? &page->mSlots[i + 1] : nullptr;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mFree) {
                    mFree = &page->mSlots[0];
                    page->mNext = std::move(mPages); mPages = std::move(page);
                }
                slot = mFree; mFree = slot->mNext;
            }
        }
        try { return new (slot->mStorage) T(std::forward<A>(args)...); }
        catch (...) { recycle(slot); throw; }
    }
    void destroy(T* value) noexcept
    {
        value->~T();
        recycle(reinterpret_cast<Slot*>(value));
    }
    ~Slab() noexcept
    {
        // Avoid recursive unique_ptr destruction when a stress run grew many pages.
        while (mPages) { std::unique_ptr<Page> next = std::move(mPages->mNext); mPages = std::move(next); }
    }
private:
    void recycle(Slot* slot) noexcept
    { std::lock_guard<std::mutex> lock(mMutex); slot->mNext = mFree; mFree = slot; }
    std::mutex mMutex;
    std::unique_ptr<Page> mPages;
    Slot* mFree = nullptr;
};

} // namespace detail
} // namespace flow
} // namespace au
#endif
