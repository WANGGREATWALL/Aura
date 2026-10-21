cmake_minimum_required(VERSION 3.14)

# ============================================================================
# Version (update here; triggers re-configure)
# ============================================================================
set(AURA_VERSION_MAJOR 1)
set(AURA_VERSION_MINOR 5)
set(AURA_VERSION_PATCH 0)
set(AURA_VERSION "${AURA_VERSION_MAJOR}.${AURA_VERSION_MINOR}.${AURA_VERSION_PATCH}")

project(Aura VERSION ${AURA_VERSION} LANGUAGES C CXX)
# Do not create or overwrite a parent's BUILD_TESTING option when embedded.
set(AURA_BUILD_TESTS_DEFAULT OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    include(CTest)
    set(AURA_BUILD_TESTS_DEFAULT ${BUILD_TESTING})
endif()
option(AURA_BUILD_TESTS "Build Aura's own tests" ${AURA_BUILD_TESTS_DEFAULT})
set(AURA_TESTS_ENABLED ${AURA_BUILD_TESTS})
if(DEFINED BUILD_TESTING AND NOT BUILD_TESTING)
    set(AURA_TESTS_ENABLED OFF)
endif()
if(AURA_TESTS_ENABLED)
    enable_testing()
endif()

message(STATUS "***********************************************************")
message(STATUS "*")
message(STATUS "* Aura v${AURA_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "* Copyright (c) 2020-2026 WANGGREATWALL")
message(STATUS "*")
message(STATUS "***********************************************************")

# ============================================================================
# Generate version.h
# ============================================================================
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/inc/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/inc/version.h
    @ONLY
)

# ============================================================================
# Source files
# ============================================================================
set(AURA_SOURCES
    src/sys/xdlib.cpp
    src/sys/xsystem.cpp
    src/sys/xsystem_vivo.cpp
    src/sys/flow_runtime.cpp
    src/sys/memory_runtime.cpp
    src/sys/memory_allocation.cpp
    src/flow/xflow.cpp
    src/flow/diagnostics.cpp
    src/gpu/pending.cpp
    src/gpu/arguments.cpp
    src/file/xpath.cpp
    src/file/xfile.cpp
    src/regex/xregex.cpp
    src/json/xjson.cpp
    src/log/xlogger.cpp
    src/cv/ximage.cpp
    src/perf/xtimer.cpp
    src/perf/xtracer.cpp
    src/mm/xmemory.cpp
    src/mm/native_backend.cpp
    src/mm/backend.cpp
    src/mm/registry.cpp
    src/mm/memory_control.cpp
    src/mm/access.cpp
    src/mm/pool_backend.cpp
    src/util/xargs.cpp
    third_party/cJSON/cJSON.c
)

# ============================================================================
# Library
# ============================================================================
option(ENABLE_OPENCL "Build optional dynamically loaded OpenCL 1.2 support" OFF)
option(ENABLE_OPENCL_HARDWARE_TESTS "Run optional GPU hardware integration tests" OFF)
find_package(Threads REQUIRED)

# Production and fault-injection variants share all non-testing configuration.
# A test-enabled build must never add fault hooks to the consumer-facing aura.
function(aura_add_library target)
    add_library(${target} STATIC ${AURA_SOURCES})
    target_compile_definitions(${target} PUBLIC AURA_STATIC)
    target_compile_features(${target} PRIVATE cxx_std_17 INTERFACE cxx_std_11)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_BINARY_DIR}/inc
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
    )
    target_link_libraries(${target} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(ANDROID)
        target_link_libraries(${target} PRIVATE android log)
    endif()
    if(ENABLE_OPENCL)
        target_sources(${target} PRIVATE src/gpu_helper/cl_symbols.cpp src/sys/opencl_runtime.cpp src/sys/opencl_memory.cpp src/sys/atomic_file.cpp src/gpu/program_cache.cpp src/gpu/opencl.cpp)
        target_include_directories(${target} PRIVATE src/gpu_helper)
        target_compile_definitions(${target} PRIVATE CL_TARGET_OPENCL_VERSION=120
            CL_HPP_TARGET_OPENCL_VERSION=120 CL_HPP_MINIMUM_OPENCL_VERSION=120
            CL_USE_DEPRECATED_OPENCL_1_1_APIS=1 CL_USE_DEPRECATED_OPENCL_1_2_APIS=1)
    else()
        target_sources(${target} PRIVATE src/gpu/opencl_unavailable.cpp)
    endif()
endfunction()

aura_add_library(aura)

# ============================================================================
# Test options (set to OFF to disable individual unit tests)
# ============================================================================
option(ENABLE_TEST_XLOGGER   "Enable xlogger unit test"   ON)
option(ENABLE_TEST_XMATH     "Enable xmath unit test"     ON)
option(ENABLE_TEST_XBUFFER   "Enable xbuffer unit test"   ON)
option(ENABLE_TEST_XREGEX    "Enable xregex unit test"    ON)
option(ENABLE_TEST_XARGS     "Enable xargs unit test"     ON)
option(ENABLE_TEST_XPATH     "Enable xpath unit test"     ON)
option(ENABLE_TEST_XTIMER    "Enable xtimer unit test"    ON)
option(ENABLE_TEST_XTRACER   "Enable xtracer unit test"   ON)
option(ENABLE_TEST_XMEMORY   "Enable xmemory unit test"   ON)
option(ENABLE_TEST_XSYSTEM   "Enable xsystem unit test"   ON)
option(ENABLE_TEST_XERROR    "Enable xerror unit test"    ON)
option(ENABLE_TEST_XFILE     "Enable xfile unit test"     ON)
option(ENABLE_TEST_XIMAGE    "Enable ximage unit test"    ON)
option(ENABLE_TEST_XFLOW     "Enable xflow unit test"     ON)
if(AURA_TESTS_ENABLED)
    aura_add_library(aura_testing)
    if(ENABLE_TEST_XFLOW)
        target_compile_definitions(aura_testing PRIVATE AURA_FLOW_TESTING=1)
    endif()
    if(ENABLE_OPENCL)
        target_compile_definitions(aura_testing PRIVATE AURA_GPU_TESTING=1)
    endif()
    if(ENABLE_TEST_XMEMORY)
        target_compile_definitions(aura_testing PRIVATE AURA_MM_TESTING=1)
    endif()
endif()

# ============================================================================
# Tests
# ============================================================================
if(AURA_TESTS_ENABLED)
set(GTEST_SOURCES
    third_party/gtest/src/gtest-assertion-result.cpp
    third_party/gtest/src/gtest-death-test.cpp
    third_party/gtest/src/gtest-filepath.cpp
    third_party/gtest/src/gtest-matchers.cpp
    third_party/gtest/src/gtest-port.cpp
    third_party/gtest/src/gtest-printers.cpp
    third_party/gtest/src/gtest-test-part.cpp
    third_party/gtest/src/gtest-typed-test.cpp
    third_party/gtest/src/gtest.cpp
)

set(AURA_TEST_SOURCES test/main.cpp ${GTEST_SOURCES})
set(AURA_TEST_DEFINITIONS "")

# Conditionally add test sources and record compile definitions
macro(aura_add_test NAME)
    string(TOUPPER ${NAME} NAME_UPPER)
    if(ENABLE_TEST_${NAME_UPPER})
        list(APPEND AURA_TEST_SOURCES test/test_${NAME}.cpp)
        list(APPEND AURA_TEST_DEFINITIONS ENABLE_TEST_${NAME_UPPER}=1)
    endif()
endmacro()

aura_add_test(xlogger)
aura_add_test(xmath)
aura_add_test(xbuffer)
aura_add_test(xregex)
aura_add_test(xargs)
aura_add_test(xpath)
aura_add_test(xtimer)
aura_add_test(xtracer)
aura_add_test(xmemory)
if(ENABLE_TEST_XMEMORY)
    list(APPEND AURA_TEST_SOURCES test/test_xaccess.cpp)
endif()
aura_add_test(xsystem)
aura_add_test(xerror)
aura_add_test(xfile)
aura_add_test(ximage)
aura_add_test(xflow)
if(ENABLE_TEST_XFLOW)
    list(APPEND AURA_TEST_SOURCES test/test_flow_runtime.cpp)
endif()
if(ENABLE_TEST_XFLOW AND ENABLE_TEST_XMEMORY)
    list(APPEND AURA_TEST_SOURCES test/test_xgpu_pending.cpp)
endif()

add_executable(aura_test ${AURA_TEST_SOURCES})
target_compile_features(aura_test PRIVATE cxx_std_17)
target_compile_definitions(aura_test PRIVATE ${AURA_TEST_DEFINITIONS})
if(ENABLE_TEST_XFLOW)
    target_compile_definitions(aura_test PRIVATE AURA_FLOW_TESTING=1)
    target_sources(aura_test PRIVATE test/test_flow_queue.cpp)
endif()
if(ENABLE_TEST_XMEMORY)
    target_compile_definitions(aura_test PRIVATE AURA_MM_TESTING=1)
endif()

target_include_directories(aura_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_BINARY_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gtest
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gtest/include
)

target_link_libraries(aura_test PRIVATE aura_testing)
if(ENABLE_OPENCL)
    if(ENABLE_TEST_XFLOW AND ENABLE_TEST_XMEMORY)
        target_sources(aura_test PRIVATE test/test_xopencl.cpp)
        target_include_directories(aura_test PRIVATE src/gpu_helper)
        target_compile_definitions(aura_test PRIVATE AURA_GPU_TESTING=1 CL_TARGET_OPENCL_VERSION=120)
        add_test(NAME flow_opencl COMMAND aura_test --gtest_filter=XOpenCL.*)
        set_tests_properties(flow_opencl PROPERTIES TIMEOUT 60)
        add_executable(opencl_teardown_test test/opencl_teardown.cpp)
        target_compile_features(opencl_teardown_test PRIVATE cxx_std_17)
        target_include_directories(opencl_teardown_test PRIVATE src src/gpu_helper)
        target_compile_definitions(opencl_teardown_test PRIVATE AURA_GPU_TESTING=1 CL_TARGET_OPENCL_VERSION=120)
        target_link_libraries(opencl_teardown_test PRIVATE aura_testing)
        add_test(NAME flow_opencl_teardown COMMAND opencl_teardown_test)
        set_tests_properties(flow_opencl_teardown PROPERTIES TIMEOUT 60)
        if(ENABLE_OPENCL_HARDWARE_TESTS)
            add_executable(opencl_hardware_test src/gpu_helper/cl_hpp_test.cpp)
            target_link_libraries(opencl_hardware_test PRIVATE aura)
            set_target_properties(opencl_hardware_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
            add_test(NAME flow_opencl_hardware COMMAND opencl_hardware_test)
            set_tests_properties(flow_opencl_hardware PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
    endif()
    add_executable(opencl_loader_test test/opencl_loader.cpp)
    target_include_directories(opencl_loader_test PRIVATE src src/gpu_helper)
    target_compile_features(opencl_loader_test PRIVATE cxx_std_17)
    target_compile_definitions(opencl_loader_test PRIVATE CL_TARGET_OPENCL_VERSION=120
        CL_HPP_TARGET_OPENCL_VERSION=120 CL_HPP_MINIMUM_OPENCL_VERSION=120)
    target_link_libraries(opencl_loader_test PRIVATE aura)
    add_test(NAME opencl_loader COMMAND opencl_loader_test)
endif()

if(ENABLE_TEST_XFLOW)
    add_test(NAME flow_unit COMMAND aura_test --gtest_filter=XFlow.*)
    add_test(NAME flow_runtime COMMAND aura_test --gtest_filter=XFlowRuntime.*)
    set_tests_properties(flow_runtime PROPERTIES TIMEOUT 90)
    if(ENABLE_TEST_XMEMORY)
        add_test(NAME flow_gpu_pending COMMAND aura_test --gtest_filter=XGpu.*)
        set_tests_properties(flow_gpu_pending PROPERTIES TIMEOUT 60)
    endif()
    set_tests_properties(flow_unit PROPERTIES TIMEOUT 60)
    if(ENABLE_TEST_XMEMORY)
        add_test(NAME flow_memory COMMAND aura_test --gtest_filter=XMemory.*)
        set_tests_properties(flow_memory PROPERTIES TIMEOUT 60)
    endif()
    add_executable(flow_cxx11_test test/flow_cxx11.cpp)
    target_include_directories(flow_cxx11_test PRIVATE src)
    set_target_properties(flow_cxx11_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(flow_cxx11_test PRIVATE aura_testing)
    add_test(NAME flow_cxx11 COMMAND flow_cxx11_test)
    add_test(NAME flow_worker_failure COMMAND flow_cxx11_test worker-failure)
    add_executable(flow_c_api_test test/flow_c_api.c)
    set_target_properties(flow_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(flow_c_api_test PRIVATE aura_testing)
    add_test(NAME flow_c_api COMMAND flow_c_api_test)
    # Exercise the same public C consumer against the actual production archive.
    add_executable(flow_production_test test/flow_c_api.c)
    set_target_properties(flow_production_test PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(flow_production_test PRIVATE aura)
    add_test(NAME flow_production COMMAND flow_production_test)
    if(CMAKE_NM AND NOT MSVC)
        add_test(NAME flow_production_symbols COMMAND ${CMAKE_COMMAND}
            "-DAURA_NM=${CMAKE_NM}" "-DAURA_LIBRARY=$<TARGET_FILE:aura>"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/flow_production_symbols.cmake)
    endif()
    add_executable(flow_shutdown_test test/flow_shutdown.cpp)
    target_link_libraries(flow_shutdown_test PRIVATE aura_testing)
    target_compile_features(flow_shutdown_test PRIVATE cxx_std_17)
    add_test(NAME flow_shutdown COMMAND flow_shutdown_test)
    set_tests_properties(flow_shutdown PROPERTIES TIMEOUT 30)
    if(UNIX)
        foreach(plugin IN ITEMS flow_plugin_a flow_plugin_b)
            add_library(${plugin} MODULE test/flow_plugin.cpp)
            # Exercise default-visible consumers: isolation must come from Aura
            # declarations and objects, not mandatory application-wide flags.
            target_include_directories(${plugin} PRIVATE src)
            if(ENABLE_OPENCL)
                target_include_directories(${plugin} PRIVATE src/gpu_helper)
                target_compile_definitions(${plugin} PRIVATE AURA_GPU_TESTING=1 CL_TARGET_OPENCL_VERSION=120)
            endif()
            target_link_libraries(${plugin} PRIVATE aura_testing)
        endforeach()
        add_executable(flow_isolation_test test/flow_isolation.cpp)
        target_include_directories(flow_isolation_test PRIVATE inc)
        target_link_libraries(flow_isolation_test PRIVATE ${CMAKE_DL_LIBS})
        add_test(NAME flow_isolation COMMAND flow_isolation_test
            $<TARGET_FILE:flow_plugin_a> $<TARGET_FILE:flow_plugin_b>)
    endif()
endif()
endif() # AURA_TESTS_ENABLED

option(ENABLE_FLOW_BENCHMARKS "Build optional host flow measurement executable" OFF)
if(ENABLE_FLOW_BENCHMARKS)
    add_executable(flow_benchmark test/flow_benchmark.cpp)
    target_link_libraries(flow_benchmark PRIVATE aura)
    target_compile_features(flow_benchmark PRIVATE cxx_std_17)
    add_executable(flow_microbenchmark test/flow_microbenchmark.cpp)
    target_link_libraries(flow_microbenchmark PRIVATE aura)
    target_compile_features(flow_microbenchmark PRIVATE cxx_std_17)
    add_executable(flow_idle_benchmark test/flow_idle_benchmark.cpp)
    target_include_directories(flow_idle_benchmark PRIVATE src)
    target_link_libraries(flow_idle_benchmark PRIVATE aura)
    target_compile_features(flow_idle_benchmark PRIVATE cxx_std_17)
endif()

# ---------------------------------------------------------------------------
# Sanitizer options for aura_test
# ---------------------------------------------------------------------------

option(ENABLE_ASAN "Enable AddressSanitizer for aura_test" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer for aura_test" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()

if(ENABLE_ASAN)
    foreach(target IN ITEMS aura aura_testing aura_test flow_cxx11_test flow_c_api_test flow_production_test flow_plugin_a flow_plugin_b flow_isolation_test opencl_loader_test opencl_hardware_test opencl_teardown_test flow_shutdown_test flow_benchmark flow_microbenchmark flow_idle_benchmark)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endforeach()
    message(STATUS "AddressSanitizer enabled for Aura and its test consumers")
endif()

if(ENABLE_TSAN)
    foreach(target IN ITEMS aura aura_testing aura_test flow_cxx11_test flow_c_api_test flow_production_test flow_plugin_a flow_plugin_b flow_isolation_test opencl_loader_test opencl_hardware_test opencl_teardown_test flow_shutdown_test flow_benchmark flow_microbenchmark flow_idle_benchmark)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
    endforeach()
    message(STATUS "ThreadSanitizer enabled for Aura and its test consumers")
endif()
