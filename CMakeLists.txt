cmake_minimum_required(VERSION 3.14)

# ============================================================================
# Version (update here; triggers re-configure)
# ============================================================================
set(AURA_VERSION_MAJOR 1)
set(AURA_VERSION_MINOR 7)
set(AURA_VERSION_PATCH 1)
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
    src/flow/xflow.cpp
    src/file/xpath.cpp
    src/file/xfile.cpp
    src/regex/xregex.cpp
    src/json/xjson.cpp
    src/log/xlogger.cpp
    src/cv/ximage.cpp
    src/perf/xtimer.cpp
    src/perf/xtracer.cpp
    src/mm/xmemory.cpp
    src/mm/backend.cpp
    src/mm/registry.cpp
    src/mm/pool_backend.cpp
    src/util/xargs.cpp
    third_party/cJSON/cJSON.c
)

# ============================================================================
# Library
# ============================================================================
find_package(Threads REQUIRED)

add_library(aura STATIC ${AURA_SOURCES})
target_compile_definitions(aura PUBLIC AURA_STATIC)
target_compile_features(aura PRIVATE cxx_std_17 INTERFACE cxx_std_11)
set_target_properties(aura PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(aura PRIVATE src/mm/native_backend.cpp)
else()
    target_sources(aura PRIVATE src/mm/native_backend_unsupported.cpp)
endif()
target_include_directories(aura PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_BINARY_DIR}/inc
)
target_include_directories(aura PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cJSON
)
target_link_libraries(aura PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(ANDROID)
    target_link_libraries(aura PRIVATE android log)
endif()

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
aura_add_test(xsystem)
aura_add_test(xerror)
aura_add_test(xfile)
aura_add_test(ximage)
aura_add_test(xflow)

add_executable(aura_test ${AURA_TEST_SOURCES})
target_compile_features(aura_test PRIVATE cxx_std_17)
target_compile_definitions(aura_test PRIVATE ${AURA_TEST_DEFINITIONS})

target_include_directories(aura_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${CMAKE_CURRENT_BINARY_DIR}/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gtest
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gtest/include
)

target_link_libraries(aura_test PRIVATE aura)

add_test(NAME aura_unit COMMAND aura_test)
set_tests_properties(aura_unit PROPERTIES TIMEOUT 90)
endif() # AURA_TESTS_ENABLED

option(ENABLE_ASAN "Enable AddressSanitizer for aura_test" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer for aura_test" OFF)

if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ENABLE_ASAN and ENABLE_TSAN are mutually exclusive")
endif()

if(ENABLE_ASAN OR ENABLE_TSAN)
    if(ENABLE_ASAN)
        set(AURA_SANITIZER address)
    else()
        set(AURA_SANITIZER thread)
    endif()
    foreach(target IN ITEMS aura aura_test)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -fsanitize=${AURA_SANITIZER} -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=${AURA_SANITIZER})
        endif()
    endforeach()
endif()
