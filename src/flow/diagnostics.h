#ifndef AURA_FLOW_DIAGNOSTICS_H_
#define AURA_FLOW_DIAGNOSTICS_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "flow/completion.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {

// Value snapshots contain no scheduler pointers or locks. Export cannot keep
// tasks alive, inspect mutable execution state, or run under the graph mutex.
struct GraphNodeSnapshot {
    std::uint64_t mId = 0, mCause = 0;
    std::string mName;
    au::flow::TaskState mState = au::flow::TaskState::Invalid;
    int mCode = 0;
    bool mGroup = false;
    std::vector<std::uint64_t> mPreds, mMembers, mChildren;
};
struct GraphSnapshot {
    std::vector<au::flow::detail::GraphNodeSnapshot> mNodes;
    std::unordered_map<std::uint64_t, std::size_t> mIndex;
    std::size_t mDropped = 0;
    bool mCaptured = false;
};
int writeDot(const au::flow::detail::GraphSnapshot& graph, const char* path) noexcept;

class Diagnostics final {
public:
    Diagnostics() noexcept;
    ~Diagnostics() noexcept;
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;
    Diagnostics(Diagnostics&&) = delete;
    Diagnostics& operator=(Diagnostics&&) = delete;
    bool enabled() const noexcept { return mEnabled.load(std::memory_order_relaxed); }
    int setEnabled(bool enabled) noexcept;
    void record(std::size_t buffer, std::uint64_t task, std::uint64_t thread, const char* name,
                char phase, int code = 0, std::uint64_t parent = 0, const char* label = nullptr,
                std::size_t labelSize = 0, const au::flow::detail::ExternalProfile* profile = nullptr) noexcept;
    int dumpTrace(const char* path) const noexcept;
private:
    struct Storage;
    std::atomic<bool> mEnabled{false};
    std::mutex mMutex;
    std::unique_ptr<Storage> mOwner;
    std::atomic<Storage*> mStorage{nullptr}; // Stable until joined scheduler destruction.
};

} // namespace detail
} // namespace flow
} // namespace au
#endif
