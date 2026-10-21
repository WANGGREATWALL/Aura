#ifndef AURA_TEST_FLOW_REFERENCE_ALLOCATORS_H_
#define AURA_TEST_FLOW_REFERENCE_ALLOCATORS_H_
#include <cstddef>
#include <new>
#include <utility>
namespace au {
namespace flow {
namespace detail {
// Experimental direct allocation. Destructors still run outside allocator locks.
template <class T, std::size_t Count>
class SystemRecords final
{
public:
    SystemRecords() noexcept                       = default;
    SystemRecords(const SystemRecords&)            = delete;
    SystemRecords& operator=(const SystemRecords&) = delete;
    template <class... A>
    T* create(A&&... args)
    {
        return new T(std::forward<A>(args)...);
    }
    void destroy(T* value) noexcept { delete value; }
};
class SystemPayloadAllocator
{
public:
    void* allocate(std::size_t size, std::size_t alignment)
    {
        return alignment > alignof(std::max_align_t) ? ::operator new(size, std::align_val_t(alignment))
                                                     : ::operator new(size);
    }
    void release(void* pointer, std::size_t, std::size_t alignment) noexcept
    {
        if (alignment > alignof(std::max_align_t))
            ::operator delete(pointer, std::align_val_t(alignment));
        else
            ::operator delete(pointer);
    }
};
}  // namespace detail
}  // namespace flow
}  // namespace au
#endif
