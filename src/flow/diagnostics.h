#ifndef AURA_FLOW_DIAGNOSTICS_H_
#define AURA_FLOW_DIAGNOSTICS_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "flow/completion.h"
#include "flow/xflow_api.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {

// Value snapshots contain no scheduler pointers or locks. Export cannot keep
// tasks alive, inspect mutable execution state, or run under the graph mutex.
struct GraphNodeSnapshot
{
    std::uint64_t              mId = 0, mCause = 0;
    std::string                mName;
    au::flow::TaskState        mState = au::flow::TaskState::Invalid;
    int                        mCode  = 0;
    bool                       mGroup = false;
    std::vector<std::uint64_t> mPreds, mMembers, mChildren;
};
struct GraphSnapshot
{
    std::vector<au::flow::detail::GraphNodeSnapshot> mNodes;
    std::unordered_map<std::uint64_t, std::size_t>   mIndex;
    std::size_t                                      mDropped  = 0;
    bool                                             mCaptured = false;
};
int writeDot(const au::flow::detail::GraphSnapshot& graph, const char* path) noexcept;

// Shared value history for one captured scope. No Node/Closure/resource owners.
// A prepared batch is published without allocation before its work can execute.
class GraphHistory final
{
public:
    GraphHistory() noexcept                      = default;
    GraphHistory(const GraphHistory&)            = delete;
    GraphHistory& operator=(const GraphHistory&) = delete;
    GraphHistory(GraphHistory&&)                 = delete;
    GraphHistory& operator=(GraphHistory&&)      = delete;
    struct Batch
    {
        std::vector<GraphNodeSnapshot> mNodes;
        std::uint64_t                  mParent = 0, mTop = 0;
        std::unique_ptr<Batch>         mNext;
    };
    ~GraphHistory() noexcept;
    void          publish(std::unique_ptr<Batch> batch) noexcept;
    void          state(GraphNodeSnapshot* node, au::flow::TaskState state) noexcept;
    void          complete(GraphNodeSnapshot* node, const au_flow_result& result) noexcept;
    bool          contains(std::uint64_t id) const noexcept;
    GraphSnapshot snapshot() const;
    void          dropped() noexcept { mDropped.fetch_add(1, std::memory_order_relaxed); }

private:
    mutable std::mutex       mMutex;
    std::unique_ptr<Batch>   mBatches;
    std::atomic<std::size_t> mDropped{0};
};
struct CapturedNode
{
    // shared_ptr is diagnostic-only, not a scheduler/Node lifetime reference.
    std::shared_ptr<GraphHistory> mHistory;
    GraphNodeSnapshot*            mRecord = nullptr;  // Borrowed from a stable published batch.
};

class Diagnostics final
{
public:
    Diagnostics() noexcept;
    ~Diagnostics() noexcept;
    Diagnostics(const Diagnostics&)            = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;
    Diagnostics(Diagnostics&&)                 = delete;
    Diagnostics& operator=(Diagnostics&&)      = delete;
    bool         enabled() const noexcept { return mEnabled.load(std::memory_order_relaxed); }
    int          setEnabled(bool enabled) noexcept;
    int          reset() noexcept;
    std::size_t  retainedBytes() const noexcept;
    void         record(std::size_t buffer, std::uint64_t task, std::uint64_t thread, const char* name, char phase,
                        int code = 0, std::uint64_t parent = 0, const char* label = nullptr, std::size_t labelSize = 0,
                        const au::flow::detail::ExternalProfile* profile = nullptr) noexcept;
    int          dumpTrace(const char* path) const noexcept;

private:
    struct Storage;
    std::atomic<bool> mEnabled{false};
    std::mutex        mMutex;
    // Atomic shared_ptr operations pin storage across reset/export/record races.
    // Only opt-in tracing pays this pin; the disabled path reads mEnabled only.
    std::shared_ptr<Storage> mStorage;
};

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
#endif
