# cmake/options.cmake — project options, version, sanitizers, compile-time constants

# Version — single source of truth.
# FORCE: always re-derive from this file on every configure, so a stale
# CMakeCache.txt can never pin an old version after a release bump.
set(CSILK_VERSION_MAJOR 1 CACHE STRING "Major version" FORCE)
set(CSILK_VERSION_MINOR 1 CACHE STRING "Minor version" FORCE)
set(CSILK_VERSION_PATCH 1 CACHE STRING "Patch version" FORCE)
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
option(CSILK_DEPRECATE_CTX_TYPES "Enable compiler deprecation warnings for direct internal context struct access" ON)
option(CSILK_ENABLE_WAMR "Integrate WAMR (WebAssembly Micro Runtime) for real WASM plugin execution" OFF)
if(CSILK_ENABLE_WAMR)
    set(CSILK_ENABLE_EXPERIMENTAL_STUBS OFF CACHE BOOL "" FORCE)
    message(STATUS "WAMR enabled — CSILK_ENABLE_EXPERIMENTAL_STUBS forced OFF")
endif()
option(CSILK_ENABLE_EXPERIMENTAL_STUBS
       "Enable experimental/simulated stub modules (AF_XDP/DPDK memory-ring simulations, WASM stub VM, HTTP/3 UDP listener placeholder)"
       OFF)
if(CSILK_ENABLE_EXPERIMENTAL_STUBS)
    message(STATUS "Experimental/simulated stubs ENABLED (AF_XDP/DPDK in-memory rings, WASM fuel-only stub VM, HTTP/3 UDP listener skeleton)")
else()
    message(STATUS "Experimental/simulated stubs DISABLED (fail-closed stubs for AF_XDP, DPDK, WASM VM, HTTP/3 listener)")
endif()

# HTTP/3 (QUIC) groundwork — ngtcp2/nghttp3 FetchContent
option(CSILK_ENABLE_HTTP3 "Enable ngtcp2/nghttp3 FetchContent for HTTP/3 QUIC groundwork (v1.1 architectural only)" OFF)

# Linux seccomp/namespaces micro-sandbox for Agent Skills (v1.2)
option(CSILK_ENABLE_SECCOMP_SANDBOX "Enable Linux seccomp/namespaces isolation for Agent Skills" ON)
if(CSILK_ENABLE_SECCOMP_SANDBOX)
    add_compile_definitions(CSILK_HAS_SECCOMP_SANDBOX=1)
endif()

# ── Configurable compile-time constants ────────────────────────────────────
set(CSILK_HEADER_BUCKETS 16  CACHE STRING "Header hash-table bucket count")
set(CSILK_MAX_PARAMS      20  CACHE STRING "Max URL path parameters per request")
set(CSILK_MAX_STORAGE    64  CACHE STRING "Max context key-value storage entries")
set(CSILK_DEFAULT_ARENA_SIZE 4096 CACHE STRING "Default arena chunk size in bytes")
set(CSILK_ARENA_TIER_COUNT 3   CACHE STRING "Number of arena chunk size tiers")
set(CSILK_MAX_TLS_CHUNKS  8   CACHE STRING "Max cached arena chunks per tier")
set(CSILK_TLS_DISPATCH_CAPACITY 128 CACHE STRING "TLS dispatch queue capacity (per worker)")
set(CSILK_WF_CLUSTER_MAX_BINDINGS 128 CACHE STRING "Max workflow-to-Raft cluster bindings")

# ── Plugin build options ─────────────────────────────────────────────────
# Each optional module can be built as a standalone shared-library plugin
# instead of being linked into the monolithic libcsilk.
option(CSILK_BUILD_PLUGIN_AI     "Build csilk-ai plugin (LLM drivers)"     OFF)
option(CSILK_BUILD_PLUGIN_MQ     "Build csilk-mq plugin (message queue)"   OFF)
option(CSILK_BUILD_PLUGIN_DB     "Build csilk-db plugin (database drivers)" OFF)
option(CSILK_BUILD_PLUGIN_VECTOR "Build csilk-vector plugin (HNSW index)"  OFF)
option(CSILK_BUILD_PLUGIN_WORKFLOW "Build csilk-workflow plugin (AI agent graph)" OFF)
option(CSILK_BUILD_PLUGIN_PROTOCOL "Build csilk-protocol plugin (WS/SSE/MCP)" OFF)
option(CSILK_BUILD_PLUGIN_WASM   "Build csilk-wasm plugin (WASM sandbox)"  OFF)
option(CSILK_BUILD_PLUGIN_BYPASS "Build csilk-bypass plugin (AF_XDP/DPDK)" OFF)
option(CSILK_PLUGIN_DIR          "Install directory for plugin .so files"  "${CMAKE_INSTALL_LIBDIR}/csilk/plugins")
