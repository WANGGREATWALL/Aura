#ifndef AURA_XFLOW_H_
#define AURA_XFLOW_H_

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "flow/xflow_api.h"

/**
 * @file xflow.h
 * @brief Lightweight control-flow scheduler for algorithm libraries, with a C++11 facade over a C ABI.
 *
 * @par Purpose
 * Overlap asynchronous tasks with caller work, express parallel branches followed by a join,
 * or synchronously split a CPU range into tiles. Scheduling is independent of memory and GPU modules.
 *
 * @par Task model
 * Tasks return int (0 means success) or void. Failed predecessors skip dependents and propagate an
 * upstream error; a multi-input join does not guarantee which error wins. enqueue() collects and
 * accepts all unsubmitted upstream tasks atomically. Completion includes the callable and payload cleanup,
 * not device work it starts asynchronously.
 *
 * @par Task states
 * A task is unsubmitted until enqueue() accepts it. Once accepted it is pending (waiting for
 * dependencies), running, or done; a second enqueue is a no-op and never runs it again.
 *
 * @par Group model
 * A group has an internal entry gate and an exit gate. Adding an unsubmitted member links
 * entry -> member -> exit; adding an accepted member links member -> exit only, because its
 * execution order is already fixed. A group is in use while its handle exists or a downstream
 * task or group depends on its exit.
 *
 * @par Ownership
 * Arguments are decayed copies or moves. std::ref and borrowing views do not extend storage lifetime;
 * moving a borrowed view still borrows. Handle destruction neither waits nor cancels, and nested
 * enqueues are independent. Handles must not cross independently linked runtime instances.
 *
 * @par Cancellation and cleanup
 * Check a caller-owned atomic flag and return kErrorAborted for cooperative cancellation; running
 * functions cannot be forcibly interrupted. Use RAII, or run mandatory cleanup after confirming all
 * relevant work is complete. A wait error does not guarantee completion of accepted work.
 *
 * @par Concurrency
 * Workers are created lazily (at most 8); releaseWorkers() drains and reclaims the pool. Waiting callers
 * may also execute tasks. There is no per-graph limit; use dependencies for ordering. Each tiled loop
 * uses its caller and up to 3 helpers.
 *
 * @par Device synchronization
 * @warning flow does not track OpenCL events. Synchronize before a CPU consumer reads device results,
 * either at the last GPU task's end or the consumer's start. Consecutive kernels may enqueue without
 * host waits; use queue order or explicit event dependencies. Prefer the needed event over clFinish().
 *
 * @par Example: parallel branches and a join
 * @code
 * auto& flow = au::flow::Flow::get();
 * auto a = flow.create([] {});  // Prepare input.
 * auto b = flow.create([] {});  // First branch.
 * auto c = flow.create([] {});  // Second branch.
 * au::flow::Tasks group{b, c};
 * auto done = flow.create([] {});  // Consume results.
 * auto chain = a >> group >> done;
 * int code = flow.enqueue(chain);
 * if (!code)
 *     code = chain.last().wait();
 * @endcode
 */

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
    auto run(Indices<I...>)
        -> decltype(au::flow::detail::invoke(std::move(mFunction), std::move(std::get<I>(mArguments))...))
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

    /// Handle validity only; does not imply successful enqueue or execution.
    bool valid() const noexcept { return mHandle != nullptr; }

    /**
     * @brief Make next depend on this task, including when this task is running or done.
     * @param next Successor; must still be unsubmitted.
     * @return 0 on success, or a build error.
     * @note Check the result before enqueueing; failed predecessors short-circuit next.
     *       Device completion is not tracked; see the module warning.
     */
    int precede(const Task& next) const { return au_flow_add_dependency(mHandle, next.mHandle); }

    int precede(const Tasks& next) const;

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
    XFlowTaskHandle mHandle = nullptr;
    int             mError  = 0;
};

/**
 * @brief A group for batch enqueue and waiting; members may run in parallel.
 * @note Copies share the same group. Releasing a group does not cancel its work.
 */
class Tasks
{
public:
    explicit Tasks(const char* name = "Tasks") : mHandle(au_flow_create_group(name)) {}

    /// Create a group from members, retaining the first build error for enqueue() and wait().
    Tasks(std::initializer_list<Task> tasks) : Tasks()
    {
        for (const auto& task : tasks)
            if (add(task))
                break;
    }

    Tasks(const Tasks& other) noexcept : mHandle(other.mHandle) { au_flow_retain_group(mHandle); }

    Tasks(Tasks&& other) noexcept : mHandle(other.mHandle) { other.mHandle = nullptr; }

    Tasks& operator=(Tasks other) noexcept
    {
        std::swap(mHandle, other.mHandle);
        return *this;
    }

    ~Tasks() { au_flow_release_group(mHandle); }

    bool valid() const noexcept { return mHandle != nullptr; }

    /**
     * @brief Add a task to a group that has not been enqueued.
     * @param t Member task; duplicate additions are ignored.
     * @return 0 on success, or a build error saved for the group's enqueue.
     * @note An unsubmitted task can belong to only one group that is still in use. It can join a
     *       new group after the previous group is released and no downstream task or group depends
     *       on its exit; its existing dependencies and saved build errors remain. An accepted task
     *       (pending, running, or done) can be shared by several groups.
     * @note The group keeps its first add error and remains failed; enqueue and wait report it.
     *       Duplicate additions are ignored. Create a new group to retry after a failed add.
     */
    int add(const Task& t) { return au_flow_group_add_task(mHandle, t.nativeHandle()); }

    int precede(const Task& t) const { return au_flow_add_group_task_dependency(mHandle, t.nativeHandle()); }

    int precede(const Tasks& g) const { return au_flow_add_group_dependency(mHandle, g.mHandle); }

    /// Wait for every member; returns the propagated result without enqueueing the group.
    int wait() const { return au_flow_wait_group(mHandle); }

    /// Borrow the raw group handle; no owning reference is added.
    XFlowGroupHandle nativeHandle() const noexcept { return mHandle; }

private:
    XFlowGroupHandle mHandle;
};

inline int Task::precede(const Tasks& next) const
{
    return au_flow_add_task_group_dependency(mHandle, next.nativeHandle());
}

namespace detail {
// Each temporary chain carries its own first error, including edits to accepted
// targets. Explicit precede() returns int; no published task result is polluted.
template <class T>
class Link
{
public:
    Link(const T& last, int code) : mLast(last), mCode(code) {}

    int code() const { return mCode; }

    const T& last() const noexcept { return mLast; }

    template <class U>
    Link<U> operator>>(const U& next) const
    {
        return Link<U>(next, mCode ? mCode : mLast.precede(next));
    }

private:
    T   mLast;
    int mCode;
};

template <class T>
struct IsLink : std::false_type
{
};

template <class T>
struct IsLink<Link<T>> : std::true_type
{
};
}  // namespace detail

/**
 * @brief Build a dependency chain without enqueueing any work.
 * @param a Predecessor task.
 * @param b Successor task; must still be unsubmitted.
 * @return A chain result carrying the first build error.
 * @note flow.enqueue(a >> b >> c) checks the chain error before enqueueing its upstream closure.
 *       An already accepted successor (pending, running, or done) returns kErrorStateInvalid
 *       without changing its result.
 *       Group overloads use the same rules; a >> (b, c) links only a to c, not both tasks.
 */
inline au::flow::detail::Link<Task> operator>>(const Task& a, const Task& b)
{
    return au::flow::detail::Link<Task>(b, a.precede(b));
}

inline au::flow::detail::Link<Tasks> operator>>(const Task& a, const Tasks& b)
{
    return au::flow::detail::Link<Tasks>(b, a.precede(b));
}

inline au::flow::detail::Link<Task> operator>>(const Tasks& a, const Task& b)
{
    return au::flow::detail::Link<Task>(b, a.precede(b));
}

inline au::flow::detail::Link<Tasks> operator>>(const Tasks& a, const Tasks& b)
{
    return au::flow::detail::Link<Tasks>(b, a.precede(b));
}

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
                task.mError = au::err::kErrorNoMemory;
                return task;
            }
            ::new (storage) Payload(std::forward<F>(f), std::forward<A>(a)...);
            task.mError = au_flow_commit_task_payload(task.mHandle);
            if (task.mError) {
                // Commit failures leave storage unconstructed from the ABI's
                // perspective; destroy our successfully constructed object.
                Payload::destroy(storage);
            }
        } catch (const std::bad_alloc&) {
            task        = Task();
            task.mError = au::err::kErrorNoMemory;
        } catch (...) {
            task        = Task();
            task.mError = au::err::kErrorUnknown;
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
                                !std::is_same<typename std::decay<F>::type, Tasks>::value &&
                                !au::flow::detail::IsLink<typename std::decay<F>::type>::value,
                            Task>::type
    enqueue(F&& f, A&&... a)
    {
        Task task = create(std::forward<F>(f), std::forward<A>(a)...);
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
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
        Task task = createNamed(name, std::forward<F>(f), std::forward<A>(a)...);
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
    }

    /**
     * @brief Enqueue a task and all its unsubmitted upstream dependencies as one batch.
     * @param task Task at the end of the requested dependency graph.
     * @return 0 if accepted, or a validation/enqueue error; failure accepts no new tasks.
     * @note Accepted tasks are not rerun. An unsubmitted member of a group that is still in use
     *       cannot be enqueued alone: enqueue the whole group, or a successor of the group, so
     *       that its exit is part of the same enqueue batch. After the group is released and no
     *       downstream task depends on its exit, the member can be enqueued with its existing
     *       dependencies.
     */
    int enqueue(const Task& task) { return task.mError ? task.mError : au_flow_enqueue_task(task.nativeHandle()); }

    /** @copydoc au_flow_enqueue_group */
    int enqueue(const Tasks& group) { return au_flow_enqueue_group(group.nativeHandle()); }

    /**
     * @brief Enqueue a dependency chain through its last task or group.
     * @param chain Result of task/group operator>> calls.
     * @return First chain error without enqueueing work, otherwise the batch enqueue result.
     * @note Wait on chain.last() or a retained tail handle. This does not construct another Tasks group.
     */
    template <class T>
    int enqueue(const au::flow::detail::Link<T>& chain)
    {
        return chain.code() ? chain.code() : enqueue(chain.last());
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
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
    }

    /**
     * @brief Enqueue a successor together with any unsubmitted predecessor group and its upstream tasks.
     * @param pred Predecessor group.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task, carrying any setup/enqueue error.
     * @note Group failures skip the successor body; validation failure publishes no new work.
     */
    template <class F, class... A>
    Task enqueueAfter(const Tasks& pred, F&& f, A&&... a)
    {
        Task task = create(std::forward<F>(f), std::forward<A>(a)...);
        if (!task.mError)
            task.mError = pred.precede(task);
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
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
    Flow() = default;

    ~Flow() = default;

    Flow(const Flow&) = delete;

    Flow& operator=(const Flow&) = delete;
};
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
