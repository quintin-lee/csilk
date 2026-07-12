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
# Release: -O2 + LTO. Native arch tuning (-march=native) is ON by default
# for maximum performance on the build machine. Set -DCSILK_PORTABLE=ON
# to produce a portable binary that runs on any x86_64/aarch64 CPU.
# Debug/RelWithDebInfo/MinSizeRel: -O2 without LTO.
option(CSILK_PORTABLE "Build portable binary (disable -march=native)" OFF)
set(CSILK_COMMON_FLAGS "")
if(CMAKE_CROSSCOMPILING OR CSILK_PORTABLE)
  set(CSILK_RELEASE_FLAGS "$<$<CONFIG:Release>:-O2;-flto;-fomit-frame-pointer>")
else()
  set(CSILK_RELEASE_FLAGS "$<$<CONFIG:Release>:-O2;-flto;-fomit-frame-pointer;-march=native>")
endif()
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

option(USE_TSAN "Enable ThreadSanitizer for data race detection" OFF)
if(USE_TSAN)
    if(USE_ASAN)
        message(FATAL_ERROR "ASAN and TSAN cannot be enabled simultaneously")
    endif()
    message(STATUS "ThreadSanitizer enabled")
    set(CSILK_TSAN_FLAGS "-fsanitize=thread;-fno-omit-frame-pointer;-g")
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

    # Collect all C/H source files for formatting checks
    file(GLOB_RECURSE CSILK_FORMAT_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    )

    add_custom_target(format
        COMMAND ${CMAKE_COMMAND}
            -DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/format.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting C/H files with clang-format..."
    )

    add_custom_target(check-format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${CSILK_FORMAT_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking code formatting (dry-run)..."
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
  # Force use system libuv to bypass network issues
  find_library(LIBUV_LIB NAMES uv)
  find_path(LIBUV_INCLUDE_DIR uv.h)
  if(LIBUV_LIB AND LIBUV_INCLUDE_DIR)
    message(STATUS "Found system libuv: ${LIBUV_LIB}")
    set(libuv_LIBRARIES ${LIBUV_LIB})
    include_directories(${LIBUV_INCLUDE_DIR})
  else()
    message(FATAL_ERROR "libuv not found on system. Please install libuv-dev.")
  endif()
  # Comment out FetchContent to ensure it's not called
  # FetchContent_Declare(
  #   libuv
  #   GIT_REPOSITORY https://github.com/libuv/libuv.git
  #   GIT_TAG        v1.48.0
  # )
  # FetchContent_MakeAvailable(libuv)
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

# Find CURL (HTTP/2 support via nghttp2)
find_package(CURL REQUIRED)

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
# Generate Python _version.py for distribution (single version source)
configure_file(
  python/csilk/_version.py.in
  ${CMAKE_CURRENT_SOURCE_DIR}/python/csilk/_version.py
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

# Find Redis (hiredis) (optional)
find_library(HIREDIS_LIB NAMES hiredis)
if(HIREDIS_LIB)
  message(STATUS "Found hiredis library: ${HIREDIS_LIB}")
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/include /usr/local/include)
  if(HIREDIS_INCLUDE_DIR)
    include_directories(${HIREDIS_INCLUDE_DIR})
    message(STATUS "Found hiredis include dir: ${HIREDIS_INCLUDE_DIR}")
    list(APPEND CSILK_SOURCES src/drivers/db/redis.c)
    set(CSILK_HAS_REDIS TRUE)
  endif()
else()
  message(STATUS "hiredis library not found — redis driver will not build")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find system libyaml (>= 0.2.0) — prefer pkg-config, fallback to find_library
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(YAML QUIET yaml>=0.2.0)
endif()
if(NOT YAML_FOUND)
  find_library(YAML_LIB NAMES yaml libyaml)
  find_path(YAML_INCLUDE_DIR yaml.h PATH_SUFFIXES yaml)
  if(YAML_LIB AND YAML_INCLUDE_DIR)
    set(YAML_FOUND TRUE)
    set(YAML_LIBRARIES ${YAML_LIB})
    set(YAML_INCLUDE_DIRS ${YAML_INCLUDE_DIR})
  endif()
endif()
if(NOT YAML_FOUND)
  message(FATAL_ERROR "libyaml >= 0.2.0 not found. Install libyaml-dev.\n"
    "  Debian/Ubuntu: sudo apt install libyaml-dev\n"
    "  macOS: brew install libyaml")
endif()
message(STATUS "Found libyaml: ${YAML_LIBRARIES}")

# Find system zlib (>= 1.2.0)
find_package(ZLIB 1.2 REQUIRED)
message(STATUS "Found zlib: ${ZLIB_VERSION_STRING}")

# Find system sqlite3 (>= 3.20.0)
find_package(SQLite3 3.20 REQUIRED)
message(STATUS "Found sqlite3: ${SQLite3_VERSION}")

# Find OpenSSL (>= 1.1.1 for TLS 1.3)
find_package(OpenSSL 1.1.1 REQUIRED)
message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")

# Option to also build as shared library
option(CSILK_BUILD_SHARED "Build shared library (in addition to static)" OFF)

# ── Configurable compile-time constants ──────────────────────────────────
# Users can override on the cmake command line, e.g. -DCSILK_HEADER_BUCKETS=128
set(CSILK_HEADER_BUCKETS 64  CACHE STRING "Header hash-table bucket count")
set(CSILK_MAX_PARAMS      20  CACHE STRING "Max URL path parameters per request")
set(CSILK_MAX_STORAGE    64  CACHE STRING "Max context key-value storage entries")
set(CSILK_DEFAULT_ARENA_SIZE 4096 CACHE STRING "Default arena chunk size in bytes")
set(CSILK_ARENA_TIER_COUNT 3   CACHE STRING "Number of arena chunk size tiers")
set(CSILK_MAX_TLS_CHUNKS  8   CACHE STRING "Max cached arena chunks per tier")

# ── Helper function: apply common compile/link/profile to a target ────────
# Usage: csilk_target_setup(csilk PUBLIC  STATIC)
#        csilk_target_setup(csilk_shared PUBLIC SHARED)
function(csilk_target_setup TARGET VISIBILITY TYPE)
  target_compile_options(${TARGET} PRIVATE
      "${CSILK_COMMON_FLAGS}"
      "${CSILK_RELEASE_FLAGS}"
      "${CSILK_DEBUG_FLAGS}"
      "${CSILK_AVX2_FLAGS}"
      "$<$<BOOL:${CSILK_HAS_AVX512}>:-DCSILK_HAS_AVX512>"
      "$<$<BOOL:${USE_COVERAGE}>:--coverage;-O0;-g>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
  )
  target_compile_definitions(${TARGET} PRIVATE
      CSILK_SWAGGER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui"
      "$<$<BOOL:${CSILK_HAS_MYSQL}>:HAS_MYSQL>"
      "$<$<BOOL:${CSILK_HAS_POSTGRES}>:HAS_POSTGRES>"
      "$<$<BOOL:${HAS_MONGODB}>:HAS_MONGODB>"
      "$<$<BOOL:${CSILK_HAS_REDIS}>:HAS_REDIS>"
      "$<$<BOOL:${ENABLE_OOM_TEST}>:TEST_OOM>"
      "$<$<BOOL:${DEBUG_ARENA}>:DEBUG_ARENA>"
      # Configurable compile-time constants (from CMake cache)
      "CSILK_HEADER_BUCKETS=${CSILK_HEADER_BUCKETS}"
      "CSILK_MAX_PARAMS=${CSILK_MAX_PARAMS}"
      "CSILK_MAX_STORAGE=${CSILK_MAX_STORAGE}"
      "CSILK_DEFAULT_ARENA_SIZE=${CSILK_DEFAULT_ARENA_SIZE}"
      "CSILK_ARENA_TIER_COUNT=${CSILK_ARENA_TIER_COUNT}"
      "CSILK_MAX_TLS_CHUNKS=${CSILK_MAX_TLS_CHUNKS}"
  )
  target_compile_definitions(${TARGET} PUBLIC
      "$<$<BOOL:${CSILK_USE_URING_ONLY}>:CSILK_USE_URING>"
  )
  target_link_options(${TARGET} PUBLIC
      "$<$<BOOL:${USE_COVERAGE}>:--coverage>"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
  )

  # gc-sections in Release removes dead code
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(${TARGET} PRIVATE
        "$<$<CONFIG:Release>:-Wl,--gc-sections>"
    )
  endif()

  # Version-script only for shared library
  if(TYPE STREQUAL "SHARED")
    if((CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang") AND NOT APPLE)
        target_link_options(${TARGET} PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/libcsilk.map")
    endif()
  endif()

  set_target_properties(${TARGET} PROPERTIES
      PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
      VERSION "${CSILK_VERSION}"
      SOVERSION "${CSILK_VERSION_MAJOR}"
      OUTPUT_NAME csilk
      INTERFACE_COMPILE_FEATURES c_std_23
  )

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_target_properties(${TARGET} PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION "$<$<CONFIG:Release>:TRUE>"
    )
  endif()

  # Common link libraries
  if(CSILK_USE_URING)
    target_link_libraries(${TARGET} PUBLIC uring cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLite3_LIBRARIES} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIBRARIES} ${ZLIB_LIBRARIES} nghttp2)
    if(NOT APPLE)
      target_compile_options(${TARGET} PUBLIC "-D_GNU_SOURCE")
    endif()
  else()
    target_link_libraries(${TARGET} PUBLIC ${libuv_LIBRARIES} cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLite3_LIBRARIES} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIBRARIES} ${ZLIB_LIBRARIES} nghttp2)
  endif()

  # Optional DB client libraries
  foreach(DB_LIB MYSQL_LIB PQ_LIB HIREDIS_LIB)
    if(${DB_LIB})
      target_link_libraries(${TARGET} PRIVATE ${${DB_LIB}})
    endif()
  endforeach()

  target_include_directories(${TARGET} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
      $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
  )

  # Precompiled Headers
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
      target_precompile_headers(${TARGET} PRIVATE
          <stdio.h> <stdlib.h> <string.h> <stdint.h> <time.h> <errno.h>
      )
  endif()
endfunction()

# ── csilk static library ─────────────────────────────────────────────────
set(CSILK_AVX2_FLAGS "")
if(CSILK_HAS_AVX2)
  set(CSILK_AVX2_FLAGS "-mavx2;-D__AVX2__")
endif()

add_library(csilk STATIC ${CSILK_SOURCES})
set_target_properties(csilk PROPERTIES POSITION_INDEPENDENT_CODE ON)
csilk_target_setup(csilk PUBLIC STATIC)

# ── csilk shared library (optional) ──────────────────────────────────────
if(CSILK_BUILD_SHARED)
    add_library(csilk_shared SHARED ${CSILK_SOURCES})
    csilk_target_setup(csilk_shared PUBLIC SHARED)
    message(STATUS "Shared library build enabled: libcsilk.so")
endif()

# Only build tests and examples when csilk is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Tests — organized per module in cmake/tests.cmake
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

# Examples
add_executable(example_server examples/basic/example_server.c)
target_link_libraries(example_server csilk)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/examples/advanced/config_multi.yaml ${CMAKE_CURRENT_BINARY_DIR}/config.yaml COPYONLY)

add_executable(example_app examples/basic/example_app.c)
target_link_libraries(example_app csilk)

add_executable(example_ai examples/ai/example_ai.c)
target_link_libraries(example_ai csilk)

add_executable(example_ai_providers examples/ai/example_ai_providers.c)
target_link_libraries(example_ai_providers csilk)

add_executable(example_ai_workflow examples/ai/example_ai_workflow.c)
target_link_libraries(example_ai_workflow csilk)

add_executable(example_workflow_ui examples/ai/example_workflow_ui.c)
target_link_libraries(example_workflow_ui csilk)

#  add_executable(example_admin_dashboard examples/middleware/example_admin_dashboard.c)
#  target_link_libraries(example_admin_dashboard csilk)

add_executable(example_db examples/database/example_db.c)
target_link_libraries(example_db csilk)

add_executable(example_ws_tls_mq examples/websocket/example_ws_tls_mq.c)
target_link_libraries(example_ws_tls_mq csilk)

add_executable(example_websocket examples/websocket/example_websocket.c)
target_link_libraries(example_websocket csilk)

add_executable(example_sse examples/middleware/example_sse.c)
target_link_libraries(example_sse csilk)

add_executable(example_tls examples/advanced/example_tls.c)
target_link_libraries(example_tls csilk)

add_executable(example_custom_driver examples/advanced/example_custom_driver.c)
target_link_libraries(example_custom_driver csilk)

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

# clang-tidy — reads checks/warnings from .clang-tidy at project root
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE}
            --quiet
            --warnings-as-errors="*"
            --extra-arg=-Wno-everything
            -p ${CMAKE_CURRENT_BINARY_DIR}
            ${CSILK_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files (using .clang-tidy config)"
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
