# cmake/options.cmake — project options, version, sanitizers, compile-time constants

# Version — single source of truth.
# FORCE: always re-derive from this file on every configure, so a stale
# CMakeCache.txt can never pin an old version after a release bump.
set(CSILK_VERSION_MAJOR 1 CACHE STRING "Major version" FORCE)
set(CSILK_VERSION_MINOR 3 CACHE STRING "Minor version" FORCE)
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
option(CSILK_DEPRECATE_CTX_TYPES "Enable compiler deprecation warnings for direct internal context struct access" ON)

# HTTP/3 (QUIC) — ngtcp2/nghttp3 FetchContent or system libraries
option(CSILK_ENABLE_HTTP3 "Enable ngtcp2/nghttp3 HTTP/3 QUIC transport" OFF)

# Linux seccomp/namespaces micro-sandbox for Agent Skills (v1.2)
option(CSILK_ENABLE_SECCOMP_SANDBOX "Enable Linux seccomp/namespaces isolation for Agent Skills" ON)
if(CSILK_ENABLE_SECCOMP_SANDBOX)
    add_compile_definitions(CSILK_HAS_SECCOMP_SANDBOX=1)
endif()

# ── Modular domain feature toggles ─────────────────────────────────────────
# By default, csilk builds only the lightweight HTTP Web Core (zero SQLite3 or
# libcurl hard dependencies). Enable individual domain modules or set
# -DCSILK_BUILD_FULL=ON to build the full feature set.
option(CSILK_BUILD_FULL "Enable all optional domain modules (DB, MQ, AI, Vector, MCP, Workflow, gRPC, Perm, Observability, CSV)" OFF)

option(CSILK_ENABLE_DB            "Build database drivers, migrations, and connection pools (requires SQLite3)" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_MQ            "Build message queue, PubSub, WAL, and Raft consensus" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_AI            "Build LLM client drivers, AI trace, and tool registry (requires libcurl)" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_VECTOR        "Build embedded HNSW and Milvus/Qdrant vector drivers (requires libcurl)" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_MCP           "Build Model Context Protocol server/client/stdio pool (requires AI + libcurl)" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_WORKFLOW      "Build AI workflow DAG engine, DSL, grid, and skill sandbox (requires AI + MQ)" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_GRPC          "Build gRPC wire codec and HTTP/JSON transcoding gateway" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_WASM          "Build WASM plugin module with WAMR runtime" OFF)
option(CSILK_ENABLE_PERM          "Build RBAC permission module" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_OBSERVABILITY "Build OTLP tracing, CPU flamegraph profiler, and Admin/APM dashboard" ${CSILK_BUILD_FULL})
option(CSILK_ENABLE_CSV           "Build RFC 4180 CSV parser/serializer utility" ${CSILK_BUILD_FULL})

# Enforce transitive feature dependencies
if(CSILK_ENABLE_MCP AND NOT CSILK_ENABLE_AI)
    set(CSILK_ENABLE_AI ON CACHE BOOL "Build LLM client drivers (enabled by CSILK_ENABLE_MCP)" FORCE)
endif()
if(CSILK_ENABLE_WORKFLOW)
    if(NOT CSILK_ENABLE_AI)
        set(CSILK_ENABLE_AI ON CACHE BOOL "Build LLM client drivers (enabled by CSILK_ENABLE_WORKFLOW)" FORCE)
    endif()
    if(NOT CSILK_ENABLE_MQ)
        set(CSILK_ENABLE_MQ ON CACHE BOOL "Build message queue (enabled by CSILK_ENABLE_WORKFLOW)" FORCE)
    endif()
endif()

# ── Configurable compile-time constants ────────────────────────────────────
set(CSILK_HEADER_BUCKETS 16  CACHE STRING "Header hash-table bucket count")
set(CSILK_MAX_PARAMS      20  CACHE STRING "Max URL path parameters per request")
set(CSILK_MAX_STORAGE    64  CACHE STRING "Max context key-value storage entries")
set(CSILK_DEFAULT_ARENA_SIZE 4096 CACHE STRING "Default arena chunk size in bytes")
set(CSILK_ARENA_TIER_COUNT 3   CACHE STRING "Number of arena chunk size tiers")
set(CSILK_MAX_TLS_CHUNKS          16  CACHE STRING "Max total cached arena chunks across all tiers")
set(CSILK_MAX_TLS_CHUNKS_PER_TIER 8   CACHE STRING "Max cached arena chunks per tier")
set(CSILK_TLS_DISPATCH_CAPACITY 128 CACHE STRING "TLS dispatch queue capacity (per worker)")
set(CSILK_WF_CLUSTER_MAX_BINDINGS 128 CACHE STRING "Max workflow-to-Raft cluster bindings")
