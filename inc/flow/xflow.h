#ifndef AURA_INC_FLOW_XFLOW_H_
#define AURA_INC_FLOW_XFLOW_H_
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
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
    static int  execute(void* data) { return au::flow::detail::call(*static_cast<Bound*>(data)); }
    static void destroy(void* data) { delete static_cast<Bound*>(data); }
};
}  // namespace detail
class Tasks;
class Flow;
class Task
{
public:
    Task() noexcept = default;
    explicit Task(XFlowTaskHandle h) noexcept : mHandle(h) {}
    Task(const Task& other) noexcept : mHandle(other.mHandle), mError(other.mError) { XFlow_RetainTask(mHandle); }
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
    ~Task() { XFlow_ReleaseTask(mHandle); }
    bool            valid() const noexcept { return mHandle != nullptr; }
    int             precede(const Task& next) const { return XFlow_AddDependency(mHandle, next.mHandle); }
    int             precede(const Tasks& next) const;
    int             wait() const { return mError ? mError : XFlow_WaitTask(mHandle); }
    int             result() const { return mError ? mError : XFlow_GetTaskResult(mHandle); }
    int64_t         costUs() const { return XFlow_GetTaskCostUs(mHandle); }
    XFlowTaskHandle nativeHandle() const noexcept { return mHandle; }

private:
    friend class Flow;
    XFlowTaskHandle mHandle = nullptr;
    int             mError  = 0;
};
class Tasks
{
public:
    explicit Tasks(const char* name = "Tasks") : mHandle(XFlow_CreateGroup(name)) {}
    Tasks(const Tasks& other) noexcept : mHandle(other.mHandle) { XFlow_RetainGroup(mHandle); }
    Tasks(Tasks&& other) noexcept : mHandle(other.mHandle) { other.mHandle = nullptr; }
    Tasks& operator=(Tasks other) noexcept
    {
        std::swap(mHandle, other.mHandle);
        return *this;
    }
    ~Tasks() { XFlow_ReleaseGroup(mHandle); }
    bool valid() const noexcept { return mHandle != nullptr; }
    int  add(const Task& t) { return XFlow_GroupAddTask(mHandle, t.nativeHandle()); }
    template <class... T>
    int add(const Task& first, const Task& second, const T&... rest)
    {
        int code = add(first);
        return code ? code : add(second, rest...);
    }
    int              precede(const Task& t) const { return XFlow_AddGroupTaskDependency(mHandle, t.nativeHandle()); }
    int              precede(const Tasks& g) const { return XFlow_AddGroupDependency(mHandle, g.mHandle); }
    int              wait() const { return XFlow_WaitGroup(mHandle); }
    const char*      dumpDot() const { return XFlow_DumpGroupDot(mHandle); }
    XFlowGroupHandle nativeHandle() const noexcept { return mHandle; }

private:
    XFlowGroupHandle mHandle;
};
inline int Task::precede(const Tasks& next) const { return XFlow_AddTaskGroupDependency(mHandle, next.nativeHandle()); }
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
#if __cplusplus < 201703L
            static_assert(alignof(Payload) <= alignof(std::max_align_t),
                          "Over-aligned callables require a C++17 consumer");
#endif
            std::unique_ptr<Payload> payload(new Payload(std::forward<F>(f), std::forward<A>(a)...));
            task.mHandle = XFlow_CreateTask(name, &Payload::execute, payload.release(), &Payload::destroy);
            if (!task.mHandle)
                task.mError = XFLOW_ERROR_NO_MEMORY;
        } catch (const std::bad_alloc&) {
            task.mError = XFLOW_ERROR_NO_MEMORY;
        } catch (...) {
            task.mError = XFLOW_ERROR_EXCEPTION;
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
    int enqueue(const Task& task) { return task.mError ? task.mError : XFlow_EnqueueTask(task.nativeHandle()); }
    int enqueue(const Tasks& group) { return XFlow_EnqueueGroup(group.nativeHandle()); }
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
    int waitAll() { return XFlow_WaitAll(); }
    int join(const Task& task) { return wait(task); }
    int join(const Tasks& group) { return wait(group); }
    int joinAll() { return waitAll(); }
    template <class F>
    void parallelizeTiledTasks(size_t range, size_t tile, F&& f)
    {
        typedef std::reference_wrapper<typename std::remove_reference<F>::type> Function;
        Function                                                                callback(f);
        XFlow_ParallelizeTile(
            range, tile, [](size_t begin, size_t end, void* data) { (*static_cast<Function*>(data))(begin, end); },
            &callback);
    }
    int setAffinity(AffinityPolicy policy, uint64_t mask = 0)
    {
        return XFlow_SetAffinity(static_cast<int>(policy), mask);
    }
    uint64_t getAffinityMask() const { return XFlow_GetAffinityMask(); }
    size_t   workerCount() const { return XFlow_GetWorkerCount(); }

private:
    Flow()                       = default;
    ~Flow()                      = default;
    Flow(const Flow&)            = delete;
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
