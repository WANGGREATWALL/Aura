#ifndef AURA_FLOW_EXECUTION_CONTEXT_H_
#define AURA_FLOW_EXECUTION_CONTEXT_H_

#include "flow/completion.h"
#include "flow/node.h"
#include "mm/access_context.h"

namespace au {
namespace flow AU_LOCAL {
namespace detail {
// Private fixed integration bridge; mm lifetime and error routing are kept
// here rather than exposed as a configurable scheduler plugin interface.
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
    bool   mCpuActive = false;  // Logical callable slice, suspended at known scheduler/device waits.
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
extern thread_local unsigned int gTileDepth;
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
    TileSuspension() noexcept : mContext(gTileContext), mDepth(gTileDepth)
    {
        gTileContext = nullptr;
        gTileDepth   = 0;
    }
    ~TileSuspension() noexcept
    {
        gTileContext = mContext;
        gTileDepth   = mDepth;
    }
    TileSuspension(const TileSuspension&)            = delete;
    TileSuspension& operator=(const TileSuspension&) = delete;

private:
    TileContext* mContext;
    unsigned int mDepth;
};

class TileExtent
{
public:
    TileExtent(TileContext* context) noexcept : mPrevious(gTileContext), mDepth(gTileDepth), mErrors(&core())
    {
        gTileContext = context;
        ++gTileDepth;
    }
    ~TileExtent() noexcept
    {
        mErrors.restore();
        gTileContext = mPrevious;
        gTileDepth   = mDepth;
    }
    TileExtent(const TileExtent&)            = delete;
    TileExtent& operator=(const TileExtent&) = delete;

private:
    TileContext*    mPrevious;
    unsigned int    mDepth;
    ExecutionErrors mErrors;
};

}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au

#endif  // AURA_FLOW_EXECUTION_CONTEXT_H_
