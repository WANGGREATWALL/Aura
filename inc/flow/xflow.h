#ifndef AURA_FLOW_XFLOW_H_
#define AURA_FLOW_XFLOW_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "flow/xflow_api.h"
#include "flow/xflow_types.h"
#include "log/xerror.h"
#include "sys/xvisibility.h"

namespace au {
namespace flow AU_LOCAL {

class XFlow;
class Tasks;

namespace detail {
inline void copyResult(const au_flow_result& in, au::flow::TaskResult& out) noexcept
{
    out.mStructSize = sizeof(out);
    out.mState = static_cast<au::flow::TaskState>(in.mState);
    out.mDomain = static_cast<au::flow::ErrorDomain>(in.mDomain);
    out.mCode = in.mCode;
    out.mNativeCode = in.mNativeCode;
    out.mCauseTaskId = in.mCauseTaskId;
}

template <std::size_t... I> struct Indices {};
template <std::size_t N, std::size_t... I>
struct MakeIndices : au::flow::detail::MakeIndices<N - 1, N - 1, I...> {};
template <std::size_t... I>
struct MakeIndices<0, I...> { typedef au::flow::detail::Indices<I...> type; };

template <class F, class... A>
auto invoke(F&& f, A&&... a)
    -> typename std::enable_if<!std::is_member_pointer<typename std::decay<F>::type>::value,
        decltype(std::forward<F>(f)(std::forward<A>(a)...))>::type
{ return std::forward<F>(f)(std::forward<A>(a)...); }

template <class F, class... A>
auto invoke(F&& f, A&&... a)
    -> typename std::enable_if<std::is_member_pointer<typename std::decay<F>::type>::value,
        decltype(std::mem_fn(f)(std::forward<A>(a)...))>::type
{ return std::mem_fn(f)(std::forward<A>(a)...); }

// Own raw binding storage only: a failed placement construction must not call
// the callable's destructor. au_flow_make takes over the constructed payload.
template <class T> struct PayloadDeleter {
    void operator()(void* payload) const noexcept
    { au_flow_payload_release(payload, sizeof(T), alignof(T)); }
};
template <class F, class... A> struct Bound {
    F mFunction;
    std::tuple<A...> mArguments;
    template <class G, class... B>
    explicit Bound(G&& f, B&&... a)
        : mFunction(std::forward<G>(f)), mArguments(std::forward<B>(a)...) {}

    template <std::size_t... I>
    int call(au::flow::detail::Indices<I...>)
    {
        static_assert(std::is_same<decltype(au::flow::detail::invoke(
            std::move(mFunction), std::move(std::get<I>(mArguments))...)), int>::value,
            "A flow task must return int; borrow lvalue-only callables with std::ref");
        return au::flow::detail::invoke(std::move(mFunction),
                                        std::move(std::get<I>(mArguments))...);
    }
    static int run(void* p) noexcept
    {
        try {
            return static_cast<au::flow::detail::Bound<F, A...>*>(p)->call(
                typename au::flow::detail::MakeIndices<sizeof...(A)>::type());
        } catch (...) { au_flow_record_exception(); return au::flow::kErrorUnhandledException; }
    }
    static void destroy(void* p) noexcept
    {
        static_cast<au::flow::detail::Bound<F, A...>*>(p)->~Bound();
        au::flow::detail::PayloadDeleter<au::flow::detail::Bound<F, A...>>()(p);
    }
};

template <class R> struct TileResult {
    template <class F> static int call(F& f, std::size_t i, std::size_t n)
    {
        static_assert(std::is_same<R, int>::value, "A tile callback must return void or int");
        return f(i, n);
    }
};
template <> struct TileResult<void> {
    template <class F> static int call(F& f, std::size_t i, std::size_t n)
    { f(i, n); return 0; }
};
template <class F> struct Tile {
    static int run(void* p, std::size_t i, std::size_t n) noexcept
    {
        try {
            F& f = *static_cast<F*>(p);
            return au::flow::detail::TileResult<decltype(f(i, n))>::call(f, i, n);
        } catch (...) { au_flow_record_exception(); return au::flow::kErrorUnhandledException; }
    }
};
} // namespace detail

// Header-only facade: only opaque identity and an allocation-free rejection value.
class Task {
public:
    Task() noexcept : mHandle(nullptr), mRejection(0) {}
    ~Task() noexcept { au_flow_release(mHandle); }
    Task(const au::flow::Task& other) noexcept
        : mHandle(other.mHandle), mRejection(other.mRejection) { au_flow_retain(mHandle); }
    Task(au::flow::Task&& other) noexcept
        : mHandle(other.mHandle), mRejection(other.mRejection)
    { other.mHandle = nullptr; other.mRejection = 0; }
    au::flow::Task& operator=(au::flow::Task other) noexcept
    {
        std::swap(mHandle, other.mHandle); std::swap(mRejection, other.mRejection);
        return *this;
    }
    bool valid() const noexcept { return mHandle != nullptr || mRejection != 0; }
    std::uint64_t id() const noexcept { return au_flow_id(mHandle); }
    au::flow::TaskState state() const noexcept
    {
        return mRejection ? au::flow::TaskState::Rejected
            : static_cast<au::flow::TaskState>(au_flow_state(mHandle));
    }
    int submissionCode() const noexcept
    { return mRejection ? mRejection : au_flow_submission(mHandle); }
    int result(au::flow::TaskResult& out) const noexcept
    {
        if (mRejection) {
            out = {sizeof(out), au::flow::TaskState::Rejected, mRejection == au::err::kErrorNoMemory
                       ? au::flow::ErrorDomain::Memory : au::flow::ErrorDomain::Flow,
                   mRejection, 0, 0};
            return 0;
        }
        au_flow_result value = {}; value.mStructSize = sizeof(value);
        const int ret = au_flow_result_get(mHandle, &value);
        if (!ret) au::flow::detail::copyResult(value, out);
        return ret;
    }
    bool ready() const noexcept { return state() >= au::flow::TaskState::Succeeded; }
    // Nonblocking: NotReady until terminal; then the original task error code.
    int code() const noexcept
    {
        au::flow::TaskResult value = {};
        const int ret = result(value);
        return ret ? ret : value.mCode;
    }
    // All C++ wait methods return the task code after successful completion.
    // A wait failure leaves out unchanged; a terminal result fills its details.
    int wait(au::flow::TaskResult* out = nullptr) const noexcept
    { return waitFor(UINT64_MAX, out); }
    int waitFor(std::uint64_t timeoutMs, au::flow::TaskResult* out = nullptr) const noexcept
    {
        if (mRejection) {
            if (out) (void)result(*out);
            return mRejection;
        }
        au_flow_result value = {}; value.mStructSize = sizeof(value);
        const int ret = au_flow_wait(mHandle, timeoutMs, &value);
        if (ret) return ret; // Leave details unchanged when the wait did not complete.
        if (out) au::flow::detail::copyResult(value, *out);
        return value.mCode;
    }
    int setName(const char* name) noexcept { return mRejection ? mRejection : au_flow_name(mHandle, name); }
    int precede(const au::flow::Task& next) const noexcept
    {
        if (mRejection) return au_flow_builder_error(next.mHandle, mRejection);
        if (next.mRejection) return next.mRejection;
        return au_flow_precede(mHandle, next.mHandle);
    }
    int precede(au::flow::Tasks& next) const noexcept;
private:
    friend class au::flow::Tasks;
    friend class au::flow::XFlow;
    explicit Task(au_flow_handle h, int error = 0) noexcept : mHandle(h), mRejection(error) {}
    au_flow_handle mHandle;
    int mRejection;
};

class Tasks {
public:
    Tasks() noexcept : mTask(), mOwnsBuilder(true)
    { mTask.mRejection = au_flow_group(&mTask.mHandle); }
    Tasks(std::initializer_list<au::flow::Task> tasks) noexcept : Tasks()
    { for (const auto& task : tasks) if (add(task)) break; }
    ~Tasks() noexcept { if (mOwnsBuilder) au_flow_abandon(mTask.mHandle); }
    Tasks(const au::flow::Tasks&) = delete;
    au::flow::Tasks& operator=(const au::flow::Tasks&) = delete;
    Tasks(au::flow::Tasks&& other) noexcept
        : mTask(std::move(other.mTask)), mOwnsBuilder(other.mOwnsBuilder)
    { other.mOwnsBuilder = false; }
    au::flow::Tasks& operator=(au::flow::Tasks&& other) noexcept
    {
        if (this != &other) {
            if (mOwnsBuilder) au_flow_abandon(mTask.mHandle);
            mTask = std::move(other.mTask); mOwnsBuilder = other.mOwnsBuilder;
            other.mOwnsBuilder = false;
        }
        return *this;
    }
    int add(const au::flow::Task& task) noexcept
    {
        if (mTask.mRejection) return mTask.mRejection;
        return task.mRejection ? au_flow_builder_error(mTask.mHandle, task.mRejection)
            : au_flow_add(mTask.mHandle, task.mHandle);
    }
    int add(au::flow::Tasks&& child) noexcept
    {
        const int ret = add(child.mTask);
        if (!ret) child.mOwnsBuilder = false;
        return ret;
    }
    au::flow::Task handle() const noexcept { return mTask; }
    int setName(const char* name) noexcept { return mTask.setName(name); }
    bool ready() const noexcept { return mTask.ready(); }
    int code() const noexcept { return mTask.code(); }
    int wait(au::flow::TaskResult* out = nullptr) const noexcept { return mTask.wait(out); }
    int waitFor(std::uint64_t timeoutMs, au::flow::TaskResult* out = nullptr) const noexcept
    { return mTask.waitFor(timeoutMs, out); }
    int precede(const au::flow::Task& next) const noexcept { return mTask.precede(next); }
    int precede(au::flow::Tasks& next) const noexcept { return mTask.precede(next.mTask); }
private:
    friend class au::flow::Task;
    friend class au::flow::XFlow;
    au::flow::Task mTask;
    bool mOwnsBuilder;
};
inline int Task::precede(au::flow::Tasks& next) const noexcept { return precede(next.mTask); }

class XFlow {
public:
    static au::flow::XFlow& get() noexcept;
    XFlow(const au::flow::XFlow&) = delete;
    au::flow::XFlow& operator=(const au::flow::XFlow&) = delete;

    template <class F, class... Args>
    typename std::enable_if<!std::is_same<typename std::decay<F>::type, au::flow::Task>::value &&
        !std::is_same<typename std::decay<F>::type, au::flow::Tasks>::value, au::flow::Task>::type
    makeTask(F&& f, Args&&... args) noexcept
    {
        try {
            typedef au::flow::detail::Bound<typename std::decay<F>::type,
                typename std::decay<Args>::type...> Payload;
            void* storage = nullptr;
            const int allocated = au_flow_payload_allocate(sizeof(Payload), alignof(Payload), &storage);
            if (allocated) return au::flow::Task(nullptr, allocated);
            std::unique_ptr<void, au::flow::detail::PayloadDeleter<Payload>> payload(storage);
            new (storage) Payload(std::forward<F>(f), std::forward<Args>(args)...);
            au_flow_handle h = nullptr;
            const int ret = au_flow_make(&Payload::run, &Payload::destroy, payload.release(), &h);
            return au::flow::Task(h, ret);
        } catch (const std::bad_alloc&) { return au::flow::Task(nullptr, au::err::kErrorNoMemory); }
        catch (...) { return au::flow::Task(nullptr, au::flow::kErrorUnhandledException); }
    }
    template <class F, class... Args>
    typename std::enable_if<!std::is_same<typename std::decay<F>::type, au::flow::Task>::value &&
        !std::is_same<typename std::decay<F>::type, au::flow::Tasks>::value, au::flow::Task>::type
    enqueue(F&& f, Args&&... args) noexcept
    { return enqueue(makeTask(std::forward<F>(f), std::forward<Args>(args)...)); }

    au::flow::Task enqueue(const au::flow::Task& task) noexcept
    {
        if (task.mRejection) { au_flow_record_error(task.mRejection); return task; }
        const int ret = au_flow_submit(task.mHandle);
        return !ret || task.state() == au::flow::TaskState::Rejected ? task : au::flow::Task(nullptr, ret);
    }
    au::flow::Task enqueue(au::flow::Tasks& tasks) noexcept { return enqueue(tasks.mTask); }
    au::flow::Task enqueue(au::flow::Tasks&& tasks) noexcept { return enqueue(tasks); }
    template <class F, class... Args>
    au::flow::Task enqueueAfter(const au::flow::Task& before, F&& f, Args&&... args) noexcept
    { return enqueueAfter({before}, std::forward<F>(f), std::forward<Args>(args)...); }
    // Every predecessor must already be accepted. Empty lists submit a root;
    // repeated predecessors are harmless. An invalid member rejects the target.
    template <class F, class... Args>
    au::flow::Task enqueueAfter(std::initializer_list<au::flow::Task> before,
                                F&& f, Args&&... args) noexcept
    {
        au::flow::Task task = makeTask(std::forward<F>(f), std::forward<Args>(args)...);
        if (!task.mRejection) for (const auto& predecessor : before) {
            const int ret = predecessor.precede(task);
            if (ret) { (void)au_flow_builder_error(task.mHandle, ret); break; }
        }
        return enqueue(task);
    }
    template <class F, class... Args>
    au::flow::Task enqueueAfter(const au::flow::Tasks& before, F&& f, Args&&... args) noexcept
    { return enqueueAfter(before.mTask, std::forward<F>(f), std::forward<Args>(args)...); }
    int wait(const au::flow::Task& task, au::flow::TaskResult* out = nullptr) noexcept
    { return task.wait(out); }
    int wait(const au::flow::Tasks& tasks, au::flow::TaskResult* out = nullptr) noexcept
    { return tasks.wait(out); }
    int waitFor(const au::flow::Task& task, std::uint64_t timeoutMs,
                au::flow::TaskResult* out = nullptr) noexcept
    { return task.waitFor(timeoutMs, out); }
    int waitFor(const au::flow::Tasks& tasks, std::uint64_t timeoutMs,
                au::flow::TaskResult* out = nullptr) noexcept
    { return tasks.waitFor(timeoutMs, out); }
    int waitAll() noexcept { return au_flow_wait_all(); }
    // Synchronous: one borrowed callback is shared by all participants until
    // they join. Mutable state inside it must support concurrent invocation.
    template <class F>
    int parallelizeTiledTasks(std::size_t range, std::size_t tile, F&& f) noexcept
    {
        auto callback = std::ref(f); // Also wraps ordinary functions without casting a function pointer to void*.
        return au_flow_tiled(range, tile, &au::flow::detail::Tile<decltype(callback)>::run, &callback);
    }
    int setAffinity(const au::flow::AffinityPolicy& policy) noexcept
    { return au_flow_affinity(static_cast<std::uint32_t>(policy.mMode), policy.mCpuIds, policy.mCount); }
    int getWorkers(au::flow::WorkerInfo* out, std::size_t capacity, std::size_t& count) const noexcept;
    int getWorkerAffinity(std::uint32_t workerId, std::uint32_t* cpus, std::size_t capacity,
                          std::size_t& count) const noexcept
    { return au_flow_worker_affinity(workerId, cpus, capacity, &count); }
    // Opt-in bounded host records and retained dynamic-child metadata. Capture
    // names are copied (64-byte trace labels can be marked truncated). DOT keeps
    // static groups independently; dynamic structure is a per-node snapshot.
    // CPU slices exclude known scheduler/device waits, not arbitrary user I/O
    // or OS preemption. Toggle boundaries may leave partial trace slices.
    int setTracing(bool enabled) noexcept { return au_flow_tracing(enabled ? 1 : 0); }
    int dumpDot(const au::flow::Task& task, const char* path) const noexcept
    { return au_flow_dot(task.mHandle, path); }
    int dumpDot(const au::flow::Tasks& tasks, const char* path) const noexcept
    { return dumpDot(tasks.mTask, path); }
    int dumpTrace(const char* path) noexcept { return au_flow_trace(path); }
    int shutdown() noexcept { return au_flow_shutdown(); }
private:
    XFlow() noexcept = default;
};

} // namespace flow
} // namespace au

#define XTHREAD_PARALLELIZE_TILED_TASK_QUOTE(RANGE, TILE) \
    au::flow::XFlow::get().parallelizeTiledTasks((RANGE), (TILE), [&](std::size_t _i, std::size_t _ti) {
#define XTHREAD_PARALLELIZE_TILED_TASK_ASSIGN(RANGE, TILE) \
    au::flow::XFlow::get().parallelizeTiledTasks((RANGE), (TILE), [=](std::size_t _i, std::size_t _ti) {
#define XTHREAD_PARALLELIZE_END });
#endif
