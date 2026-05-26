# Skill: C++ ABI-Compatible Public Interface Design

## Purpose

Design public interfaces for C++ component libraries that may be shipped as shared libraries and used by multiple caller shared libraries. The goal is to balance ABI compatibility, C++11 public-header compatibility, ergonomics, and hot-path performance.

Typical assumptions:

- Public headers must compile under C++11.
- Internal implementation may use newer C++ standards.
- Library and callers usually use the same compiler/STL family.
- Toolchain versions may differ within a controlled range, such as Android NDK r23-r26.
- Public C++ APIs may use selected `std` types, but long-term ABI risk must be explicit.

## 1. Compatibility Levels

| Level | Goal | Interface Shape |
|---|---|---|
| L0 Source-compatible | Callers rebuild from source | C++ headers, templates, `std` APIs |
| L1 Controlled C++ ABI | Same compiler/STL family binary compatibility | Pimpl, C++ facade, limited `std` parameters |
| L2 Stable C ABI | Cross compiler/language or long-term binary ABI | C ABI hourglass, plain C types |
| L3 Hot-path ABI | ABI-stable, stack-based, no per-call allocation | Stack opaque storage + inline C++ RAII wrapper |

Default selection:

- Ordinary stateful module: L1 Pimpl.
- Long-term SDK boundary: L2 hourglass.
- Timer/tracer/log scope hot path: L3 stack opaque storage.
- Simple stateless helper: free function or header-only.

## 2. Pattern Decision Matrix

| Scenario | Pattern | Public Header |
|---|---|---|
| Stateless computation | Free function | C++11 `.h` / `.hpp` |
| Stateful object, construction not hot | Pimpl | C++11 `.h` |
| Shared-library boundary with strongest ABI | Hourglass | C99 `_api.h` + C++11 `.hpp` |
| Hot-path RAII scope | Stack opaque storage | C99 `_api.h` + C++11 `.hpp` |
| Multiple backends, controlled toolchain | Abstract interface + factory | C++11 `.h` |
| Multiple backends, stable ABI | C handle + function table | C99 `_api.h` |
| Tiny zero-overhead utility | Header-only | C++11 `.hpp` |

Selection rule: use the simplest pattern that satisfies the required compatibility level. Do not use heap handles or virtual interfaces on tiny hot-path scope objects.

## 3. Public Header Rules

- Public C++ headers must remain C++11-compatible.
- Avoid heavy platform headers in public headers.
- Exported functions should be `noexcept` when possible.
- Do not let C++ exceptions cross shared-library or `extern "C"` boundaries.
- Do not export public classes with data members whose layout you may need to change.
- Do not require dynamic allocation in per-call hot paths.
- Define copy/move semantics explicitly for exported classes.

## 4. Using `std` in Public APIs

Allowed with controlled risk in L0/L1:

- `const std::string&` as input
- `std::unique_ptr<T>` in same-toolchain C++ APIs
- `std::vector<T>` in source-compatible or tightly controlled ABI APIs
- `std::array<T, N>` when layout is intentionally part of the API

Avoid for stable ABI:

- Public classes with `std::string`, `std::vector`, `std::mutex`, or `std::atomic` data members.
- Returning STL containers across shared-library boundaries as a long-term ABI contract.
- Requiring callers to free memory allocated by another shared library.

Prefer this for L1:

```cpp
class MYLIB_API Config {
public:
    void setName(const std::string& name) noexcept;
    std::string getName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
```

Prefer this for L2:

```c
MYLIB_API int mylib_config_set_name(const char* data, size_t size);
MYLIB_API int mylib_config_get_name(char* out, size_t capacity, size_t* written);
```

Android NDK guidance:

- Prefer a single runtime strategy, usually `libc++_shared.so`.
- Avoid mixing static and shared libc++ across dependent shared libraries.
- Avoid cross-DSO STL ownership transfer.
- If long-term ABI matters, provide C ABI or `char* + size` alternatives.

## 5. Symbol Visibility

Use one visibility macro from a common public header:

```cpp
#if defined(MYLIB_STATIC)
#  define MYLIB_API
#elif defined(MYLIB_EXPORTS)
#  if defined(_MSC_VER)
#    define MYLIB_API __declspec(dllexport)
#  else
#    define MYLIB_API __attribute__((visibility("default")))
#  endif
#else
#  if defined(_MSC_VER)
#    define MYLIB_API __declspec(dllimport)
#  else
#    define MYLIB_API
#  endif
#endif
```

Rules:

- Annotate every exported function and exported class.
- Keep internal symbols hidden or in anonymous/detail namespaces.
- Prefer build defaults such as hidden visibility:

```cmake
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
```

## 6. Free Function Pattern

Use for stateless utilities.

```cpp
#pragma once
#include "platform.h"
#include <cstdint>

namespace mylib {

MYLIB_API uint32_t nextPow2(uint32_t n) noexcept;

template <typename T>
inline T clamp(T v, T lo, T hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace mylib
```

Rules:

- Put non-template implementations in `.cpp`.
- Keep template and tiny inline functions in headers.
- Prefer POD and input-only `const std::string&` for stable-ish C++ APIs.

## 7. Pimpl Pattern

Use for L1 stateful objects when construction/destruction are not hot-path operations.

```cpp
#pragma once
#include "platform.h"
#include <memory>
#include <string>

namespace mylib {

class MYLIB_API XFile {
public:
    explicit XFile(const std::string& path);
    ~XFile();

    XFile(XFile&&) noexcept;
    XFile& operator=(XFile&&) noexcept;

    XFile(const XFile&) = delete;
    XFile& operator=(const XFile&) = delete;

    bool open(bool readOnly = true) noexcept;
    void close() noexcept;
    bool isOpen() const noexcept;
    size_t read(void* buf, size_t len) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace mylib
```

Rules:

- Define `Impl` only in `.cpp`.
- Define destructor and move operations in `.cpp`.
- Do not expose implementation STL containers as data members.
- Do not use Pimpl for per-scope hot-path objects if it causes heap allocation.
- Pimpl is not a cross-compiler ABI guarantee; it is a same-toolchain ABI firewall.

## 8. Hourglass C ABI Pattern

Use for L2 stable ABI.

```c
#pragma once
#include "platform.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mylib_status {
    MYLIB_STATUS_OK = 0,
    MYLIB_STATUS_INVALID_ARG = 1,
    MYLIB_STATUS_INTERNAL = 2
} mylib_status;

MYLIB_API int mylib_version_major(void);
MYLIB_API int mylib_config_set_name(const char* data, size_t size);
MYLIB_API int mylib_config_get_name(char* out, size_t capacity, size_t* written);

#ifdef __cplusplus
}
#endif
```

C ABI rules:

- Use only C types.
- Use `int` for boolean values.
- Use `const char* + size_t` for strings.
- Use caller-provided output buffers for returned strings/data.
- Catch all exceptions at the ABI boundary and return error codes.
- Add new functions for evolution; do not change existing signatures.

C++ wrapper:

```cpp
#pragma once
#include "mylib_api.h"
#include <string>

namespace mylib {

inline int setName(const std::string& name) noexcept
{
    return mylib_config_set_name(name.data(), name.size());
}

} // namespace mylib
```

Wrapper rules:

- Header-only and C++11-compatible.
- Delegate immediately to the C ABI.
- Do not duplicate complex implementation logic in the wrapper.

## 9. Stack Opaque Storage Pattern

Use for L3 hot-path RAII objects such as timers, tracers, log scopes, spans, and telemetry scopes.

Avoid heap handles in hot paths:

```c
/* Avoid for per-scope hot paths: begin/create likely allocates. */
typedef struct mylib_timer_scope mylib_timer_scope;
MYLIB_API mylib_timer_scope* mylib_timer_begin(const char* name);
MYLIB_API void mylib_timer_end(mylib_timer_scope*);
```

Prefer caller stack storage:

```c
#pragma once
#include "platform.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mylib_timer_scope {
    uint64_t opaque[8];
} mylib_timer_scope;

typedef struct mylib_trace_scope {
    uint64_t opaque[8];
} mylib_trace_scope;

MYLIB_API void mylib_timer_begin(mylib_timer_scope* s, const char* name, size_t size);
MYLIB_API void mylib_timer_sub_begin(mylib_timer_scope* s, const char* name, size_t size);
MYLIB_API void mylib_timer_sub_end(mylib_timer_scope* s);
MYLIB_API void mylib_timer_end(mylib_timer_scope* s);

MYLIB_API void mylib_trace_begin(mylib_trace_scope* s, const char* name, size_t size);
MYLIB_API void mylib_trace_sub_begin(mylib_trace_scope* s, const char* name, size_t size);
MYLIB_API void mylib_trace_sub_end(mylib_trace_scope* s);
MYLIB_API void mylib_trace_end(mylib_trace_scope* s);

#ifdef __cplusplus
}
#endif
```

C++11 wrapper:

```cpp
#pragma once
#include "mylib_perf_api.h"
#include <string>

namespace mylib {
namespace perf {

class TimerScope {
public:
    explicit TimerScope(const std::string& name) noexcept
    {
        mylib_timer_begin(&mScope, name.data(), name.size());
    }

    ~TimerScope() noexcept { mylib_timer_end(&mScope); }

    TimerScope(const TimerScope&) = delete;
    TimerScope& operator=(const TimerScope&) = delete;
    TimerScope(TimerScope&&) = delete;
    TimerScope& operator=(TimerScope&&) = delete;

    void sub(const std::string& name) noexcept
    {
        mylib_timer_sub_begin(&mScope, name.data(), name.size());
    }

    void sub() noexcept { mylib_timer_sub_end(&mScope); }

private:
    mylib_timer_scope mScope;
};

} // namespace perf
} // namespace mylib
```

Macro helpers:

```cpp
#define MYLIB_CONCAT_INNER(a, b) a##b
#define MYLIB_CONCAT(a, b) MYLIB_CONCAT_INNER(a, b)

#if defined(__COUNTER__)
#  define MYLIB_UNIQUE_NAME(prefix) MYLIB_CONCAT(prefix, __COUNTER__)
#else
#  define MYLIB_UNIQUE_NAME(prefix) MYLIB_CONCAT(prefix, __LINE__)
#endif

#define MYLIB_TIMER(name) \
    ::mylib::perf::TimerScope MYLIB_UNIQUE_NAME(_mylib_timer_)(name)
```

Opaque layout guidance:

- `uint64_t opaque[8]` gives 64 bytes, one typical cache line.
- Published opaque size must never change.
- Use separate public types for timer and tracer; internal layouts may be shared.
- Do not store long names in opaque storage.
- Copy names into internal TLS arena or consume them immediately.
- If 64 bytes is insufficient later, add a v2 type/API.

Possible internal interpretation:

```cpp
struct ScopeState {
    uint32_t magic;
    uint16_t abi;
    uint8_t  kind;
    uint8_t  flags;
    int32_t  nodeIdx;
    int32_t  subNodeIdx;
    uint32_t depth;
    uint32_t reserved0;
    int64_t  beginNs;
    int64_t  subBeginNs;
    uint64_t ctxCookie;
    uint64_t reserved1;
    uint64_t reserved2;
};
```

## 10. Abstract Interface + Factory

Use only for L1 controlled C++ ABI or internal extension points.

```cpp
class IComputeBackend {
public:
    virtual ~IComputeBackend() {}
    virtual bool init() noexcept = 0;
    virtual void submit(const void* src, void* dst, size_t len) noexcept = 0;
    virtual void sync() noexcept = 0;
};
```

Rules:

- Avoid virtual dispatch in tiny hot paths.
- Do not promise cross-compiler ABI stability with C++ virtual interfaces.
- If L2 stability is required, expose C handles instead:

```c
typedef struct mylib_compute_backend mylib_compute_backend;

MYLIB_API mylib_compute_backend* mylib_compute_create_opencl(void);
MYLIB_API void mylib_compute_destroy(mylib_compute_backend*);
MYLIB_API int mylib_compute_submit(mylib_compute_backend*, const void* src, void* dst, size_t len);
```

Heap allocation is acceptable here when the backend object is long-lived and not created per hot call.

## 11. ABI Evolution Rules

| Change | Safe? | Note |
|---|---|---|
| Add new C function | Yes | Additive |
| Add non-virtual C++ member without layout change | Usually | Same toolchain only |
| Add virtual function | No | Changes vtable |
| Change parameter type/order | No | Breaks callers |
| Remove or rename exported symbol | No | Breaks link/load |
| Change public struct size | No | Callers may stack-allocate |
| Add data member to public class | No | Changes layout |
| Add member to Pimpl `Impl` | Yes | Hidden from header |
| Add `_v2` API | Yes | Preferred breaking-change path |

## 12. Multi-DSO Rules

When multiple shared libraries call the same base library:

- Keep global mutable state inside the base library shared object.
- Avoid mutable static state in inline wrappers.
- Document whether config is process-wide or context-specific.
- Use explicit context handles for per-module isolation.
- Avoid cross-DSO ownership transfer of STL objects or raw allocated memory.
- Provide `destroy/free` APIs when memory must be returned by the producer library.
- Keep Android STL/runtime linkage strategy consistent across all DSOs.

## 13. Hot-Path Rules

- Check disabled/filter gates as early as possible.
- Avoid heap allocation.
- Avoid unbounded locks.
- Avoid virtual dispatch.
- Avoid `std::function`.
- Use TLS, fixed arenas, or ring buffers.
- Make destructors `noexcept`.
- Handle unbalanced begin/end or open sub-scope cleanup safely.
- Test nested scopes, exceptions, disabled path, and multi-thread behavior.

## 14. Testing Checklist

- Public C++ headers compile with `-std=c++11`.
- C ABI headers compile as C99.
- Exported symbol list is inspected with `nm`, `readelf`, or `dumpbin`.
- No unintended public symbols are exported.
- Old client + new library smoke test passes.
- Multiple caller DSOs can load and call the same base library.
- NDK/toolchain matrix is tested when relevant.
- ASAN/TSAN pass for stateful or concurrent modules.
- Hot-path disabled overhead is measured.
- Move/copy semantics are covered.

## 15. Anti-Patterns

| Anti-pattern | Problem | Alternative |
|---|---|---|
| Public class exposes STL data members | Layout/STL ABI risk | Pimpl |
| Per-scope hot path uses heap handle | Allocation in hot path | Stack opaque storage |
| C ABI uses C++ types | Not a C ABI | Plain C types |
| Exceptions cross ABI boundary | Undefined or fragile behavior | Catch and return status |
| Caller deletes producer-owned object | Allocator/runtime risk | Producer-provided destroy API |
| Virtual interface promised as cross-compiler ABI | Vtable ABI risk | C handle/function table |
| Macro uniqueness uses only `__LINE__` | Same-line collisions | Prefer `__COUNTER__` |
| Published struct size changes | Stack-allocation ABI break | Add v2 API/type |
| Inline wrapper owns mutable static state | State duplication across DSOs | Store state in library `.so` |

## 16. Practical Recommendation

For a base C++ component library shipped as a shared library:

1. Use Pimpl for ordinary stateful modules.
2. Use C ABI hourglass for long-term stable SDK boundaries.
3. Use stack opaque storage for hot-path scope objects.
4. Use C++ virtual interfaces only inside controlled toolchain boundaries.
5. Allow `std` in public C++ APIs for ergonomics, but do not expose `std`-based object layout or ownership as a long-term ABI contract.

Core rule:

> Use C++11 wrappers for ergonomics, C ABI for durable binary contracts, and stack opaque storage for hot paths.
