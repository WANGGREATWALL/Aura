#include "sys/atomic_file.h"
#include "sys/xsystem.h"
#include "log/xerror.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#if AU_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

int au::sys::replaceFile(const std::string& path, const void* bytes, std::size_t size) noexcept
{
    if ((!bytes && size) || path.empty()) return au::err::kErrorInvalidParam;
    try {
#if AU_OS_WINDOWS
        if (path.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return au::err::kErrorOutOfRange;
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
        if (count <= 0) return au::err::kErrorInvalidParam;
        std::wstring target(static_cast<std::size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &target[0], count))
            return au::err::kErrorInvalidParam;
        static std::atomic<unsigned long long> sequence{0};
        HANDLE handle = INVALID_HANDLE_VALUE; std::wstring temporary;
        for (unsigned int attempt = 0; attempt < 32 && handle == INVALID_HANDLE_VALUE; ++attempt) {
            // Exclusive create handles process/module collisions safely. The
            // counter only chooses a candidate, so relaxed ordering suffices.
            temporary = target + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
            handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return au::err::kErrorOpenFailed;
        }
        if (handle == INVALID_HANDLE_VALUE) return au::err::kErrorOpenFailed;
        const auto* cursor = static_cast<const unsigned char*>(bytes); std::size_t remaining = size;
        bool ok = true;
        while (remaining) {
            const DWORD chunk = static_cast<DWORD>(std::min(remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (!WriteFile(handle, cursor, chunk, &written, nullptr) || !written) { ok = false; break; }
            cursor += written; remaining -= written;
        }
        if (!CloseHandle(handle)) ok = false;
        if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) DeleteFileW(temporary.c_str());
#else
        std::string candidate = path + ".tmp.XXXXXX";
        std::vector<char> temporary(candidate.begin(), candidate.end()); temporary.push_back('\0');
#if AU_OS_LINUX || AU_OS_ANDROID
        const int fd = ::mkostemp(temporary.data(), O_CLOEXEC);
#else
        const int fd = ::mkstemp(temporary.data());
#endif
        // Exclusive, mode 0600; never follow a preexisting temporary symlink.
        if (fd < 0) return au::err::kErrorOpenFailed;
        bool ok = ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
        const auto* cursor = static_cast<const unsigned char*>(bytes); std::size_t remaining = size;
        while (ok && remaining) {
            const auto chunk = std::min(remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t written = ::write(fd, cursor, chunk);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { ok = false; break; }
            cursor += written; remaining -= static_cast<std::size_t>(written);
        }
        if (::close(fd)) ok = false;
        if (ok) ok = ::rename(temporary.data(), path.c_str()) == 0;
        if (!ok) (void)::unlink(temporary.data());
#endif
        return ok ? 0 : au::err::kErrorWriteFailed;
    } catch (...) { return au::err::kErrorNoMemory; }
}
