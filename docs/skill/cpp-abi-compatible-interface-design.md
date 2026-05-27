# Skill: C++ ABI 兼容接口设计

## 目标

用于设计可长期维护的 C++ 基础组件库公开接口，尤其适用于基础库以 `.so` 形式发布，并被多个调用方 `.so` 同时依赖的场景。

默认假设：

- 公开头文件必须兼容 C++11。
- 内部 `.cpp` 可以使用更高版本 C++。
- 基础库与调用方通常使用同一编译器/STL 家族。
- Android NDK 版本可能在 r23-r26 等小范围内浮动。
- 公开 C++ API 可以适度使用 `std`，但必须明确 ABI 风险。

## 1. 兼容等级

| 等级 | 目标 | 接口形态 |
|---|---|---|
| L0 源码兼容 | 调用方重新编译即可 | C++ header、template、`std` API |
| L1 受控 C++ ABI | 同编译器/STL 家族内二进制兼容 | Pimpl、C++ facade、有限 `std` 参数 |
| L2 稳定 C ABI | 跨编译器/跨语言/长期 ABI | C ABI 窄腰、纯 C 类型 |
| L3 热路径 ABI | ABI 稳定且无每次调用分配 | 栈上 opaque storage + C++11 RAII wrapper |

默认选择：

- 普通有状态模块：L1 Pimpl。
- 长期 SDK 边界：L2 Hourglass。
- timer/tracer/log scope 热路径：L3 Stack Opaque。
- 无状态工具函数：free function 或 header-only。

## 2. 模式决策表

| 场景 | 推荐模式 | 公开头 |
|---|---|---|
| 无状态纯计算 | Free function | C++11 `.h/.hpp` |
| 有状态对象，构造不在热路径 | Pimpl | C++11 `.h` |
| 最强 ABI 稳定边界 | Hourglass | C99 `_api.h` + C++11 `.hpp` |
| 热路径 RAII scope | Stack opaque storage | C99/C++11 API + inline wrapper |
| 多后端，同 toolchain | Abstract interface + factory | C++11 `.h` |
| 多后端，长期 ABI | C handle + function table | C99 `_api.h` |
| 极小零开销工具 | Header-only | C++11 `.hpp` |

原则：选择满足兼容等级的最简单模式。小粒度热路径对象不要使用 heap handle 或虚接口。

## 3. 公开头文件规则

- 公开 C++ header 保持 C++11。
- 不在公开头中包含重型平台头，例如 `<windows.h>`。
- 可不抛异常的函数声明为 `noexcept`。
- 不允许异常跨 `.so` 或 `extern "C"` 边界。
- 不导出未来可能修改布局的实体类。
- 热路径接口不得要求每次调用动态分配。
- 导出类必须明确 copy/move 语义。

## 4. `std` 类型使用边界

在 L0/L1 中可以谨慎使用：

- `const std::string&` 输入参数。
- 同 toolchain 内的 `std::unique_ptr<T>`。
- 源码兼容或强受控 ABI 中的 `std::vector<T>`。
- 布局本身就是接口契约时的 `std::array<T, N>`。

稳定 ABI 中应避免：

- 公开类包含 `std::string`、`std::vector`、`std::mutex`、`std::atomic` 成员。
- 把返回 STL 容器作为长期二进制 ABI。
- 让调用方释放基础库分配的 STL/heap 对象。

L1 推荐：

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

L2 推荐：

```c
MYLIB_API int mylib_config_set_name(const char* data, size_t size);
MYLIB_API int mylib_config_get_name(char* out, size_t capacity, size_t* written);
```

Android NDK 建议：

- 统一 STL runtime 策略，通常使用 `libc++_shared.so`。
- 避免多个 `.so` 混用 static/shared libc++。
- 避免跨 DSO 转移 STL 所有权。
- 长期 ABI 提供 C ABI 或 `char* + size` 版本。

## 5. 符号可见性

统一定义导出宏：

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

规则：

- 所有导出函数/类都标注导出宏。
- 内部符号 hidden 或放入匿名/detail namespace。
- CMake 推荐默认隐藏：

```cmake
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
```

## 6. Free Function

适合无状态工具：

```cpp
namespace mylib {

MYLIB_API uint32_t nextPow2(uint32_t n) noexcept;

template <typename T>
inline T clamp(T v, T lo, T hi) noexcept
{
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace mylib
```

非模板实现放 `.cpp`；模板和极小 inline 函数放 header。

## 7. Pimpl

适合 L1 有状态对象，且构造/析构不在热路径：

```cpp
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

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
```

规则：

- `Impl` 只在 `.cpp` 定义。
- 析构和 move 操作在 `.cpp` 定义。
- public class 不暴露 STL 成员。
- Pimpl 是同 toolchain ABI firewall，不是跨编译器 ABI 保证。

## 8. Hourglass C ABI

适合 L2 稳定 ABI：

```c
#ifdef __cplusplus
extern "C" {
#endif

typedef enum mylib_status {
    MYLIB_STATUS_OK = 0,
    MYLIB_STATUS_INVALID_ARG = 1,
    MYLIB_STATUS_INTERNAL = 2
} mylib_status;

MYLIB_API int mylib_config_set_name(const char* data, size_t size);
MYLIB_API int mylib_config_get_name(char* out, size_t capacity, size_t* written);

#ifdef __cplusplus
}
#endif
```

C ABI 规则：

- 只使用 C 类型。
- bool 使用 `int`。
- 字符串使用 `const char* + size_t`。
- 返回数据使用调用方提供的 buffer。
- ABI 边界 catch all，返回错误码。
- 只新增函数，不修改已发布签名。

C++11 wrapper 只做薄转发：

```cpp
inline int setName(const std::string& name) noexcept
{
    return mylib_config_set_name(name.data(), name.size());
}
```

## 9. Stack Opaque Storage

适合 L3 热路径 RAII，例如 timer/tracer/log scope。

避免每个 scope 使用 heap handle：

```c
typedef struct mylib_timer_scope mylib_timer_scope;
MYLIB_API mylib_timer_scope* mylib_timer_begin(const char* name); // 不推荐热路径
```

推荐调用方栈上 opaque storage：

```cpp
struct PerfScope {
    uint64_t opaque[8];
};

MYLIB_API void timerBegin(PerfScope* s, const std::string& name) noexcept;
MYLIB_API void timerEnd(PerfScope* s) noexcept;
```

C++11 RAII wrapper：

```cpp
class TimerScope {
public:
    explicit TimerScope(const std::string& name) noexcept
    {
        timerBegin(&mScope, name);
    }

    ~TimerScope() noexcept { timerEnd(&mScope); }

    TimerScope(const TimerScope&) = delete;
    TimerScope& operator=(const TimerScope&) = delete;
    TimerScope(TimerScope&&) = delete;
    TimerScope& operator=(TimerScope&&) = delete;

private:
    PerfScope mScope;
};
```

Opaque 规则：

- 已发布 `opaque` 大小不可改变。
- timer/tracer 可以使用同一套 opaque struct。
- 不把长字符串塞进 opaque。
- 名字在 begin 时复制到内部 TLS/arena 或立即消费。
- 64B 不够时新增 v2 type/API。

## 10. Abstract Interface + Factory

只适合 L1 或内部扩展点：

```cpp
class IComputeBackend {
public:
    virtual ~IComputeBackend() {}
    virtual bool init() noexcept = 0;
    virtual void submit(const void* src, void* dst, size_t len) noexcept = 0;
};
```

规则：

- 不把 C++ virtual interface 承诺为跨编译器 ABI。
- 小粒度热路径避免虚调用。
- L2 改用 C handle：

```c
typedef struct mylib_compute_backend mylib_compute_backend;
MYLIB_API mylib_compute_backend* mylib_compute_create_opencl(void);
MYLIB_API void mylib_compute_destroy(mylib_compute_backend*);
```

长生命周期 backend 在 create/destroy 中动态分配可以接受；每次 hot call 创建则不可以。

## 11. ABI 演进规则

| 变更 | 是否安全 | 说明 |
|---|---|---|
| 新增 C 函数 | 安全 | additive |
| 新增非虚 C++ 成员且不改布局 | 通常安全 | 同 toolchain |
| 新增 virtual 函数 | 不安全 | 改 vtable |
| 修改参数类型/顺序 | 不安全 | 破坏调用方 |
| 删除/重命名导出符号 | 不安全 | 破坏链接/加载 |
| 修改 public struct 大小 | 不安全 | 调用方可能栈分配 |
| public class 新增成员 | 不安全 | 改布局 |
| Pimpl Impl 新增成员 | 安全 | header 不可见 |
| 新增 `_v2` API | 安全 | 推荐破坏性演进方式 |

## 12. Multi-DSO 规则

- 全局可变状态放在基础库 `.so` 内。
- inline wrapper 不持有 mutable static state。
- 文档说明配置是 process-wide 还是 context-specific。
- per-module 隔离使用显式 context handle。
- 避免跨 DSO 传递 STL/heap 所有权。
- 必要时提供 producer-owned `destroy/free` API。

## 13. 热路径规则

- disabled/filter gate 尽早判断。
- 避免 heap allocation。
- 避免无界锁。
- 避免虚调用和 `std::function`。
- 使用 TLS、fixed arena、ring buffer。
- 析构函数 `noexcept`。
- 正确处理未闭合的 sub/span。
- 测试嵌套、异常、disabled path、多线程。

## 14. 测试清单

- public C++ header 用 `-std=c++11` 编译。
- C ABI header 可由 C99 编译器包含。
- 检查导出符号列表。
- old client + new library smoke test。
- 多调用方 DSO 同时加载。
- NDK/toolchain matrix。
- ASAN/TSAN。
- disabled hot-path overhead。
- copy/move 语义。

## 15. 反模式

| 反模式 | 问题 | 替代 |
|---|---|---|
| public class 暴露 STL 成员 | layout/STL ABI 风险 | Pimpl |
| 热路径 scope 用 heap handle | 分配进入热路径 | Stack opaque |
| C ABI 使用 C++ 类型 | 不是 C ABI | 纯 C 类型 |
| 异常跨 ABI 边界 | 脆弱/未定义 | catch 后返回状态 |
| 调用方 delete 生产方对象 | allocator/runtime 风险 | producer destroy API |
| virtual interface 承诺跨编译器 ABI | vtable ABI 风险 | C handle/function table |
| macro 只用 `__LINE__` 唯一化 | 同行冲突 | 优先 `__COUNTER__` |
| 已发布 struct 改大小 | ABI break | 新增 v2 |

## 16. 实用结论

基础组件库以 `.so` 发布时：

1. 普通有状态模块用 Pimpl。
2. 长期稳定边界用 C ABI hourglass。
3. 热路径 scope 用 stack opaque storage。
4. 多后端内部扩展可以用虚接口；对外长期 ABI 改 C handle。
5. 可以用 `std` 提升 C++ API 易用性，但不要把 `std` 成员布局和跨 DSO 所有权变成长期 ABI 承诺。

核心原则：

> C++11 wrapper 负责易用性，C ABI/opaque 负责稳定性，热路径避免每次调用分配。
