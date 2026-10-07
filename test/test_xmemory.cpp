#if ENABLE_TEST_XMEMORY

#include <cstring>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "log/xerror.h"
#include "mm/xmemory.h"

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

#if defined(__linux__) || defined(__ANDROID__)

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

#else

TEST(XMemory, NativeBackendUnavailable)
{
    ASSERT_EQ(au::mm::setBackend(au::mm::BackendId::Auto), au::err::kSuccess);
    au::mm::MemBlock block;
    EXPECT_EQ(au::mm::alloc(64, au::mm::MemType::Pss, block), au::err::kErrorNotSupported);
    EXPECT_EQ(block.ptr, nullptr);
    au::mm::XMemory memory(64, au::mm::MemType::Pss);
    EXPECT_FALSE(memory.valid());
}

#endif

#endif  // ENABLE_TEST_XMEMORY
