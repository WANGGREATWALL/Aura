#ifndef AURA_MM_XACCESS_H_
#define AURA_MM_XACCESS_H_

#include <initializer_list>
#include <cstddef>
#include <utility>
#include "mm/xaccess_api.h"
#include "log/xerror.h"
#include "sys/xvisibility.h"

namespace au {
namespace mm AU_LOCAL {

typedef ::au_mm_access_set Access;
enum class AccessMode : unsigned int { Read = AU_MM_READ, Write = AU_MM_WRITE, ReadWrite = AU_MM_READ_WRITE };

inline au::mm::Access memoryAccess(const void* data, std::size_t size, au::mm::AccessMode mode) noexcept
{
    au::mm::Access access = {};
    access.mStructSize = sizeof(access);
    access.mCount = 1;
    access.mRanges[0].mData = data; access.mRanges[0].mSize = size;
    access.mRanges[0].mMode = static_cast<unsigned int>(mode);
    return access;
}
inline au::mm::Access readAccess(const void* data, std::size_t size) noexcept
{ return au::mm::memoryAccess(data, size, au::mm::AccessMode::Read); }
inline au::mm::Access writeAccess(void* data, std::size_t size) noexcept
{ return au::mm::memoryAccess(data, size, au::mm::AccessMode::Write); }
inline au::mm::Access readWriteAccess(void* data, std::size_t size) noexcept
{ return au::mm::memoryAccess(data, size, au::mm::AccessMode::ReadWrite); }

// Header-only C++11 facade; no exported class layout or standard containers.
class CpuAccessScope final {
public:
    CpuAccessScope() noexcept = default;
    ~CpuAccessScope() noexcept { if (mHandle) (void)finish(); }
    CpuAccessScope(const au::mm::CpuAccessScope&) = delete;
    au::mm::CpuAccessScope& operator=(const au::mm::CpuAccessScope&) = delete;
    CpuAccessScope(au::mm::CpuAccessScope&& other) noexcept : mHandle(other.mHandle)
    { other.mHandle = nullptr; }
    au::mm::CpuAccessScope& operator=(au::mm::CpuAccessScope&& other) noexcept
    {
        if (this != &other) { if (mHandle) (void)finish(); mHandle = other.mHandle; other.mHandle = nullptr; }
        return *this;
    }
    int begin(const au::mm::Access* sets, std::size_t count) noexcept
    {
        if (mHandle) return au::err::kErrorStateInvalid;
        return ::au_mm_cpu_begin(sets, count, &mHandle);
    }
    int begin(std::initializer_list<au::mm::Access> sets) noexcept { return begin(sets.begin(), sets.size()); }
    int finish(int bodyCode = 0) noexcept
    { void* handle = mHandle; mHandle = nullptr; return ::au_mm_cpu_finish(handle, bodyCode); }
    bool active() const noexcept { return mHandle != nullptr; }
private:
    void* mHandle = nullptr;
};

class ExternalMemory final {
public:
    ExternalMemory() noexcept = default;
    ~ExternalMemory() noexcept { (void)reset(); }
    ExternalMemory(const au::mm::ExternalMemory&) = delete;
    au::mm::ExternalMemory& operator=(const au::mm::ExternalMemory&) = delete;
    ExternalMemory(au::mm::ExternalMemory&& other) noexcept : mHandle(other.mHandle) { other.mHandle = nullptr; }
    au::mm::ExternalMemory& operator=(au::mm::ExternalMemory&& other) noexcept
    {
        if (this != &other) { (void)reset(); mHandle = other.mHandle; other.mHandle = nullptr; }
        return *this;
    }
    int registerMemory(const ::au_mm_external_memory& memory) noexcept
    {
        if (mHandle) return au::err::kErrorStateInvalid;
        return ::au_mm_register(&memory, &mHandle);
    }
    int reset() noexcept { void* handle = mHandle; mHandle = nullptr; return handle ? ::au_mm_unregister(handle) : 0; }
    bool valid() const noexcept { return mHandle != nullptr; }
private:
    void* mHandle = nullptr;
};

} // namespace mm
} // namespace au
#endif
