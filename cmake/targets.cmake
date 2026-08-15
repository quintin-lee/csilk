# cmake/targets.cmake — csilk_target_setup function and modular library targets

# ── Helper function: apply common compile/link/profile to a target ────────
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
  )

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

# ── 1. csilk_core (libcsilk-core.a) ──────────────────────────────────────
add_library(csilk_core STATIC ${CSILK_CORE_SOURCES})
set_target_properties(csilk_core PROPERTIES OUTPUT_NAME "csilk-core")
csilk_target_setup(csilk_core PUBLIC STATIC)
target_link_libraries(csilk_core PUBLIC yyjson OpenSSL::Crypto Threads::Threads)
if(CSILK_USE_URING)
  target_link_libraries(csilk_core PUBLIC uring)
  if(NOT APPLE)
    target_compile_options(csilk_core PUBLIC "-D_GNU_SOURCE")
  endif()
else()
  target_link_libraries(csilk_core PUBLIC csilk::libuv)
endif()
if(NOT APPLE AND NOT WIN32)
  target_link_libraries(csilk_core PRIVATE m)
endif()
add_library(csilk::core ALIAS csilk_core)

# ── 2. csilk_http (libcsilk-http.a) ──────────────────────────────────────
add_library(csilk_http STATIC ${CSILK_HTTP_SOURCES})
set_target_properties(csilk_http PROPERTIES OUTPUT_NAME "csilk-http")
csilk_target_setup(csilk_http PUBLIC STATIC)
target_link_libraries(csilk_http PUBLIC csilk_core ZLIB::ZLIB)
if(TARGET csilk::llhttp)
  target_link_libraries(csilk_http PRIVATE csilk::llhttp)
endif()
add_library(csilk::http ALIAS csilk_http)

# ── 3. csilk_tls (libcsilk-tls.a) ────────────────────────────────────────
add_library(csilk_tls STATIC ${CSILK_TLS_SOURCES})
set_target_properties(csilk_tls PROPERTIES OUTPUT_NAME "csilk-tls")
csilk_target_setup(csilk_tls PUBLIC STATIC)
target_link_libraries(csilk_tls PUBLIC csilk_core OpenSSL::SSL OpenSSL::Crypto)
add_library(csilk::tls ALIAS csilk_tls)

# ── 4. csilk_http2 (libcsilk-http2.a) ────────────────────────────────────
add_library(csilk_http2 STATIC ${CSILK_HTTP2_SOURCES})
set_target_properties(csilk_http2 PROPERTIES OUTPUT_NAME "csilk-http2")
csilk_target_setup(csilk_http2 PUBLIC STATIC)
target_link_libraries(csilk_http2 PUBLIC csilk_core csilk_http nghttp2)
add_library(csilk::http2 ALIAS csilk_http2)

# ── 5. csilk_db (libcsilk-db.a) ──────────────────────────────────────────
add_library(csilk_db STATIC ${CSILK_DB_SOURCES})
set_target_properties(csilk_db PROPERTIES OUTPUT_NAME "csilk-db")
csilk_target_setup(csilk_db PUBLIC STATIC)
target_link_libraries(csilk_db PUBLIC csilk_core SQLite3::SQLite3)
foreach(DB_TARGET csilk::mysql csilk::pq csilk::hiredis csilk::mongoc)
  if(TARGET ${DB_TARGET})
    target_link_libraries(csilk_db PRIVATE ${DB_TARGET})
  endif()
endforeach()
add_library(csilk::db ALIAS csilk_db)

# ── 6. csilk_ai (libcsilk-ai.a) ──────────────────────────────────────────
add_library(csilk_ai STATIC ${CSILK_AI_SOURCES})
set_target_properties(csilk_ai PROPERTIES OUTPUT_NAME "csilk-ai")
csilk_target_setup(csilk_ai PUBLIC STATIC)
target_link_libraries(csilk_ai PUBLIC csilk_core CURL::libcurl)
add_library(csilk::ai ALIAS csilk_ai)

# ── 7. csilk_mq (libcsilk-mq.a) ──────────────────────────────────────────
add_library(csilk_mq STATIC ${CSILK_MQ_SOURCES})
set_target_properties(csilk_mq PROPERTIES OUTPUT_NAME "csilk-mq")
csilk_target_setup(csilk_mq PUBLIC STATIC)
target_link_libraries(csilk_mq PUBLIC csilk_core)
add_library(csilk::mq ALIAS csilk_mq)

# ── 8. csilk_workflow (libcsilk-workflow.a) ──────────────────────────────
add_library(csilk_workflow STATIC ${CSILK_WORKFLOW_SOURCES})
set_target_properties(csilk_workflow PROPERTIES OUTPUT_NAME "csilk-workflow")
csilk_target_setup(csilk_workflow PUBLIC STATIC)
target_link_libraries(csilk_workflow PUBLIC csilk_core csilk_ai csilk_mq csilk::yaml)
add_library(csilk::workflow ALIAS csilk_workflow)

# ── 9. csilk umbrella static library (libcsilk.a) ────────────────────────
add_library(csilk STATIC ${CSILK_SOURCES})
set_target_properties(csilk PROPERTIES OUTPUT_NAME "csilk")
csilk_target_setup(csilk PUBLIC STATIC)
target_link_libraries(csilk PUBLIC
    csilk_core
    csilk_http
    csilk_tls
    csilk_http2
    csilk_db
    csilk_ai
    csilk_mq
    csilk_workflow
)
add_library(csilk::csilk ALIAS csilk)

# ── 10. Shared library targets (libcsilk*.so) ────────────────────────────
if(CSILK_BUILD_SHARED)
    # csilk_core_shared (libcsilk-core.so)
    add_library(csilk_core_shared SHARED ${CSILK_CORE_SOURCES})
    set_target_properties(csilk_core_shared PROPERTIES OUTPUT_NAME "csilk-core")
    csilk_target_setup(csilk_core_shared PUBLIC SHARED)
    target_link_libraries(csilk_core_shared PUBLIC yyjson OpenSSL::Crypto Threads::Threads)
    if(CSILK_USE_URING)
      target_link_libraries(csilk_core_shared PUBLIC uring)
      if(NOT APPLE)
        target_compile_options(csilk_core_shared PUBLIC "-D_GNU_SOURCE")
      endif()
    else()
      target_link_libraries(csilk_core_shared PUBLIC csilk::libuv)
    endif()
    if(NOT APPLE AND NOT WIN32)
      target_link_libraries(csilk_core_shared PRIVATE m)
    endif()
    add_library(csilk::core_shared ALIAS csilk_core_shared)

    # csilk_http_shared (libcsilk-http.so)
    add_library(csilk_http_shared SHARED ${CSILK_HTTP_SOURCES})
    set_target_properties(csilk_http_shared PROPERTIES OUTPUT_NAME "csilk-http")
    csilk_target_setup(csilk_http_shared PUBLIC SHARED)
    target_link_libraries(csilk_http_shared PUBLIC csilk_core_shared ZLIB::ZLIB)
    if(TARGET csilk::llhttp)
      target_link_libraries(csilk_http_shared PRIVATE csilk::llhttp)
    endif()
    add_library(csilk::http_shared ALIAS csilk_http_shared)

    # csilk_tls_shared (libcsilk-tls.so)
    add_library(csilk_tls_shared SHARED ${CSILK_TLS_SOURCES})
    set_target_properties(csilk_tls_shared PROPERTIES OUTPUT_NAME "csilk-tls")
    csilk_target_setup(csilk_tls_shared PUBLIC SHARED)
    target_link_libraries(csilk_tls_shared PUBLIC csilk_core_shared OpenSSL::SSL OpenSSL::Crypto)
    add_library(csilk::tls_shared ALIAS csilk_tls_shared)

    # csilk_http2_shared (libcsilk-http2.so)
    add_library(csilk_http2_shared SHARED ${CSILK_HTTP2_SOURCES})
    set_target_properties(csilk_http2_shared PROPERTIES OUTPUT_NAME "csilk-http2")
    csilk_target_setup(csilk_http2_shared PUBLIC SHARED)
    target_link_libraries(csilk_http2_shared PUBLIC csilk_core_shared csilk_http_shared nghttp2)
    add_library(csilk::http2_shared ALIAS csilk_http2_shared)

    # csilk_db_shared (libcsilk-db.so)
    add_library(csilk_db_shared SHARED ${CSILK_DB_SOURCES})
    set_target_properties(csilk_db_shared PROPERTIES OUTPUT_NAME "csilk-db")
    csilk_target_setup(csilk_db_shared PUBLIC SHARED)
    target_link_libraries(csilk_db_shared PUBLIC csilk_core_shared SQLite3::SQLite3)
    foreach(DB_TARGET csilk::mysql csilk::pq csilk::hiredis csilk::mongoc)
      if(TARGET ${DB_TARGET})
        target_link_libraries(csilk_db_shared PRIVATE ${DB_TARGET})
      endif()
    endforeach()
    add_library(csilk::db_shared ALIAS csilk_db_shared)

    # csilk_ai_shared (libcsilk-ai.so)
    add_library(csilk_ai_shared SHARED ${CSILK_AI_SOURCES})
    set_target_properties(csilk_ai_shared PROPERTIES OUTPUT_NAME "csilk-ai")
    csilk_target_setup(csilk_ai_shared PUBLIC SHARED)
    target_link_libraries(csilk_ai_shared PUBLIC csilk_core_shared CURL::libcurl)
    add_library(csilk::ai_shared ALIAS csilk_ai_shared)

    # csilk_mq_shared (libcsilk-mq.so)
    add_library(csilk_mq_shared SHARED ${CSILK_MQ_SOURCES})
    set_target_properties(csilk_mq_shared PROPERTIES OUTPUT_NAME "csilk-mq")
    csilk_target_setup(csilk_mq_shared PUBLIC SHARED)
    target_link_libraries(csilk_mq_shared PUBLIC csilk_core_shared)
    add_library(csilk::mq_shared ALIAS csilk_mq_shared)

    # csilk_workflow_shared (libcsilk-workflow.so)
    add_library(csilk_workflow_shared SHARED ${CSILK_WORKFLOW_SOURCES})
    set_target_properties(csilk_workflow_shared PROPERTIES OUTPUT_NAME "csilk-workflow")
    csilk_target_setup(csilk_workflow_shared PUBLIC SHARED)
    target_link_libraries(csilk_workflow_shared PUBLIC csilk_core_shared csilk_ai_shared csilk_mq_shared csilk::yaml)
    add_library(csilk::workflow_shared ALIAS csilk_workflow_shared)

    # csilk_shared monolithic umbrella (libcsilk.so)
    add_library(csilk_shared SHARED ${CSILK_SOURCES})
    set_target_properties(csilk_shared PROPERTIES OUTPUT_NAME "csilk")
    csilk_target_setup(csilk_shared PUBLIC SHARED)
    target_link_libraries(csilk_shared PUBLIC
        csilk_core_shared
        csilk_http_shared
        csilk_tls_shared
        csilk_http2_shared
        csilk_db_shared
        csilk_ai_shared
        csilk_mq_shared
        csilk_workflow_shared
    )
    add_dependencies(csilk csilk_shared)
    message(STATUS "Shared library builds enabled: libcsilk*.so")
endif()
