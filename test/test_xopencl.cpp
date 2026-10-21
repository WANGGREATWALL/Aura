#include "gpu/xopencl.h"
#include "gpu/opencl_api.h"
#include "flow/xflow.h"
#include "file/xfile.h"
#include "mm/xmemory.h"
#include "mm/xaccess.h"
#include "sys/memory_runtime.h"
#include "CL/cl_ext.h"
#include "gtest/gtest.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <filesystem>
#include <string>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
// A native API model, not a GPU/cache hardware test. Nonblocking host pointers
// remain live until the last event; commands execute only when the model allows.
struct NativeModel;
struct ModelMemory {
    NativeModel* mOwner;
    cl_context mContext;
    std::atomic<int> mRefs{1};
    std::vector<unsigned char> mBytes;
    std::size_t mWidth = 0, mHeight = 0;
    void* mImported = nullptr;
};
struct ModelKernel {
    NativeModel* mOwner;
    bool mAlias = false;
    bool mFill = false;
    bool mEmpty = false;
    std::array<std::vector<unsigned char>, 3> mArgs;
};
struct ModelEvent {
    NativeModel* mOwner;
    std::atomic<int> mRefs{1};
    std::mutex mMutex;
    ModelEvent* mPrevious = nullptr;
    ModelMemory* mMemory = nullptr;
    ModelMemory* mInput = nullptr;
    void* mHost = nullptr;
    std::size_t mSize = 0, mPitch = 0;
    std::size_t mItems = 0;
    unsigned char mDelta = 0;
    int mAction = 0, mExecution = CL_QUEUED;
    bool mFill = false;
};
struct NativeModel {
    static NativeModel* sCurrent;
    au::gpu::detail::NativeApi mApi;
    std::atomic<bool> mReady{true};
    std::atomic<int> mMemory{0}, mEvents{0}, mKernels{0}, mPrograms{0}, mContexts{0}, mQueues{0};
    std::atomic<int> mCommands{0}, mWaits{0}, mQueries{0}, mFailCommand{0}, mFailQuery{0}, mFailFlush{0};
    std::atomic<int> mMissingEvent{0};
    std::mutex mUnknownMutex;
    std::vector<ModelEvent*> mUnknown;
    std::atomic<int> mExecution{CL_COMPLETE}, mAliasSeen{0};
    std::atomic<int> mStarts{0}, mEnds{0}, mImports{0};
    std::atomic<int> mFailStart{0}, mFailEnd{0};
    bool mArm = false;
    std::string mDriver = "model.driver.1";
    std::atomic<int> mSourceCreates{0}, mBinaryCreates{0}, mBufferCreates{0};
    std::atomic<bool> mRejectBinary{false};
    static cl_int stringInfo(const char* value, std::size_t capacity, void* output, std::size_t* size)
    {
        const auto bytes = std::strlen(value) + 1;
        if (size) *size = bytes;
        if (output) { if (capacity < bytes) return CL_INVALID_VALUE; std::memcpy(output, value, bytes); }
        return 0;
    }
    static cl_int CL_API_CALL platformInfo(cl_platform_id, cl_platform_info, std::size_t capacity, void* output, std::size_t* size)
    { return stringInfo("Aura.model.platform", capacity, output, size); }
    std::atomic<bool> mProfileFails{false};
    static cl_int CL_API_CALL platforms(cl_uint count, cl_platform_id* output, cl_uint* size)
    { if (size) *size = 1; if (count && output) *output = reinterpret_cast<cl_platform_id>(1); return 0; }
    static cl_int CL_API_CALL devices(cl_platform_id, cl_device_type, cl_uint count, cl_device_id* output, cl_uint* size)
    { if (size) *size = 1; if (count && output) *output = reinterpret_cast<cl_device_id>(2); return 0; }
    static cl_int CL_API_CALL deviceInfo(cl_device_id, cl_device_info name, std::size_t capacity, void* output, std::size_t* size)
    {
        if (name == CL_DEVICE_VENDOR_ID || name == CL_DEVICE_ADDRESS_BITS) {
            if (size) *size = sizeof(cl_uint);
            if (output) { if (capacity < sizeof(cl_uint)) return CL_INVALID_VALUE; *static_cast<cl_uint*>(output) = name == CL_DEVICE_VENDOR_ID ? 123u : 64u; }
            return 0;
        }
        const char* field = name == CL_DEVICE_EXTENSIONS ? (sCurrent->mArm ? "cl_arm_import_memory cl_arm_import_memory_dma_buf" : "") :
            name == CL_DRIVER_VERSION ? sCurrent->mDriver.c_str() : "Aura.model.device";
        return stringInfo(field, capacity, output, size);
    }
    static cl_context CL_API_CALL context(const cl_context_properties*, cl_uint, const cl_device_id*,
        void (CL_CALLBACK *)(const char*, const void*, std::size_t, void*), void*, cl_int* error)
    { *error = 0; sCurrent->mContexts.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<cl_context>(new int(0)); }
    static cl_int CL_API_CALL releaseContext(cl_context value)
    { delete reinterpret_cast<int*>(value); sCurrent->mContexts.fetch_sub(1, std::memory_order_relaxed); return 0; }
    static cl_command_queue CL_API_CALL queue(cl_context, cl_device_id, cl_command_queue_properties, cl_int* error)
    { *error = 0; sCurrent->mQueues.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<cl_command_queue>(new int(0)); }
    static cl_int CL_API_CALL releaseQueue(cl_command_queue value)
    { delete reinterpret_cast<int*>(value); sCurrent->mQueues.fetch_sub(1, std::memory_order_relaxed); return 0; }
    static cl_program CL_API_CALL program(cl_context, cl_uint, const char**, const std::size_t*, cl_int* error)
    { *error = 0; sCurrent->mPrograms.fetch_add(1, std::memory_order_relaxed); sCurrent->mSourceCreates.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<cl_program>(new int(0)); }
    static cl_program CL_API_CALL binaryProgram(cl_context, cl_uint count, const cl_device_id*, const std::size_t* sizes,
        const unsigned char** bytes, cl_int* binaryStatus, cl_int* error)
    {
        sCurrent->mBinaryCreates.fetch_add(1, std::memory_order_relaxed);
        const char expected[] = "Aura.model.binary.v1";
        *binaryStatus = *error = count != 1 || sizes[0] != sizeof(expected) || std::memcmp(bytes[0], expected, sizeof(expected)) ||
            sCurrent->mRejectBinary.load(std::memory_order_relaxed) ? CL_INVALID_BINARY : CL_SUCCESS;
        if (*error) return nullptr;
        sCurrent->mPrograms.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<cl_program>(new int(0));
    }
    static cl_int CL_API_CALL programInfo(cl_program, cl_program_info query, std::size_t size, void* output, std::size_t*)
    {
        const char binary[] = "Aura.model.binary.v1";
        if (query == CL_PROGRAM_NUM_DEVICES && size == sizeof(cl_uint)) *static_cast<cl_uint*>(output) = 1;
        else if (query == CL_PROGRAM_BINARY_SIZES && size == sizeof(std::size_t)) *static_cast<std::size_t*>(output) = sizeof(binary);
        else if (query == CL_PROGRAM_BINARIES && size == sizeof(unsigned char*)) std::memcpy(*static_cast<unsigned char**>(output), binary, sizeof(binary));
        else return CL_INVALID_VALUE;
        return 0;
    }
    static cl_int CL_API_CALL build(cl_program, cl_uint, const cl_device_id*, const char*, void (CL_CALLBACK *)(cl_program, void*), void*) { return 0; }
    static cl_int CL_API_CALL releaseProgram(cl_program value)
    { delete reinterpret_cast<int*>(value); sCurrent->mPrograms.fetch_sub(1, std::memory_order_relaxed); return 0; }
    static cl_kernel CL_API_CALL kernel(cl_program, const char* name, cl_int* error)
    {
        auto* value = new ModelKernel; value->mOwner = sCurrent;
        value->mAlias = std::strcmp(name, "alias") == 0 || std::strcmp(name, "alias_image") == 0;
        value->mFill = std::strcmp(name, "fill") == 0;
        value->mEmpty = std::strcmp(name, "empty") == 0;
        *error = 0; sCurrent->mKernels.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<cl_kernel>(value);
    }
    static cl_int CL_API_CALL kernelInfo(cl_kernel kernel, cl_kernel_info name, std::size_t size, void* output, std::size_t*)
    {
        if (name != CL_KERNEL_NUM_ARGS || size != sizeof(cl_uint)) return CL_INVALID_VALUE;
        const auto* value = reinterpret_cast<ModelKernel*>(kernel);
        *static_cast<cl_uint*>(output) = value->mEmpty ? 0 : value->mAlias ? 3 : 2; return 0;
    }
    static cl_int CL_API_CALL setArg(cl_kernel kernel, cl_uint index, std::size_t size, const void* data)
    {
        auto& self = *reinterpret_cast<ModelKernel*>(kernel);
        if (index >= self.mArgs.size() || !data) return CL_INVALID_ARG_VALUE;
        self.mArgs[index].resize(size); std::memcpy(self.mArgs[index].data(), data, size); return 0;
    }
    static cl_int CL_API_CALL releaseKernel(cl_kernel value)
    { delete reinterpret_cast<ModelKernel*>(value); sCurrent->mKernels.fetch_sub(1, std::memory_order_relaxed); return 0; }
    static cl_mem CL_API_CALL buffer(cl_context context, cl_mem_flags, std::size_t size, void*, cl_int* error)
    {
        auto* value = new ModelMemory; value->mOwner = sCurrent; value->mContext = context; value->mBytes.resize(size);
        *error = 0; sCurrent->mMemory.fetch_add(1, std::memory_order_relaxed); sCurrent->mBufferCreates.fetch_add(1, std::memory_order_relaxed); return reinterpret_cast<cl_mem>(value);
    }
    static cl_mem CL_API_CALL image(cl_context context, cl_mem_flags flags, const cl_image_format*, const cl_image_desc* desc, void* host, cl_int* error)
    {
        auto result = buffer(context, flags, desc->image_width * desc->image_height, host, error);
        auto* value = reinterpret_cast<ModelMemory*>(result); value->mWidth = desc->image_width; value->mHeight = desc->image_height; return result;
    }
    static cl_int CL_API_CALL retainMemory(cl_mem value)
    { reinterpret_cast<ModelMemory*>(value)->mRefs.fetch_add(1, std::memory_order_relaxed); return 0; }
    static cl_int CL_API_CALL releaseMemory(cl_mem value)
    {
        auto* self = reinterpret_cast<ModelMemory*>(value);
        if (self->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
#if defined(__linux__) || defined(__ANDROID__)
            if (self->mImported) ::munmap(self->mImported, self->mBytes.size());
#endif
            self->mOwner->mMemory.fetch_sub(1, std::memory_order_relaxed); delete self;
        }
        return 0;
    }
    static cl_int CL_API_CALL memoryInfo(cl_mem value, cl_mem_info name, std::size_t, void* output, std::size_t*)
    {
        auto* self = reinterpret_cast<ModelMemory*>(value);
        if (name == CL_MEM_CONTEXT) *static_cast<cl_context*>(output) = self->mContext;
        else if (name == CL_MEM_TYPE) *static_cast<cl_mem_object_type*>(output) = self->mWidth ? CL_MEM_OBJECT_IMAGE2D : CL_MEM_OBJECT_BUFFER;
        else return CL_INVALID_VALUE;
        return 0;
    }
    static cl_int CL_API_CALL imageInfo(cl_mem value, cl_image_info name, std::size_t size, void* output, std::size_t*)
    {
        if (!reinterpret_cast<ModelMemory*>(value)->mWidth || name != CL_IMAGE_ELEMENT_SIZE || size != sizeof(std::size_t)) return CL_INVALID_VALUE;
        *static_cast<std::size_t*>(output) = 1; return 0; // Model images are the CL_R 8-bit cases used below.
    }
    static cl_int command(ModelMemory* memory, void* host, std::size_t size, std::size_t pitch, int action,
                          cl_uint count, const cl_event* waits, cl_event* output,
                          unsigned char delta = 0, std::size_t items = 0, bool fill = false, ModelMemory* input = nullptr)
    {
        const int ordinal = sCurrent->mCommands.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal == sCurrent->mFailCommand.load(std::memory_order_relaxed)) return CL_OUT_OF_RESOURCES;
        auto* event = new ModelEvent; event->mOwner = sCurrent; event->mMemory = memory;
        event->mHost = host; event->mSize = size; event->mPitch = pitch; event->mAction = action;
        event->mDelta = delta; event->mItems = items; event->mFill = fill; event->mInput = input;
        if (memory) retainMemory(reinterpret_cast<cl_mem>(memory));
        if (input) retainMemory(reinterpret_cast<cl_mem>(input));
        if (count) { event->mPrevious = reinterpret_cast<ModelEvent*>(waits[0]); retainEvent(waits[0]); }
        sCurrent->mEvents.fetch_add(1, std::memory_order_relaxed);
        if (ordinal == sCurrent->mMissingEvent.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(sCurrent->mUnknownMutex); sCurrent->mUnknown.push_back(event);
        } else *output = reinterpret_cast<cl_event>(event);
        return 0;
    }
    static cl_int CL_API_CALL writeBuffer(cl_command_queue, cl_mem memory, cl_bool, std::size_t offset, std::size_t size,
        const void* host, cl_uint count, const cl_event* waits, cl_event* output)
    { return offset ? CL_INVALID_VALUE : command(reinterpret_cast<ModelMemory*>(memory), const_cast<void*>(host), size, 0, 0, count, waits, output); }
    static cl_int CL_API_CALL readBuffer(cl_command_queue, cl_mem memory, cl_bool, std::size_t offset, std::size_t size,
        void* host, cl_uint count, const cl_event* waits, cl_event* output)
    { return offset ? CL_INVALID_VALUE : command(reinterpret_cast<ModelMemory*>(memory), host, size, 0, 2, count, waits, output); }
    static cl_int CL_API_CALL writeImage(cl_command_queue, cl_mem memory, cl_bool, const std::size_t*, const std::size_t* region,
        std::size_t pitch, std::size_t, const void* host, cl_uint count, const cl_event* waits, cl_event* output)
    { return command(reinterpret_cast<ModelMemory*>(memory), const_cast<void*>(host), region[0] * region[1], pitch, 0, count, waits, output); }
    static cl_int CL_API_CALL readImage(cl_command_queue, cl_mem memory, cl_bool, const std::size_t*, const std::size_t* region,
        std::size_t pitch, std::size_t, void* host, cl_uint count, const cl_event* waits, cl_event* output)
    { return command(reinterpret_cast<ModelMemory*>(memory), host, region[0] * region[1], pitch, 2, count, waits, output); }
    static cl_int CL_API_CALL enqueue(cl_command_queue, cl_kernel value, cl_uint dimensions, const std::size_t*, const std::size_t* global, const std::size_t*,
        cl_uint count, const cl_event* waits, cl_event* output)
    {
        auto* kernel = reinterpret_cast<ModelKernel*>(value); ModelMemory* memory = nullptr; unsigned char delta = 0;
        if (kernel->mEmpty) return command(nullptr, nullptr, 0, 0, 3, count, waits, output);
        if (kernel->mArgs[0].size() != sizeof(cl_mem)) return CL_INVALID_KERNEL_ARGS;
        std::memcpy(&memory, kernel->mArgs[0].data(), sizeof(memory));
        const auto& scalar = kernel->mArgs[kernel->mAlias ? 2 : 1];
        if (scalar.size() != 1) return CL_INVALID_KERNEL_ARGS;
        std::memcpy(&delta, scalar.data(), 1);
        ModelMemory* input = nullptr;
        if (kernel->mAlias) {
            if (kernel->mArgs[1].size() != sizeof(cl_mem)) return CL_INVALID_KERNEL_ARGS;
            std::memcpy(&input, kernel->mArgs[1].data(), sizeof(cl_mem));
        }
        const auto code = command(memory, nullptr, memory->mBytes.size(), 0, 1, count, waits, output,
            delta, global[0] * (dimensions > 1 ? global[1] : 1) * (dimensions > 2 ? global[2] : 1), kernel->mFill, input);
        if (!code && kernel->mAlias && input == memory) sCurrent->mAliasSeen.fetch_add(1, std::memory_order_relaxed);
        return code;
    }
    static int execute(ModelEvent* event)
    {
        std::lock_guard<std::mutex> lock(event->mMutex);
        if (event->mExecution <= CL_COMPLETE) return event->mExecution;
        if (event->mPrevious && execute(event->mPrevious) < 0) return event->mExecution = CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST;
        if (sCurrent->mExecution.load(std::memory_order_relaxed) < 0) return event->mExecution = sCurrent->mExecution.load(std::memory_order_relaxed);
        if (event->mAction == 3) return event->mExecution = CL_COMPLETE;
        auto& bytes = event->mMemory->mBytes;
        if (event->mAction == 1) {
            if (event->mMemory->mImported) std::memcpy(bytes.data(), event->mMemory->mImported, bytes.size());
            for (std::size_t i = 0; i < std::min(event->mItems, bytes.size()); ++i)
                bytes[i] = event->mFill ? event->mDelta : static_cast<unsigned char>(bytes[i] + event->mDelta);
            if (event->mMemory->mImported) std::memcpy(event->mMemory->mImported, bytes.data(), bytes.size());
        }
        else if (event->mPitch) {
            for (std::size_t row = 0; row < event->mMemory->mHeight; ++row) {
                void* host = static_cast<unsigned char*>(event->mHost) + row * event->mPitch;
                void* device = bytes.data() + row * event->mMemory->mWidth;
                if (event->mAction == 0) std::memcpy(device, host, event->mMemory->mWidth);
                else std::memcpy(host, device, event->mMemory->mWidth);
            }
        } else if (event->mAction == 0) std::memcpy(bytes.data(), event->mHost, event->mSize);
        else std::memcpy(event->mHost, bytes.data(), event->mSize);
        return event->mExecution = CL_COMPLETE;
    }
    static cl_int CL_API_CALL eventInfo(cl_event value, cl_event_info name, std::size_t size, void* output, std::size_t*)
    {
        sCurrent->mQueries.fetch_add(1, std::memory_order_relaxed);
        if (sCurrent->mFailQuery.exchange(0, std::memory_order_acq_rel)) return CL_OUT_OF_RESOURCES;
        if (name != CL_EVENT_COMMAND_EXECUTION_STATUS || size != sizeof(cl_int)) return CL_INVALID_VALUE;
        *static_cast<cl_int*>(output) = sCurrent->mReady.load(std::memory_order_acquire) ? execute(reinterpret_cast<ModelEvent*>(value)) : CL_RUNNING;
        return 0;
    }
    static cl_int CL_API_CALL wait(cl_uint count, const cl_event* events)
    {
        sCurrent->mWaits.fetch_add(1, std::memory_order_relaxed);
        while (!sCurrent->mReady.load(std::memory_order_acquire)) std::this_thread::yield();
        for (cl_uint i = 0; i < count; ++i) if (execute(reinterpret_cast<ModelEvent*>(events[i])) < 0) return CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST;
        return 0;
    }
    static cl_int CL_API_CALL retainEvent(cl_event value)
    { reinterpret_cast<ModelEvent*>(value)->mRefs.fetch_add(1, std::memory_order_relaxed); return 0; }
    static cl_int CL_API_CALL releaseEvent(cl_event value)
    {
        auto* event = reinterpret_cast<ModelEvent*>(value);
        if (event->mRefs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (event->mPrevious) releaseEvent(reinterpret_cast<cl_event>(event->mPrevious));
            if (event->mMemory) releaseMemory(reinterpret_cast<cl_mem>(event->mMemory));
            if (event->mInput) releaseMemory(reinterpret_cast<cl_mem>(event->mInput));
            event->mOwner->mEvents.fetch_sub(1, std::memory_order_relaxed); delete event;
        }
        return 0;
    }
    static cl_int CL_API_CALL flush(cl_command_queue)
    { return sCurrent->mFailFlush.exchange(0, std::memory_order_acq_rel) ? CL_OUT_OF_RESOURCES : 0; }
    static cl_int CL_API_CALL finish(cl_command_queue)
    {
        if (!sCurrent->mReady.load(std::memory_order_acquire)) return CL_OUT_OF_RESOURCES;
        std::vector<ModelEvent*> unknown;
        { std::lock_guard<std::mutex> lock(sCurrent->mUnknownMutex); unknown.swap(sCurrent->mUnknown); }
        for (auto* event : unknown) { (void)execute(event); releaseEvent(reinterpret_cast<cl_event>(event)); }
        return 0;
    }
    static cl_int CL_API_CALL profile(cl_event, cl_profiling_info query, std::size_t size, void* output, std::size_t*)
    {
        if (sCurrent->mProfileFails.load(std::memory_order_relaxed)) return CL_PROFILING_INFO_NOT_AVAILABLE;
        if (size != sizeof(cl_ulong)) return CL_INVALID_VALUE;
        switch (query) {
        case CL_PROFILING_COMMAND_QUEUED: *static_cast<cl_ulong*>(output) = 1001; return 0;
        case CL_PROFILING_COMMAND_SUBMIT: *static_cast<cl_ulong*>(output) = 1101; return 0;
        case CL_PROFILING_COMMAND_START: *static_cast<cl_ulong*>(output) = 1201; return 0;
        case CL_PROFILING_COMMAND_END: *static_cast<cl_ulong*>(output) = 1301; return 0;
        default: return CL_INVALID_VALUE;
        }
    }
    static int access(int, unsigned int, bool start, int* native) noexcept
    {
        const int ordinal = (start ? sCurrent->mStarts : sCurrent->mEnds).fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal == (start ? sCurrent->mFailStart : sCurrent->mFailEnd).load(std::memory_order_relaxed)) {
            if (native) *native = 99;
            return au::err::kErrorPlatformAPI;
        }
        return 0;
    }
    static cl_mem CL_API_CALL import(cl_context context, cl_mem_flags flags, const cl_import_properties_arm*,
                                    void* memory, std::size_t size, cl_int* error)
    {
#if defined(__linux__) || defined(__ANDROID__)
        void* mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, *static_cast<int*>(memory), 0);
        if (mapping == MAP_FAILED) { *error = CL_INVALID_VALUE; return nullptr; }
        auto result = buffer(context, flags, size, nullptr, error);
        reinterpret_cast<ModelMemory*>(result)->mImported = mapping;
        sCurrent->mImports.fetch_add(1, std::memory_order_relaxed); return result;
#else
        (void)context; (void)flags; (void)memory; (void)size; *error = CL_INVALID_OPERATION; return nullptr;
#endif
    }
    static void* CL_API_CALL lookup(cl_platform_id, const char* name)
    { return sCurrent->mArm && std::strcmp(name, "clImportMemoryARM") == 0 ? reinterpret_cast<void*>(&import) : nullptr; }
    explicit NativeModel(bool arm = false, bool profiling = false) : mArm(arm)
    {
        sCurrent = this;
        mApi.mGetPlatformInfo = &platformInfo;
        mApi.mCreateProgramWithBinary = &binaryProgram; mApi.mGetProgramInfo = &programInfo;
        mApi.mGetPlatformIDs = &platforms; mApi.mGetDeviceIDs = &devices;
        mApi.mGetDeviceInfo = &deviceInfo; mApi.mGetExtensionFunctionAddressForPlatform = &lookup;
        mApi.mCreateContext = &context; mApi.mReleaseContext = &releaseContext;
        mApi.mCreateCommandQueue = &queue; mApi.mReleaseCommandQueue = &releaseQueue;
        mApi.mCreateProgramWithSource = &program; mApi.mBuildProgram = &build; mApi.mReleaseProgram = &releaseProgram;
        mApi.mCreateKernel = &kernel; mApi.mGetKernelInfo = &kernelInfo; mApi.mSetKernelArg = &setArg; mApi.mReleaseKernel = &releaseKernel;
        mApi.mCreateBuffer = &buffer; mApi.mCreateImage = &image; mApi.mGetMemObjectInfo = &memoryInfo;
        mApi.mGetImageInfo = &imageInfo;
        mApi.mRetainMemObject = &retainMemory; mApi.mReleaseMemObject = &releaseMemory;
        mApi.mEnqueueWriteBuffer = &writeBuffer; mApi.mEnqueueReadBuffer = &readBuffer;
        mApi.mEnqueueWriteImage = &writeImage; mApi.mEnqueueReadImage = &readImage; mApi.mEnqueueNDRangeKernel = &enqueue;
        mApi.mGetEventInfo = &eventInfo; mApi.mWaitForEvents = &wait; mApi.mRetainEvent = &retainEvent; mApi.mReleaseEvent = &releaseEvent; mApi.mFlush = &flush;
        mApi.mFinish = &finish;
        mApi.mGetEventProfilingInfo = &profile;
        EXPECT_EQ(au::gpu::detail::setNativeApi(0, &mApi), 0); EXPECT_EQ(au::gpu::detail::setNativeApi(1, &mApi), 0);
        EXPECT_EQ(au::gpu::CLWrapper::get().init({}, profiling), 0); EXPECT_EQ(au::gpu::OpenCLWrapper::get().init({}, profiling), 0);
        au::sys::memory::setAccessHook(&access);
    }
    ~NativeModel()
    {
        mReady.store(true, std::memory_order_release);
        (void)au::gpu::CLWrapper::get().deinit(); (void)au::gpu::OpenCLWrapper::get().deinit();
        au::sys::memory::setAccessHook(nullptr);
        EXPECT_EQ(mEvents.load(std::memory_order_relaxed), 0); EXPECT_EQ(mMemory.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(mKernels.load(std::memory_order_relaxed), 0); EXPECT_EQ(mPrograms.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(mQueues.load(std::memory_order_relaxed), 0); EXPECT_EQ(mContexts.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(au::gpu::detail::setNativeApi(0, nullptr), 0); EXPECT_EQ(au::gpu::detail::setNativeApi(1, nullptr), 0);
        sCurrent = nullptr;
    }
};
NativeModel* NativeModel::sCurrent = nullptr;
const char* gSource = "__kernel void add(__global uchar* data, uchar value) { data[get_global_id(0)] += value; }";
const char* gImageSource = "__kernel void fill(write_only image2d_t dst, uchar value) { write_imageui(dst, (int2)(get_global_id(0), get_global_id(1)), (uint4)(value)); }";
const char* gAliasSource = "__kernel void alias(__global uchar* dst, __global const uchar* src, uchar value) { size_t i=get_global_id(0); dst[i]=src[i]+value; }";
}

TEST(XOpenCL, BufferTaskAndSameFunctionCpuAccess)
{
    NativeModel model;
    au::mm::MemBlock block; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, block), 0);
    std::memset(block.ptr, 3, block.size);
    model.mReady.store(false, std::memory_order_relaxed);
    auto task = au::flow::XFlow::get().enqueue([&]() -> int {
        return au::gpu::CLWrapper::get().setNDRange({64}).enqueue("add", gSource, nullptr,
            au::gpu::buffer(au::mm::readWriteAccess(block.ptr, block.size)), static_cast<unsigned char>(7));
    });
    EXPECT_EQ(au::flow::XFlow::get().waitFor(task, 2), au::err::kErrorTimeout);
    EXPECT_EQ(static_cast<unsigned char*>(block.ptr)[0], 3);
    model.mReady.store(true, std::memory_order_release);
    au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), 0); EXPECT_EQ(result.mCode, 0);
    EXPECT_GT(model.mWaits.load(std::memory_order_relaxed), 0);
    for (std::size_t i = 0; i < block.size; ++i) EXPECT_EQ(static_cast<unsigned char*>(block.ptr)[i], 10);
    auto mixed = au::flow::XFlow::get().enqueue([&]() -> int {
        int code = au::gpu::CLWrapper::get().setNDRange({64}).enqueue("add", nullptr, nullptr,
            au::gpu::buffer(au::mm::readWriteAccess(block.ptr, block.size)), static_cast<unsigned char>(2));
        if (code) return code;
        au::mm::CpuAccessScope cpu; code = cpu.begin({au::mm::readAccess(block.ptr, block.size)});
        if (code) return code;
        return cpu.finish(static_cast<unsigned char*>(block.ptr)[0] == 12 ? 0 : 91);
    });
    ASSERT_EQ(au::flow::XFlow::get().wait(mixed, &result), 0); EXPECT_EQ(result.mCode, 0);
    EXPECT_EQ(au::gpu::CLWrapper::get().finish(), 0); EXPECT_EQ(au::mm::free(block.ptr), 0);
}

TEST(XOpenCL, StandaloneImagePaddingAndAlias)
{
    NativeModel model;
    au::mm::MemBlock block; ASSERT_EQ(au::mm::alloc(32, au::mm::MemType::Pss, block), 0);
    std::memset(block.ptr, 9, block.size);
    auto image = au::gpu::image2D(au::mm::writeAccess(block.ptr, 29), 5, 4, 8, CL_R, CL_UNSIGNED_INT8);
    ASSERT_EQ(au::gpu::CLWrapper::get().setNDRange({5, 4}).enqueue("fill", gImageSource, nullptr, image, static_cast<unsigned char>(10)), 0);
    au::mm::CpuAccessScope cpu; ASSERT_EQ(cpu.begin({au::mm::readAccess(block.ptr, block.size)}), 0);
    for (std::size_t y = 0; y < 4; ++y) for (std::size_t x = 0; x < 8; ++x)
        EXPECT_EQ(static_cast<unsigned char*>(block.ptr)[y * 8 + x], x < 5 ? 10 : 9);
    EXPECT_EQ(cpu.finish(), 0);
    const auto buffer = au::gpu::buffer(au::mm::readWriteAccess(block.ptr, block.size));
    ASSERT_EQ(au::gpu::CLWrapper::get().setNDRange({32}).enqueue("alias", gAliasSource, nullptr, buffer, buffer, static_cast<unsigned char>(1)), 0);
    EXPECT_EQ(au::gpu::CLWrapper::get().finish(), 0);
    EXPECT_EQ(model.mAliasSeen.load(std::memory_order_relaxed), 1);
    const int commands = model.mCommands.load(std::memory_order_relaxed);
    const auto inputImage = au::gpu::image2D(au::mm::readAccess(block.ptr, 29), 5, 4, 8, CL_R, CL_UNSIGNED_INT8);
    const auto outputImage = au::gpu::image2D(au::mm::writeAccess(block.ptr, 29), 5, 4, 8, CL_R, CL_UNSIGNED_INT8);
    const char* copyImage = "__kernel void alias_image(read_only image2d_t src, write_only image2d_t dst, uchar unused) { const sampler_t s=CLK_NORMALIZED_COORDS_FALSE|CLK_ADDRESS_NONE|CLK_FILTER_NEAREST; int2 p=(int2)(get_global_id(0),get_global_id(1)); write_imageui(dst,p,read_imageui(src,s,p)); }";
    EXPECT_EQ(au::gpu::CLWrapper::get().setNDRange({5, 4}).enqueue("alias_image", copyImage, nullptr,
        inputImage, outputImage, static_cast<unsigned char>(0)), au::err::kErrorNotSupported);
    EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), commands); // CL1.2 writable image aliases never reach native enqueue.
    EXPECT_EQ(au::mm::free(block.ptr), 0);
}

#if defined(__linux__) || defined(__ANDROID__)
TEST(XOpenCL, DmaImportAndStagingFollowAccessProtocol)
{
    for (bool imported : {false, true}) {
        NativeModel model(imported);
        std::FILE* file = std::tmpfile(); ASSERT_NE(file, nullptr); ASSERT_EQ(::ftruncate(::fileno(file), 4096), 0);
        void* pointer = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ::fileno(file), 0);
        ASSERT_NE(pointer, MAP_FAILED); std::memset(pointer, 4, 4096);
        au::mm::ExternalMemory memory;
        ::au_mm_external_memory descriptor{sizeof(descriptor), pointer, 4096, ::fileno(file),
            static_cast<unsigned int>(au::mm::MemType::DmaUncached), nullptr, nullptr, nullptr};
        ASSERT_EQ(memory.registerMemory(descriptor), 0);
        for (unsigned int repeat = 0; repeat < 2; ++repeat) {
            ASSERT_EQ(au::gpu::OpenCLWrapper::get().setNDRange({4096}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(pointer, 4096)), static_cast<unsigned char>(3)), 0);
            au::mm::CpuAccessScope cpu; ASSERT_EQ(cpu.begin({au::mm::readAccess(pointer, 4096)}), 0);
            EXPECT_EQ(static_cast<unsigned char*>(pointer)[0], 7 + 3 * repeat);
            EXPECT_EQ(static_cast<unsigned char*>(pointer)[4095], 7 + 3 * repeat); EXPECT_EQ(cpu.finish(), 0);
        }
        EXPECT_EQ(model.mImports.load(std::memory_order_relaxed), imported ? 1 : 0);
        EXPECT_EQ(model.mStarts.load(std::memory_order_relaxed), imported ? 2 : 6);
        EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), imported ? 2 : 6);
        EXPECT_EQ(au::gpu::OpenCLWrapper::get().finish(), 0);
        if (imported) {
            // Sibling wrapper contexts cannot consume one another's native
            // wait-list. Bridge on the host before reserving any successor.
            model.mReady.store(false, std::memory_order_release);
            const int beforeQueries = model.mQueries.load(std::memory_order_relaxed);
            ASSERT_EQ(au::gpu::CLWrapper::get().setNDRange({4096}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(pointer, 4096)), static_cast<unsigned char>(1)), 0);
            const int beforeCommands = model.mCommands.load(std::memory_order_relaxed);
            int submitted = -1;
            std::thread bridge([&] {
                submitted = au::gpu::OpenCLWrapper::get().setNDRange({4096}).enqueue("add", gSource, nullptr,
                    au::gpu::buffer(au::mm::readWriteAccess(pointer, 4096)), static_cast<unsigned char>(1));
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (model.mQueries.load(std::memory_order_relaxed) == beforeQueries && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            EXPECT_GT(model.mQueries.load(std::memory_order_relaxed), beforeQueries);
            EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), beforeCommands);
            model.mReady.store(true, std::memory_order_release); bridge.join(); EXPECT_EQ(submitted, 0);
            au::mm::CpuAccessScope cpu; ASSERT_EQ(cpu.begin({au::mm::readAccess(pointer, 4096)}), 0);
            EXPECT_EQ(static_cast<unsigned char*>(pointer)[0], 12); EXPECT_EQ(cpu.finish(), 0);
            EXPECT_EQ(au::gpu::CLWrapper::get().finish(), 0); EXPECT_EQ(au::gpu::OpenCLWrapper::get().finish(), 0);
        }
        const int oldFd = ::fileno(file);
        EXPECT_EQ(memory.reset(), 0); EXPECT_EQ(::munmap(pointer, 4096), 0); EXPECT_EQ(std::fclose(file), 0);
        if (imported) {
            // The old native cache may still hold its own fd. Reusing the
            // application's integer must import the new allocation identity.
            std::FILE* next = std::tmpfile(); ASSERT_NE(next, nullptr); EXPECT_EQ(::fileno(next), oldFd);
            ASSERT_EQ(::ftruncate(::fileno(next), 4096), 0);
            void* replacement = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ::fileno(next), 0);
            ASSERT_NE(replacement, MAP_FAILED); std::memset(replacement, 20, 4096);
            descriptor.mData = replacement; descriptor.mFd = ::fileno(next);
            ASSERT_EQ(memory.registerMemory(descriptor), 0);
            ASSERT_EQ(au::gpu::OpenCLWrapper::get().setNDRange({4096}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(replacement, 4096)), static_cast<unsigned char>(3)), 0);
            ASSERT_EQ(au::gpu::OpenCLWrapper::get().finish(), 0);
            EXPECT_EQ(model.mImports.load(std::memory_order_relaxed), 3); EXPECT_EQ(static_cast<unsigned char*>(replacement)[0], 23);
            EXPECT_EQ(memory.reset(), 0); EXPECT_EQ(::munmap(replacement, 4096), 0); EXPECT_EQ(std::fclose(next), 0);
        }
    }
}
#endif

TEST(XOpenCL, PartialSubmissionFlushQueryAndKernelReuse)
{
    for (int scenario = 0; scenario < 5; ++scenario) {
        NativeModel model;
        au::mm::MemBlock block; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, block), 0);
        std::memset(block.ptr, 5, block.size);
        model.mReady.store(false, std::memory_order_relaxed);
        if (scenario == 0) model.mFailCommand.store(2, std::memory_order_relaxed); // Upload accepted, kernel rejected.
        if (scenario == 1) model.mFailCommand.store(3, std::memory_order_relaxed); // Kernel accepted, readback rejected.
        if (scenario == 2) model.mFailFlush.store(1, std::memory_order_relaxed);
        if (scenario == 3) model.mFailQuery.store(1, std::memory_order_relaxed);
        if (scenario == 4) model.mExecution.store(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST, std::memory_order_relaxed);
        std::atomic<bool> submitted{false};
        auto task = au::flow::XFlow::get().enqueue([&]() -> int {
            const int code = au::gpu::CLWrapper::get().setNDRange({64}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(block.ptr, block.size)), static_cast<unsigned char>(7));
            // Publish native submission before testing that its unfinished
            // events keep the task alive; a short wait timeout alone proves
            // nothing about whether this task has started on a busy host.
            submitted.store(true, std::memory_order_release);
            return code;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!submitted.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        EXPECT_TRUE(submitted.load(std::memory_order_acquire));
        EXPECT_EQ(au::flow::XFlow::get().waitFor(task, 3), au::err::kErrorTimeout);
        EXPECT_GT(model.mEvents.load(std::memory_order_relaxed), 0);
        model.mReady.store(true, std::memory_order_release);
        au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), au::err::kErrorPlatformAPI);
        EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
        EXPECT_EQ(result.mNativeCode, scenario == 4 ? CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST : CL_OUT_OF_RESOURCES);
        EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
        EXPECT_EQ(static_cast<unsigned char*>(block.ptr)[0], 5); // Failed outputs are not copied back as valid data.
        EXPECT_EQ(au::gpu::CLWrapper::get().finish(), au::err::kErrorPlatformAPI);
        const int before = model.mCommands.load(std::memory_order_relaxed);
        EXPECT_EQ(au::gpu::CLWrapper::get().setNDRange({64}).enqueue("add", nullptr, nullptr,
            au::gpu::buffer(au::mm::readWriteAccess(block.ptr, block.size))), au::err::kErrorSizeMismatch);
        EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), before); // Pooled kernels cannot reuse stale missing arguments.
        EXPECT_EQ(au::mm::free(block.ptr), 0);
    }
}

TEST(XOpenCL, ConcurrentLaunchesKeepArgumentsAndRangesIndependent)
{
    NativeModel model;
    std::array<au::mm::MemBlock, 8> memory;
    std::array<au::flow::Task, 8> tasks;
    model.mReady.store(false, std::memory_order_relaxed);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory[i]), 0);
        std::memset(memory[i].ptr, 2, memory[i].size);
        tasks[i] = au::flow::XFlow::get().enqueue([&, i]() -> int {
            const auto launch = au::gpu::CLWrapper::get().setNDRange({16 + i});
            (void)au::gpu::CLWrapper::get().setNDRange({64}); // Must not overwrite the captured launch.
            return launch.enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(memory[i].ptr, memory[i].size)), static_cast<unsigned char>(i + 1));
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (model.mCommands.load(std::memory_order_relaxed) < 24 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), 24);
    model.mReady.store(true, std::memory_order_release);
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(tasks[i], &result), 0); EXPECT_EQ(result.mCode, 0);
        for (std::size_t byte = 0; byte < memory[i].size; ++byte)
            EXPECT_EQ(static_cast<unsigned char*>(memory[i].ptr)[byte], byte < 16 + i ? 3 + i : 2);
        EXPECT_EQ(au::mm::free(memory[i].ptr), 0);
    }
    EXPECT_EQ(au::gpu::CLWrapper::get().finish(), 0);
}

TEST(XOpenCL, ProfilingKeepsClockDomainsAndUnavailableExplicit)
{
    ASSERT_EQ(au::flow::XFlow::get().setTracing(true), 0);
    struct Reset { ~Reset() { (void)au::flow::XFlow::get().setTracing(false); } } reset;
    for (int scenario = 0; scenario < 3; ++scenario) {
        NativeModel model(false, scenario != 1);
        model.mProfileFails.store(scenario == 2, std::memory_order_relaxed);
        au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(16, au::mm::MemType::Pss, memory), 0);
        std::memset(memory.ptr, 1, memory.size);
        auto task = au::flow::XFlow::get().enqueue([&]() -> int {
            return au::gpu::CLWrapper::get().setNDRange({16}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)), static_cast<unsigned char>(1));
        });
        au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), 0);
        EXPECT_EQ(result.mCode, 0); // Optional profiling failure cannot turn a correct kernel into failed work.
        EXPECT_EQ(au::mm::free(memory.ptr), 0);
    }
    ASSERT_EQ(au::flow::XFlow::get().dumpTrace("/tmp/aura-flow-gpu-trace.json"), 0);
    std::string trace; ASSERT_EQ(au::file::read("/tmp/aura-flow-gpu-trace.json", trace), 0);
    EXPECT_NE(trace.find("\"clockDomain\":\"opencl-device\""), std::string::npos);
    EXPECT_NE(trace.find("\"gpuDurationNs\":100"), std::string::npos);
    EXPECT_NE(trace.find("\"gpuQueuedNs\":1001"), std::string::npos);
    EXPECT_NE(trace.find("\"profileAvailable\":false"), std::string::npos);
    EXPECT_NE(trace.find("\"profileNativeCode\":-7"), std::string::npos);
}

TEST(XOpenCL, EmptyKernelAndRejectedCallsRemainObservable)
{
    NativeModel model;
    const char* emptySource = "__kernel void empty() {}";
    auto task = au::flow::XFlow::get().enqueue([&]() -> int {
        return au::gpu::CLWrapper::get().setNDRange({1}).enqueue("empty", emptySource, nullptr);
    });
    au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), 0);
    EXPECT_EQ(result.mCode, 0);
    void* event = nullptr;
    ASSERT_EQ(au::gpu::CLWrapper::get().setNDRange({1}).enqueue("empty", nullptr, &event), 0);
    ASSERT_NE(event, nullptr);
    EXPECT_EQ(au::gpu::CLWrapper::get().releaseEvent(event), 0);
    EXPECT_EQ(au::gpu::CLWrapper::get().finish(), 0);
    int before = model.mCommands.load(std::memory_order_relaxed);
    auto& flow = au::flow::XFlow::get();
    const auto pureTiles = [&]() -> int {
        return flow.parallelizeTiledTasks(19, 1, [&](std::size_t, std::size_t) {
            (void)au::gpu::CLWrapper::get().setNDRange({1}).enqueue("empty", nullptr, nullptr);
        }); // Ignore the native wrapper result: the loop must retain rejection.
    };
    EXPECT_EQ(pureTiles(), au::err::kErrorStateInvalid);
    EXPECT_EQ(flow.enqueue(pureTiles).wait(), au::err::kErrorStateInvalid);
    EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), before);
    auto rejected = au::flow::XFlow::get().enqueue([&]() -> int {
        (void)au::gpu::CLWrapper::get().setNDRange({0}).enqueue("empty", nullptr, nullptr);
        return 0; // Dropping the synchronous error must not report a successful task.
    });
    ASSERT_EQ(au::flow::XFlow::get().wait(rejected, &result), au::err::kErrorInvalidParam);
    EXPECT_EQ(result.mCode, au::err::kErrorInvalidParam); EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), before);
    void* context = nullptr; ASSERT_EQ(au::gpu::CLWrapper::get().nativeContext(context), 0);
    cl_int native = 0;
    cl_mem raw = NativeModel::buffer(static_cast<cl_context>(context), CL_MEM_READ_WRITE, 16, nullptr, &native);
    ASSERT_EQ(native, 0);
    model.mReady.store(false, std::memory_order_relaxed);
    auto interop = au::flow::XFlow::get().enqueue([&]() -> int {
        return au::gpu::CLWrapper::get().setNDRange({16}).enqueue("add", gSource, nullptr,
            au::gpu::nativeMemory(raw), static_cast<unsigned char>(3));
    });
    EXPECT_EQ(au::flow::XFlow::get().waitFor(interop, 2), au::err::kErrorTimeout);
    EXPECT_EQ(NativeModel::releaseMemory(raw), 0); // Drop caller ownership while the wrapper/native event still retain it.
    EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 1);
    model.mReady.store(true, std::memory_order_release);
    ASSERT_EQ(au::flow::XFlow::get().wait(interop, &result), 0); EXPECT_EQ(result.mCode, 0);
    EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 0);
    before = model.mCommands.load(std::memory_order_relaxed);
    au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(32, au::mm::MemType::Pss, memory), 0);
    std::size_t offset = 0;
    EXPECT_EQ(au::gpu::bufferOffset(au::mm::readAccess(static_cast<unsigned char*>(memory.ptr) + 9, 8), offset), 0);
    EXPECT_EQ(offset, 9u);
    EXPECT_EQ(au::gpu::CLWrapper::get().setNDRange({1}).enqueue("add", gSource, nullptr,
        au::gpu::image2D(au::mm::readWriteAccess(memory.ptr, memory.size), 8, 4, 7, CL_R, CL_UNSIGNED_INT8),
        static_cast<unsigned char>(1)), au::err::kErrorOutOfRange);
    EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), before);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
}

#if defined(__linux__) || defined(__ANDROID__)
TEST(XOpenCL, CacheFailureBeforeAndAfterNativeAcceptance)
{
    for (int scenario = 0; scenario < 4; ++scenario) {
        NativeModel model;
        std::FILE* file = std::tmpfile(); ASSERT_NE(file, nullptr); ASSERT_EQ(::ftruncate(::fileno(file), 4096), 0);
        void* pointer = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ::fileno(file), 0);
        ASSERT_NE(pointer, MAP_FAILED); std::memset(pointer, 4, 4096);
        au::mm::ExternalMemory memory;
        ::au_mm_external_memory descriptor{sizeof(descriptor), pointer, 4096, ::fileno(file),
            static_cast<unsigned int>(au::mm::MemType::DmaUncached), nullptr, nullptr, nullptr};
        ASSERT_EQ(memory.registerMemory(descriptor), 0);
        if ((scenario & 1) == 0) model.mFailStart.store(scenario < 2 ? 1 : 2, std::memory_order_relaxed);
        else model.mFailEnd.store(scenario < 2 ? 1 : 2, std::memory_order_relaxed);
        auto task = au::flow::XFlow::get().enqueue([&]() -> int {
            return au::gpu::OpenCLWrapper::get().setNDRange({4096}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(pointer, 4096)), static_cast<unsigned char>(3));
        });
        au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), au::err::kErrorPlatformAPI);
        EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mNativeCode, 99);
        EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Memory);
        EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), scenario < 2 ? 0 : 3);
        EXPECT_EQ(model.mEvents.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(static_cast<unsigned char*>(pointer)[0], scenario == 3 ? 7 : 4);
        if (scenario == 1 || scenario == 3) {
            au::mm::CpuAccessScope cpu;
            EXPECT_EQ(cpu.begin({au::mm::readAccess(pointer, 4096)}), au::err::kErrorPlatformAPI);
        }
        (void)au::gpu::OpenCLWrapper::get().finish();
        EXPECT_EQ(memory.reset(), 0); EXPECT_EQ(::munmap(pointer, 4096), 0); EXPECT_EQ(std::fclose(file), 0);
    }
}
#endif

TEST(XOpenCL, MissingNativeEventRetainsWorkUntilQueueProof)
{
    for (int command = 1; command <= 3; ++command) {
        NativeModel model;
        au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
        std::memset(memory.ptr, 4, memory.size);
        model.mReady.store(false, std::memory_order_relaxed);
        model.mMissingEvent.store(command, std::memory_order_relaxed);
        auto task = au::flow::XFlow::get().enqueue([&]() -> int {
            return au::gpu::CLWrapper::get().setNDRange({64}).enqueue("add", gSource, nullptr,
                au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)), static_cast<unsigned char>(7));
        });
        EXPECT_EQ(au::flow::XFlow::get().waitFor(task, 3), au::err::kErrorTimeout);
        EXPECT_GT(model.mMemory.load(std::memory_order_relaxed), 0);
        EXPECT_GT(model.mEvents.load(std::memory_order_relaxed), 0);
        model.mReady.store(true, std::memory_order_release);
        au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), au::err::kErrorPlatformAPI);
        EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mNativeCode, CL_INVALID_EVENT);
        EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
        EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 1); // Live allocation keeps a reusable native cache entry.
        EXPECT_EQ(model.mEvents.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(au::mm::free(memory.ptr), 0);
        EXPECT_EQ(au::gpu::CLWrapper::get().finish(), au::err::kErrorPlatformAPI);
        EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 0);
    }
}

TEST(XOpenCL, ResourceFailureKeepsOriginalGpuTaskCause)
{
    NativeModel model;
    au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
    std::memset(memory.ptr, 4, memory.size);
    model.mExecution.store(CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST, std::memory_order_relaxed);
    auto submit = [&]() -> int {
        return au::gpu::CLWrapper::get().setNDRange({64}).enqueue("add", gSource, nullptr,
            au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)), static_cast<unsigned char>(7));
    };
    auto origin = au::flow::XFlow::get().enqueue(submit);
    au::flow::TaskResult result{}; ASSERT_EQ(au::flow::XFlow::get().wait(origin, &result), au::err::kErrorPlatformAPI);
    ASSERT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mCauseTaskId, origin.id());
    const int commands = model.mCommands.load(std::memory_order_relaxed);
    auto nextGpu = au::flow::XFlow::get().enqueue(submit); // Resource fence detects the failure without inventing a business DAG.
    ASSERT_EQ(au::flow::XFlow::get().wait(nextGpu, &result), au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mCauseTaskId, origin.id());
    EXPECT_EQ(result.mNativeCode, CL_EXEC_STATUS_ERROR_FOR_EVENTS_IN_WAIT_LIST);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), commands);
    auto nextCpu = au::flow::XFlow::get().enqueue([&]() -> int {
        au::mm::CpuAccessScope cpu; return cpu.begin({au::mm::readAccess(memory.ptr, memory.size)});
    });
    ASSERT_EQ(au::flow::XFlow::get().wait(nextCpu, &result), au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mCauseTaskId, origin.id());
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
}

TEST(XOpenCL, ResourceCacheUsesAllocationAndContextGeneration)
{
    NativeModel model;
    au::mm::MemBlock memory; ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
    std::memset(memory.ptr, 1, memory.size);
    const auto launch = au::gpu::CLWrapper::get().setNDRange({64});
    for (unsigned int i = 0; i < 3; ++i) {
        ASSERT_EQ(launch.enqueue("add", gSource, nullptr, au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)),
            static_cast<unsigned char>(2)), 0);
        ASSERT_EQ(au::gpu::CLWrapper::get().finish(), 0);
    }
    EXPECT_EQ(model.mBufferCreates.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(static_cast<unsigned char*>(memory.ptr)[0], 7);
    ASSERT_EQ(au::gpu::CLWrapper::get().deinit(), 0);
    EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 0);
    ASSERT_EQ(au::gpu::CLWrapper::get().init(), 0);
    ASSERT_EQ(launch.enqueue("add", gSource, nullptr, au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)),
        static_cast<unsigned char>(1)), 0);
    ASSERT_EQ(au::gpu::CLWrapper::get().finish(), 0);
    EXPECT_EQ(model.mBufferCreates.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
    ASSERT_EQ(au::gpu::CLWrapper::get().finish(), 0);
    EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 0); // Cache does not lease the freed allocation.
    ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, memory), 0);
    std::memset(memory.ptr, 30, memory.size);
    ASSERT_EQ(launch.enqueue("add", gSource, nullptr, au::gpu::buffer(au::mm::readWriteAccess(memory.ptr, memory.size)),
        static_cast<unsigned char>(1)), 0);
    ASSERT_EQ(au::gpu::CLWrapper::get().finish(), 0);
    EXPECT_EQ(model.mBufferCreates.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(static_cast<unsigned char*>(memory.ptr)[0], 31);
    EXPECT_EQ(au::mm::free(memory.ptr), 0);
}

TEST(XOpenCL, BinaryCacheValidatesMetadataAndFallsBackToSource)
{
    NativeModel model;
    const auto directory = std::filesystem::temp_directory_path() / ("aucl-cache-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { std::filesystem::path mPath; ~Cleanup() { std::error_code error; std::filesystem::remove_all(mPath, error); } } cleanup{directory};
    const auto path = directory.u8string();
    const char* source = "__kernel void empty() {} __kernel void second() {}";
    auto initialize = [&]() {
        EXPECT_EQ(au::gpu::CLWrapper::get().deinit(), 0);
        EXPECT_EQ(au::gpu::CLWrapper::get().init(path), 0);
    };
    initialize();
    ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source), 0);
    ASSERT_EQ(au::gpu::CLWrapper::get().prepare("second", source), 0);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 1); // Program shared, named kernel pools remain separate.
    auto files = au::file::listFiles(path); ASSERT_EQ(files.size(), 1u);
    initialize(); ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source), 0);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(model.mBinaryCreates.load(std::memory_order_relaxed), 1);
    model.mRejectBinary.store(true, std::memory_order_relaxed);
    initialize(); ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source), 0);
    EXPECT_EQ(model.mBinaryCreates.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 2);
    model.mRejectBinary.store(false, std::memory_order_relaxed);
    ASSERT_EQ(au::file::write(std::string("AUCLBC01broken"), files[0].string()), 0);
    initialize(); ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source), 0);
    EXPECT_EQ(model.mBinaryCreates.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 3);
    model.mDriver = "model.driver.2";
    initialize(); ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source), 0);
    EXPECT_EQ(model.mBinaryCreates.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 4);
    files = au::file::listFiles(path); EXPECT_EQ(files.size(), 2u);
    // In-process names are immutable even if another source/options key exists.
    EXPECT_EQ(au::gpu::CLWrapper::get().prepare("empty", "__kernel void empty() { int changed=1; }"), au::err::kErrorAlreadyExists);
    initialize(); ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source, "-cl-std=CL1.2 -DCHANGE=1"), 0);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 5);
    EXPECT_EQ(au::file::listFiles(path).size(), 3u);
    ASSERT_EQ(au::gpu::CLWrapper::get().deinit(), 0);
    // A regular file cannot be a directory. Optional disk failure still compiles.
    ASSERT_EQ(au::gpu::CLWrapper::get().init(files[0].string()), 0);
    ASSERT_EQ(au::gpu::CLWrapper::get().prepare("empty", source), 0);
    EXPECT_EQ(model.mSourceCreates.load(std::memory_order_relaxed), 6);
}

#if defined(__linux__) || defined(__ANDROID__)
TEST(XOpenCL, NativeChainsSubmitWithoutHostWaitAndKeepRootFailure)
{
    for (bool fail : {false, true}) {
        NativeModel model(true);
        std::FILE* file = std::tmpfile(); ASSERT_NE(file, nullptr); ASSERT_EQ(::ftruncate(::fileno(file), 4096), 0);
        void* pointer = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ::fileno(file), 0);
        ASSERT_NE(pointer, MAP_FAILED); std::memset(pointer, 1, 4096);
        au::mm::ExternalMemory memory;
        ::au_mm_external_memory description{sizeof(description), pointer, 4096, ::fileno(file),
            static_cast<unsigned int>(au::mm::MemType::DmaCached), nullptr, nullptr, nullptr};
        ASSERT_EQ(memory.registerMemory(description), 0);
        model.mReady.store(false, std::memory_order_release);
        std::atomic<unsigned int> returned{0};
        std::vector<au::flow::Task> tasks; tasks.reserve(64);
        for (unsigned int i = 0; i < 64; ++i) {
            tasks.push_back(au::flow::XFlow::get().enqueue([&]() -> int {
                const auto code = au::gpu::OpenCLWrapper::get().setNDRange({4096}).enqueue("add", gSource, nullptr,
                    au::gpu::buffer(au::mm::readWriteAccess(pointer, 4096)), static_cast<unsigned char>(1));
                returned.fetch_add(1, std::memory_order_release); return code;
            }));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (returned.load(std::memory_order_acquire) != i + 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            if (returned.load(std::memory_order_acquire) != i + 1) {
                model.mReady.store(true, std::memory_order_release); // Always unlock worker before test teardown.
                FAIL() << "native chain blocked its submitting host function";
            }
        }
        EXPECT_EQ(model.mImports.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(model.mCommands.load(std::memory_order_relaxed), 64);
        EXPECT_EQ(model.mWaits.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(au::flow::XFlow::get().waitFor(tasks.back(), 2), au::err::kErrorTimeout);
        if (fail) model.mExecution.store(-9, std::memory_order_relaxed);
        model.mReady.store(true, std::memory_order_release);
        au::flow::TaskResult result{};
        ASSERT_EQ(au::flow::XFlow::get().wait(tasks.back(), &result), fail ? au::err::kErrorPlatformAPI : 0);
        EXPECT_EQ(result.mCode, fail ? au::err::kErrorPlatformAPI : 0);
        if (fail) { EXPECT_EQ(result.mNativeCode, -9); EXPECT_EQ(result.mCauseTaskId, tasks.front().id()); }
        else {
            au::mm::CpuAccessScope cpu; ASSERT_EQ(cpu.begin({au::mm::readAccess(pointer, 4096)}), 0);
            EXPECT_EQ(static_cast<unsigned char*>(pointer)[0], 65); EXPECT_EQ(cpu.finish(), 0);
        }
        for (auto& task : tasks) EXPECT_EQ(au::flow::XFlow::get().wait(task), fail ? au::err::kErrorPlatformAPI : 0);
        EXPECT_EQ(memory.reset(), 0);
        EXPECT_EQ(au::gpu::OpenCLWrapper::get().finish(), fail ? au::err::kErrorPlatformAPI : 0);
        EXPECT_EQ(model.mMemory.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(model.mEvents.load(std::memory_order_relaxed), 0);
        EXPECT_EQ(::munmap(pointer, 4096), 0); EXPECT_EQ(std::fclose(file), 0);
    }
}
#endif
