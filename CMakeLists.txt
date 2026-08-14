# CMakeLists.txt — thin orchestrator; logic lives in cmake/*.cmake modules
cmake_minimum_required(VERSION 3.11)
cmake_policy(VERSION 3.11)

project(c_csilk_framework C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Place all build artifacts in predictable directories so out-of-source
# builds work correctly with parallel make (no link-order races).
# Guards preserve values set via -D on the command line (e.g. from setup.py).
if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# Default to Release build — critical for performance libraries.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Common optimization flags (no -Werror; controlled per-target).
# Release builds default to portable binaries (-O2 -fomit-frame-pointer)
# safe for Docker, CI artifacts, and deployment across varying CPU architectures.
# For maximum performance on the local host CPU (benchmarks/profiling),
# enable -DCSILK_ENABLE_NATIVE_ARCH=ON to append -march=native.
option(CSILK_ENABLE_NATIVE_ARCH "Enable native host CPU tuning (-march=native)" OFF)

# Backward compatibility alias for CSILK_PORTABLE
if(DEFINED CSILK_PORTABLE)
  if(CSILK_PORTABLE)
    set(CSILK_ENABLE_NATIVE_ARCH OFF CACHE BOOL "Enable native host CPU tuning (-march=native)" FORCE)
  else()
    set(CSILK_ENABLE_NATIVE_ARCH ON CACHE BOOL "Enable native host CPU tuning (-march=native)" FORCE)
  endif()
endif()

set(CSILK_COMMON_FLAGS "")
if(CSILK_ENABLE_NATIVE_ARCH AND NOT CMAKE_CROSSCOMPILING)
  set(CSILK_RELEASE_FLAGS "$<$<CONFIG:Release>:-O2;-fomit-frame-pointer;-march=native>")
else()
  set(CSILK_RELEASE_FLAGS "$<$<CONFIG:Release>:-O2;-fomit-frame-pointer>")
endif()
set(CSILK_DEBUG_FLAGS   "$<$<NOT:$<CONFIG:Release>>:-O2>")


# Suppress benign third-party warnings (cJSON/yyjson macros, nghttp2/libuv deprecations).
add_compile_options(-Wno-keyword-macro -Wno-deprecated-declarations)

# SIMD acceleration for router path matching (AVX2/AVX-512 on x86_64, NEON on aarch64)
include(CheckCSourceCompiles)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(CMAKE_REQUIRED_FLAGS "-mavx2")
  check_c_source_compiles("
    #include <immintrin.h>
    int main(void) {
      __m256i v = _mm256_setzero_si256();
      (void)v;
      return 0;
    }
  " CSILK_HAS_AVX2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(CSILK_HAS_AVX2 FALSE)
  message(STATUS "ARM platform — AVX2 disabled, using word-aligned fast path")
endif()
set(CMAKE_REQUIRED_FLAGS "")
if(CSILK_HAS_AVX2)
  message(STATUS "AVX2 SIMD enabled for router path matching")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  set(CMAKE_REQUIRED_FLAGS "-mavx512f -mavx512bw")
  check_c_source_compiles("
    #include <immintrin.h>
    int main(void) {
      __m512i v = _mm512_setzero_si512();
      (void)v;
      return 0;
    }
  " CSILK_HAS_AVX512)
  set(CMAKE_REQUIRED_FLAGS "")
  if(CSILK_HAS_AVX512)
    message(STATUS "AVX-512 SIMD enabled for router path matching")
  endif()
endif()

# ── Modular includes ─────────────────────────────────────────────────────
include(cmake/options.cmake)
include(cmake/dependencies.cmake)

# Source files — organized per module in cmake/sources.cmake
include(cmake/sources.cmake)

if(LLHTTP_SOURCES)
  list(APPEND CSILK_SOURCES ${LLHTTP_SOURCES})
endif()

# Conditional driver sources
if(MYSQL_LIB AND MYSQL_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/db/mysql.c)
  set(CSILK_HAS_MYSQL TRUE)
endif()

if(PQ_LIB AND PQ_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/db/postgres.c)
  set(CSILK_HAS_POSTGRES TRUE)
endif()

if(HAS_MONGODB)
  list(APPEND CSILK_SOURCES src/drivers/db/mongodb.c)
endif()

if(HIREDIS_LIB AND HIREDIS_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/db/redis.c
                            src/drivers/db/redis_storage.c)
  set(CSILK_HAS_REDIS TRUE)
endif()

include(cmake/targets.cmake)

# Only build tests and examples when csilk is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(cmake/tests.cmake)

  # test_timeout needs an extended CTest timeout
  add_csilk_test(test_timeout tests/core/test_timeout.c)
  set_tests_properties(test_timeout PROPERTIES TIMEOUT 10)
  list(APPEND CSILK_ALL_TEST_NAMES test_timeout)

  # test_multi_worker needs a longer timeout for multi-threaded requests
  set_tests_properties(test_multi_worker PROPERTIES TIMEOUT 30)

  # Concurrent scenario tests involve thread spawning or multiple streams
  set_tests_properties(test_mq_concurrent PROPERTIES TIMEOUT 30)
  set_tests_properties(test_ws_concurrent PROPERTIES TIMEOUT 15)
  set_tests_properties(test_sse_concurrent PROPERTIES TIMEOUT 15)

  if(ENABLE_OOM_TEST)
      add_executable(test_oom tests/core/test_oom.c)
      target_compile_definitions(test_oom PRIVATE TEST_OOM)
      target_link_libraries(test_oom csilk pthread)
      add_test(NAME test_oom COMMAND test_oom)

      add_executable(test_oom_io tests/core/test_oom_io.c)
      target_compile_definitions(test_oom_io PRIVATE TEST_OOM)
      target_link_libraries(test_oom_io csilk pthread)
      add_test(NAME test_oom_io COMMAND test_oom_io)
  endif()

  option(USE_FUZZER "Enable libFuzzer" OFF)
  if(USE_FUZZER)
      add_executable(fuzz_test fuzz/fuzz_test.c)
      target_compile_options(fuzz_test PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_test csilk)

      add_executable(fuzz_yaml fuzz/fuzz_yaml.c)
      target_compile_options(fuzz_yaml PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_yaml PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_yaml csilk)

      add_executable(fuzz_url fuzz/fuzz_url.c)
      target_compile_options(fuzz_url PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_url PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_url csilk)

      add_executable(fuzz_headers fuzz/fuzz_headers.c)
      target_compile_options(fuzz_headers PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_headers PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_headers csilk)
  endif()

  include(cmake/examples.cmake)
endif()

# Add run_tests custom target
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS csilk ${CSILK_ALL_TEST_NAMES}
    COMMENT "Running all tests with ctest"
)

include(cmake/tooling.cmake)
include(cmake/install.cmake)
