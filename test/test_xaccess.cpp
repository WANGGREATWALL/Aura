#if ENABLE_TEST_XMEMORY
#include "mm/xaccess.h"
#include "mm/xmemory.h"
#include "mm/access.h"
#include "mm/registry.h"
#include "cv/ximage_access.h"
#include "flow/xflow.h"
#include "sys/memory_runtime.h"
#include "gtest/gtest.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct AccessModel {
    std::atomic<int> mStarts{0}, mEnds{0};
    std::atomic<int> mMode{0};
    std::atomic<int> mFailStart{0}, mFailEnd{0};
    static AccessModel* sCurrent;
    static int access(int, unsigned int mode, bool start, int* native) noexcept
    {
        AccessModel& self = *sCurrent;
        self.mMode.store(static_cast<int>(mode), std::memory_order_relaxed);
        const int ordinal = (start ? self.mStarts : self.mEnds).fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal == (start ? self.mFailStart : self.mFailEnd).load(std::memory_order_relaxed)) {
            if (native) *native = 77;
            return au::err::kErrorPlatformAPI;
        }
        return 0;
    }
    AccessModel() { sCurrent = this; au::sys::memory::setAccessHook(&AccessModel::access); }
    ~AccessModel() { au::sys::memory::setAccessHook(nullptr); sCurrent = nullptr; }
};
AccessModel* AccessModel::sCurrent = nullptr;
struct Mapping {
    std::FILE* mFile = std::tmpfile();
    void* mFirst = MAP_FAILED;
    void* mSecond = MAP_FAILED;
    std::atomic<int> mRetains{0}, mReleases{0};
    Mapping()
    {
        if (!mFile || ::ftruncate(::fileno(mFile), 256) != 0) return;
        mFirst = ::mmap(nullptr, 256, PROT_READ | PROT_WRITE, MAP_SHARED, ::fileno(mFile), 0);
        mSecond = ::mmap(nullptr, 256, PROT_READ | PROT_WRITE, MAP_SHARED, ::fileno(mFile), 0);
    }
    ~Mapping()
    {
        if (mSecond != MAP_FAILED) ::munmap(mSecond, 256);
        if (mFirst != MAP_FAILED) ::munmap(mFirst, 256);
        if (mFile) std::fclose(mFile);
    }
    bool valid() const { return mFirst != MAP_FAILED && mSecond != MAP_FAILED; }
    static int retain(void* owner) { static_cast<Mapping*>(owner)->mRetains.fetch_add(1, std::memory_order_relaxed); return 0; }
    static void release(void* owner) { static_cast<Mapping*>(owner)->mReleases.fetch_add(1, std::memory_order_relaxed); }
    ::au_mm_external_memory description(void* pointer, unsigned int type = 1)
    {
        return {sizeof(::au_mm_external_memory), pointer, 256, ::fileno(mFile), type,
                this, &Mapping::retain, &Mapping::release};
    }
};
struct FenceModel {
    std::atomic<bool> mReady{false};
    std::atomic<int> mWaits{0};
    int mCode = 0;
    int mNative = 0;
    std::uint64_t mCause = 0;
    bool mDevice = false;
    static au::mm::detail::FenceStatus wait(void* payload) noexcept
    {
        auto* self = static_cast<FenceModel*>(payload);
        self->mWaits.fetch_add(1, std::memory_order_relaxed);
        while (!self->mReady.load(std::memory_order_acquire)) std::this_thread::yield();
        return {self->mCode, self->mNative, self->mCause, self->mDevice};
    }
};
std::shared_ptr<au::mm::detail::DeviceFence> fence(FenceModel& model)
{
    // The test scope owns model; native providers use an owning event record.
    auto result = std::make_shared<au::mm::detail::DeviceFence>();
    result->mPayload = &model; result->mWait = &FenceModel::wait;
    return result;
}
}

TEST(XMemory, CpuAccessAliasesReadersAndConflicts)
{
    AccessModel model;
    Mapping mapping;
    ASSERT_TRUE(mapping.valid());
    au::mm::ExternalMemory registration;
    ASSERT_EQ(registration.registerMemory(mapping.description(mapping.mFirst)), 0);
    au::mm::CpuAccessScope first, second, conflict;
    ASSERT_EQ(first.begin({au::mm::readAccess(mapping.mFirst, 128)}), 0);
    ASSERT_EQ(second.begin({au::mm::readAccess(mapping.mFirst, 256)}), 0);
    EXPECT_EQ(model.mStarts.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(conflict.begin({au::mm::writeAccess(mapping.mFirst, 256)}), au::err::kErrorBusy);
    EXPECT_EQ(first.finish(), 0);
    EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(second.finish(), 0);
    EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), 1);
    EXPECT_EQ(model.mMode.load(std::memory_order_relaxed), AU_MM_READ);
    ASSERT_EQ(first.begin({au::mm::readAccess(mapping.mFirst, 64),
        au::mm::writeAccess(static_cast<unsigned char*>(mapping.mFirst) + 64, 192)}), 0);
    EXPECT_EQ(model.mStarts.load(std::memory_order_relaxed), 2);
    EXPECT_EQ(model.mMode.load(std::memory_order_relaxed), AU_MM_READ_WRITE);
    au::mm::detail::DeviceAccess device;
    const au::mm::Access write = au::mm::writeAccess(mapping.mFirst, 256);
    EXPECT_EQ(device.begin(&write, 1), au::err::kErrorBusy);
    EXPECT_EQ(first.finish(), 0);
    EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), 2);
}

TEST(XMemory, CpuAccessStartRollbackAndEndFailure)
{
    AccessModel model;
    Mapping a, b;
    ASSERT_TRUE(a.valid()); ASSERT_TRUE(b.valid());
    au::mm::ExternalMemory ra, rb;
    ASSERT_EQ(ra.registerMemory(a.description(a.mFirst)), 0);
    ASSERT_EQ(rb.registerMemory(b.description(b.mFirst, 2)), 0); // Uncached still has START/END.
    model.mFailStart.store(2, std::memory_order_relaxed);
    au::mm::CpuAccessScope scope;
    EXPECT_EQ(scope.begin({au::mm::readAccess(a.mFirst, 256), au::mm::writeAccess(b.mFirst, 256)}), au::err::kErrorPlatformAPI);
    EXPECT_FALSE(scope.active());
    EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), 1);
    model.mFailStart.store(0, std::memory_order_relaxed);
    ASSERT_EQ(scope.begin({au::mm::readAccess(a.mFirst, 256), au::mm::writeAccess(b.mFirst, 256)}), 0);
    model.mFailEnd.store(2, std::memory_order_relaxed);
    EXPECT_EQ(scope.finish(71), 71); // Preserve body failure while also reporting END failure.
    EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), 3);
    EXPECT_EQ(scope.begin({au::mm::readAccess(b.mFirst, 256)}), au::err::kErrorPlatformAPI);
    ASSERT_EQ(scope.begin({au::mm::writeAccess(a.mFirst, 256)}), 0);
    EXPECT_EQ(scope.finish(), 0);
}

TEST(XMemory, ExternalAliasesRetirementAndFdReuse)
{
    Mapping mapping;
    ASSERT_TRUE(mapping.valid());
    au::mm::ExternalMemory first, second;
    ASSERT_EQ(first.registerMemory(mapping.description(mapping.mFirst)), 0);
    ASSERT_EQ(second.registerMemory(mapping.description(mapping.mSecond)), 0);
    au::mm::detail::MemoryLease a, b;
    ASSERT_EQ(au::mm::Registry::instance().acquire(mapping.mFirst, 256, a), 0);
    ASSERT_EQ(au::mm::Registry::instance().acquire(mapping.mSecond, 256, b), 0);
    EXPECT_TRUE(a.identity() == b.identity());
    EXPECT_EQ(a.offset(), 0u); EXPECT_EQ(b.offset(), 0u);
    EXPECT_NE(a.block().fd, ::fileno(mapping.mFile));
    EXPECT_EQ(au::mm::free(mapping.mFirst), au::err::kErrorStateInvalid);
    EXPECT_EQ(first.reset(), 0); EXPECT_EQ(second.reset(), 0);
    EXPECT_FALSE(au::mm::isManaged(mapping.mFirst));
    EXPECT_EQ(mapping.mReleases.load(std::memory_order_relaxed), 0);
    a.reset();
    EXPECT_EQ(mapping.mReleases.load(std::memory_order_relaxed), 0); // Canonical and alias maps both pinned by b.
    b.reset();
    EXPECT_EQ(mapping.mReleases.load(std::memory_order_relaxed), 2);

    // Same numeric descriptor, different backing file: stable identity changes.
    ASSERT_EQ(first.registerMemory(mapping.description(mapping.mFirst)), 0);
    ASSERT_EQ(au::mm::Registry::instance().acquire(mapping.mFirst, 1, a), 0);
    const int reused = ::fileno(mapping.mFile);
    std::fclose(mapping.mFile); mapping.mFile = nullptr;
    Mapping next;
    ASSERT_TRUE(next.valid());
    ASSERT_EQ(::fileno(next.mFile), reused);
    ASSERT_EQ(second.registerMemory(next.description(next.mFirst)), 0);
    ASSERT_EQ(au::mm::Registry::instance().acquire(next.mFirst, 1, b), 0);
    EXPECT_FALSE(a.identity() == b.identity());
    a.reset(); b.reset();
    EXPECT_EQ(first.reset(), 0); EXPECT_EQ(second.reset(), 0);
}

TEST(XMemory, DeviceFenceBeforeCpuAndAtomicAbort)
{
    au::mm::XMemory memory(256), other(256);
    ASSERT_TRUE(memory.valid()); ASSERT_TRUE(other.valid());
    const au::mm::Access write = au::mm::writeAccess(memory.data(), memory.size());
    FenceModel model;
    au::mm::detail::DeviceAccess device;
    ASSERT_EQ(device.begin(&write, 1), 0);
    au::mm::CpuAccessScope scope;
    EXPECT_EQ(scope.begin({write}), au::err::kErrorBusy);
    device.abort();
    ASSERT_EQ(scope.begin({write}), 0); EXPECT_EQ(scope.finish(), 0);
    ASSERT_EQ(device.begin(&write, 1), 0);
    ASSERT_EQ(device.commit(fence(model)), 0);
    std::thread complete([&] {
        while (model.mWaits.load(std::memory_order_relaxed) == 0) std::this_thread::yield();
        std::memset(memory.data(), 0x6a, memory.size());
        model.mReady.store(true, std::memory_order_release);
    });
    EXPECT_EQ(scope.begin({au::mm::readAccess(memory.data(), memory.size())}), 0);
    complete.join();
    EXPECT_EQ(static_cast<unsigned char*>(memory.data())[255], 0x6a);
    EXPECT_EQ(scope.finish(), 0);
    // Reservations are all-or-none even when a later resource is busy.
    ASSERT_EQ(scope.begin({au::mm::writeAccess(other.data(), other.size())}), 0);
    const au::mm::Access both[] = {write, au::mm::writeAccess(other.data(), other.size())};
    au::mm::detail::DeviceAccess rejected;
    EXPECT_EQ(rejected.begin(both, 2), au::err::kErrorBusy);
    au::mm::CpuAccessScope available;
    EXPECT_EQ(available.begin({write}), 0); EXPECT_EQ(available.finish(), 0);
    EXPECT_EQ(scope.finish(), 0);
}

TEST(XMemory, CpuAccessGeometryBoundsAndTiledLease)
{
    au::cv::XImage image(17, 13, au::cv::kImageFormatNV12, au::mm::MemType::Pss);
    ASSERT_TRUE(image.isValid());
    au::mm::CpuAccessScope scope;
    au::cv::Image view = image.view();
    au::cv::Image invalid = view; invalid.stride[0] = 1;
    EXPECT_EQ(scope.begin({au::cv::readAccess(invalid)}), au::err::kErrorInvalidParam);
    invalid = view; invalid.dataSize[1] += 10000;
    EXPECT_EQ(scope.begin({au::cv::readAccess(invalid)}), au::err::kErrorOutOfRange);
    ASSERT_EQ(scope.begin({au::cv::writeAccess(image)}), 0);
    const int count = view.dataSize[0];
    EXPECT_EQ(au::flow::XFlow::get().parallelizeTiledTasks(static_cast<std::size_t>(count), 7,
        [&view](std::size_t offset, std::size_t size) { std::memset(view.data[0] + offset, 12, size); }), 0);
    EXPECT_EQ(view.data[0][count - 1], 12);
    image.reset(); // Physical storage remains leased by scope through finish.
    EXPECT_EQ(view.data[0][0], 12);
    EXPECT_EQ(scope.finish(), 0);
    EXPECT_FALSE(au::mm::isManaged(view.data[0]));
    EXPECT_EQ(scope.begin({au::mm::readAccess(view.data[0], 1)}), au::err::kErrorInvalidAddr);
}

TEST(XMemory, FenceSnapshotRetriesWithoutHoldingCpuReservation)
{
    au::mm::XMemory memory(64);
    ASSERT_TRUE(memory.valid());
    FenceModel first, next;
    const au::mm::Access write = au::mm::writeAccess(memory.data(), memory.size());
    au::mm::detail::DeviceAccess a, b;
    ASSERT_EQ(a.begin(&write, 1), 0);
    ASSERT_EQ(a.commit(fence(first)), 0);
    int result = -1, observed = 0;
    std::thread cpu([&] {
        au::mm::CpuAccessScope scope;
        result = scope.begin({au::mm::readAccess(memory.data(), memory.size())});
        if (!result) observed = static_cast<unsigned char*>(memory.data())[0];
        if (!result) result = scope.finish();
    });
    while (!first.mWaits.load(std::memory_order_relaxed)) std::this_thread::yield();
    // CPU waits without claiming any access rights. A new device submission
    // invalidates its snapshot and must also be observed before CPU resumes.
    const int reservation = b.begin(&write, 1);
    EXPECT_EQ(reservation, 0);
    if (!reservation) EXPECT_EQ(b.commit(fence(next)), 0);
    static_cast<unsigned char*>(memory.data())[0] = 42;
    first.mReady.store(true, std::memory_order_release);
    if (!reservation) {
        while (!next.mWaits.load(std::memory_order_relaxed)) std::this_thread::yield();
        static_cast<unsigned char*>(memory.data())[0] = 43;
    }
    next.mReady.store(true, std::memory_order_release);
    cpu.join();
    EXPECT_EQ(result, 0); EXPECT_EQ(observed, reservation ? 42 : 43);
    EXPECT_EQ(a.finish(), 0); EXPECT_EQ(b.finish(), 0);
}

TEST(XMemory, ConcurrentReadersShareExactlyOneCpuSession)
{
    AccessModel model;
    Mapping mapping;
    ASSERT_TRUE(mapping.valid());
    au::mm::ExternalMemory registration;
    ASSERT_EQ(registration.registerMemory(mapping.description(mapping.mFirst)), 0);
    for (int round = 0; round < 16; ++round) {
        au::mm::CpuAccessScope leader;
        ASSERT_EQ(leader.begin({au::mm::readAccess(mapping.mFirst, 256)}), 0);
        std::atomic<int> entered{0};
        std::atomic<bool> finish{false};
        std::array<std::thread, 4> readers;
        for (auto& thread : readers) thread = std::thread([&] {
            au::mm::CpuAccessScope scope;
            const int code = scope.begin({au::mm::readAccess(mapping.mFirst, 128)});
            EXPECT_EQ(code, 0);
            entered.fetch_add(1, std::memory_order_release);
            while (!finish.load(std::memory_order_acquire)) std::this_thread::yield();
            if (!code) EXPECT_EQ(scope.finish(), 0);
        });
        while (entered.load(std::memory_order_acquire) != 4) std::this_thread::yield();
        EXPECT_EQ(leader.finish(), 0);
        EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), round);
        finish.store(true, std::memory_order_release);
        for (auto& thread : readers) thread.join();
        EXPECT_EQ(model.mStarts.load(std::memory_order_relaxed), round + 1);
        EXPECT_EQ(model.mEnds.load(std::memory_order_relaxed), round + 1);
    }
}

TEST(XMemory, FailedDeviceFenceRetainsSourceAndBlocksCpuRead)
{
    au::mm::XMemory memory(64);
    ASSERT_TRUE(memory.valid());
    auto producer = au::flow::XFlow::get().enqueue([]() -> int { return 0; });
    ASSERT_EQ(au::flow::XFlow::get().wait(producer), 0);
    FenceModel model;
    model.mCode = au::err::kErrorPlatformAPI; model.mNative = -7;
    model.mCause = producer.id(); model.mDevice = true;
    model.mReady.store(true, std::memory_order_release);
    au::mm::detail::DeviceAccess device;
    const auto write = au::mm::writeAccess(memory.data(), memory.size());
    ASSERT_EQ(device.begin(&write, 1), 0);
    ASSERT_EQ(device.commit(fence(model)), 0);
    auto consumer = au::flow::XFlow::get().enqueue([&]() -> int {
        au::mm::CpuAccessScope scope;
        EXPECT_EQ(scope.begin({au::mm::readAccess(memory.data(), memory.size())}), au::err::kErrorPlatformAPI);
        EXPECT_FALSE(scope.active());
        return 0;
    });
    au::flow::TaskResult result{};
    ASSERT_EQ(au::flow::XFlow::get().wait(consumer, &result), 0);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI); EXPECT_EQ(result.mNativeCode, -7);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::OpenCL); EXPECT_EQ(result.mCauseTaskId, producer.id());
    EXPECT_EQ(device.finish(), au::err::kErrorPlatformAPI);
}

TEST(XMemory, DeviceHostBridgeWaitsBeforeReservingResources)
{
    for (bool hostStage : {false, true}) {
        au::mm::XMemory memory(64);
        ASSERT_TRUE(memory.valid());
        FenceModel model;
        auto pending = fence(model);
        pending->mContext = 17; pending->mHostCompletion = hostStage;
        const auto write = au::mm::writeAccess(memory.data(), memory.size());
        au::mm::detail::DeviceAccess producer, consumer;
        ASSERT_EQ(producer.begin(&write, 1, 17), 0);
        ASSERT_EQ(producer.commit(pending), 0);
        int accepted = -1;
        std::thread bridge([&] {
            accepted = consumer.begin(&write, 1, hostStage ? 17 : 23);
            if (!accepted) consumer.abort();
        });
        while (!model.mWaits.load(std::memory_order_relaxed)) std::this_thread::yield();
        // While the bridge waits, it must not own mDevicePreparing. Check via
        // the control lock; starting a CPU access would also wait this fence.
        au::mm::detail::MemoryLease lease;
        EXPECT_EQ(au::mm::Registry::instance().acquire(memory.data(), 1, lease), 0);
        if (lease.valid()) {
            std::lock_guard<std::mutex> lock(lease.control()->mAccessMutex);
            EXPECT_FALSE(lease.control()->mDevicePreparing);
        }
        model.mReady.store(true, std::memory_order_release);
        bridge.join();
        EXPECT_EQ(accepted, 0); EXPECT_EQ(producer.finish(), 0);
    }
}

TEST(XMemory, AccessFailuresJoinTaskAndNativeCause)
{
    AccessModel model;
    Mapping mapping;
    ASSERT_TRUE(mapping.valid());
    au::mm::ExternalMemory registration;
    ASSERT_EQ(registration.registerMemory(mapping.description(mapping.mFirst)), 0);
    model.mFailEnd.store(1, std::memory_order_relaxed);
    auto task = au::flow::XFlow::get().enqueue([&]() -> int {
        au::mm::CpuAccessScope scope;
        const int code = scope.begin({au::mm::writeAccess(mapping.mFirst, 256)});
        if (code) return code;
        return 0; // Destructor must report the failed END to the task scope.
    });
    au::flow::TaskResult result{};
    ASSERT_EQ(au::flow::XFlow::get().wait(task, &result), 0);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Memory);
    EXPECT_EQ(result.mNativeCode, 77);
    EXPECT_EQ(result.mCauseTaskId, task.id());

    // A captured scope is destroyed during final closure cleanup, after the
    // function body returned. Attribute its END error to this task too.
    Mapping later;
    ASSERT_TRUE(later.valid());
    au::mm::ExternalMemory laterRegistration;
    ASSERT_EQ(laterRegistration.registerMemory(later.description(later.mFirst)), 0);
    model.mFailEnd.store(2, std::memory_order_relaxed);
    std::unique_ptr<au::mm::CpuAccessScope> owned(new au::mm::CpuAccessScope());
    auto captured = au::flow::XFlow::get().enqueue([scope = std::move(owned), &later]() -> int {
        return scope->begin({au::mm::writeAccess(later.mFirst, 256)});
    });
    ASSERT_EQ(au::flow::XFlow::get().wait(captured, &result), 0);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Memory);
    EXPECT_EQ(result.mNativeCode, 77);
    EXPECT_EQ(result.mCauseTaskId, captured.id());

    Mapping explicitFinish;
    ASSERT_TRUE(explicitFinish.valid());
    au::mm::ExternalMemory explicitRegistration;
    ASSERT_EQ(explicitRegistration.registerMemory(explicitFinish.description(explicitFinish.mFirst)), 0);
    model.mFailEnd.store(3, std::memory_order_relaxed);
    auto forwarded = au::flow::XFlow::get().enqueue([&]() -> int {
        au::mm::CpuAccessScope scope;
        const int code = scope.begin({au::mm::writeAccess(explicitFinish.mFirst, 256)});
        return code ? code : scope.finish();
    });
    ASSERT_EQ(au::flow::XFlow::get().wait(forwarded, &result), 0);
    EXPECT_EQ(result.mCode, au::err::kErrorPlatformAPI);
    EXPECT_EQ(result.mDomain, au::flow::ErrorDomain::Memory); EXPECT_EQ(result.mNativeCode, 77);
    EXPECT_EQ(result.mCauseTaskId, forwarded.id());
}

#endif
#endif
