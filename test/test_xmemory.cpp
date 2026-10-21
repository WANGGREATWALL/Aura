#if ENABLE_TEST_XMEMORY

#include <cstring>
#include <string>
#include <utility>
#include <atomic>
#include <thread>

#include "gtest/gtest.h"
#include "log/xerror.h"
#include "mm/xmemory.h"
#include "mm/registry.h"

TEST(XMemory, DefaultAndInvalidArguments)
{
    au::mm::XMemory memory;
    EXPECT_FALSE(memory.valid());
    EXPECT_EQ(memory.data(), nullptr);
    EXPECT_EQ(memory.size(), 0u);
    EXPECT_EQ(memory.fd(), -1);
    EXPECT_EQ(memory.type(), au::mm::MemType::Pss);
    EXPECT_EQ(memory.backend(), au::mm::BackendId::Auto);
    EXPECT_EQ(memory.syncCpuToDevice(), au::err::kErrorNotInitialized);
    EXPECT_EQ(memory.syncDeviceToCpu(), au::err::kErrorNotInitialized);

    au::mm::MemBlock block;
    EXPECT_EQ(au::mm::alloc(0, au::mm::MemType::Pss, block), au::err::kErrorInvalidSize);
    EXPECT_EQ(au::mm::free(nullptr), au::err::kErrorNullPointer);
    EXPECT_EQ(au::mm::query(nullptr, block), au::err::kErrorNullPointer);
    EXPECT_EQ(au::mm::syncCpuToDevice(nullptr), au::err::kErrorNullPointer);
    EXPECT_EQ(au::mm::syncDeviceToCpu(nullptr), au::err::kErrorNullPointer);
    EXPECT_FALSE(au::mm::isManaged(nullptr));

    int unrelated = 0;
    EXPECT_FALSE(au::mm::isManaged(&unrelated));
    EXPECT_EQ(au::mm::query(&unrelated, block), au::err::kErrorInvalidAddr);
    EXPECT_EQ(au::mm::free(&unrelated), au::err::kErrorInvalidAddr);
}

TEST(XMemory, BackendSelection)
{
    ASSERT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);
    const au::mm::BackendId before = au::mm::currentBackend();
    EXPECT_EQ(before, au::mm::BackendId::Native);
    EXPECT_EQ(au::mm::setBackend(au::mm::BackendId::Pool), au::err::kErrorNotSupported);
    EXPECT_EQ(au::mm::currentBackend(), before);
    EXPECT_EQ(au::mm::setBackend(static_cast<au::mm::BackendId>(99)), au::err::kErrorInvalidParam);
    EXPECT_EQ(au::mm::currentBackend(), before);
    EXPECT_EQ(au::mm::setBackend(au::mm::BackendId::Native), au::err::kSuccess);
    EXPECT_EQ(au::mm::currentBackend(), au::mm::BackendId::Native);
    EXPECT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);

    EXPECT_STREQ(au::mm::backendName(au::mm::BackendId::Auto), "auto");
    EXPECT_STREQ(au::mm::backendName(au::mm::BackendId::Pool), "pool");
    EXPECT_STREQ(au::mm::backendName(au::mm::BackendId::Native), "native");
    EXPECT_STREQ(au::mm::backendName(static_cast<au::mm::BackendId>(99)), "unknown");
}

TEST(XMemory, NativeAllocationAndRegistry)
{
    ASSERT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);
    au::mm::MemBlock block;
    ASSERT_EQ(au::mm::alloc(128, au::mm::MemType::Pss, block), au::err::kSuccess);
    ASSERT_NE(block.ptr, nullptr);
    EXPECT_EQ(block.size, 128u);
    EXPECT_EQ(block.fd, -1);
    EXPECT_EQ(block.type, au::mm::MemType::Pss);
    EXPECT_EQ(block.backend, au::mm::BackendId::Native);
    EXPECT_TRUE(au::mm::isManaged(block.ptr));

    au::mm::MemBlock queried;
    EXPECT_EQ(au::mm::query(block.ptr, queried), au::err::kSuccess);
    EXPECT_EQ(queried.ptr, block.ptr);
    EXPECT_EQ(queried.size, block.size);
    std::memset(block.ptr, 0x5a, block.size);
    EXPECT_EQ(static_cast<unsigned char*>(block.ptr)[0], 0x5au);
    EXPECT_EQ(au::mm::syncCpuToDevice(block.ptr), au::err::kSuccess);
    EXPECT_EQ(au::mm::syncDeviceToCpu(block.ptr), au::err::kSuccess);

    EXPECT_EQ(au::mm::setBackend(au::mm::BackendId::Native), au::err::kSuccess);
    EXPECT_EQ(au::mm::free(block.ptr), au::err::kSuccess);
    EXPECT_FALSE(au::mm::isManaged(block.ptr));
    EXPECT_EQ(au::mm::query(block.ptr, queried), au::err::kErrorInvalidAddr);
    EXPECT_EQ(au::mm::free(block.ptr), au::err::kErrorInvalidAddr);
    EXPECT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);
    for (std::size_t size : {std::size_t(1), std::size_t(64), std::size_t(1024 * 1024), std::size_t(1024 * 1024 + 1)}) {
        au::mm::MemBlock sample;
        ASSERT_EQ(au::mm::alloc(size, au::mm::MemType::Pss, sample), 0);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(sample.ptr) % 64, 0u);
        static_cast<unsigned char*>(sample.ptr)[size - 1] = 79;
        EXPECT_EQ(static_cast<unsigned char*>(sample.ptr)[size - 1], 79);
        EXPECT_EQ(au::mm::free(sample.ptr), 0);
    }
}

TEST(XMemory, StableIdentityInteriorViewsAndDeferredRelease)
{
    au::mm::MemBlock block;
    ASSERT_EQ(au::mm::alloc(128, au::mm::MemType::Pss, block), 0);
    au::mm::detail::MemoryLease whole, alias;
    ASSERT_EQ(au::mm::Registry::instance().acquire(block.ptr, block.size, whole), 0);
    ASSERT_EQ(au::mm::Registry::instance().acquire(static_cast<unsigned char*>(block.ptr) + 96, 32, alias), 0);
    const au::mm::detail::MemoryIdentity identity = whole.identity();
    EXPECT_NE(identity.mOwner, 0u); EXPECT_NE(identity.mAllocationId, 0u);
    EXPECT_TRUE(identity == alias.identity());
    EXPECT_EQ(alias.offset(), 96u); EXPECT_EQ(alias.size(), 32u);
    EXPECT_EQ(au::mm::Registry::instance().acquire(static_cast<unsigned char*>(block.ptr) + 96, 33, alias),
              au::err::kErrorOutOfRange);
    EXPECT_EQ(au::mm::Registry::instance().acquire(nullptr, 1, alias), au::err::kErrorNullPointer);
    EXPECT_EQ(au::mm::Registry::instance().acquire(block.ptr, 0, alias), au::err::kErrorInvalidSize);
    std::shared_ptr<au::mm::detail::ReleaseStatus> report = whole.releaseStatus();
    EXPECT_EQ(au::mm::free(block.ptr), 0);
    EXPECT_FALSE(au::mm::isManaged(block.ptr));
    EXPECT_FALSE(report->mDone.load(std::memory_order_acquire));
    std::memset(whole.block().ptr, 0x6a, whole.block().size); // Internal lease owns storage after public retirement.
    EXPECT_EQ(static_cast<unsigned char*>(alias.block().ptr)[127], 0x6a);
    alias.reset();
    EXPECT_FALSE(report->mDone.load(std::memory_order_acquire));
    whole.reset();
    ASSERT_TRUE(report->mDone.load(std::memory_order_acquire));
    EXPECT_EQ(report->mCode, 0);
    au::mm::MemBlock next;
    ASSERT_EQ(au::mm::alloc(128, au::mm::MemType::Pss, next), 0);
    ASSERT_EQ(au::mm::Registry::instance().acquire(next.ptr, next.size, whole), 0);
    EXPECT_FALSE(identity == whole.identity()); // New allocation, even if the allocator reused its address.
    EXPECT_EQ(au::mm::free(next.ptr), 0);
}

TEST(XMemory, LeaseProtectsBackingAcrossConcurrentRetirement)
{
    for (int i = 0; i < 64; ++i) {
        au::mm::MemBlock block;
        ASSERT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, block), 0);
        static_cast<unsigned char*>(block.ptr)[0] = 37;
        std::atomic<bool> acquired{false}, retired{false};
        std::thread user([&] {
            au::mm::detail::MemoryLease lease;
            EXPECT_EQ(au::mm::Registry::instance().acquire(block.ptr, block.size, lease), 0);
            acquired.store(true, std::memory_order_release);
            while (!retired.load(std::memory_order_acquire)) std::this_thread::yield();
            EXPECT_EQ(static_cast<unsigned char*>(lease.block().ptr)[0], 37);
        });
        while (!acquired.load(std::memory_order_acquire)) std::this_thread::yield();
        EXPECT_EQ(au::mm::free(block.ptr), 0);
        retired.store(true, std::memory_order_release);
        user.join();
    }
}

TEST(XMemory, MoveAndLifetime)
{
    ASSERT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);
    void* released = nullptr;
    {
        au::mm::XMemory first(64, au::mm::MemType::Pss);
        ASSERT_TRUE(first.valid());
        EXPECT_EQ(first.size(), 64u);
        EXPECT_EQ(first.type(), au::mm::MemType::Pss);
        EXPECT_EQ(first.backend(), au::mm::BackendId::Native);
        EXPECT_EQ(first.fd(), -1);
        EXPECT_NE(first.info().find("size=64"), std::string::npos);
        EXPECT_EQ(first.syncCpuToDevice(), au::err::kSuccess);
        EXPECT_EQ(first.syncDeviceToCpu(), au::err::kSuccess);

        au::mm::XMemory moved(std::move(first));
        EXPECT_FALSE(first.valid());
        EXPECT_TRUE(moved.valid());
        EXPECT_EQ(moved.size(), 64u);

        au::mm::XMemory target(16, au::mm::MemType::Pss);
        ASSERT_TRUE(target.valid());
        void* oldTarget = target.data();
        target = std::move(moved);
        EXPECT_FALSE(moved.valid());
        EXPECT_TRUE(target.valid());
        EXPECT_EQ(target.size(), 64u);
        EXPECT_FALSE(au::mm::isManaged(oldTarget));
        released = target.data();
    }
    EXPECT_FALSE(au::mm::isManaged(released));
}

TEST(XMemory, OptionalDmaAllocation)
{
    ASSERT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);
    au::mm::MemBlock block;
    const int result = au::mm::alloc(4096, au::mm::MemType::DmaCached, block);
    if (result == au::err::kSuccess) {
        EXPECT_NE(block.ptr, nullptr);
        EXPECT_GE(block.fd, 0);
        EXPECT_EQ(block.type, au::mm::MemType::DmaCached);
        EXPECT_EQ(block.backend, au::mm::BackendId::Native);
        EXPECT_TRUE(au::mm::isManaged(block.ptr));
        EXPECT_EQ(au::mm::free(block.ptr), au::err::kSuccess);
    } else {
        EXPECT_LT(result, 0);
        EXPECT_EQ(block.ptr, nullptr);
    }
}

#endif  // ENABLE_TEST_XMEMORY
