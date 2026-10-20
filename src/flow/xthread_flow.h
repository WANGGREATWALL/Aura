#ifndef AURA_FLOW_XTHREAD_FLOW_H_
#define AURA_FLOW_XTHREAD_FLOW_H_

/**
 * @file xthread_flow.h
 * @brief Parallel execution flow: tiled task parallelism and pipeline execution.
 *
 * @example
 *   auto& flow = au::flow::XFlow::get();
 *   flow.init(4, 2);  // 4 workers, 2 pipelines
 *
 *   // Parallel tiled loop
 *   flow.parallelizeTiledTasks(100, 10, [](size_t i, size_t tile) {
 *       // process items [i, i+tile)
 *   });
 */

#include <memory>

#include "xthreadpool.h"

namespace au {
namespace flow {

class XFlow
{
public:
    static XFlow& get()
    {
        static XFlow instance;
        return instance;
    }

    ~XFlow();

    XFlow(const XFlow&)            = delete;
    XFlow& operator=(const XFlow&) = delete;

    int init(size_t workers, size_t pipelines);

    int parallelizeTiledTasks(size_t range, size_t tile, std::function<void(size_t, size_t)>&& f);

    template <class F, class... Args>
    auto addPipeline(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>>;

private:
    XFlow();

    bool                         mInited = false;
    std::unique_ptr<XThreadpool> mWorkers;
    std::unique_ptr<XThreadpool> mPipelines;
};

}  // namespace flow
}  // namespace au

#include "xthread_flow.impl.h"

#endif  // AURA_FLOW_XTHREAD_FLOW_H_
