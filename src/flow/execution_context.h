#ifndef AURA_FLOW_EXECUTION_CONTEXT_H_
#define AURA_FLOW_EXECUTION_CONTEXT_H_

#include <atomic>
#include <utility>

#include "flow/completion.h"
#include "mm/access_context.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
class Core;
struct Node;
Core& core() noexcept;
void  initializeMemoryContext() noexcept;
class ExecutionErrors
{
public:
    explicit ExecutionErrors(Core* core, bool mayWait = true) noexcept;
    ~ExecutionErrors() noexcept;
    void restore() noexcept;
    ExecutionErrors(const ExecutionErrors&)            = delete;
    ExecutionErrors& operator=(const ExecutionErrors&) = delete;

private:
    au::mm::detail::AccessErrorSink mPrevious;
    bool                            mInstalled = true;
};
struct Frame
{
    Node*  mNode;
    Frame* mPrevious;
};
// One invocation, owned by the thread currently executing a tile. The loop
// gathers these records only after every participant has joined.
struct TileContext
{
    ExternalStatus     mError;
    std::atomic<bool>* mStop;
    bool               mException = false;
    void               record(int code, au::flow::ErrorDomain domain, int native, std::uint64_t cause) noexcept
    {
        if (!code)
            return;
        if (!mError.mCode)
            mError = {code, native, domain, cause};
        mStop->store(true, std::memory_order_release);  // Stop new tickets, not callbacks already in flight.
    }
};
// Definitions live in execution_context.cpp, never GNU-unique public inline TLS.
extern thread_local Frame*       gFrame;
extern thread_local int          gWorker;
extern thread_local unsigned int gDepth;
extern thread_local TileContext* gTileContext;
extern thread_local unsigned int gCleanupDepth;
extern thread_local Node*        gFinish;
extern thread_local bool         gFinishing;
extern thread_local Node*        gReclaim;
extern thread_local bool         gReclaiming;
extern thread_local unsigned int gCompletionBurst;

// Cooperative work and cleanup belong to their own task, never to a suspended
// tile on this thread. Restore after their entire dispatch/cleanup has ended.
class TileSuspension
{
public:
    TileSuspension() noexcept : mContext(std::exchange(gTileContext, nullptr)) {}
    ~TileSuspension() noexcept { gTileContext = mContext; }
    TileSuspension(const TileSuspension&)            = delete;
    TileSuspension& operator=(const TileSuspension&) = delete;

private:
    TileContext* mContext;
};

class TileExtent
{
public:
    TileExtent(TileContext* context) noexcept : mPrevious(gTileContext), mErrors(&core()) { gTileContext = context; }
    ~TileExtent() noexcept
    {
        mErrors.restore();
        gTileContext = mPrevious;
    }
    TileExtent(const TileExtent&)            = delete;
    TileExtent& operator=(const TileExtent&) = delete;

private:
    TileContext*    mPrevious;
    ExecutionErrors mErrors;
};

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#endif  // AURA_FLOW_EXECUTION_CONTEXT_H_
