#ifndef AURA_MM_REGISTRY_H_
#define AURA_MM_REGISTRY_H_

#include <mutex>
#include <unordered_map>

#include "mm/xmemory.h"

namespace au {
namespace mm {

/// Process-wide ptr -> MemBlock table. Single mutex, allocations are
/// coarse-grained (image/model buffers, not malloc spam).
class Registry
{
public:
    static Registry& instance() noexcept;

    int insert(const MemBlock& block) noexcept;
    int find(void* ptr, MemBlock& out) const noexcept;
    int take(void* ptr, MemBlock& out) noexcept;

private:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    mutable std::mutex mMutex;
    std::unordered_map<void*, MemBlock> mTable;
};

}  // namespace mm
}  // namespace au

#endif  // AURA_MM_REGISTRY_H_
