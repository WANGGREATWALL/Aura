#if ENABLE_TEST_XMEMORY

#include "gtest/gtest.h"
#include "log/xerror.h"
#include "mm/xmemory.h"

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#include <unistd.h>
#endif

#if defined(__linux__) || defined(__ANDROID__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#endif

#include <cstring>
#include <utility>

using au::mm::Backend;
using au::mm::CachePolicy;
using au::mm::HandleOwnership;
using au::mm::MapAccess;
using au::mm::Memory;
using au::mm::MemoryDesc;
using au::mm::MemoryInfo;
using au::mm::MemoryKind;
using au::mm::NativeHandle;
using au::mm::NativeHandleType;

TEST(XMemory, DefaultInvalid)
{
    Memory mem;
    EXPECT_FALSE(mem.valid());
    EXPECT_EQ(mem.size(), 0u);
    EXPECT_EQ(mem.capabilities(), 0u);
    EXPECT_EQ(mem.flush(0, 0), err::kErrorInvalidHandle);

    MemoryInfo info;
    EXPECT_EQ(mem.queryInfo(&info), err::kErrorInvalidHandle);
    EXPECT_EQ(au::mm::queryInfo(mem, nullptr), err::kErrorNullPointer);

    int errCode = err::kSuccess;
    auto mapped = au::mm::map(mem, MapAccess::Read, 0, 1, &errCode);
    EXPECT_FALSE(mapped.valid());
    EXPECT_EQ(errCode, err::kErrorInvalidHandle);
}

TEST(XMemory, HostAllocateMapWriteRead)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size      = 64;
    desc.alignment = 32;
    desc.kind      = MemoryKind::Host;

    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mem.valid());
    EXPECT_EQ(mem.size(), 64u);
    EXPECT_EQ(mem.kind(), MemoryKind::Host);
    EXPECT_EQ(mem.backend(), Backend::Host);
    EXPECT_EQ(mem.actualCachePolicy(), CachePolicy::Cached);
    EXPECT_TRUE(mem.isCpuMappable());
    EXPECT_FALSE(mem.canExportNativeHandle());

    MemoryInfo info;
    ASSERT_EQ(mem.queryInfo(&info), err::kSuccess);
    EXPECT_EQ(info.size, 64u);
    EXPECT_EQ(info.alignment, 32u);
    EXPECT_EQ(info.kind, MemoryKind::Host);
    EXPECT_EQ(info.backend, Backend::Host);
    EXPECT_TRUE((info.capabilities & au::mm::MEMORY_CAP_CPU_MAPPABLE) != 0);

    auto mapped = au::mm::map(mem, MapAccess::ReadWrite, 4, 16, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mapped.valid());
    ASSERT_EQ(mapped.size(), 16u);

    std::memset(mapped.data(), 0x5a, mapped.size());
    EXPECT_EQ(mapped.flush(0, mapped.size()), err::kSuccess);
    EXPECT_EQ(mapped.invalidate(0, mapped.size()), err::kSuccess);
    EXPECT_EQ(mapped.validate(0, mapped.size()), err::kSuccess);
    EXPECT_EQ(mem.validate(0, mem.size()), err::kSuccess);

    const unsigned char* bytes = static_cast<const unsigned char*>(mapped.data());
    for (size_t i = 0; i < mapped.size(); ++i) {
        EXPECT_EQ(bytes[i], 0x5au);
    }
    EXPECT_EQ(mapped.unmap(), err::kSuccess);
    EXPECT_FALSE(mapped.valid());
}

TEST(XMemory, HostMoveInvalidatesSource)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size = 16;

    Memory a = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(a.valid());

    Memory b = std::move(a);
    EXPECT_FALSE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.size(), 16u);
}

TEST(XMemory, MappingKeepsMemoryAlive)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size = 32;

    auto mapped = au::mm::MappedRange();
    {
        Memory mem = au::mm::allocate(desc, &errCode);
        ASSERT_EQ(errCode, err::kSuccess);
        mapped = au::mm::map(mem, MapAccess::ReadWrite, 0, desc.size, &errCode);
        ASSERT_EQ(errCode, err::kSuccess);
        ASSERT_TRUE(mapped.valid());
        static_cast<unsigned char*>(mapped.data())[0] = 0x33;
    }

    ASSERT_TRUE(mapped.valid());
    EXPECT_EQ(static_cast<unsigned char*>(mapped.data())[0], 0x33u);
    EXPECT_EQ(mapped.unmap(), err::kSuccess);
}

TEST(XMemory, InvalidSizeAndRangeChecks)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size = 0;
    Memory empty = au::mm::allocate(desc, &errCode);
    EXPECT_FALSE(empty.valid());
    EXPECT_EQ(errCode, err::kErrorInvalidSize);

    desc.size = 8;
    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);

    auto mapped = au::mm::map(mem, MapAccess::Read, 4, 8, &errCode);
    EXPECT_FALSE(mapped.valid());
    EXPECT_EQ(errCode, err::kErrorOutOfRange);
    EXPECT_EQ(mem.flush(7, 2), err::kErrorOutOfRange);
}

TEST(XMemory, AlignmentIsNormalizedAndInvalidAlignmentRejected)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size      = 16;
    desc.alignment = 1;
    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mem.valid());

    MemoryInfo info;
    ASSERT_EQ(au::mm::queryInfo(mem, &info), err::kSuccess);
    EXPECT_GE(info.alignment, sizeof(void*));

    desc.alignment = 24;
    Memory bad = au::mm::allocate(desc, &errCode);
    EXPECT_FALSE(bad.valid());
    EXPECT_EQ(errCode, err::kErrorAlignmentFault);
}

TEST(XMemory, HostNativeHandleUnsupported)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size = 16;
    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);

    NativeHandle handle;
    EXPECT_EQ(au::mm::exportNativeHandle(mem, NativeHandleType::PosixFd, &handle), err::kErrorNotSupported);
    EXPECT_EQ(handle.type, NativeHandleType::None);
    EXPECT_EQ(au::mm::exportNativeHandle(mem, NativeHandleType::None, nullptr), err::kErrorNullPointer);
    EXPECT_STREQ(au::mm::backendName(Backend::Host), "Host");
    EXPECT_STREQ(au::mm::memoryKindName(MemoryKind::Host), "Host");
}

TEST(XMemory, DmaHeapAvailabilityQuery)
{
    bool available = true;
    EXPECT_EQ(au::mm::queryDmaHeapAvailable(nullptr, &available), err::kErrorInvalidParam);
    EXPECT_FALSE(available);
    EXPECT_EQ(au::mm::queryDmaHeapAvailable("", &available), err::kErrorInvalidParam);
    EXPECT_EQ(au::mm::queryDmaHeapAvailable("system", nullptr), err::kErrorNullPointer);

#if defined(__linux__) || defined(__ANDROID__)
    int ret = au::mm::queryDmaHeapAvailable("aura_heap_that_should_not_exist", &available);
    EXPECT_EQ(ret, err::kSuccess);
    EXPECT_FALSE(available);
#else
    EXPECT_EQ(au::mm::queryDmaHeapAvailable("system", &available), err::kErrorNotSupported);
#endif
}

#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
TEST(XMemory, PosixSharedExportImportRoundTrip)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size    = 128;
    desc.kind    = MemoryKind::Shared;
    desc.backend = Backend::PosixSharedMemory;

    Memory a = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(a.canExportNativeHandle());

    NativeHandle handle;
    ASSERT_EQ(au::mm::exportNativeHandle(a, NativeHandleType::PosixFd, &handle), err::kSuccess);
    ASSERT_EQ(handle.type, NativeHandleType::PosixFd);
    ASSERT_GE(handle.value0, 0);

    Memory b = au::mm::importNativeHandle(handle, desc, &errCode);
    close(static_cast<int>(handle.value0));
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(b.valid());

    MemoryInfo info;
    ASSERT_EQ(b.queryInfo(&info), err::kSuccess);
    EXPECT_EQ(info.size, desc.size);
    EXPECT_EQ(info.kind, MemoryKind::Shared);
    EXPECT_TRUE((info.capabilities & au::mm::MEMORY_CAP_EXPORT_FD) != 0);
    EXPECT_TRUE((info.capabilities & au::mm::MEMORY_CAP_CROSS_PROCESS) != 0);

    auto mapA = au::mm::map(a, MapAccess::ReadWrite, 0, desc.size, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    auto mapB = au::mm::map(b, MapAccess::ReadWrite, 0, desc.size, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);

    static_cast<unsigned char*>(mapA.data())[3] = 0x7b;
    EXPECT_EQ(static_cast<unsigned char*>(mapB.data())[3], 0x7bu);
}
#endif

#if !defined(__linux__) && !defined(__ANDROID__)
TEST(XMemory, DmaBufUnsupportedOnNonLinuxHost)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size    = 4096;
    desc.kind    = MemoryKind::DmaBuf;
    desc.backend = Backend::LinuxDmaBufHeap;

    Memory mem = au::mm::allocate(desc, &errCode);
    EXPECT_FALSE(mem.valid());
    EXPECT_EQ(errCode, err::kErrorNotSupported);
}
#endif

#if defined(__linux__) || defined(__ANDROID__)
TEST(XMemory, DmaBufHeapSystemAllocatesWhenAvailable)
{
    struct stat st;
    if (stat("/dev/dma_heap/system", &st) != 0 || access("/dev/dma_heap/system", R_OK) != 0) {
        int errCode = err::kSuccess;
        MemoryDesc desc;
        desc.size     = 4096;
        desc.kind     = MemoryKind::DmaBuf;
        desc.backend  = Backend::LinuxDmaBufHeap;
        desc.heapName = "system";

        Memory mem = au::mm::allocate(desc, &errCode);
        EXPECT_FALSE(mem.valid());
        EXPECT_TRUE(errCode == err::kErrorNotSupported || errCode == err::kErrorOpenFailed ||
                    errCode == err::kErrorPermissionDenied);
        return;
    }

    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size      = 4096;
    desc.kind      = MemoryKind::DmaBuf;
    desc.backend   = Backend::LinuxDmaBufHeap;
    desc.heapName  = "system";
    desc.debugName = "xmemory_dmabuf_system_test";

    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mem.valid());
    EXPECT_EQ(mem.kind(), MemoryKind::DmaBuf);
    EXPECT_EQ(mem.backend(), Backend::LinuxDmaBufHeap);
    EXPECT_TRUE(mem.supports(au::mm::MEMORY_CAP_DMA_BUF));
    EXPECT_TRUE(mem.supports(au::mm::MEMORY_CAP_CACHE_SYNC));

    MemoryInfo info;
    ASSERT_EQ(mem.queryInfo(&info), err::kSuccess);
    EXPECT_EQ(info.size, desc.size);
    EXPECT_STREQ(info.heapName, "system");

    NativeHandle handle;
    ASSERT_EQ(au::mm::exportNativeHandle(mem, NativeHandleType::DmaBufFd, &handle), err::kSuccess);
    ASSERT_EQ(handle.type, NativeHandleType::DmaBufFd);
    ASSERT_GE(handle.value0, 0);
    close(static_cast<int>(handle.value0));

    auto mapped = au::mm::map(mem, MapAccess::ReadWrite, 0, desc.size, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mapped.valid());
    static_cast<unsigned char*>(mapped.data())[0] = 0xa5;
    EXPECT_EQ(mapped.flush(0, 1), err::kSuccess);
    EXPECT_EQ(mapped.unmap(), err::kSuccess);
}
#endif

#if defined(__ANDROID__)
TEST(XMemory, AndroidHardwareBufferRawBlob)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size    = 4096;
    desc.kind    = MemoryKind::AndroidHardwareBuffer;
    desc.backend = Backend::AndroidHardwareBuffer;
    desc.usage   = au::mm::MEMORY_USAGE_CPU_READ | au::mm::MEMORY_USAGE_CPU_WRITE;

    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mem.valid());
    EXPECT_EQ(mem.kind(), MemoryKind::AndroidHardwareBuffer);
    EXPECT_EQ(mem.backend(), Backend::AndroidHardwareBuffer);
    EXPECT_TRUE(mem.supports(au::mm::MEMORY_CAP_ANDROID_AHB));

    auto mapped = au::mm::map(mem, MapAccess::ReadWrite, 0, desc.size, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mapped.valid());
    static_cast<unsigned char*>(mapped.data())[0] = 0x5c;
    EXPECT_EQ(mapped.unmap(), err::kSuccess);

    NativeHandle handle;
    ASSERT_EQ(au::mm::exportNativeHandle(mem, NativeHandleType::AndroidHardwareBuffer, &handle), err::kSuccess);
    EXPECT_EQ(handle.type, NativeHandleType::AndroidHardwareBuffer);
    EXPECT_NE(handle.value0, 0);
    AHardwareBuffer_release(reinterpret_cast<AHardwareBuffer*>(handle.value0));
}

TEST(XMemory, AndroidHardwareBufferRejectsImageUsageInBlobPhase)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size    = 4096;
    desc.kind    = MemoryKind::AndroidHardwareBuffer;
    desc.backend = Backend::AndroidHardwareBuffer;
    desc.usage   = au::mm::MEMORY_USAGE_CPU_READ | au::mm::MEMORY_USAGE_GPU_SAMPLED;

    Memory mem = au::mm::allocate(desc, &errCode);
    EXPECT_FALSE(mem.valid());
    EXPECT_EQ(errCode, err::kErrorNotSupported);
}

TEST(XMemory, AndroidHardwareBufferWithoutCpuUsageIsNotCpuMappable)
{
    int errCode = err::kSuccess;
    MemoryDesc desc;
    desc.size    = 4096;
    desc.kind    = MemoryKind::AndroidHardwareBuffer;
    desc.backend = Backend::AndroidHardwareBuffer;
    desc.usage   = 0;

    Memory mem = au::mm::allocate(desc, &errCode);
    ASSERT_EQ(errCode, err::kSuccess);
    ASSERT_TRUE(mem.valid());
    EXPECT_FALSE(mem.isCpuMappable());
    auto mapped = au::mm::map(mem, MapAccess::Read, 0, desc.size, &errCode);
    EXPECT_FALSE(mapped.valid());
    EXPECT_EQ(errCode, err::kErrorNotSupported);
}
#endif

#endif  // ENABLE_TEST_XMEMORY
