# cmake/targets.cmake — csilk_target_setup function and library targets

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
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES C)
    if(ipo_supported AND TYPE STREQUAL "SHARED")
      set_target_properties(${TARGET} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
  endif()

  if(CSILK_USE_URING)
    target_link_libraries(${TARGET} PUBLIC uring yyjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLite3_LIBRARIES} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIBRARIES} ${ZLIB_LIBRARIES} nghttp2 m)
    if(NOT APPLE)
      target_compile_options(${TARGET} PUBLIC "-D_GNU_SOURCE")
    endif()
  else()
    target_link_libraries(${TARGET} PUBLIC ${libuv_LIBRARIES} yyjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLite3_LIBRARIES} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIBRARIES} ${ZLIB_LIBRARIES} nghttp2 m)
  endif()

  foreach(DB_LIB MYSQL_LIB PQ_LIB HIREDIS_LIB)
    if(${DB_LIB})
      target_link_libraries(${TARGET} PRIVATE ${${DB_LIB}})
    endif()
  endforeach()

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
