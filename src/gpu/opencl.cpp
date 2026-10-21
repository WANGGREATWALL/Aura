#include "gpu_helper/cl_symbols.h"
#include "gpu/xopencl_api.h"
#include "gpu/opencl_api.h"
#include "gpu/program_cache.h"
#include "file/xfile.h"
#include "gpu/pending.h"
#include "CL/cl_ext.h"
#include "mm/access.h"
#include "mm/registry.h"
#include "sys/opencl_memory.h"
#include "log/xerror.h"
#include "log/xlogger.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {
au::gpu::detail::PendingStatus nativeStatus(cl_int code) noexcept
{
    if (!code) return {};
    return {code == CL_OUT_OF_HOST_MEMORY ? au::err::kErrorNoMemory : au::err::kErrorPlatformAPI,
            code, au::gpu::detail::ErrorDomain::OpenCL};
}
int report(const au::gpu::detail::PendingStatus& status) noexcept
{
    return status.mCode;
}
template<class F> int boundary(F&& callable) noexcept
{
    try {
        const int code = callable();
        return code ? report({code, 0, code == au::err::kErrorNoMemory ? au::gpu::detail::ErrorDomain::Memory : au::gpu::detail::ErrorDomain::OpenCL}) : 0;
    }
    catch (const std::bad_alloc&) { return report({au::err::kErrorNoMemory, 0, au::gpu::detail::ErrorDomain::Memory}); }
    catch (const std::system_error& error) { return report({au::err::kErrorPlatformAPI, error.code().value(), au::gpu::detail::ErrorDomain::System}); }
    catch (...) { return report({au::err::kErrorUnknown, 0, au::gpu::detail::ErrorDomain::System}); }
}
template<class T> class Object {
public:
    T mValue = nullptr;
    cl_int (CL_API_CALL *mRelease)(T) = nullptr;
    Object() noexcept = default;
    Object(T value, cl_int (CL_API_CALL *release)(T)) noexcept : mValue(value), mRelease(release) {}
    ~Object() noexcept { (void)reset(); }
    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;
    Object(Object&& other) noexcept : mValue(other.mValue), mRelease(other.mRelease) { other.mValue = nullptr; }
    Object& operator=(Object&& other) noexcept
    { if (this != &other) { (void)reset(); mValue = other.mValue; mRelease = other.mRelease; other.mValue = nullptr; } return *this; }
    cl_int reset() noexcept { const auto value = mValue; mValue = nullptr; return value && mRelease ? mRelease(value) : CL_SUCCESS; }
};
struct KernelSlot {
    KernelSlot() noexcept = default;
    KernelSlot(const KernelSlot&) = delete;
    KernelSlot& operator=(const KernelSlot&) = delete;
    KernelSlot(KernelSlot&&) noexcept = default;
    KernelSlot& operator=(KernelSlot&&) noexcept = default;
    Object<cl_kernel> mKernel;
    std::unique_ptr<KernelSlot> mNext;
};
struct CompiledProgram {
    Object<cl_program> mProgram;
    std::string mSource, mOptions;
};
struct Program {
    Program() = default;
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&&) = delete; // Stable mutex/cache identity; shared leases move instead.
    Program& operator=(Program&&) = delete;
    std::shared_ptr<CompiledProgram> mCompiled; // Shared by named kernel families and concurrent native submissions.
    std::mutex mMutex;
    std::unique_ptr<KernelSlot> mFree;
    ~Program() noexcept
    { while (mFree) { auto slot = std::move(mFree); mFree = std::move(slot->mNext); } }
    void recycle(std::unique_ptr<KernelSlot> slot) noexcept
    {
        if (!slot) return;
        std::lock_guard<std::mutex> lock(mMutex);
        slot->mNext = std::move(mFree); mFree = std::move(slot); // No driver calls or allocation under the cache lock.
    }
};
struct ResourceKey {
    std::uintptr_t mOwner = 0, mContextGeneration = 0;
    std::uint64_t mAllocationId = 0, mAllocationGeneration = 0;
    std::size_t mOffset = 0, mWidth = 0, mHeight = 0;
    std::uint32_t mOrder = 0, mType = 0;
    bool mImage = false;
    bool operator<(const ResourceKey& other) const noexcept
    {
        return std::tie(mOwner, mAllocationId, mAllocationGeneration, mContextGeneration, mImage,
                        mOffset, mWidth, mHeight, mOrder, mType) <
               std::tie(other.mOwner, other.mAllocationId, other.mAllocationGeneration, other.mContextGeneration, other.mImage,
                        other.mOffset, other.mWidth, other.mHeight, other.mOrder, other.mType);
    }
};
struct CachedMemory {
    CachedMemory() noexcept = default;
    CachedMemory(const CachedMemory&) = delete;
    CachedMemory& operator=(const CachedMemory&) = delete;
    CachedMemory(CachedMemory&&) = delete; // Cache and active submissions share a stable native resource.
    CachedMemory& operator=(CachedMemory&&) = delete;
    Object<cl_mem> mMemory;
    std::weak_ptr<au::mm::detail::MemoryControl> mControl; // Never pin the host allocation or its fence through the cache.
    int mOwnedFd = -1;
    bool mImported = false;
    std::size_t mPixelBytes = 0;
    ~CachedMemory() noexcept
    { (void)mMemory.reset(); if (mOwnedFd >= 0) au::sys::memory::closeFd(mOwnedFd); }
};
struct State {
    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete; // Context generations have stable shared identity.
    State& operator=(State&&) = delete;
    const au::gpu::detail::NativeApi* mApi = nullptr;
    Object<cl_context> mContext;
    Object<cl_command_queue> mQueue;
    cl_platform_id mPlatform = nullptr;
    cl_device_id mDevice = nullptr;
    au::sys::opencl::ImportCapabilities mImports;
    std::uintptr_t mGeneration = 0;
    std::mutex mCacheMutex;
    std::map<std::string, std::shared_ptr<Program>> mPrograms; // Cache and asynchronous submissions concurrently retain immutable programs.
    std::map<std::pair<std::string, std::string>, std::shared_ptr<CompiledProgram>> mCompiled;
    std::string mBinaryDirectory, mBinaryDevice;
    std::map<ResourceKey, std::shared_ptr<CachedMemory>> mResources; // Native objects shared by cache lookup and in-flight submissions.
    ResourceKey mPruneAfter{};
    bool mHasPruneAfter = false;
    void pruneResources() noexcept
    {
        // Fixed work per ordinary entry/completion, with all native destruction
        // after releasing the cache lock. Expired controls cannot be resurrected.
        std::array<decltype(mResources)::node_type, 16> retired;
        {
            std::lock_guard<std::mutex> lock(mCacheMutex);
            auto cursor = mHasPruneAfter ? mResources.upper_bound(mPruneAfter) : mResources.begin();
            if (cursor == mResources.end()) cursor = mResources.begin();
            for (std::size_t i = 0; i < retired.size() && cursor != mResources.end(); ++i) {
                mPruneAfter = cursor->first; mHasPruneAfter = true;
                auto current = cursor++;
                if (current->second->mControl.expired()) retired[i] = mResources.extract(current);
            }
        }
    }
    // Resource/program/queue/context teardown runs without cache locks.
};
struct Transfer {
    Transfer() noexcept = default;
    Transfer(const Transfer&) = delete;
    Transfer& operator=(const Transfer&) = delete;
    Transfer(Transfer&&) noexcept = default;
    Transfer& operator=(Transfer&&) noexcept = default;
    au::mm::detail::MemoryControl* mControl = nullptr; // DeviceAccess owns the storage lease.
    Object<cl_mem> mMemory;
    std::unique_ptr<unsigned char[]> mBytes; // Fully initialized by host copy before any nonblocking native use.
    std::size_t mByteCount = 0;
    unsigned int mMode = 0;
    std::size_t mOffset = 0, mWidth = 0, mHeight = 0, mPitch = 0, mPixelBytes = 0;
    std::uint32_t mOrder = 0, mType = 0;
    bool mImage = false;
    bool mImported = false;
};
struct NativeEventLease { Object<cl_event> mEvent; };
struct Submission {
    Submission() = default;
    Submission(const Submission&) = delete;
    Submission& operator=(const Submission&) = delete;
    Submission(Submission&&) = delete; // Pending provider operations address this stable record until finalization.
    Submission& operator=(Submission&&) = delete;
    std::shared_ptr<State> mState; // Shared across active wrapper entries and asynchronous native commands.
    std::shared_ptr<Program> mProgram;
    std::unique_ptr<KernelSlot> mKernel;
    au::mm::detail::DeviceAccess mAccess;
    std::vector<Transfer> mTransfers;
    std::vector<Object<cl_mem>> mNative;
    std::vector<std::size_t> mArgumentTransfers;
    Object<cl_event> mEvent;
    std::shared_ptr<NativeEventLease> mNativeFence; // Fence/submitter race over an immutable event lease, without mm leases.
    std::vector<cl_event> mWaitEvents; // Previous fences in DeviceAccess retain all handles, including duplicates removed here.
    bool mKernelAccepted = false;
    bool mAccessPrepared = false;
    bool mNeedsQueueProof = false; // A nonconforming success-without-event cannot be treated as a rejected command.
    ~Submission() noexcept { if (mProgram) mProgram->recycle(std::move(mKernel)); }
    static int query(void* payload, bool* done, au::gpu::detail::PendingStatus* status) noexcept
    {
        auto& self = *static_cast<Submission*>(payload);
        const auto& api = *self.mState->mApi;
        cl_int execution = CL_QUEUED;
        cl_int code = CL_SUCCESS;
        if (self.mNeedsQueueProof) {
            code = api.mFinish(self.mState->mQueue.mValue); // Fault path only; no event can prove this command stopped.
            if (code) { *status = nativeStatus(code); return status->mCode; }
            execution = CL_COMPLETE;
        } else {
            code = api.mGetEventInfo(self.mEvent.mValue, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(execution), &execution, nullptr);
            if (code) { *status = nativeStatus(code); return status->mCode; }
            if (execution > CL_COMPLETE) return 0;
        }
        // The registry advances predecessor records without recursive queries.
        for (const auto& entry : self.mAccess.entries()) if (entry.mPrevious && entry.mPrevious->mNativeEvent &&
            !entry.mPrevious->mHostCompletion && entry.mPrevious->mContext == reinterpret_cast<std::uintptr_t>(self.mState->mContext.mValue) &&
            !entry.mPrevious->ready()) return 0;
        au::gpu::detail::PendingStatus predecessor;
        for (const auto& entry : self.mAccess.entries()) if (entry.mPrevious && entry.mPrevious->ready()) {
            const auto prior = entry.mPrevious->wait(); // Cached ready observation; no native/host wait in cleanup.
            if (!predecessor.mCode && prior.mCode) predecessor = {prior.mCode, prior.mNativeCode,
                prior.mDeviceError ? au::gpu::detail::ErrorDomain::OpenCL : au::gpu::detail::ErrorDomain::Memory, prior.mCauseTaskId};
        }
        // clGetEventInfo is deliberately not a host synchronization point. Wait
        // even after observing CL_COMPLETE before consuming readback bytes.
        if (!self.mNeedsQueueProof) code = api.mWaitForEvents(1, &self.mEvent.mValue);
        *done = true; // The native execution status already proved termination.
        *status = predecessor.mCode ? predecessor : nativeStatus(execution < 0 ? execution : code);
        return 0;
    }
    static au::gpu::detail::PendingStatus finalize(void* payload, const au::gpu::detail::PendingStatus& incoming) noexcept
    {
        auto& self = *static_cast<Submission*>(payload);
        auto status = incoming;
        for (auto& transfer : self.mTransfers) {
            if (!status.mCode && self.mKernelAccepted && !transfer.mImported && (transfer.mMode & AU_MM_WRITE)) {
                int native = 0;
                int code = au::mm::detail::syncCpuAccess(transfer.mControl, AU_MM_WRITE, true, &native);
                if (!code) {
                    auto* destination = static_cast<unsigned char*>(transfer.mControl->mBlock.ptr) + transfer.mOffset;
                    if (transfer.mImage) {
                        for (std::size_t y = 0; y < transfer.mHeight; ++y)
                            std::memcpy(destination + y * transfer.mPitch, transfer.mBytes.get() + y * transfer.mWidth * transfer.mPixelBytes,
                                        transfer.mWidth * transfer.mPixelBytes);
                    } else std::memcpy(destination, transfer.mBytes.get(), transfer.mByteCount);
                    code = au::mm::detail::syncCpuAccess(transfer.mControl, AU_MM_WRITE, false, &native);
                    if (code) { // A failed END poisons future access, even if a prior task result is no longer retained.
                        std::lock_guard<std::mutex> lock(transfer.mControl->mAccessMutex);
                        transfer.mControl->mAccessError = code; transfer.mControl->mAccessNativeError = native;
                    }
                }
                if (code) status = {code, native, au::gpu::detail::ErrorDomain::Memory};
            }
            const auto release = nativeStatus(transfer.mMemory.reset());
            if (!status.mCode) status = release;
        }
        for (auto& native : self.mNative) { const auto release = nativeStatus(native.reset()); if (!status.mCode) status = release; }
        const auto eventRelease = nativeStatus(self.mEvent.reset());
        if (!status.mCode) status = eventRelease;
        const int cleanup = self.mAccess.releaseCompleted({status.mCode, status.mNativeCode, 0,
            status.mDomain == au::gpu::detail::ErrorDomain::OpenCL});
        if (!status.mCode && cleanup) status = {cleanup, self.mAccess.nativeCode(), au::gpu::detail::ErrorDomain::Memory};
        self.mState->pruneResources();
        return status;
    }
    static void destroy(void* payload) noexcept { delete static_cast<Submission*>(payload); }
};
const au::gpu::detail::PendingOps gOps{&Submission::query, &Submission::finalize, &Submission::destroy};

bool multiply(std::size_t first, std::size_t second, std::size_t& result) noexcept
{ if (first && second > std::numeric_limits<std::size_t>::max() / first) return false; result = first * second; return true; }
au::gpu::detail::PendingStatus getProgram(const std::shared_ptr<State>& state, const char* name, const char* source,
    const char* options, std::shared_ptr<Program>& out)
{
    if (!name || !*name) return {au::err::kErrorInvalidParam, 0, au::gpu::detail::ErrorDomain::OpenCL};
    {
        std::lock_guard<std::mutex> lock(state->mCacheMutex);
        const auto found = state->mPrograms.find(name);
        if (found != state->mPrograms.end()) out = found->second;
    }
    if (out) {
        if ((source && out->mCompiled->mSource != source) || (options && out->mCompiled->mOptions != options))
            return {au::err::kErrorAlreadyExists, 0, au::gpu::detail::ErrorDomain::OpenCL};
        return {};
    }
    if (!source || !*source) return {au::err::kErrorNotInitialized, 0, au::gpu::detail::ErrorDomain::OpenCL};
    const std::pair<std::string, std::string> sourceKey{source, options ? options : "-cl-std=CL1.2"};
    std::shared_ptr<CompiledProgram> compiled;
    {
        std::lock_guard<std::mutex> lock(state->mCacheMutex);
        const auto found = state->mCompiled.find(sourceKey);
        if (found != state->mCompiled.end()) compiled = found->second;
    }
    if (!compiled) {
        auto candidate = std::make_shared<CompiledProgram>();
        candidate->mSource = sourceKey.first; candidate->mOptions = sourceKey.second;
        std::string cacheKey, cachePath;
        cl_int native = CL_SUCCESS;
        if (!state->mBinaryDirectory.empty()) {
            cacheKey = au::gpu::detail::binaryKey(state->mBinaryDevice, candidate->mSource, candidate->mOptions);
            cachePath = au::gpu::detail::binaryPath(state->mBinaryDirectory, cacheKey);
            std::vector<unsigned char> binary;
            if (au::gpu::detail::readBinary(cachePath, cacheKey, binary)) {
                const auto length = binary.size(); const auto* bytes = binary.data(); cl_int binaryStatus = CL_SUCCESS;
                candidate->mProgram = Object<cl_program>(state->mApi->mCreateProgramWithBinary(state->mContext.mValue,
                    1, &state->mDevice, &length, &bytes, &binaryStatus, &native), state->mApi->mReleaseProgram);
                if (!native && !binaryStatus && candidate->mProgram.mValue)
                    native = state->mApi->mBuildProgram(candidate->mProgram.mValue, 1, &state->mDevice, candidate->mOptions.c_str(), nullptr, nullptr);
                if (native || binaryStatus || !candidate->mProgram.mValue) (void)candidate->mProgram.reset();
            }
        }
        if (!candidate->mProgram.mValue) {
            const std::size_t length = candidate->mSource.size(); const char* text = candidate->mSource.c_str();
            candidate->mProgram = Object<cl_program>(state->mApi->mCreateProgramWithSource(state->mContext.mValue, 1, &text, &length, &native),
                                                   state->mApi->mReleaseProgram);
            if (native || !candidate->mProgram.mValue) return nativeStatus(native ? native : CL_INVALID_PROGRAM);
            native = state->mApi->mBuildProgram(candidate->mProgram.mValue, 1, &state->mDevice, candidate->mOptions.c_str(), nullptr, nullptr);
            if (native) {
                std::size_t size = 0;
                if (!state->mApi->mGetProgramBuildInfo(candidate->mProgram.mValue, state->mDevice, CL_PROGRAM_BUILD_LOG, 0, nullptr, &size) &&
                    size && size < std::numeric_limits<std::size_t>::max()) {
                    std::vector<char> log(size + 1, '\0');
                    if (!state->mApi->mGetProgramBuildInfo(candidate->mProgram.mValue, state->mDevice, CL_PROGRAM_BUILD_LOG, size, log.data(), nullptr))
                        XLOG_E("OpenCL kernel=%s build failed native=%d: %s\n", name, native, log.data());
                }
                return nativeStatus(native);
            }
            if (!cachePath.empty()) au::gpu::detail::writeBinary(cachePath, cacheKey, *state->mApi, candidate->mProgram.mValue);
        }
        std::map<std::pair<std::string, std::string>, std::shared_ptr<CompiledProgram>> prepared;
        prepared.emplace(sourceKey, candidate); auto node = prepared.extract(prepared.begin());
        {
            std::lock_guard<std::mutex> lock(state->mCacheMutex);
            auto inserted = state->mCompiled.insert(std::move(node));
            compiled = inserted.position->second; node = std::move(inserted.node);
        }
    }
    auto program = std::make_shared<Program>(); program->mCompiled = std::move(compiled);
    // Allocate the associative node before taking the cache lock. Concurrent
    // cold compilers may duplicate preparation; only one immutable record wins.
    std::map<std::string, std::shared_ptr<Program>> prepared;
    prepared.emplace(name, program);
    auto node = prepared.extract(prepared.begin());
    {
        std::lock_guard<std::mutex> lock(state->mCacheMutex);
        auto inserted = state->mPrograms.insert(std::move(node));
        out = inserted.position->second;
        node = std::move(inserted.node);
    }
    if (out->mCompiled->mSource != program->mCompiled->mSource || out->mCompiled->mOptions != program->mCompiled->mOptions)
        return {au::err::kErrorAlreadyExists, 0, au::gpu::detail::ErrorDomain::OpenCL};
    return {};
}
au::gpu::detail::PendingStatus leaseKernel(Submission& submission, const char* name)
{
    {
        std::lock_guard<std::mutex> lock(submission.mProgram->mMutex);
        if (submission.mProgram->mFree) {
            submission.mKernel = std::move(submission.mProgram->mFree);
            submission.mProgram->mFree = std::move(submission.mKernel->mNext);
        }
    }
    if (submission.mKernel) return {};
    auto slot = std::make_unique<KernelSlot>();
    cl_int code = CL_SUCCESS;
    slot->mKernel = Object<cl_kernel>(submission.mState->mApi->mCreateKernel(submission.mProgram->mCompiled->mProgram.mValue, name, &code),
                                      submission.mState->mApi->mReleaseKernel);
    if (code || !slot->mKernel.mValue) return nativeStatus(code ? code : CL_INVALID_KERNEL);
    submission.mKernel = std::move(slot); return {};
}
int validateRange(const ::au_gpu_range* range) noexcept
{
    if (!range || range->mStructSize != sizeof(*range)) return au::err::kErrorVersionMismatch;
    if (!range->mDimensions || range->mDimensions > 3) return au::err::kErrorInvalidParam;
    const bool local = range->mLocal[0] != 0;
    for (std::size_t index = 0; index < range->mDimensions; ++index) {
        if (!range->mGlobal[index] || local != (range->mLocal[index] != 0)) return au::err::kErrorInvalidParam;
        if (range->mGlobal[index] > std::numeric_limits<std::size_t>::max() - range->mOffset[index]) return au::err::kErrorOverflow;
        if (local && range->mGlobal[index] % range->mLocal[index]) return au::err::kErrorInvalidParam; // OpenCL 1.2 uniform groups.
    }
    return 0;
}

class Backend final {
public:
    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&&) = delete; // Stable per-module registry/lifecycle identity.
    Backend& operator=(Backend&&) = delete;
    std::mutex mMutex;
    std::condition_variable mIdle;
    std::shared_ptr<State> mState;
    au::gpu::detail::PendingRegistry mPending;
    const au::gpu::detail::NativeApi* mTestApi = nullptr;
    std::size_t mEntrants = 0;
    std::uintptr_t mGeneration = 0;
    enum class Phase { Empty, Starting, Running, Stopping } mPhase = Phase::Empty;
    ~Backend() noexcept
    {
        (void)stop(true);
    }
    struct Entry {
        Backend* mOwner = nullptr;
        std::shared_ptr<State> mState;
        ~Entry() noexcept {
            mState.reset(); // Finish state/native release before publishing that this entry has left.
            if (mOwner) { std::lock_guard<std::mutex> lock(mOwner->mMutex); --mOwner->mEntrants; mOwner->mIdle.notify_all(); }
        }
    };
    int enter(Entry& entry) noexcept
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mPhase != Phase::Running) return mPhase == Phase::Empty ? au::err::kErrorNotInitialized : au::err::kErrorBusy;
        if (mEntrants == std::numeric_limits<std::size_t>::max()) return au::err::kErrorOverflow;
        entry.mOwner = this; entry.mState = mState; ++mEntrants; return 0;
    }
    int init(bool profiling, const char* binaryDirectory)
    {
        const au::gpu::detail::NativeApi* api;
        std::uintptr_t generation;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mPhase != Phase::Empty) return au::err::kErrorAlreadyInited;
            if (mGeneration == std::numeric_limits<std::uintptr_t>::max()) return au::err::kErrorOverflow;
            mPhase = Phase::Starting; generation = ++mGeneration; api = mTestApi;
        }
        struct Rollback {
            Backend* mOwner;
            ~Rollback() noexcept { if (mOwner) { std::lock_guard<std::mutex> lock(mOwner->mMutex); mOwner->mPhase = Phase::Empty; } }
        } rollback{this};
        static const au::gpu::detail::NativeApi defaults;
        if (!api) {
            if (!au::gpu::CLSymbols::isLoaded()) return au::err::kErrorNotSupported;
            api = &defaults;
        }
        auto state = std::make_shared<State>(); state->mApi = api;
        // The owner address distinguishes simultaneous sibling contexts. This
        // generation prevents resource cache reuse after reinitialization.
        state->mGeneration = generation;
        cl_uint count = 0;
        cl_int native = api->mGetPlatformIDs(0, nullptr, &count);
        if (native == CL_PLATFORM_NOT_FOUND_KHR || native == CL_DEVICE_NOT_FOUND || (!native && !count))
            return report({au::err::kErrorNotSupported, native, au::gpu::detail::ErrorDomain::OpenCL});
        if (native) return report(nativeStatus(native));
        std::vector<cl_platform_id> platforms(count);
        native = api->mGetPlatformIDs(count, platforms.data(), nullptr);
        if (native) return report(nativeStatus(native));
        for (auto platform : platforms) {
            cl_device_id device = nullptr;
            native = api->mGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 1, &device, nullptr);
            if (native && native != CL_DEVICE_NOT_FOUND) return report(nativeStatus(native));
            if (!native && device) { state->mPlatform = platform; state->mDevice = device; break; }
        }
        if (!state->mDevice) return au::err::kErrorNotSupported;
        if (binaryDirectory && *binaryDirectory) {
            state->mBinaryDevice = au::gpu::detail::binaryDeviceKey(*api, state->mPlatform, state->mDevice);
            if (!state->mBinaryDevice.empty() && !au::file::createDirs(binaryDirectory)) state->mBinaryDirectory = binaryDirectory;
            else XLOG_W("OpenCL binary cache unavailable; using source compilation\n");
        }
        int importNative = 0;
        const int importCode = au::sys::opencl::queryImports(state->mPlatform, state->mDevice,
            api->mGetDeviceInfo, api->mGetExtensionFunctionAddressForPlatform, state->mImports, &importNative);
        if (importCode) return report({importCode, importNative, au::gpu::detail::ErrorDomain::OpenCL});
        const cl_context_properties properties[]{CL_CONTEXT_PLATFORM, reinterpret_cast<cl_context_properties>(state->mPlatform), 0};
        state->mContext = Object<cl_context>(api->mCreateContext(properties, 1, &state->mDevice, nullptr, nullptr, &native), api->mReleaseContext);
        if (native || !state->mContext.mValue) return report(nativeStatus(native ? native : CL_INVALID_CONTEXT));
        state->mQueue = Object<cl_command_queue>(api->mCreateCommandQueue(state->mContext.mValue, state->mDevice,
            profiling ? CL_QUEUE_PROFILING_ENABLE : 0, &native), api->mReleaseCommandQueue);
        if (native || !state->mQueue.mValue) return report(nativeStatus(native ? native : CL_INVALID_COMMAND_QUEUE));
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mState = std::move(state); mPhase = Phase::Running;
        }
        rollback.mOwner = nullptr; return 0;
    }
    int stop(bool deinit) noexcept
    {
        if (au::gpu::detail::pendingCleanupActive() || mPending.finalizingHere()) return au::err::kErrorBusy;
        std::shared_ptr<State> retired;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (mPhase == Phase::Empty) return 0;
            if (mPhase != Phase::Running) return au::err::kErrorBusy;
            mPhase = Phase::Stopping;
            mIdle.wait(lock, [this] { return !mEntrants; });
            retired = mState;
        }
        const auto flush = nativeStatus(retired->mApi->mFlush(retired->mQueue.mValue));
        const auto completion = mPending.drain(); // Never hold lifecycle/cache locks while querying or releasing resources.
        retired->pruneResources();
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (deinit) mState.reset(); // retired keeps the state alive until outside the lock.
            mPhase = deinit ? Phase::Empty : Phase::Running;
        }
        return report(flush.mCode ? flush : completion);
    }
};
Backend* backend(unsigned int id)
{
    if (id > AU_GPU_DMA_BUFFER) return nullptr;
    // Construct the native loader before wrapper states, so it outlives native
    // object teardown at normal static destruction too.
    (void)au::gpu::CLSymbols::isLoaded();
    (void)au::mm::Registry::instance(); // Resource cleanup outlives backend teardown.
    static std::array<Backend, 2> instances;
    return &instances[id];
}

au::gpu::detail::PendingStatus prepareArguments(Submission& submission, unsigned int backendId,
    const ::au_gpu_arg* args, std::size_t count)
{
    const auto& api = *submission.mState->mApi;
    std::vector<::au_mm_access_set> access;
    access.reserve(count); submission.mTransfers.reserve(count); submission.mNative.reserve(count);
    submission.mArgumentTransfers.assign(count, std::numeric_limits<std::size_t>::max());
    for (std::size_t index = 0; index < count; ++index) {
        if (args[index].mStructSize != sizeof(::au_gpu_arg)) return {au::err::kErrorVersionMismatch, 0, au::gpu::detail::ErrorDomain::OpenCL};
        if (args[index].mKind == AU_GPU_BUFFER || args[index].mKind == AU_GPU_IMAGE2D) access.push_back(args[index].mAccess);
        else if (args[index].mKind > AU_GPU_NATIVE_MEMORY) return {au::err::kErrorInvalidParam, 0, au::gpu::detail::ErrorDomain::OpenCL};
        else if ((args[index].mKind == AU_GPU_SCALAR && (!args[index].mValue || !args[index].mSize)) ||
                 (args[index].mKind == AU_GPU_LOCAL && !args[index].mSize))
            return {au::err::kErrorInvalidParam, 0, au::gpu::detail::ErrorDomain::OpenCL};
        else if (args[index].mKind == AU_GPU_NATIVE_MEMORY && !args[index].mValue) return nativeStatus(CL_INVALID_MEM_OBJECT);
    }
    int code = access.empty() ? 0 : submission.mAccess.begin(access.data(), access.size(),
        reinterpret_cast<std::uintptr_t>(submission.mState->mContext.mValue), true,
        backendId != AU_GPU_DMA_BUFFER || !submission.mState->mImports.mArmImport);
    if (code) return {code, submission.mAccess.nativeCode(), submission.mAccess.deviceError() ? au::gpu::detail::ErrorDomain::OpenCL : au::gpu::detail::ErrorDomain::Memory};
    submission.mAccessPrepared = !access.empty();
    submission.mNativeFence = std::make_shared<NativeEventLease>(); // Before any accepted native command.
    submission.mWaitEvents.reserve(submission.mAccess.entries().size());
    for (const auto& entry : submission.mAccess.entries()) if (entry.mPrevious && !entry.mPrevious->mHostCompletion &&
        entry.mPrevious->mContext == reinterpret_cast<std::uintptr_t>(submission.mState->mContext.mValue) && entry.mPrevious->mNativeEvent) {
        auto event = static_cast<cl_event>(entry.mPrevious->mNativeEvent);
        if (std::find(submission.mWaitEvents.begin(), submission.mWaitEvents.end(), event) == submission.mWaitEvents.end())
            submission.mWaitEvents.push_back(event);
    }
    if (submission.mWaitEvents.size() > std::numeric_limits<cl_uint>::max()) return {au::err::kErrorOverflow, 0, au::gpu::detail::ErrorDomain::OpenCL};
    std::size_t leaseIndex = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& arg = args[index];
        if (arg.mKind != AU_GPU_BUFFER && arg.mKind != AU_GPU_IMAGE2D) continue;
        const std::size_t ranges = arg.mAccess.mCount;
        if (!ranges || (arg.mKind == AU_GPU_IMAGE2D && ranges != 1)) return {au::err::kErrorInvalidParam, 0, au::gpu::detail::ErrorDomain::Memory};
        const auto& lease = submission.mAccess.leases()[leaseIndex];
        auto* control = lease.control();
        const std::size_t offset = submission.mAccess.leases()[leaseIndex].offset();
        unsigned int mode = 0;
        for (std::size_t range = 0; range < ranges; ++range) {
            if (submission.mAccess.leases()[leaseIndex++].control() != control)
                return {au::err::kErrorNotSupported, 0, au::gpu::detail::ErrorDomain::Memory};
            mode |= arg.mAccess.mRanges[range].mMode;
        }
        if (backendId == AU_GPU_DMA_BUFFER && (control->mBlock.fd < 0 || arg.mKind == AU_GPU_IMAGE2D))
            return {au::err::kErrorNotSupported, 0, au::gpu::detail::ErrorDomain::OpenCL};
        std::size_t slot = 0;
        for (; slot < submission.mTransfers.size(); ++slot) if (submission.mTransfers[slot].mControl == control) break;
        if (slot < submission.mTransfers.size()) {
            auto& existing = submission.mTransfers[slot];
            if (existing.mImage != (arg.mKind == AU_GPU_IMAGE2D) || (existing.mImage &&
                (existing.mOffset != offset || existing.mWidth != arg.mWidth || existing.mHeight != arg.mHeight ||
                 existing.mPitch != arg.mRowPitch || existing.mOrder != arg.mChannelOrder || existing.mType != arg.mChannelType)))
                return {au::err::kErrorNotSupported, 0, au::gpu::detail::ErrorDomain::OpenCL}; // Different native views cannot silently break in-place aliasing.
            if (existing.mImage) {
                // CL1.2 cannot promise same-image read/write kernel semantics.
                // Read aliases share one object; writable image aliases require
                // an explicit separate output. Buffer in-place aliases are supported.
                if ((existing.mMode | mode) & AU_MM_WRITE) return {au::err::kErrorNotSupported, 0, au::gpu::detail::ErrorDomain::OpenCL};
                const std::size_t footprint = (existing.mHeight - 1) * existing.mPitch + existing.mWidth * existing.mPixelBytes;
                if (footprint > arg.mAccess.mRanges[0].mSize) return {au::err::kErrorOutOfRange, 0, au::gpu::detail::ErrorDomain::Memory};
            }
            existing.mMode |= mode;
        } else {
            Transfer transfer; transfer.mControl = control; transfer.mMode = mode;
            transfer.mImage = arg.mKind == AU_GPU_IMAGE2D;
            std::size_t bytes = control->mBlock.size;
            const auto identity = lease.identity();
            const ResourceKey key{identity.mOwner, submission.mState->mGeneration, identity.mAllocationId, identity.mGeneration,
                transfer.mImage ? offset : 0, transfer.mImage ? arg.mWidth : 0, transfer.mImage ? arg.mHeight : 0,
                transfer.mImage ? arg.mChannelOrder : 0, transfer.mImage ? arg.mChannelType : 0, transfer.mImage};
            std::shared_ptr<CachedMemory> resource; // A cache hit remains alive across concurrent pruning/deinit.
            {
                std::lock_guard<std::mutex> lock(submission.mState->mCacheMutex);
                const auto found = submission.mState->mResources.find(key);
                if (found != submission.mState->mResources.end()) resource = found->second;
            }
            cl_int native = CL_SUCCESS;
            if (!resource) {
                resource = std::make_shared<CachedMemory>(); resource->mControl = lease.weakControl();
                if (transfer.mImage) {
                    // Validate the cheap lower bound before asking the driver for
                    // the actual format element size. The exact footprint follows.
                    std::size_t minimumRows = 0;
                    if (!arg.mWidth || !arg.mHeight || arg.mRowPitch < arg.mWidth ||
                        !multiply(arg.mHeight - 1, arg.mRowPitch, minimumRows) ||
                        minimumRows > std::numeric_limits<std::size_t>::max() - arg.mWidth ||
                        minimumRows + arg.mWidth > arg.mAccess.mRanges[0].mSize)
                        return {au::err::kErrorOutOfRange, 0, au::gpu::detail::ErrorDomain::Memory};
                    const cl_image_format format{arg.mChannelOrder, arg.mChannelType};
                    cl_image_desc description{}; description.image_type = CL_MEM_OBJECT_IMAGE2D;
                    description.image_width = arg.mWidth; description.image_height = arg.mHeight;
                    resource->mMemory = Object<cl_mem>(api.mCreateImage(submission.mState->mContext.mValue, CL_MEM_READ_WRITE,
                        &format, &description, nullptr, &native), api.mReleaseMemObject);
                    if (native || !resource->mMemory.mValue) return nativeStatus(native ? native : CL_INVALID_MEM_OBJECT);
                    native = api.mGetImageInfo(resource->mMemory.mValue, CL_IMAGE_ELEMENT_SIZE,
                        sizeof(resource->mPixelBytes), &resource->mPixelBytes, nullptr);
                    if (native) return nativeStatus(native);
                } else {
                    if (control->mBlock.fd >= 0 && submission.mState->mImports.mArmImport) {
                        au::sys::memory::FdIdentity fdIdentity;
                        const int duplicate = au::sys::memory::duplicateFd(control->mBlock.fd, bytes, resource->mOwnedFd, fdIdentity);
                        if (duplicate) return {duplicate, 0, au::gpu::detail::ErrorDomain::Memory};
                        cl_mem imported = nullptr; int importNative = 0;
                        const int importedCode = au::sys::opencl::importDmaBuf(submission.mState->mImports,
                            submission.mState->mContext.mValue, resource->mOwnedFd, bytes, &imported, &importNative);
                        resource->mMemory = Object<cl_mem>(imported, api.mReleaseMemObject);
                        if (importedCode) return {importedCode, importNative, au::gpu::detail::ErrorDomain::OpenCL};
                        resource->mImported = true;
                    } else resource->mMemory = Object<cl_mem>(api.mCreateBuffer(
                        submission.mState->mContext.mValue, CL_MEM_READ_WRITE, bytes, nullptr, &native), api.mReleaseMemObject);
                }
                if (native || !resource->mMemory.mValue) return nativeStatus(native ? native : CL_INVALID_MEM_OBJECT);
                std::map<ResourceKey, std::shared_ptr<CachedMemory>> prepared;
                prepared.emplace(key, resource); auto node = prepared.extract(prepared.begin());
                {
                    std::lock_guard<std::mutex> lock(submission.mState->mCacheMutex);
                    auto inserted = submission.mState->mResources.insert(std::move(node));
                    resource = inserted.position->second; node = std::move(inserted.node);
                }
                // Losing cold creations release outside the lock through node.
            }
            if (transfer.mImage) {
                transfer.mOffset = offset; transfer.mWidth = arg.mWidth; transfer.mHeight = arg.mHeight; transfer.mPitch = arg.mRowPitch;
                transfer.mOrder = arg.mChannelOrder; transfer.mType = arg.mChannelType; transfer.mPixelBytes = resource->mPixelBytes;
                std::size_t row = 0, preceding = 0;
                if (!transfer.mWidth || !transfer.mHeight || !transfer.mPixelBytes || !multiply(arg.mWidth, transfer.mPixelBytes, row) ||
                    arg.mRowPitch < row || arg.mRowPitch % transfer.mPixelBytes || !multiply(arg.mHeight - 1, arg.mRowPitch, preceding) ||
                    preceding > std::numeric_limits<std::size_t>::max() - row || offset > control->mBlock.size ||
                    preceding + row > control->mBlock.size - offset || preceding + row > arg.mAccess.mRanges[0].mSize ||
                    !multiply(arg.mHeight, row, bytes)) return {au::err::kErrorOutOfRange, 0, au::gpu::detail::ErrorDomain::Memory};
            }
            transfer.mImported = resource->mImported;
            native = api.mRetainMemObject(resource->mMemory.mValue);
            if (native) return nativeStatus(native);
            transfer.mMemory = Object<cl_mem>(resource->mMemory.mValue, api.mReleaseMemObject);
            if (!transfer.mImported) {
                transfer.mBytes.reset(new unsigned char[bytes]); // Avoid zeroing bytes that the validated host copy overwrites.
                transfer.mByteCount = bytes;
            }
            submission.mTransfers.push_back(std::move(transfer));
        }
        submission.mArgumentTransfers[index] = slot;
    }
    // Preserve the entire allocation for buffer writes too, so ROI kernels do
    // not copy uninitialized staging bytes over untouched planes/padding.
    for (auto& transfer : submission.mTransfers) {
        if (transfer.mImported) continue;
        int native = 0;
        code = au::mm::detail::syncCpuAccess(transfer.mControl, AU_MM_READ, true, &native);
        if (code) return {code, native, au::gpu::detail::ErrorDomain::Memory};
        auto* source = static_cast<unsigned char*>(transfer.mControl->mBlock.ptr) + transfer.mOffset;
        if (transfer.mImage) {
            const std::size_t row = transfer.mWidth * transfer.mPixelBytes;
            for (std::size_t y = 0; y < transfer.mHeight; ++y)
                std::memcpy(transfer.mBytes.get() + y * row, source + y * transfer.mPitch, row);
        } else std::memcpy(transfer.mBytes.get(), source, transfer.mByteCount);
        code = au::mm::detail::syncCpuAccess(transfer.mControl, AU_MM_READ, false, &native);
        if (code) {
            { std::lock_guard<std::mutex> lock(transfer.mControl->mAccessMutex);
              transfer.mControl->mAccessError = code; transfer.mControl->mAccessNativeError = native; }
            return {code, native, au::gpu::detail::ErrorDomain::Memory};
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto& arg = args[index]; cl_int native = CL_SUCCESS;
        if (arg.mKind == AU_GPU_BUFFER || arg.mKind == AU_GPU_IMAGE2D) {
            const auto memory = submission.mTransfers[submission.mArgumentTransfers[index]].mMemory.mValue;
            native = api.mSetKernelArg(submission.mKernel->mKernel.mValue, static_cast<cl_uint>(index), sizeof(memory), &memory);
        } else if (arg.mKind == AU_GPU_NATIVE_MEMORY) {
            auto memory = static_cast<cl_mem>(const_cast<void*>(arg.mValue));
            if (!memory) return nativeStatus(CL_INVALID_MEM_OBJECT);
            cl_context context = nullptr; cl_mem_object_type type = 0;
            native = api.mGetMemObjectInfo(memory, CL_MEM_CONTEXT, sizeof(context), &context, nullptr);
            if (!native) native = api.mGetMemObjectInfo(memory, CL_MEM_TYPE, sizeof(type), &type, nullptr);
            if (!native && context != submission.mState->mContext.mValue) native = CL_INVALID_CONTEXT;
            if (!native && backendId == AU_GPU_DMA_BUFFER && type != CL_MEM_OBJECT_BUFFER) native = CL_INVALID_MEM_OBJECT;
            if (!native) native = api.mRetainMemObject(memory);
            if (!native) {
                submission.mNative.emplace_back(memory, api.mReleaseMemObject);
                native = api.mSetKernelArg(submission.mKernel->mKernel.mValue, static_cast<cl_uint>(index), sizeof(memory), &memory);
            }
        } else {
            if (!arg.mSize || (arg.mKind == AU_GPU_SCALAR && !arg.mValue)) return {au::err::kErrorInvalidParam, 0, au::gpu::detail::ErrorDomain::OpenCL};
            native = api.mSetKernelArg(submission.mKernel->mKernel.mValue, static_cast<cl_uint>(index), arg.mSize,
                                      arg.mKind == AU_GPU_LOCAL ? nullptr : arg.mValue);
        }
        if (native) return nativeStatus(native);
    }
    return {};
}

au::gpu::detail::PendingStatus enqueueCommands(Submission& submission, const ::au_gpu_range& range, void** output) noexcept
{
    const auto& api = *submission.mState->mApi;
    const auto queue = submission.mState->mQueue.mValue;
    auto accept = [&](cl_int code, cl_event event) -> au::gpu::detail::PendingStatus {
        if (code) { if (event) (void)api.mReleaseEvent(event); return nativeStatus(code); }
        if (!event) {
            submission.mNeedsQueueProof = true;
            return nativeStatus(CL_INVALID_EVENT); // Retain ALL resources until queue completion is independently established.
        }
        submission.mEvent = Object<cl_event>(event, api.mReleaseEvent); return {};
    };
    for (auto& transfer : submission.mTransfers) {
        if (transfer.mImported) continue;
        cl_event event = nullptr;
        const cl_uint waitCount = submission.mEvent.mValue ? 1 : static_cast<cl_uint>(submission.mWaitEvents.size());
        const cl_event* waitList = submission.mEvent.mValue ? &submission.mEvent.mValue :
            (waitCount ? submission.mWaitEvents.data() : nullptr);
        cl_int code;
        if (transfer.mImage) {
            const std::size_t origin[]{0, 0, 0}, region[]{transfer.mWidth, transfer.mHeight, 1};
            code = api.mEnqueueWriteImage(queue, transfer.mMemory.mValue, CL_FALSE, origin, region,
                transfer.mWidth * transfer.mPixelBytes, 0, transfer.mBytes.get(), waitCount, waitList, &event);
        } else code = api.mEnqueueWriteBuffer(queue, transfer.mMemory.mValue, CL_FALSE, 0, transfer.mByteCount,
                                             transfer.mBytes.get(), waitCount, waitList, &event);
        const auto status = accept(code, event); if (status.mCode) return status;
    }
    cl_event event = nullptr;
    cl_int native = api.mEnqueueNDRangeKernel(queue, submission.mKernel->mKernel.mValue, range.mDimensions,
        range.mOffset, range.mGlobal, range.mLocal[0] ? range.mLocal : nullptr,
        submission.mEvent.mValue ? 1 : static_cast<cl_uint>(submission.mWaitEvents.size()),
        submission.mEvent.mValue ? &submission.mEvent.mValue : (submission.mWaitEvents.empty() ? nullptr : submission.mWaitEvents.data()), &event);
    auto status = accept(native, event); if (status.mCode) return status;
    submission.mKernelAccepted = true;
    for (auto& transfer : submission.mTransfers) if (!transfer.mImported && (transfer.mMode & AU_MM_WRITE)) {
        event = nullptr;
        if (transfer.mImage) {
            const std::size_t origin[]{0, 0, 0}, region[]{transfer.mWidth, transfer.mHeight, 1};
            native = api.mEnqueueReadImage(queue, transfer.mMemory.mValue, CL_FALSE, origin, region, transfer.mWidth * transfer.mPixelBytes, 0,
                transfer.mBytes.get(), 1, &submission.mEvent.mValue, &event);
        } else native = api.mEnqueueReadBuffer(queue, transfer.mMemory.mValue, CL_FALSE, 0, transfer.mByteCount,
                                              transfer.mBytes.get(), 1, &submission.mEvent.mValue, &event);
        status = accept(native, event); if (status.mCode) return status;
    }
    if (output) {
        native = api.mRetainEvent(submission.mEvent.mValue);
        if (native) return nativeStatus(native);
        *output = submission.mEvent.mValue;
    }
    return {};
}
}

extern "C" std::uint32_t au_gpu_opencl_version() noexcept { return AURA_GPU_OPENCL_ABI_VERSION; }
extern "C" int au_gpu_available(std::uint32_t id) noexcept
{
    try {
        auto* instance = backend(id); if (!instance) return 0;
        std::lock_guard<std::mutex> lock(instance->mMutex); return instance->mPhase == Backend::Phase::Running ? 1 : 0;
    } catch (...) { return 0; }
}
extern "C" int au_gpu_init(std::uint32_t id, const char* binaryDirectory, int profiling) noexcept
{
    if (au::gpu::detail::pendingCleanupActive()) return report({au::err::kErrorBusy, 0, au::gpu::detail::ErrorDomain::System});
    return boundary([&] { auto* instance = backend(id); return instance ? instance->init(profiling != 0, binaryDirectory) : au::err::kErrorInvalidParam; });
}
extern "C" int au_gpu_context(std::uint32_t id, void** context) noexcept
{
    if (!context) return report({au::err::kErrorNullPointer, 0, au::gpu::detail::ErrorDomain::OpenCL});
    *context = nullptr;
    return boundary([&] {
        auto* instance = backend(id); if (!instance) return au::err::kErrorInvalidParam;
        Backend::Entry entry; const int code = instance->enter(entry); if (code) return code;
        *context = entry.mState->mContext.mValue; return 0;
    });
}
extern "C" int au_gpu_deinit(std::uint32_t id) noexcept
{ return boundary([&] { auto* instance = backend(id); return instance ? instance->stop(true) : au::err::kErrorInvalidParam; }); }
extern "C" int au_gpu_finish(std::uint32_t id) noexcept
{ return boundary([&] { auto* instance = backend(id); return instance ? instance->stop(false) : au::err::kErrorInvalidParam; }); }
extern "C" int au_gpu_flush(std::uint32_t id) noexcept
{
    return boundary([&] {
        auto* instance = backend(id); if (!instance) return au::err::kErrorInvalidParam;
        Backend::Entry entry; const int code = instance->enter(entry); if (code) return code;
        instance->mPending.pollSome();
        entry.mState->pruneResources(); return report(nativeStatus(entry.mState->mApi->mFlush(entry.mState->mQueue.mValue)));
    });
}
extern "C" int au_gpu_prepare(std::uint32_t id, const char* name, const char* source, const char* options) noexcept
{
    if (au::gpu::detail::pendingCleanupActive()) return report({au::err::kErrorBusy, 0, au::gpu::detail::ErrorDomain::System});
    return boundary([&] {
        auto* instance = backend(id); if (!instance) return au::err::kErrorInvalidParam;
        Backend::Entry entry; const int code = instance->enter(entry); if (code) return code;
        instance->mPending.pollSome();
        entry.mState->pruneResources(); std::shared_ptr<Program> program;
        return report(getProgram(entry.mState, name, source, options, program));
    });
}
extern "C" int au_gpu_enqueue(std::uint32_t id, const ::au_gpu_range* range, const char* name, const char* source,
    const ::au_gpu_arg* args, std::size_t count, void** event) noexcept
{
    if (event) *event = nullptr;
    if (au::gpu::detail::pendingCleanupActive()) return report({au::err::kErrorBusy, 0, au::gpu::detail::ErrorDomain::System});
    return boundary([&] {
        auto* instance = backend(id); if (!instance) return au::err::kErrorInvalidParam;
        int code = validateRange(range); if (code) return report({code, 0, au::gpu::detail::ErrorDomain::OpenCL});
        if ((!args && count) || count > std::numeric_limits<cl_uint>::max()) return au::err::kErrorInvalidParam;
        Backend::Entry entry; code = instance->enter(entry); if (code) return code;
        instance->mPending.pollSome();
        entry.mState->pruneResources();
        auto payload = std::make_unique<Submission>(); payload->mState = entry.mState;
        auto status = getProgram(entry.mState, name, source, nullptr, payload->mProgram);
        if (!status.mCode) status = leaseKernel(*payload, name);
        if (!status.mCode) {
            cl_uint expected = 0;
            status = nativeStatus(entry.mState->mApi->mGetKernelInfo(payload->mKernel->mKernel.mValue,
                CL_KERNEL_NUM_ARGS, sizeof(expected), &expected, nullptr));
            if (!status.mCode && expected != count) status = {au::err::kErrorSizeMismatch, 0, au::gpu::detail::ErrorDomain::OpenCL};
        }
        if (status.mCode) return report(status);
        au::gpu::detail::PreparedSubmission submission;
        code = submission.prepare(instance->mPending, gOps, payload.get(), reinterpret_cast<std::uintptr_t>(entry.mState->mContext.mValue));
        if (code) return report({code, 0, code == au::err::kErrorNoMemory ? au::gpu::detail::ErrorDomain::Memory : au::gpu::detail::ErrorDomain::System});
        auto* raw = payload.release(); // PreparedSubmission now owns every preparation rollback.
        status = prepareArguments(*raw, id, args, count);
        if (status.mCode) { report(status); submission.cancel(status); return status.mCode; }
        status = enqueueCommands(*raw, *range, event);
        if (!raw->mEvent.mValue && !raw->mNeedsQueueProof) { report(status); submission.cancel(status); return status.mCode; }
        // Even a partial enqueue must commit its last accepted event. No later
        // fallible allocation is allowed between native acceptance and publish.
        const bool hostCompletion = raw->mNeedsQueueProof || std::any_of(raw->mTransfers.begin(), raw->mTransfers.end(),
            [](const Transfer& transfer) noexcept { return !transfer.mImported; });
        if (!hostCompletion && raw->mEvent.mValue) {
            const int retained = entry.mState->mApi->mRetainEvent(raw->mEvent.mValue);
            if (!retained) {
                raw->mNativeFence->mEvent = Object<cl_event>(raw->mEvent.mValue, entry.mState->mApi->mReleaseEvent);
                submission.fence()->mNativeOwner = raw->mNativeFence;
                submission.fence()->mNativeEvent = raw->mEvent.mValue;
                submission.fence()->mHostCompletion = false; // Immutable before publishing any resource fence.
            } else if (!status.mCode) status = nativeStatus(retained);
        }
        const int commit = raw->mAccessPrepared ? raw->mAccess.commit(submission.fence()) : 0;
        if (commit && !status.mCode) status = {commit, 0, au::gpu::detail::ErrorDomain::Memory};
        const auto flush = nativeStatus(entry.mState->mApi->mFlush(entry.mState->mQueue.mValue));
        if (!status.mCode) status = flush;
        report(status); // Preserve synchronous native provenance before a concurrent completion query can report another error.
        submission.publish(status);
        return status.mCode;
    });
}
extern "C" int au_gpu_event_release(std::uint32_t id, void* event) noexcept
{
    return boundary([&] {
        if (!event) return au::err::kErrorInvalidHandle;
        auto* instance = backend(id); if (!instance) return au::err::kErrorInvalidParam;
        decltype(&::clReleaseEvent) release = &::clReleaseEvent;
        {
            std::lock_guard<std::mutex> lock(instance->mMutex);
            const auto* api = instance->mState ? instance->mState->mApi : instance->mTestApi;
            if (api) release = api->mReleaseEvent;
        }
        return report(nativeStatus(release(static_cast<cl_event>(event))));
    });
}
#ifdef AURA_GPU_TESTING
void* au::gpu::detail::backendIdentity(unsigned int id) noexcept
{ try { return backend(id); } catch (...) { return nullptr; } }
int au::gpu::detail::setNativeApi(unsigned int id, const au::gpu::detail::NativeApi* api) noexcept
{
    return boundary([&] {
        auto* instance = backend(id); if (!instance) return au::err::kErrorInvalidParam;
        std::lock_guard<std::mutex> lock(instance->mMutex);
        if (instance->mPhase != Backend::Phase::Empty) return au::err::kErrorBusy;
        instance->mTestApi = api; return 0;
    });
}
#endif
