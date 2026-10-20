<div align="center">
<pre>
      ------    ----    ---- -----------     ------    
     ********   ****    **** ***********    ********   
    ----------  ----    ---- ----    ---   ----------  
   ****    **** ****    **** *********    ****    **** 
   ------------ ----    ---- ---------    ------------ 
   ************ ************ ****  ****   ************ 
   ----    ---- ------------ ----   ----  ----    ---- 
   ****    **** ************ ****    **** ****    **** 
</pre>
</div>

<h1 align="center">𝔸𝕦𝕣𝕒</h1>

<p align="center">
  <strong>A Modern, Lightweight, and High-Performance C++17 Infrastructure Library</strong>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat&logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/macOS-supported-000000?style=flat&logo=apple" alt="macOS">
  <img src="https://img.shields.io/badge/Linux-supported-FCC624?style=flat&logo=linux" alt="Linux">
  <img src="https://img.shields.io/badge/Windows-supported-0078D6?style=flat&logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/Android-NDK-3DDC84?style=flat&logo=android" alt="Android">
  <img src="https://img.shields.io/badge/CMake-3.14%2B-064F8C?style=flat&logo=cmake" alt="CMake">
</p>

<hr />

Aura is a curated collection of high-performance C++17 components designed to streamline the development of efficient, cross-platform applications. It focuses on zero-overhead abstractions, robust diagnostics, and simplified system-level operations. Public headers target C++11 compatibility; the library implementation uses C++17.

### Module Status

- ![stable](https://img.shields.io/badge/stable-31A843?style=flat) **stable** — Public API in `inc/`, complete & passing tests.
- ![verified](https://img.shields.io/badge/verified-2F80ED?style=flat) **verified** — Internal in `src/`, passing tests with switch ON.
- ![draft](https://img.shields.io/badge/draft-DBA400?style=flat) **draft** — Work in progress; behavior and tests have not yet been verified as complete.
- ![plan](https://img.shields.io/badge/plan-9E9E9E?style=flat) **plan** — Internal in `src/`, not yet implemented.

### Core Modules

| Category | Module | Key Features | Status |
| :--- | :--- | :--- | :---: |
| `au::file` | `xpath` | Pythonic path manipulation (join, split, stem, extension) and filesystem queries. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| | `xfile` | High-performance file I/O with RAII handles and memory-mapped reads. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| `au::log` | `xerror` | Error code definitions (X-Macro), isError predicate, and code-to-string conversion. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| | `xlogger` | Multithreaded hierarchical logging with color output, level filtering, and Android logcat support. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| `au::math` | `xmath` | Constants (π, e), min/max/clamp, power-of-two alignment, and trig helpers. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| `au::perf` | `xtimer` | Public C++11 timer API with shared opaque scope storage, Release/Debug dual-mode, and aggregate flush. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| | `xtracer` | Public C++11 Android Perfetto / ftrace tracer API sharing `PerfScope` with timer and `AU_PERF_SCOPE`. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| `au::re` | `xregex` | std::regex convenience wrappers: match, extract groups, replace, and split. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| `au::memory` | `xbuffer` | Shared-memory buffer with zero-copy reference counting and move semantics. | ![verified](https://img.shields.io/badge/verified-2F80ED?style=flat) |
| `au::util` | `xargs` | Lightweight CLI argument parser with short/long options and quoted values. | ![verified](https://img.shields.io/badge/verified-2F80ED?style=flat) |
| `au::sys` | `xplatform` | Hardware topology (CPU cores, memory), environment variables, and OS detection. | ![stable](https://img.shields.io/badge/stable-31A843?style=flat) |
| | `xdlib` | Unified cross-platform dynamic library loading (dlopen / LoadLibrary). | ![plan](https://img.shields.io/badge/plan-9E9E9E?style=flat) |
| `au::cv` | `ximage` | Multi-plane image descriptors, explicit formats and color spaces, single-block allocation, borrowed copies, and ownership-transferring `XImage` moves. | ![draft](https://img.shields.io/badge/draft-DBA400?style=flat) |
| `au::flow` | `xflow` | Dynamic task dependencies, structured completion, cooperative waits, and tiled parallelism with private workers. | ![draft](https://img.shields.io/badge/draft-DBA400?style=flat) |
| `au::json` | `xjson` | Rapid JSON parsing and serialization built on cJSON with C++ RAII wrappers. | ![plan](https://img.shields.io/badge/plan-9E9E9E?style=flat) |

`au::cv::Image` is a plain descriptor: storage returned by `createImage()` requires `destroyImage()`. An `XImage` created from dimensions owns its storage; copying it or constructing one from an `Image` lvalue borrows that storage and requires the original owner to remain alive. Moving an `XImage` transfers ownership. `Image` rvalues cannot be adopted by `XImage`.

### Supported Platforms
* **Desktop**: macOS (Apple Silicon/Intel), Linux (x86_64/AArch64), Windows (MSVC)
* **Mobile**: Android (NDK)

### Quick Start

#### Integration
Aura is designed for seamless integration via CMake:

```cmake
add_subdirectory(third_party/Aura)
target_link_libraries(your_app PRIVATE aura)
```

#### Build & Test
Ensure you have CMake 3.14+ and a C++17 compliant compiler.

```bash
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
./aura_test
```

`aura` is the production library and is built without fault-injection hooks,
even when tests are enabled. Tests that need hooks link the separate
`aura_testing` library; `flow_production_test` exercises the public C API against
the production archive. Use `-DBUILD_TESTING=OFF` for a production-only top-level
build. When Aura is embedded with `add_subdirectory`, its own tests default to
OFF; opt in with `-DAURA_BUILD_TESTS=ON` when the parent permits testing. The
parent's `BUILD_TESTING` option is preserved. Android thread support is selected
through `Threads::Threads`, and Android logging links `log`.

The current flow review and staged acceptance checklist is tracked in
[flow_remediation_plan.md](docs/flow_remediation_plan.md).

### Design Philosophy
* **Minimal Dependencies**: Strictly avoids heavy external frameworks.
* **Performance First**: Prioritizes cache-friendly data structures and zero-copy semantics.
* **Developer Experience**: Clean, consistent API design with extensive test coverage.

---

<p align="center">
  Copyright (c) 2020-2026 WANGGREATWALL. All rights reserved.
</p>

au::flow 的同步 tiled 迭代及微基准见 [B1/B2 报告](docs/flow_validation/B1-B2-2026-10-20.md)。开启 `ENABLE_FLOW_BENCHMARKS=ON` 可构建 `flow_microbenchmark`；它链接生产库，不启用故障钩子。tile 回调现在禁止异步 Task/GPU 发布，外部同步循环由 shutdown 排空、从 waitAll 的异步根快照中排除，详见报告中的迁移说明。
