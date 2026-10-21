#ifndef AURA_INC_FLOW_XFLOW_H_
#define AURA_INC_FLOW_XFLOW_H_

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

class Task
{
public:
    Task() noexcept = default;

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

    bool valid() const noexcept { return mHandle != nullptr; }

    int precede(const Task& next) const { return au_flow_add_dependency(mHandle, next.mHandle); }

    int precede(const Tasks& next) const;

    int wait() const { return mError ? mError : au_flow_wait_task(mHandle); }

    int result() const { return mError ? mError : au_flow_get_task_result(mHandle); }

    int64_t costUs() const { return au_flow_get_task_cost_us(mHandle); }

    XFlowTaskHandle nativeHandle() const noexcept { return mHandle; }

private:
    friend class Flow;
    XFlowTaskHandle mHandle = nullptr;
    int             mError  = 0;
};

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

    int add(const Task& t) { return au_flow_group_add_task(mHandle, t.nativeHandle()); }

    template <class... T>
    int add(const Task& first, const Task& second, const T&... rest)
    {
        int code = add(first);
        return code ? code : add(second, rest...);
    }

    int precede(const Task& t) const { return au_flow_add_group_task_dependency(mHandle, t.nativeHandle()); }

    int precede(const Tasks& g) const { return au_flow_add_group_dependency(mHandle, g.mHandle); }

    int wait() const { return au_flow_wait_group(mHandle); }

    const char* dumpDot() const { return au_flow_dump_group_dot(mHandle); }

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

class Flow
{
public:
    static Flow& get() noexcept
    {
        static Flow instance;
        return instance;
    }

    template <class F, class... A>
    Task create(F&& f, A&&... a)
    {
        return createNamed(nullptr, std::forward<F>(f), std::forward<A>(a)...);
    }

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

    template <class F, class... A>
    Task enqueueAfter(const Task& pred, F&& f, A&&... a)
    {
        return enqueueAfter({pred}, std::forward<F>(f), std::forward<A>(a)...);
    }

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

    int waitAll() { return au_flow_wait_all(); }

    int join(const Task& task) { return wait(task); }

    int join(const Tasks& group) { return wait(group); }

    int joinAll() { return waitAll(); }

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

    int setAffinity(AffinityPolicy policy, uint64_t mask = 0)
    {
        return au_flow_set_affinity(static_cast<int>(policy), mask);
    }

    uint64_t getAffinityMask() const { return au_flow_get_affinity_mask(); }

    size_t workerCount() const { return au_flow_get_worker_count(); }

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
