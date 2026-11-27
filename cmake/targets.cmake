# cmake/targets.cmake — modular csilk targets and compatibility aliases

# ── Common target setup ───────────────────────────────────────────────────
# Shared compile-time setup: flags, definitions, include dirs, PCH.
# Eliminates duplication between the target and object setup functions.
function(csilk_compile_setup TARGET)
  target_compile_options(${TARGET} PRIVATE
      "${CSILK_COMMON_FLAGS}"
      "${CSILK_RELEASE_FLAGS}"
      "${CSILK_DEBUG_FLAGS}"
      "${CSILK_AVX2_FLAGS}"
      "$<$<BOOL:${CSILK_HAS_AVX512}>:-DCSILK_HAS_AVX512>"
      "$<$<BOOL:${USE_COVERAGE}>:--coverage;-O0;-g>"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
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
  )
  target_compile_definitions(${TARGET} PUBLIC
      _GNU_SOURCE
      "$<$<BOOL:${CSILK_HAS_MYSQL}>:HAS_MYSQL>"
      "$<$<BOOL:${CSILK_HAS_POSTGRES}>:HAS_POSTGRES>"
      "$<$<BOOL:${HAS_MONGODB}>:HAS_MONGODB>"
      "$<$<BOOL:${CSILK_HAS_REDIS}>:HAS_REDIS>"
      "$<$<BOOL:${CSILK_USE_URING_ONLY}>:CSILK_USE_URING>"
      "$<$<BOOL:${CSILK_ENABLE_DB}>:CSILK_HAS_DB=1>"
      "$<$<BOOL:${CSILK_ENABLE_MQ}>:CSILK_HAS_MQ=1>"
      "$<$<BOOL:${CSILK_ENABLE_AI}>:CSILK_HAS_AI=1>"
      "$<$<BOOL:${CSILK_ENABLE_VECTOR}>:CSILK_HAS_VECTOR=1>"
      "$<$<BOOL:${CSILK_ENABLE_MCP}>:CSILK_HAS_MCP=1>"
      "$<$<BOOL:${CSILK_ENABLE_WORKFLOW}>:CSILK_HAS_WORKFLOW=1>"
      "$<$<BOOL:${CSILK_ENABLE_GRPC}>:CSILK_HAS_GRPC=1>"
      "$<$<BOOL:${CSILK_ENABLE_WASM}>:CSILK_HAS_WASM=1>"
      "$<$<BOOL:${CSILK_ENABLE_PERM}>:CSILK_HAS_PERM=1>"
      "$<$<BOOL:${CSILK_ENABLE_HTTP3}>:CSILK_HAS_HTTP3=1>"
      "$<$<BOOL:${CSILK_ENABLE_OBSERVABILITY}>:CSILK_HAS_OBSERVABILITY=1>"
      "$<$<BOOL:${CSILK_ENABLE_CSV}>:CSILK_HAS_CSV=1>"
      "CSILK_HEADER_BUCKETS=${CSILK_HEADER_BUCKETS}"
      "CSILK_MAX_PARAMS=${CSILK_MAX_PARAMS}"
      "CSILK_MAX_STORAGE=${CSILK_MAX_STORAGE}"
      "CSILK_DEFAULT_ARENA_SIZE=${CSILK_DEFAULT_ARENA_SIZE}"
      "CSILK_ARENA_TIER_COUNT=${CSILK_ARENA_TIER_COUNT}"
      "CSILK_MAX_TLS_CHUNKS=${CSILK_MAX_TLS_CHUNKS}"
      "CSILK_MAX_TLS_CHUNKS_PER_TIER=${CSILK_MAX_TLS_CHUNKS_PER_TIER}"
      "CSILK_TLS_DISPATCH_CAPACITY=${CSILK_TLS_DISPATCH_CAPACITY}"
      "CSILK_WF_CLUSTER_MAX_BINDINGS=${CSILK_WF_CLUSTER_MAX_BINDINGS}"
  )
  target_include_directories(${TARGET} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
      $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}>
      $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>
  )
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(${TARGET} PRIVATE
        <stdio.h> <stdlib.h> <string.h> <stdint.h> <time.h> <errno.h>
    )
  endif()
endfunction()

function(csilk_target_setup TARGET VISIBILITY TYPE)

  csilk_compile_setup(${TARGET})

  target_link_options(${TARGET} PUBLIC
      "$<$<BOOL:${USE_COVERAGE}>:--coverage>"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
  )

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(${TARGET} PRIVATE
        "$<$<CONFIG:Release>:-Wl,--gc-sections>"
    )
  endif()

  if(TYPE STREQUAL "SHARED")
    if("${TARGET}" STREQUAL "csilk_shared")
      if((CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang") AND NOT APPLE)
        target_link_options(${TARGET} PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/libcsilk.map")
      endif()
    endif()
    if(APPLE)
      set_target_properties(${TARGET} PROPERTIES
          INSTALL_NAME_DIR         "@loader_path"
          BUILD_RPATH_USE_ORIGIN   TRUE
          INSTALL_RPATH            "@loader_path"
      )
      target_link_options(${TARGET} PRIVATE "-undefined" "dynamic_lookup")
      if(CMAKE_LIBRARY_PATH AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.13)
        target_link_directories(${TARGET} PRIVATE ${CMAKE_LIBRARY_PATH})
      endif()
    else()
      set_target_properties(${TARGET} PROPERTIES
          BUILD_RPATH_USE_ORIGIN   TRUE
          INSTALL_RPATH            "$ORIGIN"
      )
    endif()
  endif()

  set_target_properties(${TARGET} PROPERTIES
      PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
      VERSION "${CSILK_VERSION}"
      SOVERSION "2"
      INTERFACE_COMPILE_FEATURES c_std_23
      POSITION_INDEPENDENT_CODE ON
  )

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
    if(ipo_supported AND TYPE STREQUAL "SHARED" AND NOT USE_ASAN AND NOT USE_TSAN AND NOT USE_COVERAGE)
      set_target_properties(${TARGET} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
  endif()

  target_include_directories(${TARGET} PUBLIC
      $<INSTALL_INTERFACE:include>
  )
endfunction()

# Object libraries are the single compilation owner for each module. Static
# and shared wrappers reuse these objects, so CSILK_BUILD_SHARED does not
# compile the same source files twice.
function(csilk_object_setup TARGET)

  csilk_compile_setup(${TARGET})

  if(CMAKE_INCLUDE_PATH)
    target_include_directories(${TARGET} PRIVATE ${CMAKE_INCLUDE_PATH})
  endif()
  # Give every module OBJECT library PRIVATE visibility to all discovered
  # third-party targets so non-default include roots are always present.
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.12)
    foreach(_ext_dep
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        SQLite3::SQLite3
        CURL::libcurl
        nghttp2
        csilk::llhttp
        csilk::yaml
        csilk::libuv
        uring)
      if(TARGET ${_ext_dep})
        target_link_libraries(${TARGET} PRIVATE ${_ext_dep})
      endif()
    endforeach()
  endif()
  set_target_properties(${TARGET} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      OPTIMIZE_DEPENDENCIES ON)
endfunction()

set(CSILK_AVX2_FLAGS "")
if(CSILK_HAS_AVX2)
  set(CSILK_AVX2_FLAGS "-mavx2;-D__AVX2__")
endif()

function(csilk_link_module TARGET)
  target_link_libraries(${TARGET} PUBLIC ${ARGN})

  # The OBJECT library is the real compilation owner, so it must see the
  # dependencies' usage requirements (include dirs of libuv/OpenSSL/yaml/...).
  # Linking only the static/shared wrappers left third-party headers reachable
  # solely through compiler default paths: fine on Linux (/usr/include), fatal
  # on macOS where Homebrew's /opt/homebrew/include is not searched.
  if(TARGET ${TARGET}_objects AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.12)
    target_link_libraries(${TARGET}_objects PRIVATE ${ARGN})
  endif()

  if(TARGET ${TARGET}_shared)
    set(_shared_dependencies)
    foreach(_dependency IN LISTS ARGN)
      if(TARGET ${_dependency}_shared)
        list(APPEND _shared_dependencies ${_dependency}_shared)
      else()
        list(APPEND _shared_dependencies ${_dependency})
      endif()
    endforeach()
    target_link_libraries(${TARGET}_shared PUBLIC ${_shared_dependencies})
  endif()
endfunction()

function(csilk_add_module NAME SOURCE_VAR OUTPUT_NAME)
  # Each module owns one OBJECT library. Static module archives, module shared
  # libraries, and the monolithic shared ABI all reuse these objects, so a
  # shared build never recompiles a module source list.
  set(_objects "${NAME}_objects")
  add_library(${_objects} OBJECT ${${SOURCE_VAR}})
  csilk_object_setup(${_objects})

  add_library(${NAME} STATIC $<TARGET_OBJECTS:${_objects}>)
  set_target_properties(${NAME} PROPERTIES OUTPUT_NAME "${OUTPUT_NAME}")
  csilk_target_setup(${NAME} PUBLIC STATIC)

  string(REPLACE "csilk_" "" _short_name "${NAME}")
  add_library(csilk::${_short_name} ALIAS ${NAME})

  if(CSILK_BUILD_SHARED)
    add_library(${NAME}_shared SHARED $<TARGET_OBJECTS:${_objects}>)
    set_target_properties(${NAME}_shared PROPERTIES OUTPUT_NAME "${OUTPUT_NAME}")
    csilk_target_setup(${NAME}_shared PUBLIC SHARED)
    add_library(csilk::${_short_name}_shared ALIAS ${NAME}_shared)
  endif()

  set(CSILK_MODULE_OBJECTS
      ${CSILK_MODULE_OBJECTS}
      $<TARGET_OBJECTS:${_objects}>
      PARENT_SCOPE)
endfunction()

# ── Core Web & HTTP Modules (4 Layered Targets, Always Built) ────────────
set(CSILK_BASE_LAYER_SOURCES
    ${CSILK_BASE_SOURCES}
    ${CSILK_JSON_SOURCES}
    ${CSILK_CRYPTO_SOURCES}
    ${CSILK_REFLECTION_SOURCES}
)
set(CSILK_RUNTIME_LAYER_SOURCES
    ${CSILK_RUNTIME_SOURCES}
    ${CSILK_TLS_SOURCES}
)
set(CSILK_HTTP_LAYER_SOURCES
    ${CSILK_HTTP_SOURCES}
    ${CSILK_HTTP2_SOURCES}
    ${CSILK_PROTOCOLS_SOURCES}
)
set(CSILK_APP_LAYER_SOURCES
    ${CSILK_APP_SOURCES}
    ${CSILK_MIDDLEWARE_SOURCES}
)

csilk_add_module(csilk_base CSILK_BASE_LAYER_SOURCES "csilk-base")
csilk_add_module(csilk_runtime CSILK_RUNTIME_LAYER_SOURCES "csilk-runtime")
csilk_add_module(csilk_http CSILK_HTTP_LAYER_SOURCES "csilk-http")
csilk_add_module(csilk_app CSILK_APP_LAYER_SOURCES "csilk-app")

# ── Optional Feature Modules (Controlled via CSILK_ENABLE_* / CSILK_BUILD_FULL) ──
if(CSILK_ENABLE_HTTP3)
  csilk_add_module(csilk_h3 CSILK_HTTP3_SOURCES "csilk-h3")
endif()
if(CSILK_ENABLE_MCP)
  csilk_add_module(csilk_mcp CSILK_MCP_SOURCES "csilk-mcp")
endif()
if(CSILK_ENABLE_GRPC)
  csilk_add_module(csilk_grpc CSILK_GRPC_SOURCES "csilk-grpc")
endif()
if(CSILK_ENABLE_WASM)
  csilk_add_module(csilk_wasm CSILK_WASM_SOURCES "csilk-wasm")
endif()
if(CSILK_ENABLE_MQ)
  csilk_add_module(csilk_mq CSILK_MQ_SOURCES "csilk-mq")
endif()
if(CSILK_ENABLE_DB)
  csilk_add_module(csilk_db CSILK_DB_SOURCES "csilk-db")
endif()
if(CSILK_ENABLE_VECTOR)
  csilk_add_module(csilk_vector CSILK_VECTOR_SOURCES "csilk-vector")
endif()
if(CSILK_ENABLE_AI)
  csilk_add_module(csilk_ai CSILK_AI_SOURCES "csilk-ai")
endif()
if(CSILK_ENABLE_PERM)
  csilk_add_module(csilk_permission CSILK_PERMISSION_SOURCES "csilk-permission")
endif()
if(CSILK_ENABLE_WORKFLOW)
  csilk_add_module(csilk_workflow CSILK_WORKFLOW_SOURCES "csilk-workflow")
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  csilk_add_module(csilk_observability CSILK_OBSERVABILITY_SOURCES "csilk-observability")
endif()

# ── Compatibility targets ────────────────────────────────────────────────
# Keep fine-grained csilk_* / csilk::* targets as INTERFACE wrappers backed
# by the 4 layered Web Core libraries so external consumers keep working.
function(csilk_add_compat_alias NAME LAYER_TARGET)
  add_library(${NAME} INTERFACE)
  target_link_libraries(${NAME} INTERFACE ${LAYER_TARGET})
  string(REPLACE "csilk_" "" _short_name "${NAME}")
  add_library(csilk::${_short_name} ALIAS ${NAME})

  if(CSILK_BUILD_SHARED)
    add_library(${NAME}_shared INTERFACE)
    target_link_libraries(${NAME}_shared INTERFACE ${LAYER_TARGET}_shared)
    add_library(csilk::${_short_name}_shared ALIAS ${NAME}_shared)
  endif()
endfunction()

csilk_add_compat_alias(csilk_json csilk_base)
csilk_add_compat_alias(csilk_crypto csilk_base)
csilk_add_compat_alias(csilk_reflection csilk_base)
csilk_add_compat_alias(csilk_core csilk_runtime)
csilk_add_compat_alias(csilk_tls csilk_runtime)
csilk_add_compat_alias(csilk_http2 csilk_http)
csilk_add_compat_alias(csilk_protocols csilk_http)
csilk_add_compat_alias(csilk_middleware csilk_app)

# ── Static module dependencies ────────────────────────────────────────────
if(CSILK_USE_URING)
  csilk_link_module(csilk_base yyjson OpenSSL::Crypto Threads::Threads uring)
else()
  csilk_link_module(csilk_base yyjson OpenSSL::Crypto Threads::Threads csilk::libuv)
endif()
if(NOT APPLE AND NOT WIN32)
  target_link_libraries(csilk_base PRIVATE m)
  if(TARGET csilk_base_shared)
    target_link_libraries(csilk_base_shared PRIVATE m)
  endif()
endif()

csilk_link_module(csilk_runtime
    csilk_base
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(TARGET csilk::yaml)
  csilk_link_module(csilk_runtime csilk::yaml)
endif()
if(CSILK_USE_URING)
  csilk_link_module(csilk_runtime uring)
else()
  csilk_link_module(csilk_runtime csilk::libuv)
  target_compile_definitions(csilk_runtime PUBLIC CSILK_USE_LIBUV)
  if(TARGET csilk_runtime_shared)
    target_compile_definitions(csilk_runtime_shared PUBLIC CSILK_USE_LIBUV)
  endif()
endif()
if(NOT APPLE AND NOT WIN32)
  target_link_libraries(csilk_runtime PRIVATE m)
  if(TARGET csilk_runtime_shared)
    target_link_libraries(csilk_runtime_shared PRIVATE m)
  endif()
endif()

# csilk_io_* wrappers in <csilk/core/sys_io.h> and <csilk/core/sync.h> are
# static inline in the libuv backend, so every module whose objects reference
# them must link the I/O backend directly.
csilk_link_module(csilk_http
    csilk_runtime
    csilk_base
    nghttp2
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(TARGET csilk::llhttp)
  csilk_link_module(csilk_http csilk::llhttp)
endif()

csilk_link_module(csilk_app
    csilk_http
    csilk_runtime
    csilk_base
    ZLIB::ZLIB
    OpenSSL::Crypto
    Threads::Threads
)

if(CSILK_ENABLE_HTTP3)
  csilk_link_module(csilk_h3
      csilk_runtime
      csilk_http
      csilk::ngtcp2
      csilk::nghttp3
      OpenSSL::SSL
      OpenSSL::Crypto
  )
endif()
if(CSILK_ENABLE_WASM)
  csilk_link_module(csilk_wasm csilk_app csilk_runtime)
  if(TARGET csilk::vmlib)
    csilk_link_module(csilk_wasm csilk::vmlib)
  elseif(TARGET vmlib)
    csilk_link_module(csilk_wasm vmlib)
  endif()
endif()
if(CSILK_ENABLE_MQ)
  if(TARGET csilk::libuv)
    csilk_link_module(csilk_mq csilk_runtime csilk::libuv)
  else()
    csilk_link_module(csilk_mq csilk_runtime)
  endif()
endif()
if(CSILK_ENABLE_DB)
  csilk_link_module(csilk_db csilk_runtime csilk_base SQLite3::SQLite3)
  foreach(DB_TARGET csilk::mysql csilk::pq csilk::hiredis csilk::mongoc)
    if(TARGET ${DB_TARGET})
      csilk_link_module(csilk_db ${DB_TARGET})
    endif()
  endforeach()
endif()
if(CSILK_ENABLE_VECTOR)
  if(NOT APPLE AND NOT WIN32)
    csilk_link_module(csilk_vector csilk_runtime csilk_base CURL::libcurl m)
  else()
    csilk_link_module(csilk_vector csilk_runtime csilk_base CURL::libcurl)
  endif()
endif()
if(CSILK_ENABLE_AI)
  if(NOT APPLE AND NOT WIN32)
    csilk_link_module(csilk_ai csilk_runtime csilk_base CURL::libcurl m ${CMAKE_DL_LIBS})
  else()
    csilk_link_module(csilk_ai csilk_runtime csilk_base CURL::libcurl)
  endif()
endif()
if(CSILK_ENABLE_PERM)
  csilk_link_module(csilk_permission csilk_runtime csilk_base)
endif()
if(CSILK_ENABLE_WORKFLOW)
  if(NOT APPLE AND NOT WIN32)
    csilk_link_module(csilk_workflow csilk_runtime csilk_base csilk_ai csilk_mq m csilk::yaml)
  else()
    csilk_link_module(csilk_workflow csilk_runtime csilk_base csilk_ai csilk_mq csilk::yaml)
  endif()
  if(CSILK_ENABLE_WASM)
    csilk_link_module(csilk_workflow csilk_wasm)
  endif()
endif()
if(CSILK_ENABLE_MCP)
  csilk_link_module(csilk_mcp csilk_http csilk_runtime csilk_base csilk_ai CURL::libcurl)
endif()
if(CSILK_ENABLE_GRPC)
  csilk_link_module(csilk_grpc csilk_app csilk_http csilk_runtime csilk_base)
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  csilk_link_module(csilk_observability csilk_app csilk_http csilk_runtime csilk_base Threads::Threads)
endif()

# ── Umbrella targets ──────────────────────────────────────────────────────
add_library(csilk INTERFACE)

set(CSILK_ENABLED_MODULES
    csilk_app
    csilk_http
    csilk_runtime
    csilk_base
    yyjson
)
if(CSILK_ENABLE_WORKFLOW)
  list(APPEND CSILK_ENABLED_MODULES csilk_workflow)
endif()
if(CSILK_ENABLE_MCP)
  list(APPEND CSILK_ENABLED_MODULES csilk_mcp)
endif()
if(CSILK_ENABLE_GRPC)
  list(APPEND CSILK_ENABLED_MODULES csilk_grpc)
endif()
if(CSILK_ENABLE_HTTP3)
  list(APPEND CSILK_ENABLED_MODULES csilk_h3)
endif()
if(CSILK_ENABLE_PERM)
  list(APPEND CSILK_ENABLED_MODULES csilk_permission)
endif()
if(CSILK_ENABLE_AI)
  list(APPEND CSILK_ENABLED_MODULES csilk_ai)
endif()
if(CSILK_ENABLE_DB)
  list(APPEND CSILK_ENABLED_MODULES csilk_db)
endif()
if(CSILK_ENABLE_VECTOR)
  list(APPEND CSILK_ENABLED_MODULES csilk_vector)
endif()
if(CSILK_ENABLE_MQ)
  list(APPEND CSILK_ENABLED_MODULES csilk_mq)
endif()
if(CSILK_ENABLE_WASM)
  list(APPEND CSILK_ENABLED_MODULES csilk_wasm)
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  list(APPEND CSILK_ENABLED_MODULES csilk_observability)
endif()
if(CSILK_USE_URING AND TARGET uring)
  list(APPEND CSILK_ENABLED_MODULES uring)
endif()

# The split static archives within Web Core have intentional mutual references
# (csilk_client_t spans http/tls/http2; server lifecycle uses protocols/swagger).
# A linker group preserves monolithic link behavior for umbrella consumers.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_libraries(csilk INTERFACE
      "$<LINK_GROUP:RESCAN;${CSILK_ENABLED_MODULES}>"
  )
else()
  target_link_libraries(csilk INTERFACE ${CSILK_ENABLED_MODULES})
endif()
# Transitive INTERFACE libraries referenced only through a LINK_GROUP genex
# are dropped by CMake releases before 4.x. Link external INTERFACE deps explicitly.
if(CSILK_USE_URING)
  if(TARGET uring)
    target_link_libraries(csilk INTERFACE uring)
  endif()
else()
  if(TARGET csilk::libuv)
    target_link_libraries(csilk INTERFACE csilk::libuv)
  endif()
endif()
if(CSILK_ENABLE_HTTP3)
  if(TARGET csilk::ngtcp2)
    target_link_libraries(csilk INTERFACE csilk::ngtcp2)
  endif()
  if(TARGET csilk::nghttp3)
    target_link_libraries(csilk INTERFACE csilk::nghttp3)
  endif()
endif()
if(CSILK_ENABLE_WASM)
  if(TARGET csilk::vmlib)
    target_link_libraries(csilk INTERFACE csilk::vmlib)
  elseif(TARGET vmlib)
    target_link_libraries(csilk INTERFACE vmlib)
  endif()
endif()
target_include_directories(csilk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
add_library(csilk::csilk ALIAS csilk)

if(CSILK_BUILD_SHARED)
  # The monolithic shared library remains the compatibility ABI artifact.
  # It reuses the module object closure of enabled modules.
  add_library(csilk_shared SHARED ${CSILK_MODULE_OBJECTS})
  set_target_properties(csilk_shared PROPERTIES OUTPUT_NAME "csilk")
  csilk_target_setup(csilk_shared PUBLIC SHARED)
  target_link_libraries(csilk_shared PRIVATE
      Threads::Threads
      OpenSSL::SSL
      OpenSSL::Crypto
      ZLIB::ZLIB
      yyjson
      nghttp2
  )
  if(CSILK_ENABLE_DB)
    target_link_libraries(csilk_shared PRIVATE SQLite3::SQLite3)
    foreach(_db_target csilk::mysql csilk::pq csilk::hiredis csilk::mongoc)
      if(TARGET ${_db_target})
        target_link_libraries(csilk_shared PRIVATE ${_db_target})
      endif()
    endforeach()
  endif()
  if(CSILK_NEED_CURL)
    target_link_libraries(csilk_shared PRIVATE CURL::libcurl)
  endif()
  if(CSILK_ENABLE_WASM AND TARGET vmlib)
    target_link_libraries(csilk_shared PRIVATE vmlib)
  endif()
  if(CSILK_ENABLE_HTTP3)
    target_link_libraries(csilk_shared PRIVATE csilk::ngtcp2 csilk::nghttp3)
  endif()
  if(TARGET csilk::yaml)
    target_link_libraries(csilk_shared PRIVATE csilk::yaml)
  endif()
  if(TARGET csilk::llhttp)
    target_link_libraries(csilk_shared PRIVATE csilk::llhttp)
  endif()
  if(CSILK_USE_URING)
    target_link_libraries(csilk_shared PRIVATE uring)
  else()
    target_link_libraries(csilk_shared PRIVATE csilk::libuv)
  endif()
  if(NOT APPLE AND NOT WIN32)
    target_link_libraries(csilk_shared PRIVATE m ${CMAKE_DL_LIBS})
  endif()
  add_library(csilk::shared ALIAS csilk_shared)
  message(STATUS "Shared library builds enabled: libcsilk*.so")
endif()
