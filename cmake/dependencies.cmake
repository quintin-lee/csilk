# cmake/dependencies.cmake — FetchContent and find_package for all dependencies

include(FetchContent)

# ── WAMR (WebAssembly Micro Runtime) — fetched when CSILK_ENABLE_WASM=ON ──
if(CSILK_ENABLE_WASM)
  message(STATUS "WASM enabled — fetching wasm-micro-runtime (WAMR)")
  set(WAMR_BUILD_INTERP 1 CACHE STRING "" FORCE)
  set(WAMR_BUILD_AOT 0 CACHE STRING "" FORCE)
  set(WAMR_BUILD_JIT 0 CACHE STRING "" FORCE)
  set(WAMR_BUILD_INSTRUCTION_METERING 1 CACHE STRING "" FORCE)
  set(WAMR_BUILD_LIBC_BUILTIN 1 CACHE STRING "" FORCE)
  set(WAMR_BUILD_LIBC_WASI 1 CACHE STRING "" FORCE)
  set(WAMR_BUILD_LIB_PTHREAD OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_THREADS OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_NN OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_SOCKET OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_SOCKET OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_DOM OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_FS OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_CLAP OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_HTTP PROXY CACHE STRING "" FORCE)
  set(WAMR_BUILD_LIB_WASI_CLOCK OFF CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_RANDOM ON CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_ENVIRONMENT ON CACHE BOOL "" FORCE)
  set(WAMR_BUILD_LIB_WASI_FILESYSTEM OFF CACHE BOOL "" FORCE)
  if(APPLE)
    set(WAMR_BUILD_PLATFORM "darwin" CACHE STRING "" FORCE)
  else()
    set(WAMR_BUILD_PLATFORM "linux" CACHE STRING "" FORCE)
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm64|aarch64")
    set(WAMR_BUILD_TARGET "AARCH64" CACHE STRING "" FORCE)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^x86_64|AMD64|amd64")
    set(WAMR_BUILD_TARGET "X86_64" CACHE STRING "" FORCE)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv[0-9]")
    set(WAMR_BUILD_TARGET "ARM" CACHE STRING "" FORCE)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^riscv64")
    set(WAMR_BUILD_TARGET "RISCV64" CACHE STRING "" FORCE)
  else()
    set(WAMR_BUILD_TARGET "X86_64" CACHE STRING "" FORCE)
  endif()
  if(DEFINED BUILD_SHARED_LIBS)
    set(_csilk_had_build_shared_libs TRUE)
    set(_csilk_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
  else()
    set(_csilk_had_build_shared_libs FALSE)
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    wamr
    GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
    GIT_TAG        WAMR-2.4.4
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""
  )
  FetchContent_MakeAvailable(wamr)
  if(_csilk_had_build_shared_libs)
    set(BUILD_SHARED_LIBS "${_csilk_saved_build_shared_libs}" CACHE BOOL "" FORCE)
  else()
    unset(BUILD_SHARED_LIBS CACHE)
  endif()
  if(wamr_SOURCE_DIR)
    if(TARGET vmlib AND NOT TARGET csilk::vmlib)
      add_library(csilk::vmlib INTERFACE IMPORTED)
      target_link_libraries(csilk::vmlib INTERFACE vmlib)
    endif()
  endif()
endif()

# ── ngtcp2 + nghttp3 (HTTP/3 QUIC) — optional ────────────────────────────
if(CSILK_ENABLE_HTTP3)
  message(STATUS "HTTP/3 enabled — locating ngtcp2 + nghttp3")

  # Prefer system ngtcp2/nghttp3 when available; fall back to FetchContent.
  find_library(NGTCP2_LIB NAMES ngtcp2 libngtcp2)
  find_path(NGTCP2_INCLUDE_DIR ngtcp2/ngtcp2.h)
  find_library(NGHTTP3_LIB NAMES nghttp3 libnghttp3)
  find_path(NGHTTP3_INCLUDE_DIR nghttp3/nghttp3.h)
  find_library(NGTCP2_CRYPTO_OSSL_LIB NAMES ngtcp2_crypto_ossl libngtcp2_crypto_ossl)

  if(NGTCP2_LIB AND NGTCP2_INCLUDE_DIR AND NGHTTP3_LIB AND NGHTTP3_INCLUDE_DIR)
    message(STATUS "Found system ngtcp2: ${NGTCP2_LIB}, nghttp3: ${NGHTTP3_LIB}")
    if(NOT TARGET csilk::ngtcp2)
      add_library(csilk::ngtcp2 INTERFACE IMPORTED)
      set_target_properties(csilk::ngtcp2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NGTCP2_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${NGTCP2_LIB};${NGTCP2_CRYPTO_OSSL_LIB};OpenSSL::SSL;OpenSSL::Crypto"
      )
    endif()
    if(NOT TARGET csilk::nghttp3)
      add_library(csilk::nghttp3 INTERFACE IMPORTED)
      set_target_properties(csilk::nghttp3 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP3_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${NGHTTP3_LIB}"
      )
    endif()
    set(CSILK_HAS_NGTCP2 TRUE)
    add_compile_definitions(CSILK_HAS_NGTCP2=1)
  else()
    message(STATUS "System ngtcp2/nghttp3 not found — fetching from source")
    FetchContent_Declare(
      ngtcp2
      GIT_REPOSITORY https://github.com/ngtcp2/ngtcp2.git
      GIT_TAG        v1.25.0
      GIT_SHALLOW    TRUE
    )
    FetchContent_Declare(
      nghttp3
      GIT_REPOSITORY https://github.com/ngtcp2/nghttp3.git
      GIT_TAG        v1.18.0
      GIT_SHALLOW    TRUE
    )
    # ngtcp2 requires nghttp3 headers
    FetchContent_MakeAvailable(ngtcp2)
    FetchContent_MakeAvailable(nghttp3)
    if(ngtcp2_SOURCE_DIR)
      set(CSILK_HAS_NGTCP2 TRUE)
      add_compile_definitions(CSILK_HAS_NGTCP2=1)
    endif()
  endif()
endif()

# ── I/O Backend (io_uring or libuv) ──────────────────────────────────────
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

    include_directories(${liburing_SOURCE_DIR}/src/include)
    include_directories(${liburing_BINARY_DIR}/src/include)
    include_directories(${liburing_SOURCE_DIR}/src)

    set(LIBURING_SOURCES
      "${liburing_SOURCE_DIR}/src/queue.c"
      "${liburing_SOURCE_DIR}/src/setup.c"
      "${liburing_SOURCE_DIR}/src/syscall.c"
      "${liburing_SOURCE_DIR}/src/register.c"
      "${liburing_SOURCE_DIR}/src/ffi.c"
      "${liburing_SOURCE_DIR}/src/version.c"
    )
    add_library(uring STATIC ${LIBURING_SOURCES})
    target_include_directories(uring PUBLIC
      $<BUILD_INTERFACE:${liburing_SOURCE_DIR}/src/include>
      $<BUILD_INTERFACE:${liburing_BINARY_DIR}/src/include>
      $<INSTALL_INTERFACE:include>
    )
    target_compile_definitions(uring PRIVATE -D_GNU_SOURCE)
  endif()
else()
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBUV QUIET IMPORTED_TARGET libuv)
  endif()
  if(TARGET PkgConfig::LIBUV)
    if(NOT TARGET csilk::libuv)
      add_library(csilk::libuv INTERFACE IMPORTED)
      target_link_libraries(csilk::libuv INTERFACE PkgConfig::LIBUV)
    endif()
    message(STATUS "Found libuv via pkg-config")
  else()
    find_library(LIBUV_LIB NAMES uv libuv)
    find_path(LIBUV_INCLUDE_DIR uv.h)
    if(LIBUV_LIB AND LIBUV_INCLUDE_DIR)
      message(STATUS "Found system libuv: ${LIBUV_LIB}")
      if(NOT TARGET csilk::libuv)
        add_library(csilk::libuv INTERFACE IMPORTED)
        set_target_properties(csilk::libuv PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${LIBUV_INCLUDE_DIR}"
          INTERFACE_LINK_LIBRARIES "${LIBUV_LIB}"
        )
      endif()
    else()
      message(FATAL_ERROR "libuv not found on system. Please install libuv-dev.")
    endif()
  endif()
endif()

# ── HTTP/1 Parser (llhttp) ────────────────────────────────────────────────
find_library(LLHTTP_LIB NAMES llhttp libllhttp)
if(NOT LLHTTP_LIB)
  message(STATUS "System llhttp not found — fetching from source and creating csilk::llhttp")
  FetchContent_Declare(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp.git
    GIT_TAG        release/v9.4.1
  )
  FetchContent_MakeAvailable(llhttp)
  if(llhttp_SOURCE_DIR)
    include_directories(${llhttp_SOURCE_DIR}/include)
    set(LLHTTP_SOURCES
      ${llhttp_SOURCE_DIR}/src/api.c
      ${llhttp_SOURCE_DIR}/src/http.c
      ${llhttp_SOURCE_DIR}/src/llhttp.c
    )
    if(NOT TARGET csilk_llhttp)
      add_library(csilk_llhttp STATIC ${LLHTTP_SOURCES})
      set_target_properties(csilk_llhttp PROPERTIES POSITION_INDEPENDENT_CODE ON)
      target_include_directories(csilk_llhttp PUBLIC
        $<BUILD_INTERFACE:${llhttp_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
      )
      add_library(csilk::llhttp ALIAS csilk_llhttp)
    endif()
  endif()
else()
  message(STATUS "Found system llhttp: ${LLHTTP_LIB}")
  find_path(LLHTTP_INCLUDE_DIR llhttp.h)
  if(NOT TARGET csilk::llhttp)
    add_library(csilk::llhttp INTERFACE IMPORTED)
    if(LLHTTP_INCLUDE_DIR)
      set_target_properties(csilk::llhttp PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${LLHTTP_INCLUDE_DIR}")
    endif()
    set_target_properties(csilk::llhttp PROPERTIES INTERFACE_LINK_LIBRARIES "${LLHTTP_LIB}")
  endif()
endif()

# ── JSON Engine (yyjson) ──────────────────────────────────────────────────
FetchContent_Declare(
  yyjson
  GIT_REPOSITORY https://github.com/ibireme/yyjson.git
  GIT_TAG        0.12.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(yyjson)

# ── HTTP/2 Engine (nghttp2) ───────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBNGHTTP2 QUIET IMPORTED_TARGET libnghttp2)
endif()
if(TARGET PkgConfig::LIBNGHTTP2)
  if(NOT TARGET nghttp2)
    add_library(nghttp2 INTERFACE IMPORTED)
    target_link_libraries(nghttp2 INTERFACE PkgConfig::LIBNGHTTP2)
  endif()
  message(STATUS "Found nghttp2 via pkg-config")
else()
  find_library(NGHTTP2_LIB NAMES nghttp2 libnghttp2)
  find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
  if(NGHTTP2_LIB AND NGHTTP2_INCLUDE_DIR)
    message(STATUS "Found system nghttp2: ${NGHTTP2_LIB}")
    if(NOT TARGET nghttp2)
      add_library(nghttp2 INTERFACE IMPORTED)
      set_target_properties(nghttp2 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${NGHTTP2_LIB}"
      )
    endif()
  else()
    set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
    set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      nghttp2
      GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
      GIT_TAG        v1.61.0
      GIT_SHALLOW    TRUE
      GIT_SUBMODULES ""
    )
    FetchContent_MakeAvailable(nghttp2)
    if(nghttp2_SOURCE_DIR)
      include_directories(${nghttp2_SOURCE_DIR}/lib/includes)
      include_directories(${nghttp2_BINARY_DIR}/lib/includes)
      if(TARGET nghttp2_static AND NOT TARGET nghttp2)
        add_library(nghttp2 INTERFACE IMPORTED)
        target_link_libraries(nghttp2 INTERFACE nghttp2_static)
      endif()
    endif()
  endif()
endif()

# ── HTTP Client & SSL (CURL, OpenSSL, Threads) ───────────────────────────
set(CSILK_NEED_CURL OFF)
if(CSILK_ENABLE_AI OR CSILK_ENABLE_VECTOR OR CSILK_ENABLE_MCP)
  set(CSILK_NEED_CURL ON)
  find_package(CURL REQUIRED)
  message(STATUS "Found libcurl (required by AI/Vector/MCP)")
endif()
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenSSL 1.1.1 REQUIRED)
message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")

# ── Compression & Storage (ZLIB, SQLite3) ────────────────────────────────
find_package(ZLIB 1.2 REQUIRED)
message(STATUS "Found zlib: ${ZLIB_VERSION_STRING}")
if(CSILK_ENABLE_DB)
  find_package(SQLite3 3.20 REQUIRED)
  message(STATUS "Found sqlite3: ${SQLite3_VERSION}")
  if(TARGET SQLite::SQLite3 AND NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 INTERFACE IMPORTED)
    target_link_libraries(SQLite3::SQLite3 INTERFACE SQLite::SQLite3)
  endif()
  if(TARGET SQLite3::SQLite3 AND NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 INTERFACE IMPORTED)
    target_link_libraries(SQLite::SQLite3 INTERFACE SQLite3::SQLite3)
  endif()
endif()

# ── YAML Parser (libyaml) ─────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(YAML QUIET IMPORTED_TARGET yaml>=0.2.0)
endif()
if(TARGET PkgConfig::YAML)
  if(NOT TARGET csilk::yaml)
    add_library(csilk::yaml INTERFACE IMPORTED)
    target_link_libraries(csilk::yaml INTERFACE PkgConfig::YAML)
  endif()
  set(YAML_FOUND TRUE)
else()
  find_library(YAML_LIB NAMES yaml libyaml)
  find_path(YAML_INCLUDE_DIR yaml.h PATH_SUFFIXES yaml)
  if(YAML_LIB AND YAML_INCLUDE_DIR)
    set(YAML_FOUND TRUE)
    if(NOT TARGET csilk::yaml)
      add_library(csilk::yaml INTERFACE IMPORTED)
      set_target_properties(csilk::yaml PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${YAML_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${YAML_LIB}"
      )
    endif()
  endif()
endif()
if(NOT YAML_FOUND)
  message(FATAL_ERROR "libyaml >= 0.2.0 not found. Install libyaml-dev.\n"
    "  Debian/Ubuntu: sudo apt install libyaml-dev\n"
    "  macOS: brew install libyaml")
endif()
message(STATUS "Found libyaml target")

# ── Include Directories & Version Config ──────────────────────────────────
include_directories(include)
include_directories(src)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/include)
include_directories(${yyjson_SOURCE_DIR})
if(CSILK_NEED_CURL)
  include_directories(${CURL_INCLUDE_DIRS})
endif()

configure_file(
  include/csilk/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/csilk/version.h
  @ONLY
)
configure_file(
  python/csilk/_version.py.in
  ${CMAKE_CURRENT_SOURCE_DIR}/python/csilk/_version.py
  @ONLY
)

# ── Optional Database Client Drivers ──────────────────────────────────────
if(CSILK_ENABLE_DB)
# Find MySQL
find_library(MYSQL_LIB NAMES mysqlclient libmysqlclient)
if(MYSQL_LIB)
  message(STATUS "Found MySQL client library: ${MYSQL_LIB}")
  find_path(MYSQL_INCLUDE_DIR mysql.h PATHS /usr/include/mysql /usr/local/include/mysql)
  if(MYSQL_INCLUDE_DIR)
    include_directories(${MYSQL_INCLUDE_DIR})
    message(STATUS "Found MySQL include dir: ${MYSQL_INCLUDE_DIR}")
    if(NOT TARGET csilk::mysql)
      add_library(csilk::mysql INTERFACE IMPORTED)
      set_target_properties(csilk::mysql PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${MYSQL_LIB}"
      )
    endif()
    set(CSILK_HAS_MYSQL TRUE)
  endif()
else()
  message(STATUS "MySQL client library not found — mysql driver will not build")
endif()

# Find PostgreSQL
find_library(PQ_LIB NAMES pq libpq)
if(PQ_LIB)
  message(STATUS "Found PostgreSQL client library: ${PQ_LIB}")
  find_path(PQ_INCLUDE_DIR libpq-fe.h PATHS /usr/include/postgresql /usr/local/include/postgresql)
  if(PQ_INCLUDE_DIR)
    include_directories(${PQ_INCLUDE_DIR})
    message(STATUS "Found PostgreSQL include dir: ${PQ_INCLUDE_DIR}")
    if(NOT TARGET csilk::pq)
      add_library(csilk::pq INTERFACE IMPORTED)
      set_target_properties(csilk::pq PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PQ_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${PQ_LIB}"
      )
    endif()
    set(CSILK_HAS_POSTGRES TRUE)
  endif()
else()
  message(STATUS "PostgreSQL client library not found — postgres driver will not build")
endif()

# Find MongoDB
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
    if(NOT TARGET csilk::mongoc)
      add_library(csilk::mongoc INTERFACE IMPORTED)
      set_target_properties(csilk::mongoc PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${MONGOC_INCLUDE_DIR};${BSON_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${MONGOC_LIB};${BSON_LIB}"
      )
    endif()
    set(HAS_MONGODB TRUE)
    set(CSILK_HAS_MONGODB TRUE)
  endif()
else()
  message(STATUS "MongoDB client library not found — mongodb driver will not build")
endif()

# Find Redis (hiredis)
find_library(HIREDIS_LIB NAMES hiredis)
if(HIREDIS_LIB)
  message(STATUS "Found hiredis library: ${HIREDIS_LIB}")
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/include /usr/local/include)
  if(HIREDIS_INCLUDE_DIR)
    include_directories(${HIREDIS_INCLUDE_DIR})
    message(STATUS "Found hiredis include dir: ${HIREDIS_INCLUDE_DIR}")
    if(NOT TARGET csilk::hiredis)
      add_library(csilk::hiredis INTERFACE IMPORTED)
      set_target_properties(csilk::hiredis PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${HIREDIS_LIB}"
      )
    endif()
    set(CSILK_HAS_REDIS TRUE)
  endif()
else()
  message(STATUS "hiredis library not found — redis driver will not build")
endif()
endif() # CSILK_ENABLE_DB
