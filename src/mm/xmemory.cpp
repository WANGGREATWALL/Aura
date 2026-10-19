#include "mm/xmemory.h"

#include <cstdio>
#include <utility>

#include "log/xerror.h"
#include "log/xlogger.h"
#include "mm/backend.h"
#include "mm/pool_backend.h"
#include "mm/registry.h"

namespace au {
namespace mm {

// ---------------------------------------------------------------------------
//  Free functions
// ---------------------------------------------------------------------------

int alloc(size_t size, MemType type, MemBlock& out) noexcept
{
    if (size == 0)
        return au::err::kErrorInvalidSize;

    IBackend& backend = resolve();
    int retAlloc = backend.alloc(size, type, out);
    if (retAlloc != au::err::kSuccess) {
        return retAlloc;
    }
    int retRegis = Registry::instance().insert(out);
    if (retRegis != au::err::kSuccess) {
        // Rolling back the allocation is mandatory: keeping it would leak the
        // dma-buf fd and mmap mapping with no way for the caller to recover.
        backend.release(out);
        out = MemBlock{};
        return retRegis;
    }
    return au::err::kSuccess;
}

int free(void* ptr) noexcept
{
    if (ptr == nullptr)
        return au::err::kErrorNullPointer;

    MemBlock block;
    int retTake = Registry::instance().take(ptr, block);
    if (retTake != au::err::kSuccess)
        return retTake;

    return forBackend(block.backend).release(block);
}

int syncCpuToDevice(void* ptr) noexcept
{
    MemBlock block;
    int retFind = Registry::instance().find(ptr, block);
    if (retFind != au::err::kSuccess)
        return retFind;
    return forBackend(block.backend).syncCpuToDevice(block);
}

int syncDeviceToCpu(void* ptr) noexcept
{
    MemBlock block;
    int retFind = Registry::instance().find(ptr, block);
    if (retFind != au::err::kSuccess)
        return retFind;
    return forBackend(block.backend).syncDeviceToCpu(block);
}

int query(void* ptr, MemBlock& out) noexcept
{
    return Registry::instance().find(ptr, out);
}

bool isManaged(void* ptr) noexcept
{
    MemBlock dummy;
    return Registry::instance().find(ptr, dummy) == au::err::kSuccess;
}

// ---------------------------------------------------------------------------
//  Backend selection
// ---------------------------------------------------------------------------

int setBackend(BackendId id) noexcept
{
    if (id != BackendId::Auto && id != BackendId::Pool && id != BackendId::Native) {
        return au::err::kErrorInvalidParam;
    }
    if (id == BackendId::Pool && !poolBackend().available()) {
        XLOG_E("au::mm::Pool backend is not implemented; use BackendId::Native or Auto\n");
        return au::err::kErrorNotSupported;
    }
    setSelection(id);
    return au::err::kSuccess;
}

BackendId currentBackend() noexcept
{
    return effectiveSelection();
}

const char* backendName(BackendId id) noexcept
{
    switch (id) {
        case BackendId::Auto: return "auto";
        case BackendId::Pool: return "pool";
        case BackendId::Native: return "native";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
//  XMemory
// ---------------------------------------------------------------------------

XMemory::XMemory(size_t size, MemType type)
{
    int retAlloc = alloc(size, type, mBlock);
    if (retAlloc != au::err::kSuccess) {
        mBlock = MemBlock{};
    }
}

XMemory::~XMemory()
{
    if (mBlock.ptr != nullptr) {
        int retFree = free(mBlock.ptr);
        if (retFree != au::err::kSuccess) {
            XLOG_E("au::mm::XMemory::~XMemory free failed ret=%d ptr=%p\n", retFree, mBlock.ptr);
        }
    }
}

XMemory::XMemory(XMemory&& other) noexcept : mBlock(other.mBlock)
{
    other.mBlock = MemBlock{};
}

XMemory& XMemory::operator=(XMemory&& other) noexcept
{
    if (this != &other) {
        if (mBlock.ptr != nullptr) {
            free(mBlock.ptr);
        }
        mBlock = other.mBlock;
        other.mBlock = MemBlock{};
    }
    return *this;
}

int XMemory::syncCpuToDevice() noexcept
{
    if (!valid())
        return au::err::kErrorNotInitialized;
    return mm::syncCpuToDevice(mBlock.ptr);
}

int XMemory::syncDeviceToCpu() noexcept
{
    if (!valid())
        return au::err::kErrorNotInitialized;
    return mm::syncDeviceToCpu(mBlock.ptr);
}

std::string XMemory::info() const
{
    char buf[160];
    std::snprintf(buf, sizeof(buf), "XMemory[ptr=%p, size=%zu, fd=%d, type=%d, backend=%s]", mBlock.ptr, mBlock.size,
                  mBlock.fd, static_cast<int>(mBlock.type), backendName(mBlock.backend));
    return std::string(buf);
}

}  // namespace mm
}  // namespace au
