# cmake/options.cmake — project options, version, sanitizers, compile-time constants

# Version — single source of truth.
# FORCE: always re-derive from this file on every configure, so a stale
# CMakeCache.txt can never pin an old version after a release bump.
set(CSILK_VERSION_MAJOR 0 CACHE STRING "Major version" FORCE)
set(CSILK_VERSION_MINOR 4 CACHE STRING "Minor version" FORCE)
set(CSILK_VERSION_PATCH 0 CACHE STRING "Patch version" FORCE)
set(CSILK_VERSION "${CSILK_VERSION_MAJOR}.${CSILK_VERSION_MINOR}.${CSILK_VERSION_PATCH}" CACHE STRING "Full version" FORCE)

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

option(ENABLE_OOM_TEST "Enable OOM (Out Of Memory) testing" OFF)
option(DEBUG_ARENA "Enable arena redzone guards for buffer overflow detection" OFF)
option(CSILK_BUILD_SHARED "Build shared library (in addition to static)" ON)

# ── Configurable compile-time constants ──────────────────────────────────
set(CSILK_HEADER_BUCKETS 64  CACHE STRING "Header hash-table bucket count")
set(CSILK_MAX_PARAMS      20  CACHE STRING "Max URL path parameters per request")
set(CSILK_MAX_STORAGE    64  CACHE STRING "Max context key-value storage entries")
set(CSILK_DEFAULT_ARENA_SIZE 4096 CACHE STRING "Default arena chunk size in bytes")
set(CSILK_ARENA_TIER_COUNT 3   CACHE STRING "Number of arena chunk size tiers")
set(CSILK_MAX_TLS_CHUNKS  8   CACHE STRING "Max cached arena chunks per tier")
