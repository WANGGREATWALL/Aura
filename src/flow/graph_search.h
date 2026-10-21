#ifndef AURA_FLOW_GRAPH_SEARCH_H_
#define AURA_FLOW_GRAPH_SEARCH_H_
#include <chrono>

#include "flow/execution_context.h"
#include "flow/node.h"
namespace au {
namespace flow AU_LOCAL {
namespace detail {
// All scopes below require Core::mMutex. Search scratch is borrowed, restored
// on every exit and never owns Nodes. Only one search may run under that lock.
class GraphTargets
{
public:
    GraphTargets(Node* target, bool ancestors, Frame* frames = nullptr) noexcept;
    ~GraphTargets() noexcept;
    GraphTargets(const GraphTargets&)            = delete;
    GraphTargets& operator=(const GraphTargets&) = delete;

private:
    void   mark(bool value) noexcept;
    Node*  mTarget;
    bool   mAncestors;
    Frame* mFrames;
};
class GraphSearch
{
public:
    explicit GraphSearch(bool completion) noexcept;
    ~GraphSearch() noexcept;
    void add(Node* node) noexcept;
    bool reachesTarget() noexcept;
    GraphSearch(const GraphSearch&)            = delete;
    GraphSearch& operator=(const GraphSearch&) = delete;

private:
    Node* mHead = nullptr;
    Node* mTail = nullptr;
    bool  mCompletion;
#ifdef AURA_FLOW_TESTING
    std::chrono::steady_clock::time_point mStarted;
    std::uint64_t                         mNodes = 0;
    std::uint64_t                         mEdges = 0;
#endif
};
void linkChild(Node* node) noexcept;
void unlinkChild(Node* node) noexcept;
}  // namespace detail
}  // namespace flow AU_LOCAL
}  // namespace au
#endif
