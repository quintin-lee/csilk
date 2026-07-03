# CMakeLists.txt
cmake_minimum_required(VERSION 3.11)
cmake_policy(VERSION 3.11)

project(c_csilk_framework C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release build — critical for performance libraries.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Common optimization flags (no -Werror; controlled per-target).
# Release: -O2 + LTO + native arch tuning for the build machine.
# Debug/RelWithDebInfo/MinSizeRel: -O2 without LTO.
set(CSILK_COMMON_FLAGS "")
set(CSILK_RELEASE_FLAGS "$<$<CONFIG:Release>:-O2;-flto;-fomit-frame-pointer;-march=native>")
set(CSILK_DEBUG_FLAGS   "$<$<NOT:$<CONFIG:Release>>:-O2>")


# Suppress benign third-party warnings (cJSON macros, nghttp2/libuv deprecations).
add_compile_options(-Wno-keyword-macro -Wno-deprecated-declarations)

# SIMD acceleration for router path matching (AVX2/AVX-512 on x86_64, NEON on aarch64)
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-mavx2")
check_c_source_compiles("
  #include <immintrin.h>
  int main(void) {
    __m256i v = _mm256_setzero_si256();
    (void)v;
    return 0;
  }
" CSILK_HAS_AVX2)
set(CMAKE_REQUIRED_FLAGS "")
if(CSILK_HAS_AVX2)
  message(STATUS "AVX2 SIMD enabled for router path matching")
else()
  message(STATUS "AVX2 not available — using word-aligned fast path")
endif()

set(CSILK_AVX2_FLAGS "")

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

# Version — single source of truth
set(CSILK_VERSION_MAJOR 0 CACHE STRING "Major version")
set(CSILK_VERSION_MINOR 3 CACHE STRING "Minor version")
set(CSILK_VERSION_PATCH 0 CACHE STRING "Patch version")
set(CSILK_VERSION "${CSILK_VERSION_MAJOR}.${CSILK_VERSION_MINOR}.${CSILK_VERSION_PATCH}" CACHE STRING "Full version")

# Build acceleration: ccache (if available)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Found ccache: ${CCACHE_PROGRAM}")
endif()

enable_testing()

option(CSILK_USE_URING "Use io_uring backend instead of libuv" OFF)

option(USE_ASAN "Enable AddressSanitizer for memory leak detection" OFF)
if(USE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    set(CSILK_ASAN_FLAGS "-fsanitize=address;-fno-omit-frame-pointer")
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
      list(APPEND CSILK_ASAN_FLAGS -static-libasan)
    endif()
endif()

option(USE_COVERAGE "Enable code coverage with gcov" OFF)
if(USE_COVERAGE)
    message(STATUS "Code coverage enabled")
endif()

# Option to enable OOM testing
option(ENABLE_OOM_TEST "Enable OOM (Out Of Memory) testing" OFF)

# Option to enable arena redzone guards for buffer overflow detection
option(DEBUG_ARENA "Enable arena redzone guards for buffer overflow detection" OFF)

# 查找 clang-format
find_program(CLANG_FORMAT_EXECUTABLE clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXECUTABLE}")
    
    add_custom_target(format ALL
        COMMAND ${CMAKE_COMMAND}
            -DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/format.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting C/H files with clang-format..."
    )

    # Treat implicit function declarations as errors (caught by clang-format scan)
    add_compile_options(-Werror=implicit-function-declaration)
endif()

# 查找 doxygen
find_program(DOXYGEN_EXECUTABLE doxygen)
find_program(CURL_EXECUTABLE curl)
if(DOXYGEN_EXECUTABLE)
    set(MERMAID_JS "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets/mermaid.min.js")
    set(MERMAID_URL "https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js")
    if(CURL_EXECUTABLE)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets"
            COMMAND ${CURL_EXECUTABLE} -fsSL --connect-timeout 10 --retry 3 -o "${MERMAID_JS}" "${MERMAID_URL}"
            COMMAND ${CMAKE_COMMAND} -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" -DBIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" -DCSILK_VERSION="${CSILK_VERSION}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-doxyfile.cmake
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (Mermaid diagrams enabled)"
        )
        message(STATUS "Found curl: ${CURL_EXECUTABLE}")
    else()
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" -DBIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" -DCSILK_VERSION="${CSILK_VERSION}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-doxyfile.cmake
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (curl not found, Mermaid may be disabled)"
        )
        message(WARNING "curl not found — Mermaid diagrams may not render in Doxygen output")
    endif()
endif()

include(FetchContent)

if(CSILK_USE_URING)
  message(STATUS "io_uring backend selected — fetching liburing")
  set(CSILK_USE_URING_ONLY TRUE)
  FetchContent_Declare(
    liburing
    GIT_REPOSITORY https://github.com/axboe/liburing.git
    GIT_TAG        liburing-2.5
  )
  FetchContent_MakeAvailable(liburing)
  if(liburing_SOURCE_DIR AND liburing_BINARY_DIR)
    # liburing 2.5 ships without a CMakeLists.txt (only Makefile+configure).
    # FetchContent downloads but does NOT run ./configure, so the generated
    # headers (compat.h, io_uring_version.h) are missing.  Generate them
    # into the build tree, and compile liburing's .c files into a static
    # library to satisfy the -luring link dependency.

    # --- generate missing headers ---
    set(LIBURING_GEN_DIR "${liburing_BINARY_DIR}/src/include/liburing")
    file(MAKE_DIRECTORY "${LIBURING_GEN_DIR}")
    file(WRITE "${LIBURING_GEN_DIR}/compat.h"
"/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H

/* generated by csilk CMakeLists.txt for liburing v2.5 */

#include <linux/openat2.h>   /* struct open_how (Linux >= 5.6) */

#if defined(__has_include)
#  if __has_include(\"linux/time_types.h\")
#    include <linux/time_types.h>
#  else
struct __kernel_timespec {
    int64_t tv_sec;
    long long tv_nsec;
};
#  endif
#endif

#endif /* LIBURING_COMPAT_H */
")
    file(WRITE "${LIBURING_GEN_DIR}/io_uring_version.h"
"/* SPDX-License-Identifier: MIT */
#ifndef LIBURING_VERSION_H
#define LIBURING_VERSION_H

/* generated by csilk CMakeLists.txt for liburing v2.5 */

#define IO_URING_VERSION_MAJOR 2
#define IO_URING_VERSION_MINOR 5

#endif /* LIBURING_VERSION_H */
")

    # --- include paths ---
    include_directories(${liburing_SOURCE_DIR}/src/include)
    include_directories(${liburing_BINARY_DIR}/src/include)
    include_directories(${liburing_SOURCE_DIR}/src)  # for lib.h / int_flags.h

    # --- build liburing as a static library from source ---
    # nolibc.c intentionally omitted: it requires CONFIG_NOLIBC (no-libc build).
    # ffi.c is included but unused by csilk (it adds io_uring_ffi symbols).
    set(LIBURING_SOURCES
      "${liburing_SOURCE_DIR}/src/queue.c"
      "${liburing_SOURCE_DIR}/src/setup.c"
      "${liburing_SOURCE_DIR}/src/syscall.c"
      "${liburing_SOURCE_DIR}/src/register.c"
      "${liburing_SOURCE_DIR}/src/ffi.c"
      "${liburing_SOURCE_DIR}/src/version.c"
    )
    add_library(uring STATIC ${LIBURING_SOURCES})
    # PRIVATE: only needed to compile uring itself; csilk picks up the same
    #          paths from the include_directories() calls above (lines 207-209).
    target_include_directories(uring PRIVATE
      "${liburing_SOURCE_DIR}/src/include"
      "${liburing_BINARY_DIR}/src/include"
    )
    target_compile_definitions(uring PRIVATE -D_GNU_SOURCE)
  endif()
else()
  # Fetch libuv
  FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG        v1.48.0
  )
  FetchContent_MakeAvailable(libuv)
endif()

# Prefer system llhttp; fallback to FetchContent from source
find_library(LLHTTP_LIB NAMES llhttp libllhttp)
if(NOT LLHTTP_LIB)
  message(STATUS "System llhttp not found — fetching from source")
  FetchContent_Declare(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp.git
    GIT_TAG        release/v9.4.1
  )
  set(LLHTTP_BUILD_STATIC_LIBS ON)
  FetchContent_MakeAvailable(llhttp)
  # llhttp v9.4+ creates llhttp_shared / llhttp_static; alias llhttp::llhttp
  # points to whichever was defined last (static when both are built).
  set(LLHTTP_LIB llhttp::llhttp)
  if(llhttp_SOURCE_DIR)
    include_directories(${llhttp_SOURCE_DIR}/include)
  endif()
else()
  message(STATUS "Found system llhttp: ${LLHTTP_LIB}")
endif()

# Fetch cJSON
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(CJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG v1.7.18
)
FetchContent_MakeAvailable(cjson)

# Fetch nghttp2
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC_LIB ON CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nghttp2
  GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
  GIT_TAG        v1.61.0
)
FetchContent_MakeAvailable(nghttp2)
if(nghttp2_SOURCE_DIR)
  include_directories(${nghttp2_SOURCE_DIR}/lib/includes)
  include_directories(${nghttp2_BINARY_DIR}/lib/includes)
endif()

# Find CURL (>= 7.80.0 for HTTP/2 support)
find_package(CURL 7.80.0 REQUIRED)

include_directories(include)
include_directories(src)   # for internal headers (ctx_types.h moved from include/)
include_directories(${CMAKE_CURRENT_BINARY_DIR})   # for generated version.h
include_directories(${cjson_SOURCE_DIR})
include_directories(${CURL_INCLUDE_DIRS})

# Generate version.h from CMake variable
configure_file(
  include/csilk/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/csilk/version.h
  @ONLY
)
configure_file(
  include/csilk/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/csilk/version.h
  @ONLY
)

# Find MySQL (optional)
find_library(MYSQL_LIB NAMES mysqlclient libmysqlclient)
if(MYSQL_LIB)
  message(STATUS "Found MySQL client library: ${MYSQL_LIB}")
  find_path(MYSQL_INCLUDE_DIR mysql.h PATHS /usr/include/mysql /usr/local/include/mysql)
  if(MYSQL_INCLUDE_DIR)
    include_directories(${MYSQL_INCLUDE_DIR})
    message(STATUS "Found MySQL include dir: ${MYSQL_INCLUDE_DIR}")
  endif()
else()
  message(STATUS "MySQL client library not found — mysql driver will not build")
endif()

# Find PostgreSQL (optional)
find_library(PQ_LIB NAMES pq libpq)
if(PQ_LIB)
  message(STATUS "Found PostgreSQL client library: ${PQ_LIB}")
  find_path(PQ_INCLUDE_DIR libpq-fe.h PATHS /usr/include/postgresql /usr/local/include/postgresql)
  if(PQ_INCLUDE_DIR)
    include_directories(${PQ_INCLUDE_DIR})
    message(STATUS "Found PostgreSQL include dir: ${PQ_INCLUDE_DIR}")
  endif()
else()
  message(STATUS "PostgreSQL client library not found — postgres driver will not build")
endif()

# Find MongoDB (optional)
find_library(MONGOC_LIB NAMES mongoc-1.0)
find_library(BSON_LIB NAMES bson-1.0)
if(MONGOC_LIB AND BSON_LIB)
  message(STATUS "Found MongoDB client library: ${MONGOC_LIB}")
  find_path(MONGOC_INCLUDE_DIR mongoc/mongoc.h PATHS /usr/include/libmongoc-1.0 /usr/local/include/libmongoc-1.0)
  find_path(BSON_INCLUDE_DIR bson/bson.h PATHS /usr/include/libbson-1.0 /usr/local/include/libbson-1.0)
  if(MONGOC_INCLUDE_DIR AND BSON_INCLUDE_DIR)
    include_directories(${MONGOC_INCLUDE_DIR})
    include_directories(${BSON_INCLUDE_DIR})
    message(STATUS "Found MongoDB include dir: ${MONGOC_INCLUDE_DIR}")
    set(HAS_MONGODB TRUE)
  endif()
else()
  message(STATUS "MongoDB client library not found — mongodb driver will not build")
endif()

# Source files — organized per module in cmake/sources.cmake
include(cmake/sources.cmake)

# Conditional driver sources
if(MYSQL_LIB AND MYSQL_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/data/drivers/mysql.c)
  set(CSILK_HAS_MYSQL TRUE)
endif()

if(PQ_LIB AND PQ_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/data/drivers/postgres.c)
  set(CSILK_HAS_POSTGRES TRUE)
endif()

if(HAS_MONGODB)
  list(APPEND CSILK_SOURCES src/data/drivers/mongodb.c)
endif()

# Find Redis (hiredis) (optional)
find_library(HIREDIS_LIB NAMES hiredis)
if(HIREDIS_LIB)
  message(STATUS "Found hiredis library: ${HIREDIS_LIB}")
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/include /usr/local/include)
  if(HIREDIS_INCLUDE_DIR)
    include_directories(${HIREDIS_INCLUDE_DIR})
    message(STATUS "Found hiredis include dir: ${HIREDIS_INCLUDE_DIR}")
    list(APPEND CSILK_SOURCES src/data/drivers/redis.c)
    set(CSILK_HAS_REDIS TRUE)
  endif()
else()
  message(STATUS "hiredis library not found — redis driver will not build")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find system libyaml (>= 0.2.0)
find_library(YAML_LIB NAMES yaml libyaml REQUIRED)
if(NOT YAML_LIB)
  message(FATAL_ERROR "libyaml library not found. Install libyaml-dev (>= 0.2.0)")
endif()
message(STATUS "Found libyaml: ${YAML_LIB}")

# Find system zlib (>= 1.2.0)
find_library(ZLIB_LIB NAMES z libz REQUIRED)
if(NOT ZLIB_LIB)
  message(FATAL_ERROR "zlib library not found. Install zlib1g-dev (>= 1.2.0)")
endif()
message(STATUS "Found zlib: ${ZLIB_LIB}")

# Find system sqlite3 (>= 3.20.0)
find_library(SQLITE3_LIB NAMES sqlite3 REQUIRED)
if(NOT SQLITE3_LIB)
  message(FATAL_ERROR "sqlite3 library not found. Install libsqlite3-dev (>= 3.20.0)")
endif()
message(STATUS "Found sqlite3: ${SQLITE3_LIB}")

# Find OpenSSL (>= 1.1.1 for TLS 1.3)
find_package(OpenSSL 1.1.1 REQUIRED)
message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")

# Option to also build as shared library
option(CSILK_BUILD_SHARED "Build shared library (in addition to static)" OFF)

# Build static library
add_library(csilk STATIC ${CSILK_SOURCES})
set_target_properties(csilk PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Optimization flags per-configuration via generator expressions
set(CSILK_AVX2_FLAGS "")
if(CSILK_HAS_AVX2)
  set(CSILK_AVX2_FLAGS "-mavx2;-D__AVX2__")
endif()

target_compile_options(csilk PRIVATE
    "${CSILK_COMMON_FLAGS}"
    "${CSILK_RELEASE_FLAGS}"
    "${CSILK_DEBUG_FLAGS}"
    "${CSILK_AVX2_FLAGS}"
    "$<$<BOOL:${CSILK_HAS_AVX512}>:-DCSILK_HAS_AVX512>"
    "$<$<BOOL:${USE_COVERAGE}>:--coverage;-O0;-g>"
)
target_compile_definitions(csilk PRIVATE
    CSILK_SWAGGER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui"
    "$<$<BOOL:${CSILK_HAS_MYSQL}>:HAS_MYSQL>"
    "$<$<BOOL:${CSILK_HAS_POSTGRES}>:HAS_POSTGRES>"
    "$<$<BOOL:${HAS_MONGODB}>:HAS_MONGODB>"
    "$<$<BOOL:${CSILK_HAS_REDIS}>:HAS_REDIS>"
    "$<$<BOOL:${ENABLE_OOM_TEST}>:TEST_OOM>"
    "$<$<BOOL:${DEBUG_ARENA}>:DEBUG_ARENA>"
)
target_compile_definitions(csilk PUBLIC
    "$<$<BOOL:${CSILK_USE_URING_ONLY}>:CSILK_USE_URING>"
)

target_link_options(csilk PUBLIC
    "$<$<BOOL:${USE_COVERAGE}>:--coverage>"
    "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
)

# Linker flags: gc-sections in Release removes dead code
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_options(csilk PRIVATE
      "$<$<CONFIG:Release>:-Wl,--gc-sections>"
  )
endif()

# Linker flags need per-target scope too (Release LTO via generator expression)
set_target_properties(csilk PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
    VERSION "${CSILK_VERSION}"
    SOVERSION "${CSILK_VERSION_MAJOR}"
    OUTPUT_NAME csilk
    INTERFACE_COMPILE_FEATURES c_std_23
)

if(CSILK_USE_URING)
  target_link_libraries(csilk PUBLIC uring cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLITE3_LIB} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB} nghttp2)
  if(NOT APPLE)
    target_compile_options(csilk PUBLIC "-D_GNU_SOURCE")
  endif()
else()
  target_link_libraries(csilk PUBLIC uv cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLITE3_LIB} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB} nghttp2)
endif()
if(MYSQL_LIB)
  target_link_libraries(csilk PRIVATE ${MYSQL_LIB})
endif()
if(PQ_LIB)
  target_link_libraries(csilk PRIVATE ${PQ_LIB})
endif()
if(HIREDIS_LIB)
  target_link_libraries(csilk PRIVATE ${HIREDIS_LIB})
endif()
target_include_directories(csilk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_target_properties(csilk PROPERTIES
      INTERPROCEDURAL_OPTIMIZATION "$<$<CONFIG:Release>:TRUE>"
  )
endif()
if(MYSQL_LIB)
  target_link_libraries(csilk PRIVATE ${MYSQL_LIB})
endif()
if(PQ_LIB)
  target_link_libraries(csilk PRIVATE ${PQ_LIB})
endif()
if(HIREDIS_LIB)
  target_link_libraries(csilk PRIVATE ${HIREDIS_LIB})
endif()

# Precompiled Headers (PCH) — accelerate builds by pre-including stable headers
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(csilk PRIVATE
        <stdio.h>
        <stdlib.h>
        <string.h>
        <stdint.h>
        <time.h>
        <errno.h>
    )
endif()

# Build shared library if requested
if(CSILK_BUILD_SHARED)
    add_library(csilk_shared SHARED ${CSILK_SOURCES})
    target_link_libraries(csilk_shared PUBLIC uv cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLITE3_LIB} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB} nghttp2)
    if(MYSQL_LIB)
      target_link_libraries(csilk_shared PRIVATE ${MYSQL_LIB})
    endif()
    if(PQ_LIB)
      target_link_libraries(csilk_shared PRIVATE ${PQ_LIB})
    endif()
    if(HIREDIS_LIB)
      target_link_libraries(csilk_shared PRIVATE ${HIREDIS_LIB})
    endif()
    target_include_directories(csilk_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
    )
    target_compile_options(csilk_shared PRIVATE
        "${CSILK_COMMON_FLAGS}"
        "${CSILK_RELEASE_FLAGS}"
        "${CSILK_DEBUG_FLAGS}"
        "${CSILK_AVX2_FLAGS}"
        "$<$<BOOL:${CSILK_HAS_AVX512}>:-DCSILK_HAS_AVX512>"
    )
    target_compile_definitions(csilk_shared PRIVATE
        CSILK_SWAGGER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui"
        "$<$<BOOL:${CSILK_USE_URING_ONLY}>:CSILK_USE_URING>"
        "$<$<BOOL:${CSILK_HAS_MYSQL}>:HAS_MYSQL>"
        "$<$<BOOL:${CSILK_HAS_POSTGRES}>:HAS_POSTGRES>"
        "$<$<BOOL:${HAS_MONGODB}>:HAS_MONGODB>"
        "$<$<BOOL:${CSILK_HAS_REDIS}>:HAS_REDIS>"
        "$<$<BOOL:${ENABLE_OOM_TEST}>:TEST_OOM>"
        "$<$<BOOL:${DEBUG_ARENA}>:DEBUG_ARENA>"
    )
    target_compile_options(csilk_shared PRIVATE
        "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
        "$<$<BOOL:${USE_COVERAGE}>:--coverage;-O0;-g>"
    )
    set_target_properties(csilk_shared PROPERTIES
        PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
        VERSION "${CSILK_VERSION}"
        SOVERSION "${CSILK_VERSION_MAJOR}"
        OUTPUT_NAME csilk
        INTERFACE_COMPILE_FEATURES c_std_23
        INTERPROCEDURAL_OPTIMIZATION "$<$<CONFIG:Release>:TRUE>"
    )
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
        target_precompile_headers(csilk_shared PRIVATE
            <stdio.h>
            <stdlib.h>
            <string.h>
            <stdint.h>
            <time.h>
            <errno.h>
        )
    endif()
    target_link_options(csilk_shared PRIVATE
        "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
    )
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
        target_link_options(csilk_shared PRIVATE
            "$<$<CONFIG:Release>:-Wl,--gc-sections>"
        )
    endif()
    if((CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang") AND NOT APPLE)
        target_link_options(csilk_shared PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libcsilk.map")
    endif()
    message(STATUS "Shared library build enabled: libcsilk.so")
endif()

# Only build tests and examples when csilk is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Tests — organized per module in cmake/tests.cmake
  include(cmake/tests.cmake)

  # test_timeout needs an extended CTest timeout
  add_csilk_test(test_timeout tests/test_timeout.c)
  set_tests_properties(test_timeout PROPERTIES TIMEOUT 10)
  list(APPEND CSILK_ALL_TEST_NAMES test_timeout)

  # test_multi_worker needs a longer timeout for multi-threaded requests
  set_tests_properties(test_multi_worker PROPERTIES TIMEOUT 30)

  # Concurrent scenario tests involve thread spawning or multiple streams
  set_tests_properties(test_mq_concurrent PROPERTIES TIMEOUT 30)
  set_tests_properties(test_ws_concurrent PROPERTIES TIMEOUT 15)
  set_tests_properties(test_sse_concurrent PROPERTIES TIMEOUT 15)

  if(ENABLE_OOM_TEST)
      add_executable(test_oom tests/test_oom.c)
      target_compile_definitions(test_oom PRIVATE TEST_OOM)
      target_link_libraries(test_oom csilk pthread)
      add_test(NAME test_oom COMMAND test_oom)

      add_executable(test_oom_io tests/test_oom_io.c)
      target_compile_definitions(test_oom_io PRIVATE TEST_OOM)
      target_link_libraries(test_oom_io csilk pthread)
      add_test(NAME test_oom_io COMMAND test_oom_io)
  endif()

  option(USE_FUZZER "Enable libFuzzer" OFF)
  if(USE_FUZZER)
      add_executable(fuzz_test tests/fuzz_test.c)
      target_compile_options(fuzz_test PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_test csilk)

      add_executable(fuzz_yaml tests/fuzz_yaml.c)
      target_compile_options(fuzz_yaml PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_yaml PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_yaml csilk)

      add_executable(fuzz_url tests/fuzz_url.c)
      target_compile_options(fuzz_url PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_url PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_url csilk)
  endif()

  # Examples
  add_executable(example_server examples/example_server.c)
  target_link_libraries(example_server csilk)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config_multi.yaml ${CMAKE_CURRENT_BINARY_DIR}/config.yaml COPYONLY)

  add_executable(example_app examples/example_app.c)
  target_link_libraries(example_app csilk)

  add_executable(example_ai examples/example_ai.c)
  target_link_libraries(example_ai csilk)

  add_executable(example_ai_providers examples/example_ai_providers.c)
  target_link_libraries(example_ai_providers csilk)

  add_executable(example_ai_workflow examples/example_ai_workflow.c)
  target_link_libraries(example_ai_workflow csilk)

  add_executable(example_workflow_ui examples/example_workflow_ui.c)
  target_link_libraries(example_workflow_ui csilk)

#  add_executable(example_admin_dashboard examples/example_admin_dashboard.c)
#  target_link_libraries(example_admin_dashboard csilk)

  add_executable(example_db examples/example_db.c)
  target_link_libraries(example_db csilk)

  add_executable(example_ws_tls_mq examples/example_ws_tls_mq.c)
  target_link_libraries(example_ws_tls_mq csilk)

  add_executable(example_websocket examples/example_websocket.c)
  target_link_libraries(example_websocket csilk)

  add_executable(example_sse examples/example_sse.c)
  target_link_libraries(example_sse csilk)

  add_executable(example_tls examples/example_tls.c)
  target_link_libraries(example_tls csilk)

  add_executable(example_custom_driver examples/example_custom_driver.c)
  target_link_libraries(example_custom_driver csilk)

  # Scaffold Tool (TUI)
  file(GLOB SCAFFOLD_SOURCES tools/csilk-scaffold/*.c)
  if(SCAFFOLD_SOURCES)
      add_executable(csilk-scaffold ${SCAFFOLD_SOURCES})
      target_include_directories(csilk-scaffold PRIVATE tools/csilk-scaffold)
      message(STATUS "Built scaffold tool: csilk-scaffold")
  endif()
endif()

# Add run_tests custom target
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS csilk ${CSILK_ALL_TEST_NAMES}
    COMMENT "Running all tests with ctest"
)

if(USE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)
    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR}
                --filter "src/"
                --exclude "tests/"
                --exclude "examples/"
                --exclude "build_release/"
                --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
                --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS run_tests
            COMMENT "Generating code coverage report"
        )
        add_custom_target(coverage_summary
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR}
                --filter "src/"
                --exclude "tests/"
                --exclude "examples/"
                --exclude "build_release/"
                --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS run_tests
            COMMENT "Printing coverage summary"
        )
    else()
        message(WARNING "gcovr not found — install with 'pip install gcovr' for coverage reports")
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "gcovr not found — tests run without coverage report"
        )
    endif()
endif()


# Install targets
include(GNUInstallDirs)
if(CSILK_USE_URING)
    set(CSILK_INSTALL_TARGETS csilk uring)
else()
    set(CSILK_INSTALL_TARGETS csilk)
endif()
install(TARGETS ${CSILK_INSTALL_TARGETS}
    EXPORT csilk-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CSILK_BUILD_SHARED)
    install(TARGETS csilk_shared
        EXPORT csilk-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY share/swagger-ui/ DESTINATION ${CMAKE_INSTALL_DATADIR}/csilk/swagger-ui)

install(EXPORT csilk-targets
    FILE csilk-config.cmake
    NAMESPACE csilk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/csilk
)

# Build-tree export disabled: csilk's FetchContent dependencies (uv, cjson)
# would also need export sets. Use add_subdirectory with CSILK_SOURCE_DIR
# or install csilk and use find_package instead.
# export(EXPORT csilk-targets FILE "${CMAKE_BINARY_DIR}/cmake/csilk-config.cmake" NAMESPACE csilk::)

# clang-tidy
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
    set(CSILK_TIDY_CHECKS "clang-analyzer-*,performance-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-performance-no-int-to-ptr")
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE}
            --quiet
            --checks=-*,${CSILK_TIDY_CHECKS}
            --warnings-as-errors="*"
            --extra-arg=-Wno-everything
            ${CSILK_SOURCES}
            -- -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${CMAKE_CURRENT_BINARY_DIR}
            -I${cjson_SOURCE_DIR}
            -I${cjson_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files"
    )
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
else()
    message(STATUS "clang-tidy not found — skipping")
endif()

# Mermaid diagram validation
find_program(PYTHON3_EXECUTABLE python3)
if(PYTHON3_EXECUTABLE)
    add_custom_target(check-mermaid
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mermaid.py
            ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Validating Mermaid diagrams in documentation"
    )
    message(STATUS "Found python3: ${PYTHON3_EXECUTABLE}")
else()
    message(STATUS "python3 not found — skipping Mermaid validation target")
endif()

# Benchmark target
add_custom_target(bench
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_benchmarks.sh --save
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running performance benchmarks with wrk"
)

# Profiling target (perf + FlameGraph)
add_custom_target(profile
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/profile.sh --duration 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Profiling example_server with perf + FlameGraph"
)

# CPack packaging
set(CPACK_PACKAGE_NAME "csilk")
set(CPACK_PACKAGE_VERSION "${CSILK_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Csilk")
include(CPack)
