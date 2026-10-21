#ifndef AURA_FLOW_PAYLOAD_POOL_H_
#define AURA_FLOW_PAYLOAD_POOL_H_
#include <cstddef>
#include <new>

#include "flow/slab.h"

namespace au {
namespace flow {
namespace detail {
template <std::size_t Size>
struct PayloadBlock
{
    alignas(std::max_align_t) unsigned char mData[Size];
    PayloadBlock() noexcept {}  // No per-reuse clearing; placement construction initializes the callable.
};
// Retain only the small hot callable class. Larger captures use exact-size
// allocation instead of keeping three padded, high-water slab classes alive.
class PayloadPool final
{
public:
    PayloadPool() noexcept                                                         = default;
    PayloadPool(const au::flow::detail::PayloadPool&)                              = delete;
    au::flow::detail::PayloadPool& operator=(const au::flow::detail::PayloadPool&) = delete;
    PayloadPool(au::flow::detail::PayloadPool&&)                                   = delete;
    au::flow::detail::PayloadPool& operator=(au::flow::detail::PayloadPool&&)      = delete;
    void*                          allocate(std::size_t size, std::size_t alignment)
    {
        if (alignment > alignof(std::max_align_t))
            return ::operator new(size, std::align_val_t(alignment));
        if (size <= 64)
            return mSmall.create();
        return ::operator new(size);  // Elastic cold path, no business-size rejection.
    }
    void release(void* pointer, std::size_t size, std::size_t alignment) noexcept
    {
        if (!pointer)
            return;
        if (alignment > alignof(std::max_align_t))
            ::operator delete(pointer, std::align_val_t(alignment));
        else if (size <= 64)
            mSmall.destroy(static_cast<au::flow::detail::PayloadBlock<64>*>(pointer));
        else
            ::operator delete(pointer);
    }

private:
    au::flow::detail::Slab<au::flow::detail::PayloadBlock<64>, 64> mSmall;
};
}  // namespace detail
}  // namespace flow
}  // namespace au
#endif
