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

namespace au {
namespace flow AU_LOCAL {
enum class AffinityPolicy : int
{
    kDefault     = XFLOW_AFFINITY_DEFAULT,
    kPerformance = XFLOW_AFFINITY_PERFORMANCE,
    kPowerSaving = XFLOW_AFFINITY_POWERSAVING,
    kCustomMask  = XFLOW_AFFINITY_CUSTOM_MASK
};

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

    /// Handle validity only; does not imply successful submission or execution.
    bool valid() const noexcept { return mHandle != nullptr; }

    /**
     * @brief Make next depend on this task, including when this task is running or done.
     * @param next Successor; must not be submitted yet.
     * @return 0 on success, or a build error.
     * @note Check the result before submitting; failed predecessors short-circuit next.
     */
    int precede(const Task& next) const { return au_flow_add_dependency(mHandle, next.mHandle); }

    int precede(const Tasks& next) const;

    /**
     * @brief Wait for completion without submitting the task.
     * @return Task result, a saved creation/submission error, or a wait error.
     * @note May execute other ready work; an unsubmitted task returns kErrorNotReady or its build error.
     */
    int wait() const { return mError ? mError : au_flow_wait_task(mHandle); }

    /**
     * @brief Read the result without waiting.
     * @return Task result when done, kErrorNotReady while unfinished, or a saved error.
     */
    int result() const { return mError ? mError : au_flow_get_task_result(mHandle); }

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
 * @brief A group for batch submission and waiting; members may run in parallel.
 * @note Copies share the same group. Releasing a group does not cancel its work.
 */
class Tasks
{
public:
    explicit Tasks(const char* name = "Tasks") : mHandle(au_flow_create_group(name)) {}

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
     * @brief Add a task to a group that has not been submitted.
     * @param t Member task; duplicate additions are ignored.
     * @return 0 on success, or a build error saved for group submission.
     * @note Unsubmitted tasks can join one group; submitted tasks may be shared as completion dependencies.
     */
    int add(const Task& t) { return au_flow_group_add_task(mHandle, t.nativeHandle()); }

    /// Add several members, stopping at the first error.
    template <class... T>
    int add(const Task& first, const Task& second, const T&... rest)
    {
        int code = add(first);
        return code ? code : add(second, rest...);
    }

    int precede(const Task& t) const { return au_flow_add_group_task_dependency(mHandle, t.nativeHandle()); }

    int precede(const Tasks& g) const { return au_flow_add_group_dependency(mHandle, g.mHandle); }

    /// Wait for every member; returns the propagated result without submitting the group.
    int wait() const { return au_flow_wait_group(mHandle); }

    /**
     * @brief Export dependencies and task results as Graphviz DOT.
     * @return Thread-local text, or null on failure.
     * @note Copy to keep it; the next dump on this thread may invalidate the pointer. Do not free it.
     */
    const char* dumpDot() const { return au_flow_dump_group_dot(mHandle); }

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

    template <class U>
    Link<U> operator>>(const U& next) const
    {
        return Link<U>(next, mCode ? mCode : mLast.precede(next));
    }

private:
    T   mLast;
    int mCode;
};
}  // namespace detail

/**
 * @brief Build a dependency chain without submitting any work.
 * @param a Predecessor task.
 * @param b Successor task; must not be submitted yet.
 * @return A chain result carrying the first build error.
 * @note Check (a >> b >> c).code() before enqueue; group overloads use the same rules.
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
     * @brief Create a task for later submission or dependency building.
     * @param f Callable returning int or void; void means success.
     * @param a Arguments copied or moved into task storage; use std::ref to borrow.
     * @return A task; creation errors are returned by wait(), result() or enqueue().
     * @note Copies of pointers or borrowed views do not extend the backing storage lifetime.
     */
    template <class F, class... A>
    Task create(F&& f, A&&... a)
    {
        return createNamed(nullptr, std::forward<F>(f), std::forward<A>(a)...);
    }

    /**
     * @brief Create a named task with the same ownership rules as create().
     * @param name Optional copied name, used for tracing and DOT output.
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
     * @brief Create and immediately submit a task.
     * @param f Callable returning int or void.
     * @param a Arguments copied or moved into task storage.
     * @return A task carrying its execution result or creation/submission error.
     * @note Submission is asynchronous; nested submissions are independent and are not implicitly joined.
     */
    template <class F, class... A>
    typename std::enable_if<!std::is_same<typename std::decay<F>::type, Task>::value &&
                                !std::is_same<typename std::decay<F>::type, Tasks>::value,
                            Task>::type
    enqueue(F&& f, A&&... a)
    {
        Task task = create(std::forward<F>(f), std::forward<A>(a)...);
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
    }

    /**
     * @brief Create and submit a named task.
     * @param name Optional copied name for tracing and DOT output.
     * @param f Callable returning int or void.
     * @param a Arguments copied or moved into task storage.
     * @return A task carrying its execution result or creation/submission error.
     */
    template <class F, class... A>
    Task enqueueNamed(const char* name, F&& f, A&&... a)
    {
        Task task = createNamed(name, std::forward<F>(f), std::forward<A>(a)...);
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
    }

    int enqueue(const Task& task) { return task.mError ? task.mError : au_flow_enqueue_task(task.nativeHandle()); }

    int enqueue(const Tasks& group) { return au_flow_enqueue_group(group.nativeHandle()); }

    /**
     * @brief Create and submit a successor of an accepted or completed task.
     * @param pred Predecessor task.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task; setup/submission errors are available through wait() or result().
     * @note The callable is skipped if the predecessor fails. Waiting does not submit an unsubmitted predecessor.
     */
    template <class F, class... A>
    Task enqueueAfter(const Task& pred, F&& f, A&&... a)
    {
        return enqueueAfter({pred}, std::forward<F>(f), std::forward<A>(a)...);
    }

    /**
     * @brief Submit a successor that waits for all given predecessors.
     * @param preds Accepted or completed predecessors; an empty list adds no dependency.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task, carrying any setup/submission error.
     * @note Any predecessor failure skips the successor body and propagates its error.
     */
    template <class F, class... A>
    Task enqueueAfter(std::initializer_list<Task> preds, F&& f, A&&... a)
    {
        Task task = create(std::forward<F>(f), std::forward<A>(a)...);
        for (const auto& pred : preds)
            if (!task.mError)
                task.mError = pred.precede(task);
        if (!task.mError)
            task.mError = enqueue(task);
        return task;
    }

    /**
     * @brief Submit a successor that waits for an accepted or completed group.
     * @param pred Predecessor group.
     * @param f Successor callable returning int or void.
     * @param a Arguments for the successor.
     * @return Successor task, carrying any setup/submission error.
     * @note Does not submit the predecessor group; group failures skip the successor body.
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

    int wait(const Task& task) { return task.wait(); }

    int wait(const Tasks& group) { return group.wait(); }

    /** @copydoc au_flow_wait_all */
    int waitAll() { return au_flow_wait_all(); }

    int join(const Task& task) { return wait(task); }

    int join(const Tasks& group) { return wait(group); }

    int joinAll() { return waitAll(); }

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
     * @brief Set pool-worker affinity; does not bind the calling thread.
     * @param policy Default, performance, powersaving or custom mask.
     * @param mask Used for kCustomMask only; bit N selects CPU N (CPU6+7 = 0xC0).
     * @return 0 if accepted, or an argument/platform error.
     * @note Selected CPUs must be allowed; workers apply changes asynchronously.
     */
    int setAffinity(AffinityPolicy policy, uint64_t mask = 0)
    {
        return au_flow_set_affinity(static_cast<int>(policy), mask);
    }

    /** @copydoc au_flow_get_affinity_mask */
    uint64_t getAffinityMask() const { return au_flow_get_affinity_mask(); }

    size_t workerCount() const { return au_flow_get_worker_count(); }

    /** @copydoc au_flow_release_idle_workers */
    void releaseIdleWorkers() { au_flow_release_idle_workers(); }

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
