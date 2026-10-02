#include "file/xfile.h"
#include "file/xfile_xmemory.h"
#include "log/xerror.h"
#include "log/xlogger.h"
#include "regex/xregex.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <vector>

#if AU_OS_WINDOWS
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace au {
namespace file {
namespace {

namespace fs = std::filesystem;

template<typename Fn>
int guardedStatus(Fn&& fn) noexcept
{
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        XLOG_E("file: out of memory\n");
        return err::kErrorNoMemory;
    } catch (const std::exception& e) {
        XLOG_E("file: operation failed: %s\n", e.what());
        return err::kErrorPlatformAPI;
    } catch (...) {
        XLOG_E("file: operation failed\n");
        return err::kErrorPlatformAPI;
    }
}

#if AU_OS_WINDOWS
// Convert UTF-8 paths to UTF-16 before calling the wide Windows file APIs.
std::wstring toWide(const std::string& path)
{
    if (path.empty() || path.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
        nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()),
            &wide[0], count) != count) {
        return {};
    }
    return wide;
}
#endif

// Read exactly size bytes at offset. A short read is an error, including when
// the file changes after its size was checked by the public entry point.
int readRawAt(const std::string& path, size_t offset, void* data, size_t size)
{
#if AU_OS_WINDOWS
    const std::wstring wide = toWide(path);
    if (wide.empty() || offset > static_cast<size_t>(std::numeric_limits<LONGLONG>::max())) {
        return err::kErrorOutOfRange;
    }
    HANDLE handle = CreateFileW(
        wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        XLOG_E("file::read open failed: %s (error=%lu)\n",
               path.c_str(), static_cast<unsigned long>(GetLastError()));
        return err::kErrorOpenFailed;
    }

    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        CloseHandle(handle);
        return err::kErrorReadFailed;
    }

    size_t remaining = size;
    auto* cursor = static_cast<unsigned char*>(data);
    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(
            std::min(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD n = 0;
        if (!ReadFile(handle, cursor, chunk, &n, nullptr) || n == 0) {
            CloseHandle(handle);
            XLOG_E("file::read incomplete: %s\n", path.c_str());
            return err::kErrorReadFailed;
        }
        cursor += n;
        remaining -= n;
    }
    const BOOL closed = CloseHandle(handle);
    return closed ? err::kSuccess : err::kErrorReadFailed;
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        XLOG_E("file::read open failed: %s (%s)\n", path.c_str(), std::strerror(errno));
        return err::kErrorOpenFailed;
    }
    if (offset > static_cast<size_t>(std::numeric_limits<off_t>::max())) {
        ::close(fd);
        return err::kErrorOutOfRange;
    }

    size_t remaining = size;
    auto* cursor = static_cast<unsigned char*>(data);
    off_t position = static_cast<off_t>(offset);
    while (remaining != 0) {
        const size_t chunk = std::min(
            remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = ::pread(fd, cursor, chunk, position);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ::close(fd);
            XLOG_E("file::read incomplete: %s (%s)\n", path.c_str(), std::strerror(errno));
            return err::kErrorReadFailed;
        }
        cursor += n;
        remaining -= static_cast<size_t>(n);
        if (remaining != 0) {
            if (position > std::numeric_limits<off_t>::max() - n) {
                ::close(fd);
                return err::kErrorOutOfRange;
            }
            position += n;
        }
    }
    return ::close(fd) == 0 ? err::kSuccess : err::kErrorReadFailed;
#endif
}

int readRaw(const std::string& path, void* data, size_t size)
{
    return readRawAt(path, 0, data, size);
}

// Shared implementation for write() and append(). Empty writes still create
// or truncate the target as specified by the corresponding public operation.
int writeImpl(const void* data, size_t size, const std::string& path, bool appendMode)
{
#if AU_OS_WINDOWS
    const std::wstring wide = toWide(path);
    if (wide.empty()) {
        return err::kErrorInvalidParam;
    }
    HANDLE handle = CreateFileW(
        wide.c_str(), GENERIC_WRITE, 0, nullptr,
        appendMode ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        XLOG_E("file::write open failed: %s (error=%lu)\n",
               path.c_str(), static_cast<unsigned long>(GetLastError()));
        return err::kErrorOpenFailed;
    }
    if (appendMode) {
        LARGE_INTEGER end{};
        if (!SetFilePointerEx(handle, end, nullptr, FILE_END)) {
            CloseHandle(handle);
            return err::kErrorWriteFailed;
        }
    }

    size_t remaining = size;
    const auto* cursor = static_cast<const unsigned char*>(data);
    while (remaining != 0) {
        const DWORD chunk = static_cast<DWORD>(
            std::min(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD n = 0;
        if (!WriteFile(handle, cursor, chunk, &n, nullptr) || n == 0) {
            CloseHandle(handle);
            XLOG_E("file::write incomplete: %s\n", path.c_str());
            return err::kErrorWriteFailed;
        }
        cursor += n;
        remaining -= n;
    }
    return CloseHandle(handle) ? err::kSuccess : err::kErrorWriteFailed;
#else
    const int flags = O_WRONLY | O_CREAT | (appendMode ? O_APPEND : O_TRUNC);
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        XLOG_E("file::write open failed: %s (%s)\n", path.c_str(), std::strerror(errno));
        return err::kErrorOpenFailed;
    }
    size_t remaining = size;
    const auto* cursor = static_cast<const unsigned char*>(data);
    while (remaining != 0) {
        const size_t chunk = std::min(
            remaining, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t n = ::write(fd, cursor, chunk);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ::close(fd);
            XLOG_E("file::write incomplete: %s (%s)\n", path.c_str(), std::strerror(errno));
            return err::kErrorWriteFailed;
        }
        cursor += n;
        remaining -= static_cast<size_t>(n);
    }
    return ::close(fd) == 0 ? err::kSuccess : err::kErrorWriteFailed;
#endif
}

std::vector<XPath> listEntries(
    const std::string& dir, const std::string& regex, bool absolute, bool directories)
{
    std::vector<XPath> result;
    try {
        std::error_code ec;
        fs::directory_iterator it(fs::u8path(dir), ec);
        if (ec) {
            XLOG_E("file::list opendir failed: %s (%s)\n", dir.c_str(), ec.message().c_str());
            return result;
        }
        const fs::directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                XLOG_E("file::list readdir failed: %s (%s)\n", dir.c_str(), ec.message().c_str());
                break;
            }
            const fs::file_type type = it->symlink_status(ec).type();
            if (ec) {
                ec.clear();
                continue;
            }
            if (directories ? type != fs::file_type::directory
                            : type != fs::file_type::regular) {
                continue;
            }
            const std::string name = it->path().filename().u8string();
            if (!regex.empty() && !au::regex::match(name, regex)) {
                continue;
            }
            result.emplace_back(absolute ? XPath(dir) / name : XPath(name));
        }
    } catch (const std::exception& e) {
        XLOG_E("file::list failed: %s (%s)\n", dir.c_str(), e.what());
        result.clear();
    } catch (...) {
        XLOG_E("file::list failed: %s\n", dir.c_str());
        result.clear();
    }
    return result;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Existence checks and file attributes
// ---------------------------------------------------------------------------

bool existFile(const std::string& path)
{
#if AU_OS_WINDOWS
    return [&]() noexcept -> bool {
        try {
            const std::wstring wide = toWide(path);
            if (wide.empty()) return false;
            const DWORD attr = GetFileAttributesW(wide.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        } catch (...) {
            return false;
        }
    }();
#else
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool existDir(const std::string& path)
{
#if AU_OS_WINDOWS
    return [&]() noexcept -> bool {
        try {
            const std::wstring wide = toWide(path);
            if (wide.empty()) return false;
            const DWORD attr = GetFileAttributesW(wide.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
        } catch (...) {
            return false;
        }
    }();
#else
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool exists(const std::string& path)
{
#if AU_OS_WINDOWS
    return [&]() noexcept -> bool {
        try {
            const std::wstring wide = toWide(path);
            return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
        } catch (...) {
            return false;
        }
    }();
#else
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
#endif
}

size_t sizeOf(const std::string& path)
{
#if AU_OS_WINDOWS
    try {
        const std::wstring wide = toWide(path);
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (wide.empty() ||
            !GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &attr) ||
            (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return 0;
        }
        const uint64_t size =
            (static_cast<uint64_t>(attr.nFileSizeHigh) << 32u) | attr.nFileSizeLow;
        return size <= std::numeric_limits<size_t>::max() ? static_cast<size_t>(size) : 0;
    } catch (...) {
        return 0;
    }
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        return 0;
    }
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    return size <= std::numeric_limits<size_t>::max() ? static_cast<size_t>(size) : 0;
#endif
}

// ---------------------------------------------------------------------------
// Directory operations
// ---------------------------------------------------------------------------

int createDir(const std::string& path)
{
    return guardedStatus([&] {
        if (path.empty()) return err::kErrorInvalidParam;
#if AU_OS_WINDOWS
        const std::wstring wide = toWide(path);
        if (wide.empty()) return err::kErrorInvalidParam;
        if (CreateDirectoryW(wide.c_str(), nullptr) || existDir(path)) {
            return err::kSuccess;
        }
        XLOG_E("file::createDir failed: %s (error=%lu)\n",
               path.c_str(), static_cast<unsigned long>(GetLastError()));
#else
        if (::mkdir(path.c_str(), 0755) == 0 || existDir(path)) {
            return err::kSuccess;
        }
        XLOG_E("file::createDir failed: %s (%s)\n", path.c_str(), std::strerror(errno));
#endif
        return err::kErrorOpenFailed;
    });
}

int createDirs(const std::string& path)
{
    return guardedStatus([&] {
        if (path.empty()) return err::kErrorInvalidParam;
        std::error_code ec;
        fs::create_directories(fs::u8path(path), ec);
        if (!ec && existDir(path)) return err::kSuccess;
        XLOG_E("file::createDirs failed: %s (%s)\n", path.c_str(), ec.message().c_str());
        return err::kErrorOpenFailed;
    });
}

// ---------------------------------------------------------------------------
// File create / remove
// ---------------------------------------------------------------------------

int createFile(const std::string& path)
{
    return guardedStatus([&] {
        if (path.empty()) return err::kErrorInvalidParam;
        return writeImpl(nullptr, 0, path, false);
    });
}

int removeFile(const std::string& path)
{
    return guardedStatus([&] {
        if (path.empty()) return err::kErrorInvalidParam;
#if AU_OS_WINDOWS
        const std::wstring wide = toWide(path);
        if (wide.empty()) return err::kErrorInvalidParam;
        if (DeleteFileW(wide.c_str())) return err::kSuccess;
        XLOG_E("file::removeFile failed: %s (error=%lu)\n",
               path.c_str(), static_cast<unsigned long>(GetLastError()));
#else
        if (::unlink(path.c_str()) == 0) return err::kSuccess;
        XLOG_E("file::removeFile failed: %s (%s)\n", path.c_str(), std::strerror(errno));
#endif
        return err::kErrorFileNotFound;
    });
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

int read(const std::string& path, std::string& buffer)
{
    return guardedStatus([&] {
        if (!existFile(path)) {
            XLOG_E("file::read file not found: %s\n", path.c_str());
            return err::kErrorFileNotFound;
        }
        const size_t size = sizeOf(path);
        buffer.clear();
        buffer.resize(size);
        return size == 0 ? err::kSuccess : readRaw(path, &buffer[0], size);
    });
}

int read(const std::string& path, mm::XMemory& buffer)
{
    return guardedStatus([&] {
        if (!existFile(path)) return err::kErrorFileNotFound;
        const size_t size = sizeOf(path);
        if (size == 0) return err::kSuccess;
        if (!buffer.valid()) {
            buffer = mm::XMemory(size, mm::MemType::Pss);
        }
        if (!buffer.valid() || buffer.size() != size) {
            return err::kErrorNoMemory;
        }
        return readRaw(path, buffer.data(), size);
    });
}

int read(const std::string& path, void* data, size_t sizeInByte)
{
    return guardedStatus([&] {
        if (!data) return err::kErrorNullPointer;
        if (!existFile(path)) return err::kErrorFileNotFound;
        if (sizeOf(path) != sizeInByte) return err::kErrorFileSizeMismatch;
        return readRaw(path, data, sizeInByte);
    });
}

int readAt(const std::string& path, size_t offset, size_t size, mm::XMemory& buffer)
{
    return guardedStatus([&] {
        if (!existFile(path)) return err::kErrorFileNotFound;
        const size_t fileSize = sizeOf(path);
        if (offset > fileSize || size > fileSize - offset) {
            return err::kErrorOutOfRange;
        }
        if (size == 0) return err::kSuccess;
        if (!buffer.valid()) {
            buffer = mm::XMemory(size, mm::MemType::Pss);
        }
        if (!buffer.valid() || buffer.size() != size) {
            return err::kErrorNoMemory;
        }
        return readRawAt(path, offset, buffer.data(), size);
    });
}

int readAt(const std::string& path, size_t offset, void* data, size_t sizeInByte)
{
    return guardedStatus([&] {
        if (!data) return err::kErrorNullPointer;
        if (!existFile(path)) return err::kErrorFileNotFound;
        const size_t fileSize = sizeOf(path);
        if (offset > fileSize || sizeInByte > fileSize - offset) {
            return err::kErrorOutOfRange;
        }
        return readRawAt(path, offset, data, sizeInByte);
    });
}

// ---------------------------------------------------------------------------
// Write / append
// ---------------------------------------------------------------------------

int write(const std::string& content, const std::string& path)
{
    return guardedStatus([&] {
        return writeImpl(content.data(), content.size(), path, false);
    });
}

int write(const mm::XMemory& content, const std::string& path)
{
    return guardedStatus([&] {
        if (!content.valid() || content.size() == 0) return err::kErrorInvalidParam;
        return writeImpl(content.data(), content.size(), path, false);
    });
}

int write(const void* data, size_t sizeInByte, const std::string& path)
{
    return guardedStatus([&] {
        if (!data) return err::kErrorNullPointer;
        return writeImpl(data, sizeInByte, path, false);
    });
}

int append(const std::string& content, const std::string& path)
{
    return guardedStatus([&] {
        return writeImpl(content.data(), content.size(), path, true);
    });
}

int append(const void* data, size_t sizeInByte, const std::string& path)
{
    return guardedStatus([&] {
        if (!data) return err::kErrorNullPointer;
        return writeImpl(data, sizeInByte, path, true);
    });
}

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

std::vector<XPath> listFiles(
    const std::string& dir, const std::string& regex, bool absolute)
{
    return listEntries(dir, regex, absolute, false);
}

std::vector<XPath> listDirs(
    const std::string& dir, const std::string& regex, bool absolute)
{
    return listEntries(dir, regex, absolute, true);
}

}  // namespace file
}  // namespace au
