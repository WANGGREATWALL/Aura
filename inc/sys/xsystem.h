#ifndef AURA_XSYSTEM_H_
#define AURA_XSYSTEM_H_

/** @file xsystem.h
 *  @brief Platform macros, ABI visibility and cross-platform system queries.
 *
 *  Public declarations use C++11. All AU_OS_* and AU_ARCH_* macros have a
 *  numeric 0/1 value; use #if rather than #ifdef to select a platform.
 */

#include <cstdint>
#include <string>

#if defined(_WIN32)
#  define AU_OS_WINDOWS 1
#else
#  define AU_OS_WINDOWS 0
#endif

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  define AU_OS_APPLE 1
#  if TARGET_OS_IPHONE
#    define AU_OS_IOS 1
#    define AU_OS_MACOS 0
#  else
#    define AU_OS_IOS 0
#    define AU_OS_MACOS 1
#  endif
#else
#  define AU_OS_APPLE 0
#  define AU_OS_IOS 0
#  define AU_OS_MACOS 0
#endif

#if defined(__ANDROID__)
#  define AU_OS_ANDROID 1
#else
#  define AU_OS_ANDROID 0
#endif

#if defined(__linux__) && !AU_OS_ANDROID
#  define AU_OS_LINUX 1
#else
#  define AU_OS_LINUX 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define AU_ARCH_X86_64 1
#else
#  define AU_ARCH_X86_64 0
#endif
#if defined(__i386__) || defined(_M_IX86)
#  define AU_ARCH_X86 1
#else
#  define AU_ARCH_X86 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#  define AU_ARCH_ARM64 1
#else
#  define AU_ARCH_ARM64 0
#endif
#if defined(__arm__) || defined(_M_ARM)
#  define AU_ARCH_ARM 1
#else
#  define AU_ARCH_ARM 0
#endif

// AURA_STATIC is PUBLIC for static Aura; AURA_EXPORTS is PRIVATE for a shared
// library producer. Neither macro is set in consumers of a shared Aura.
#if AU_OS_WINDOWS
#  if defined(AURA_STATIC)
#    define AU_API
#  elif defined(AURA_EXPORTS)
#    define AU_API __declspec(dllexport)
#  else
#    define AU_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  if defined(AURA_STATIC)
#    define AU_API
#  else
#    define AU_API __attribute__((visibility("default")))
#  endif
#else
#  define AU_API
#endif

namespace au {
namespace sys {

enum class Platform { Windows, Linux, macOS, iOS, Android, Unknown };
enum class Arch { x86, x86_64, ARM, ARM64, Unknown };
enum class SocVendor { Qualcomm, MediaTek, Samsung, HiSilicon, Apple, Unknown };

#if AU_OS_WINDOWS
constexpr Platform kBuildPlatform = Platform::Windows;
#elif AU_OS_MACOS
constexpr Platform kBuildPlatform = Platform::macOS;
#elif AU_OS_IOS
constexpr Platform kBuildPlatform = Platform::iOS;
#elif AU_OS_ANDROID
constexpr Platform kBuildPlatform = Platform::Android;
#elif AU_OS_LINUX
constexpr Platform kBuildPlatform = Platform::Linux;
#else
constexpr Platform kBuildPlatform = Platform::Unknown;
#endif

#if AU_ARCH_X86_64
constexpr Arch kBuildArch = Arch::x86_64;
#elif AU_ARCH_X86
constexpr Arch kBuildArch = Arch::x86;
#elif AU_ARCH_ARM64
constexpr Arch kBuildArch = Arch::ARM64;
#elif AU_ARCH_ARM
constexpr Arch kBuildArch = Arch::ARM;
#else
constexpr Arch kBuildArch = Arch::Unknown;
#endif

/// Static names have process lifetime; never free the returned pointers.
AU_API const char* platformName(Platform platform) noexcept;
AU_API const char* archName(Arch arch) noexcept;

/// Returns Unknown when the SoC vendor cannot be identified.
AU_API SocVendor getSocVendor() noexcept;

/// Configured logical CPUs and currently online CPUs; 0 on query failure.
AU_API int getLogicalCpuCount() noexcept;
AU_API int getOnlineCpuCount() noexcept;
/// Empty string on query or allocation failure.
AU_API std::string getCpuModelName() noexcept;

/// Physical bytes; 0 on query failure. Available memory is an OS estimate.
AU_API uint64_t getMemoryTotalBytes() noexcept;
AU_API uint64_t getMemoryAvailableBytes() noexcept;

/// Empty string or 0 on query failure.
AU_API std::string getHostName() noexcept;
AU_API int getHardwareConcurrency() noexcept;

/// OS-native numeric IDs; 0 on query failure.
AU_API uint64_t getCurrentProcessId() noexcept;
AU_API uint64_t getCurrentThreadId() noexcept;

/// Missing variables return defaultVal; empty variables remain empty.
/// An allocation failure returns an empty string. Concurrent environment
/// mutation must be synchronized by the caller.
AU_API std::string getEnvVar(const std::string& name,
                             const std::string& defaultVal = "") noexcept;
AU_API bool hasEnvVar(const std::string& name) noexcept;
/// Returns err::kSuccess or an err::kError* code. Rejects empty names and '='.
AU_API int setEnvVar(const std::string& name, const std::string& value) noexcept;

/// Android properties: missing or malformed values return defaultVal.
/// On other platforms all getters return defaultVal. Allocation failure in
/// the string getter returns an empty string.
AU_API int getSystemPropertyValue(const std::string& property, int defaultVal) noexcept;
AU_API float getSystemPropertyValue(const std::string& property, float defaultVal) noexcept;
AU_API std::string getSystemPropertyValue(const std::string& property,
                                          std::string defaultVal) noexcept;

/// Returns err::kErrorNotSupported on non-Android systems; otherwise returns
/// err::kSuccess, err::kErrorInvalidParam, err::kErrorPlatformAPI or
/// err::kErrorNoMemory.
AU_API int setSystemPropertyValue(const std::string& property, int value) noexcept;
AU_API int setSystemPropertyValue(const std::string& property, float value) noexcept;
AU_API int setSystemPropertyValue(const std::string& property, std::string value) noexcept;

}  // namespace sys
}  // namespace au

#endif  // AURA_XSYSTEM_H_
