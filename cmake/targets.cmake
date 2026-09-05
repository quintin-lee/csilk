# cmake/targets.cmake — modular csilk targets and compatibility aliases

# ── Common target setup ───────────────────────────────────────────────────
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
  )
  target_compile_definitions(${TARGET} PUBLIC
      _GNU_SOURCE
      "$<$<BOOL:${CSILK_USE_URING_ONLY}>:CSILK_USE_URING>"
      "CSILK_HEADER_BUCKETS=${CSILK_HEADER_BUCKETS}"
      "CSILK_MAX_PARAMS=${CSILK_MAX_PARAMS}"
      "CSILK_MAX_STORAGE=${CSILK_MAX_STORAGE}"
      "CSILK_DEFAULT_ARENA_SIZE=${CSILK_DEFAULT_ARENA_SIZE}"
      "CSILK_ARENA_TIER_COUNT=${CSILK_ARENA_TIER_COUNT}"
      "CSILK_MAX_TLS_CHUNKS=${CSILK_MAX_TLS_CHUNKS}"
  )
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
      SOVERSION "${CSILK_VERSION_MAJOR}"
      INTERFACE_COMPILE_FEATURES c_std_23
      POSITION_INDEPENDENT_CODE ON
  )

  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
    if(ipo_supported AND TYPE STREQUAL "SHARED")
      set_target_properties(${TARGET} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
  endif()

  target_include_directories(${TARGET} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
      $<INSTALL_INTERFACE:include>
      $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}>
      $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>
  )

  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(${TARGET} PRIVATE
        <stdio.h> <stdlib.h> <string.h> <stdint.h> <time.h> <errno.h>
    )
  endif()
endfunction()

# Object libraries are the single compilation owner for each module. Static
# and shared wrappers reuse these objects, so CSILK_BUILD_SHARED does not
# compile the same source files twice.
function(csilk_object_setup TARGET)
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
  )
  target_compile_definitions(${TARGET} PUBLIC
      _GNU_SOURCE
      "$<$<BOOL:${CSILK_USE_URING_ONLY}>:CSILK_USE_URING>"
      "CSILK_HEADER_BUCKETS=${CSILK_HEADER_BUCKETS}"
      "CSILK_MAX_PARAMS=${CSILK_MAX_PARAMS}"
      "CSILK_MAX_STORAGE=${CSILK_MAX_STORAGE}"
      "CSILK_DEFAULT_ARENA_SIZE=${CSILK_DEFAULT_ARENA_SIZE}"
      "CSILK_ARENA_TIER_COUNT=${CSILK_ARENA_TIER_COUNT}"
      "CSILK_MAX_TLS_CHUNKS=${CSILK_MAX_TLS_CHUNKS}"
  )
  target_include_directories(${TARGET} PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
      $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}>
      $<BUILD_INTERFACE:${yyjson_SOURCE_DIR}/src>
  )
  set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(${TARGET} PRIVATE
        <stdio.h> <stdlib.h> <string.h> <stdint.h> <time.h> <errno.h>
    )
  endif()
endfunction()

set(CSILK_AVX2_FLAGS "")
if(CSILK_HAS_AVX2)
  set(CSILK_AVX2_FLAGS "-mavx2;-D__AVX2__")
endif()

function(csilk_link_module TARGET)
  target_link_libraries(${TARGET} PUBLIC ${ARGN})

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

# ── Module implementations ──────────────────────────────────────────────
csilk_add_module(csilk_base CSILK_BASE_SOURCES "csilk-base")
csilk_add_module(csilk_json CSILK_JSON_SOURCES "csilk-json")
csilk_add_module(csilk_crypto CSILK_CRYPTO_SOURCES "csilk-crypto")
csilk_add_module(csilk_runtime CSILK_RUNTIME_SOURCES "csilk-runtime")
csilk_add_module(csilk_wasm CSILK_WASM_SOURCES "csilk-wasm")
csilk_add_module(csilk_bypass CSILK_BYPASS_SOURCES "csilk-bypass")
csilk_add_module(csilk_tls CSILK_TLS_SOURCES "csilk-tls")
csilk_add_module(csilk_mq CSILK_MQ_SOURCES "csilk-mq")
csilk_add_module(csilk_http2 CSILK_HTTP2_SOURCES "csilk-http2")
csilk_add_module(csilk_http CSILK_HTTP_SOURCES "csilk-http")
csilk_add_module(csilk_db CSILK_DB_SOURCES "csilk-db")
csilk_add_module(csilk_vector CSILK_VECTOR_SOURCES "csilk-vector")
csilk_add_module(csilk_ai CSILK_AI_SOURCES "csilk-ai")
csilk_add_module(csilk_reflection CSILK_REFLECTION_SOURCES "csilk-reflection")
csilk_add_module(csilk_permission CSILK_PERMISSION_SOURCES "csilk-permission")
csilk_add_module(csilk_workflow CSILK_WORKFLOW_SOURCES "csilk-workflow")
csilk_add_module(csilk_protocols CSILK_PROTOCOLS_SOURCES "csilk-protocols")
csilk_add_module(csilk_middleware CSILK_MIDDLEWARE_SOURCES "csilk-middleware")
csilk_add_module(csilk_app CSILK_APP_SOURCES "csilk-app")

# ── Compatibility target ─────────────────────────────────────────────────
# Keep csilk::core as a stable consumer entry point while runtime owns the
# implementation sources.
add_library(csilk_core INTERFACE)
target_link_libraries(csilk_core INTERFACE csilk_runtime)
add_library(csilk::core ALIAS csilk_core)

if(CSILK_BUILD_SHARED)
  add_library(csilk_core_shared INTERFACE)
  target_link_libraries(csilk_core_shared INTERFACE csilk_runtime_shared)
  add_library(csilk::core_shared ALIAS csilk_core_shared)
endif()

# ── Static module dependencies ────────────────────────────────────────────
target_link_libraries(csilk_base PUBLIC Threads::Threads)
if(NOT APPLE AND NOT WIN32)
  target_link_libraries(csilk_base PRIVATE m)
  if(TARGET csilk_base_shared)
    target_link_libraries(csilk_base_shared PRIVATE m)
  endif()
endif()

csilk_link_module(csilk_json yyjson)
csilk_link_module(csilk_crypto csilk_base OpenSSL::Crypto Threads::Threads)

csilk_link_module(csilk_runtime
    csilk_base
    csilk_crypto
    csilk_json
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

csilk_link_module(csilk_wasm csilk_runtime)
csilk_link_module(csilk_bypass csilk_runtime)
csilk_link_module(csilk_tls csilk_runtime OpenSSL::SSL OpenSSL::Crypto)
csilk_link_module(csilk_mq csilk_runtime)
csilk_link_module(csilk_http2 csilk_runtime csilk_tls nghttp2)
csilk_link_module(csilk_http
    csilk_runtime
    csilk_json
    csilk_tls
    csilk_http2
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
)
if(TARGET csilk::llhttp)
  csilk_link_module(csilk_http csilk::llhttp)
endif()

csilk_link_module(csilk_db csilk_runtime csilk_json SQLite3::SQLite3 CURL::libcurl)
foreach(DB_TARGET csilk::mysql csilk::pq csilk::hiredis csilk::mongoc)
  if(TARGET ${DB_TARGET})
    csilk_link_module(csilk_db ${DB_TARGET})
  endif()
endforeach()

csilk_link_module(csilk_vector csilk_runtime csilk_json CURL::libcurl)
csilk_link_module(csilk_ai csilk_runtime csilk_json CURL::libcurl)
csilk_link_module(csilk_reflection csilk_runtime csilk_json)
csilk_link_module(csilk_permission csilk_runtime csilk_json)
csilk_link_module(csilk_workflow
    csilk_runtime
    csilk_json
    csilk_ai
    csilk_mq
    csilk_wasm
    csilk::yaml
)
csilk_link_module(csilk_protocols
    csilk_http
    csilk_http2
    csilk_runtime
    csilk_json
    csilk_reflection
    csilk_workflow
    csilk_mq
)
csilk_link_module(csilk_middleware
    csilk_http
    csilk_runtime
    csilk_json
    csilk_tls
    ZLIB::ZLIB
    OpenSSL::Crypto
    Threads::Threads
)
csilk_link_module(csilk_app
    csilk_http
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_db
    csilk_ai
)

# ── Umbrella targets ──────────────────────────────────────────────────────
add_library(csilk INTERFACE)
# The split static archives have intentional back-references (runtime teardown
# reaches protocol and messaging implementations). A linker group preserves the
# old monolithic link behavior for umbrella consumers without duplicating source
# ownership. Keep this GNU/Clang-only because --start-group is not portable.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  # CMake's native RESCAN group keeps all split static archives in one
  # linker group, including back-references from runtime teardown code.
  # The io_uring backend archive must be inside the group: runtime's
  # uring_*.c objects are only pulled in by later archives' back-references,
  # and liburing.a placed before the group would be scanned too early and
  # dropped, leaving io_uring_* undefined at link time.
  if(CSILK_USE_URING)
    target_link_libraries(csilk INTERFACE
        "$<LINK_GROUP:RESCAN;csilk_runtime;csilk_app;csilk_workflow;csilk_protocols;csilk_permission;csilk_middleware;csilk_reflection;csilk_ai;csilk_db;csilk_vector;csilk_mq;csilk_http;csilk_http2;csilk_tls;csilk_wasm;csilk_bypass;csilk_crypto;csilk_json;csilk_base;yyjson;uring>"
    )
  else()
    target_link_libraries(csilk INTERFACE
        "$<LINK_GROUP:RESCAN;csilk_runtime;csilk_app;csilk_workflow;csilk_protocols;csilk_permission;csilk_middleware;csilk_reflection;csilk_ai;csilk_db;csilk_vector;csilk_mq;csilk_http;csilk_http2;csilk_tls;csilk_wasm;csilk_bypass;csilk_crypto;csilk_json;csilk_base;yyjson>"
    )
  endif()
else()
  # CMake 3.11–3.23 fallback. The explicit runtime-first ordering preserves
  # the historical archive scan order on toolchains without LINK_GROUP.
  target_link_libraries(csilk INTERFACE
      csilk_runtime
      csilk_app
      csilk_workflow
      csilk_protocols
      csilk_permission
      csilk_middleware
      csilk_reflection
      csilk_ai
      csilk_db
      csilk_vector
      csilk_mq
      csilk_http
      csilk_http2
      csilk_tls
      csilk_wasm
      csilk_bypass
      csilk_crypto
      csilk_json
      csilk_base
  )
endif()
target_include_directories(csilk INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
add_library(csilk::csilk ALIAS csilk)

if(CSILK_BUILD_SHARED)
  # The monolithic shared library remains the compatibility ABI artifact.
  # It reuses the module object closure; module shared targets are separate
  # installable libraries and are not linked here, avoiding duplicate symbols
  # while keeping one compilation owner per implementation source.
  add_library(csilk_shared SHARED ${CSILK_MODULE_OBJECTS})
  set_target_properties(csilk_shared PROPERTIES OUTPUT_NAME "csilk")
  csilk_target_setup(csilk_shared PUBLIC SHARED)
  target_link_libraries(csilk_shared PRIVATE
      Threads::Threads
      OpenSSL::SSL
      OpenSSL::Crypto
      ZLIB::ZLIB
      SQLite3::SQLite3
      CURL::libcurl
      yyjson
      nghttp2
  )
  if(TARGET csilk::yaml)
    target_link_libraries(csilk_shared PRIVATE csilk::yaml)
  endif()
  if(TARGET csilk::llhttp)
    target_link_libraries(csilk_shared PRIVATE csilk::llhttp)
  endif()
  foreach(_db_target csilk::mysql csilk::pq csilk::hiredis csilk::mongoc)
    if(TARGET ${_db_target})
      target_link_libraries(csilk_shared PRIVATE ${_db_target})
    endif()
  endforeach()
  if(CSILK_USE_URING)
    target_link_libraries(csilk_shared PRIVATE uring)
  else()
    target_link_libraries(csilk_shared PRIVATE csilk::libuv)
  endif()
  if(NOT APPLE AND NOT WIN32)
    target_link_libraries(csilk_shared PRIVATE m)
  endif()
  add_library(csilk::shared ALIAS csilk_shared)
  message(STATUS "Shared library builds enabled: libcsilk*.so")
endif()
