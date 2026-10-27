#ifndef AURA_XFLOW_H_
#define AURA_XFLOW_H_

/**
 * @file xflow.h
 * @brief Lightweight control-flow scheduler for algorithm libraries, with a C++11 facade over a C ABI.
 *
 * @par Purpose
 * Run tasks asynchronously while the caller continues working, express fork/join dependencies,
 * or run a synchronous parallel loop over an index range. Scheduling is independent of the
 * memory and GPU modules; flow tracks control flow only, not device work.
 *
 * @par Task model
 * Tasks return int (0 means success) or void. Failed predecessors skip dependents and propagate an
 * upstream error; a multi-input join does not guarantee which error wins. enqueue() collects and
 * accepts all unsubmitted upstream tasks atomically. A task completes when its body returns or is
 * skipped and its payload (the stored callable and bound arguments) is destroyed; asynchronous
 * device work it starts is not tracked.
 *
 * @par Task states
 * A task is unsubmitted until enqueue() accepts it. Once accepted it is pending (waiting for
 * dependencies), running, or done; a second enqueue is a no-op and never runs it again.
 *
 * @par Expressions
 * Task is one callable; Tasks is a value describing shared nodes, attachment heads and one exit.
 * Use a >> (b | c) >> done for serial/parallel composition, or Tasks::parallel(first, last) for
 * many branches. An attachment head is where a preceding fragment must connect, even if that
 * node already has other dependencies. Operators build edges immediately; enqueue() returns
 * the completion Task. Empty Tasks is an identity. Parenthesize parallel sections: >> binds
 * tighter than |, so a >> b | c >> done means (a >> b) | (c >> done).
 * Copies share work, not independent graph templates: later composition accumulates dependencies.
 * Built edges are not rolled back when an expression is discarded. A bare b | c adds only a
 * join after its branches, not a shared predecessor before them. Chaining k branches with |
 * creates k-1 joins linked in series; Tasks{...} or Tasks::parallel(first, last) creates one
 * join for all branches and sorts the heads once.
 *
 * @par Ownership
 * Arguments are decayed copies or moves; std::ref and borrowing views do not extend storage
 * lifetime, and moving a borrowed view still borrows. Releasing task or expression handles only drops
 * a reference: it never waits for or cancels work. Destroying the last reference to an unsubmitted
 * task destroys its payload without running it. A task may enqueue more tasks; those are
 * independent and are not implicitly joined or cancelled with the parent. Handles belong to the
 * Aura runtime instance that created them: do not pass them to another linked copy of Aura or use
 * them after it is unloaded.
 *
 * @par Cancellation and cleanup
 * flow cannot interrupt a running task. For cooperative cancellation, have the task check a
 * caller-owned atomic flag and return kErrorAborted; dependents then skip and propagate the error,
 * but independent tasks keep running. Use RAII so early returns still release resources.
 * Do not destroy shared state until every task that may use it has finished: a wait error does not
 * mean other accepted tasks have stopped. Use waitAll()/releaseWorkers() or explicit waits on all
 * relevant handles. Device work needs its own synchronization.
 *
 * @par Concurrency
 * Workers are created lazily (at most 8); releaseWorkers() drains and reclaims the pool. Waiting callers
 * may also execute tasks. There is no per-graph limit; use dependencies for ordering. Each tiled loop
 * uses its caller and up to 3 helpers. waitAll()/releaseWorkers() affect the whole shared runtime;
 * individual algorithms normally wait on their own completion Task.
 *
 * @par Device synchronization
 * @warning flow does not track OpenCL events. Synchronize before a CPU consumer reads device results,
 * either at the last GPU task's end or the consumer's start. Consecutive kernels may enqueue without
 * host waits; use queue order or explicit event dependencies. Prefer the needed event over clFinish().
 *
 * @par Example: parallel branches and a join
 *        ┌── b ──┐
 * a ─────┤       ├──> done
 *        └── c ──┘
 * @code
 * auto& flow = au::flow::Flow::get();
 * auto a = flow.create([] {});  // Prepare input.
 * auto b = flow.create([] {});  // First branch.
 * auto c = flow.create([] {});  // Second branch.
 * auto done = flow.create([] {});  // Consume results.
 * int code = flow.enqueue(a >> (b | c) >> done).wait();
 * @endcode
 */

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "flow/xflow_api.h"

namespace au {
namespace flow AU_LOCAL {

namespace detail {
template <std::size_t... I>
struct Indices
{
};

template <std::size_t N, std::size_t... I>
struct Sequence : Sequence<N - 1, N - 1, I...>
{
};

template <std::size_t... I>
struct Sequence<0, I...>
{
    typedef Indices<I...> Type;
};

template <class F, class... A>
auto invoke(F&& f, A&&... a) -> typename std::enable_if<!std::is_member_pointer<typename std::decay<F>::type>::value,
                                                        decltype(std::forward<F>(f)(std::forward<A>(a)...))>::type
{
    return std::forward<F>(f)(std::forward<A>(a)...);
}

template <class F, class... A>
auto invoke(F&& f, A&&... a) -> typename std::enable_if<std::is_member_pointer<typename std::decay<F>::type>::value,
                                                        decltype(std::mem_fn(f)(std::forward<A>(a)...))>::type
{
    return std::mem_fn(f)(std::forward<A>(a)...);
}

template <class C>
typename std::enable_if<std::is_void<decltype(std::declval<C&>()())>::value, int>::type call(C& c)
{
    c();

    return 0;
}

template <class C>
typename std::enable_if<!std::is_void<decltype(std::declval<C&>()())>::value, int>::type call(C& c)
{
    static_assert(std::is_same<decltype(c()), int>::value, "A flow task must return int or void");

    return c();
}

template <class F, class... A>
struct Bound
{
    F                mFunction;
    std::tuple<A...> mArguments;

    template <class G, class... B>
    Bound(G&& f, B&&... a) : mFunction(std::forward<G>(f)), mArguments(std::forward<B>(a)...)
    {
    }

    template <std::size_t... I>
    auto run(Indices<I...>) -> decltype(au::flow::detail::invoke(std::move(mFunction),
                                                                 std::move(std::get<I>(mArguments))...))
    {
        return au::flow::detail::invoke(std::move(mFunction), std::move(std::get<I>(mArguments))...);
    }

    auto operator()() -> decltype(this->run(typename Sequence<sizeof...(A)>::Type()))
    {
        return run(typename Sequence<sizeof...(A)>::Type());
    }

    static int execute(void* data) { return au::flow::detail::call(*static_cast<Bound*>(data)); }

    static void destroy(void* data) { static_cast<Bound*>(data)->~Bound(); }
};

// Prefer the indexed callback when both signatures are callable (C++11).
template <class F>
auto invokeTile(F& f, size_t begin, size_t end, size_t worker, int) -> decltype(f(begin, end, worker), void())
{
    f(begin, end, worker);
}

template <class F>
auto invokeTile(F& f, size_t begin, size_t end, size_t, long) -> decltype(f(begin, end), void())
{
    f(begin, end);
}
}  // namespace detail

class Tasks;
class Flow;

/**
 * @brief Shared task handle; copies share the task and moves transfer the handle.
 * @note Destruction releases the handle without waiting or cancelling accepted work.
 */
class Task
{
public:
    Task() noexcept = default;

    /// Adopt one owned reference; does not retain the raw handle.
    explicit Task(XFlowTaskHandle h) noexcept : mHandle(h) {}

    Task(const Task& other) noexcept : mHandle(other.mHandle), mError(other.mError) { au_flow_retain_task(mHandle); }

    Task(Task&& other) noexcept : mHandle(other.mHandle), mError(other.mError)
    {
        other.mHandle = nullptr;
        other.mError  = 0;
    }

    Task& operator=(Task other) noexcept
    {
        std::swap(mHandle, other.mHandle);
        std::swap(mError, other.mError);
        return *this;
    }

    ~Task() { au_flow_release_task(mHandle); }

    /// Owns a node; empty handles and failed create/enqueue results return false. Not proof of task success.
    bool valid() const noexcept { return mHandle != nullptr; }

    /**
     * @brief Make next depend on this task, including when this task is running or done.
     * @param next Successor; must still be unsubmitted.
     * @return 0 on success, or a build error.
     * @note Check the result before enqueueing; failed predecessors short-circuit next.
     *       Saved successor errors return directly. A saved predecessor error also attempts to
     *       invalidate an unsubmitted next with kErrorInvalidParam, while returning the original error.
     *       Device completion is not tracked; see the module warning.
     */
    int precede(const Task& next) const
    {
        if (next.mError)
            return next.mError;
        if (mError) {
            if (next.mHandle)
                (void)au_flow_add_dependency(nullptr, next.mHandle);
            return mError;
        }
        return au_flow_add_dependency(mHandle, next.mHandle);
    }

    /**
     * @brief Wait for completion without enqueueing the task.
     * @return Task result, a saved creation/enqueue error, or a wait error.
     * @note May execute other ready tasks, without tracking device completion.
     *       Unsubmitted tasks return kErrorStateInvalid or their saved build error.
     */
    int wait() const { return mError ? mError : au_flow_wait_task(mHandle); }

    /**
     * @brief Read the result without waiting.
     * @return Final result, a saved error, or kErrorNotReady while unfinished; empty handles return kErrorInvalidParam.
     */
    int code() const { return mError ? mError : au_flow_get_task_result(mHandle); }

    /**
     * @brief Check completion without waiting.
     * @return True after completion or a saved creation/enqueue failure; false for unsubmitted or
     *         empty handles.
     * @note Use code() for the result; completion does not imply success or device completion.
     */
    bool done() const { return mError || au_flow_task_done(mHandle); }

    /**
     * @brief Read final execution time in microseconds without waiting.
     * @return Final elapsed time; 0 while unfinished or when unavailable.
     * @note Wall time excludes queue/dependency delay; a completed task can take less than one microsecond.
     */
    int64_t costUs() const { return au_flow_get_task_cost_us(mHandle); }

    /// Borrow the raw handle; retain it separately if it must outlive this owner.
    XFlowTaskHandle nativeHandle() const noexcept { return mHandle; }

private:
    friend class Flow;
    friend class Tasks;
    XFlowTaskHandle mHandle = nullptr;
    int             mError  = 0;
};

/**
 * @brief Serial/parallel expression sharing task nodes; an empty expression is an identity.
 * @note Copies share nodes and later composition accumulates dependencies; nodes run only once.
 *       Wait on the Task returned by Flow::enqueue().
 */
class Tasks
{
public:
    Tasks() noexcept = default;

    /// Convert one task without creating a node. An invalid task is an error, not an empty expression.
    Tasks(const Task& task) : mLast(task), mError(task.mError)
    {
        if (!mError) {
            if (!task.valid())
                mError = au::err::kErrorInvalidParam;
            else {
                try {
                    mHeads.push_back(task);
                } catch (const std::bad_alloc&) {
                    mError = au::err::kErrorNoMemory;
                }
            }
        }
    }

    /// Build flat parallel branches; elements may themselves contain serial/parallel expressions.
    Tasks(std::initializer_list<Tasks> branches) : Tasks(parallel(branches.begin(), branches.end())) {}

    Tasks(const Tasks& other) : mLast(other.mLast), mError(other.mError)
    {
        if (!mError) {
            try {
                mHeads = other.mHeads;
            } catch (const std::bad_alloc&) {
                mError = au::err::kErrorNoMemory;
            }
        }
    }

    Tasks(Tasks&& other) noexcept : mHeads(std::move(other.mHeads)), mLast(std::move(other.mLast)), mError(other.mError)
    {
        other.mHeads.clear();
        other.mError = 0;
    }

    Tasks& operator=(Tasks other) noexcept
    {
        mHeads.swap(other.mHeads);
        std::swap(mLast, other.mLast);
        std::swap(mError, other.mError);
        return *this;
    }

    /**
     * @brief Build flat parallel branches from a single-pass range of Task or Tasks.
     * @param first First branch (inclusive).
     * @param last One past the final branch.
     * @return An expression; build errors are reported by enqueue().wait()/code().
     * @note Empty branches are identities; one branch creates no node, multiple branches create
     *       one ordinary join task. Prefer this or Tasks{...} over a long chain of binary |, which
     *       creates one join per level and re-sorts the accumulated heads at every level.
     */
    template <class Iterator>
    static Tasks parallel(Iterator first, Iterator last);

private:
    friend class Flow;
    friend Tasks      operator>>(const Tasks&, const Tasks&);
    std::vector<Task> mHeads;
    Task              mLast;
    int               mError = 0;
};

/**
 * @brief Build a dependency chain without enqueueing any work.
 * @param a Predecessor fragment; its exit may already be accepted.
 * @param b Successor fragment; each attachment head must still be unsubmitted.
 * @return An expression carrying the first build error, checked before enqueue accepts any work.
 * @note Connects a's exit to each of b's heads. Already built edges are not rolled back on failure.
 *       Accepted successors report kErrorStateInvalid without changing their execution results.
 *       a >> (b, c) uses the comma operator and links only to c; use a >> (b | c) instead.
 */
inline Tasks operator>>(const Tasks& a, const Tasks& b)
{
    if (a.mError)
        return a;
    if (b.mError)
        return b;
    if (a.mHeads.empty())
        return b;
    if (b.mHeads.empty())
        return a;
    Tasks result(a);
    result.mLast = b.mLast;
    for (const auto& head : b.mHeads) {
        if (result.mError)
            break;
        result.mError = a.mLast.precede(head);
    }
    return result;
}

/**
 * @brief Build two parallel branches and an ordinary join task, without enqueueing work.
 * @param a First fragment; accepted tasks are allowed.
 * @param b Second fragment; accepted tasks are allowed.
 * @return An expression; branch failures skip its join and propagate an upstream error.
 * @note No entry task is created. Use parentheses around parallel sections when mixing | and >>.
 * @note Chaining k branches with | builds k-1 join tasks linked in series: every level adds a
 *       real node, one more dispatch hop on the way to the exit, and a copy plus re-sort of the
 *       accumulated head list, so construction is superlinear. Tasks{...} and Tasks::parallel()
 *       build one join for all branches and sort once. Heads, exit condition and skip-on-failure
 *       behavior are the same either way; only graph topology and cost differ.
 */
inline Tasks operator|(const Tasks& a, const Tasks& b);

/**
 * @brief Shared scheduler with lazy pool workers and caller-assisted CPU tiles.
 * @note Task completion does not join child tasks or GPU work automatically.
 */
class Flow
{
public:
    static Flow& get() noexcept
    {
        static Flow instance;
        return instance;
    }

    /**
     * @brief Create a task for later enqueue or dependency building.
     * @param f Callable returning int or void; void means success.
     * @param a Arguments copied or moved into task storage; use std::ref to borrow.
     * @return A task; creation errors are returned by wait(), code() or enqueue().
     * @note Copies of pointers or borrowed views do not extend the backing storage lifetime.
     */
    template <class F, class... A>
    Task create(F&& f, A&&... a)
    {
        return createNamed(nullptr, std::forward<F>(f), std::forward<A>(a)...);
    }

    /**
     * @brief Create a named task with the same ownership rules as create().
     * @param name Optional copied name, used for tracing.
     * @param f Callable returning int or void.
     * @param a Arguments copied or moved into task storage.
     * @return An unsubmitted task, carrying any creation error.
     * @note Payload construction and commit are handled automatically.
     */
    template <class F, class... A>
    Task createNamed(const char* name, F&& f, A&&... a)
    {
        Task task;
        try {
            typedef au::flow::detail::Bound<typename std::decay<F>::type, typename std::decay<A>::type...> Payload;
            void* storage = nullptr;
            task.mHandle  = au_flow_create_task_ex(name, sizeof(Payload), alignof(Payload), &Payload::execute,
                                                   &Payload::destroy, &storage);
            if (!task.mHandle) {
                return failure(au::err::kErrorNoMemory);
            }
            ::new (storage) Payload(std::forward<F>(f), std::forward<A>(a)...);
            task.mError = au_flow_commit_task_payload(task.mHandle);
            if (task.mError) {
                // Commit failures leave storage unconstructed from the ABI's
                // perspective; destroy our successfully constructed object.
                Payload::destroy(storage);
                return failure(task.mError);
            }
        } catch (const std::bad_alloc&) {
            return failure(au::err::kErrorNoMemory);
        } catch (...) {
            return failure(au::err::kErrorUnknown);
        }
        return task;
    }

    /**
     * @brief Create and immediately enqueue a task.
     * @param f Callable returning int or void.
     * @param a Arguments copied or moved into task storage.
     * @return A task carrying its execution result or creation/enqueue error.
     * @note Nested enqueues are independent. Device completion is not tracked; see the module warning.
     */
    template <class F, class... A>
    typename std::enable_if<!std::is_same<typename std::decay<F>::type, Task>::value &&
                                !std::is_same<typename std::decay<F>::type, Tasks>::value,
                            Task>::type
    enqueue(F&& f, A&&... a)
    {
        return submit(create(std::forward<F>(f), std::forward<A>(a)...));
    }

    /**
     * @brief Create and enqueue a named task.
     * @param name Optional copied name for tracing.
     * @param f Callable returning int or void.
     * @param a Arguments copied or moved into task storage.
     * @return A task carrying its execution result or creation/enqueue error.
     */
    template <class F, class... A>
    Task enqueueNamed(const char* name, F&& f, A&&... a)
    {
        return submit(createNamed(name, std::forward<F>(f), std::forward<A>(a)...));
    }

    /// Submit an existing Task directly; the constraint preserves enqueue({}) as an empty expression.
    template <class T>
    typename std::enable_if<std::is_same<T, Task>::value, Task>::type enqueue(const T& task)
    {
        return submit(task);
    }

    /**
     * @brief Enqueue an expression and its unsubmitted upstream dependencies atomically.
     * @param tasks Serial/parallel expression.
     * @return Its completion Task, or a task carrying the build/submission error; failure accepts no new work.
     * @note Accepted nodes are not rerun. Empty expressions submit one empty task. costUs() measures
     *       only the exit task, not the whole graph. Failed results own no node and leave originals unchanged.
     */
    Task enqueue(const Tasks& tasks)
    {
        if (tasks.mError) {
            return failure(tasks.mError);
        }
        return submit(tasks.mHeads.empty() ? create([] {}) : tasks.mLast);
    }

    /**
     * @brief Create and enqueue a successor, including any unsubmitted upstream tasks.
     * @param pred Predecessor task.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task; setup/enqueue errors are available through wait() or code().
     * @note The callable is skipped if the predecessor fails. Waiting itself never enqueues work.
     */
    template <class F, class... A>
    Task enqueueAfter(const Task& pred, F&& f, A&&... a)
    {
        return enqueueAfter({pred}, std::forward<F>(f), std::forward<A>(a)...);
    }

    /**
     * @brief Enqueue a successor that waits for all given predecessors.
     * @param preds Predecessors in any state; an empty list adds no dependency.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task, carrying any setup/enqueue error.
     * @note Unsubmitted predecessors are included atomically; a failure leaves them unsubmitted.
     *       Any predecessor failure skips the successor body and propagates its error.
     */
    template <class F, class... A>
    Task enqueueAfter(std::initializer_list<Task> preds, F&& f, A&&... a)
    {
        Task task = create(std::forward<F>(f), std::forward<A>(a)...);
        for (const auto& pred : preds)
            if (!task.mError)
                task.mError = pred.mError ? pred.mError : pred.precede(task);
        return submit(std::move(task));
    }

    /**
     * @brief Enqueue a successor of a fragment's exit, including its unsubmitted upstream tasks.
     * @param pred Predecessor expression; an empty expression adds no dependency.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task, carrying any setup/enqueue error.
     * @note Upstream failures skip the successor body; validation failure publishes no new work.
     */
    template <class F, class... A>
    Task enqueueAfter(const Tasks& pred, F&& f, A&&... a)
    {
        Task task = create(std::forward<F>(f), std::forward<A>(a)...);
        if (!task.mError) {
            task.mError = pred.mError;
            if (!task.mError && !pred.mHeads.empty())
                task.mError = pred.mLast.precede(task);
        }
        return submit(std::move(task));
    }

    /** @copydoc au_flow_wait_all */
    int waitAll() { return au_flow_wait_all(); }

    /**
     * @brief Finish a tiled CPU loop before returning, using the caller and up to 3 helpers.
     * @param range Number of elements; zero does nothing.
     * @param tile Elements per tile; zero means 1.
     * @param f Callback (begin, end) or (begin, end, worker_idx), using [begin, end).
     * @note Callback must not throw or wait on unfinished tasks. Lane IDs are caller=0 and pool slots=1..8.
     *       Nested loops run serially with lane 0; concurrent loops need separate scratch storage.
     *       Callback return values are ignored; record errors in caller-owned state and check after return.
     */
    template <class F>
    void parallelizeTiledTasks(size_t range, size_t tile, F&& f)
    {
        typedef std::reference_wrapper<typename std::remove_reference<F>::type> Function;
        Function                                                                callback(f);
        au_flow_parallelize_tile_ex(
            range, tile,
            [](size_t begin, size_t end, size_t worker, void* data) {
                au::flow::detail::invokeTile((*static_cast<Function*>(data)).get(), begin, end, worker, 0);
            },
            &callback);
    }

    /**
     * @brief Request worker affinity; the calling thread is not bound.
     * @param mask CPU bits, e.g. (UINT64_C(1) << 6) | (UINT64_C(1) << 7); 0 restores defaults.
     * @return 0 if accepted, or an argument/platform error.
     * @note Use sys::selectCpuMask() to select CPUs. Applied asynchronously; failures are logged.
     */
    int setAffinity(uint64_t mask = 0) { return au_flow_set_affinity_mask(mask); }

    /** @copydoc au_flow_get_affinity_mask */
    uint64_t getAffinityMask() const { return au_flow_get_affinity_mask(); }

    size_t workerCount() const { return au_flow_get_worker_count(); }

    /** @copydoc au_flow_release_workers */
    int releaseWorkers() { return au_flow_release_workers(); }

private:
    static Task failure(int code) noexcept
    {
        Task failed;
        failed.mError = code;
        return failed;
    }

    Task submit(Task task)
    {
        if (!task.mError)
            task.mError = au_flow_enqueue_task(task.mHandle);
        return task.mError ? failure(task.mError) : std::move(task);
    }

    Flow() = default;

    ~Flow() = default;

    Flow(const Flow&) = delete;

    Flow& operator=(const Flow&) = delete;
};

template <class Iterator>
Tasks Tasks::parallel(Iterator first, Iterator last)
{
    Tasks result;
    Task  join;
    try {
        for (; first != last; ++first) {
            const Tasks& branch = *first;
            if (branch.mError)
                return branch;
            if (branch.mHeads.empty())
                continue;
            if (result.mHeads.empty()) {
                result = branch;
                if (result.mError)
                    return result;
                continue;
            }
            if (!join.valid()) {
                join          = Flow::get().create([] {});
                result.mError = join.mError ? join.mError : result.mLast.precede(join);
                if (result.mError)
                    return result;
                result.mLast = join;
            }
            result.mError = branch.mLast.precede(join);
            if (result.mError)
                return result;
            result.mHeads.insert(result.mHeads.end(), branch.mHeads.begin(), branch.mHeads.end());
        }
        // Collect once, then deduplicate once; never fold the range through binary |.
        std::sort(result.mHeads.begin(), result.mHeads.end(), [](const Task& a, const Task& b) {
            return std::less<XFlowTaskHandle>()(a.nativeHandle(), b.nativeHandle());
        });
        result.mHeads.erase(
            std::unique(result.mHeads.begin(), result.mHeads.end(),
                        [](const Task& a, const Task& b) { return a.nativeHandle() == b.nativeHandle(); }),
            result.mHeads.end());
    } catch (const std::bad_alloc&) {
        result.mError = au::err::kErrorNoMemory;
    } catch (...) {
        result.mError = au::err::kErrorUnknown;
    }
    return result;
}

inline Tasks operator|(const Tasks& a, const Tasks& b)
{
    const std::reference_wrapper<const Tasks> branches[] = {std::cref(a), std::cref(b)};
    return Tasks::parallel(branches, branches + 2);
}
}  // namespace flow AU_LOCAL
}  // namespace au

#define XTHREAD_PARALLELIZE_TILED_TASK_QUOTE(range, tile) \
    ::au::flow::Flow::get().parallelizeTiledTasks((range), (tile), [&](size_t start, size_t end) { \
        for (size_t i = start; i < end; ++i) {
#define XTHREAD_PARALLELIZE_TILED_TASK_ASSIGN(range, tile) \
    ::au::flow::Flow::get().parallelizeTiledTasks((range), (tile), [=](size_t start, size_t end) { \
        for (size_t i = start; i < end; ++i) {
#define XTHREAD_PARALLELIZE_END \
    }                           \
    });
#endif
