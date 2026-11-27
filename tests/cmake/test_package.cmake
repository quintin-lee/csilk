# Verify that an installed csilk package exposes the expected module targets,
# CMake configuration flags, and pkg-config manifests.

if(NOT DEFINED CSILK_BUILD_DIR OR NOT DEFINED CSILK_TEST_BINARY_DIR)
  message(FATAL_ERROR "CSILK_BUILD_DIR and CSILK_TEST_BINARY_DIR are required")
endif()

set(_prefix "${CSILK_TEST_BINARY_DIR}/prefix")
set(_consumer "${CSILK_TEST_BINARY_DIR}/consumer")
file(REMOVE_RECURSE "${CSILK_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${_consumer}")

set(_install_targets
    csilk_base
    csilk_crypto
    csilk_runtime
    csilk_json
    csilk_http
    csilk_tls
    csilk_http2
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_app
    yyjson)

if(CSILK_ENABLE_HTTP3)
  list(APPEND _install_targets csilk_h3)
endif()
if(CSILK_ENABLE_MCP)
  list(APPEND _install_targets csilk_mcp)
endif()
if(CSILK_ENABLE_GRPC)
  list(APPEND _install_targets csilk_grpc)
endif()
if(CSILK_ENABLE_WASM)
  list(APPEND _install_targets csilk_wasm)
endif()
if(CSILK_ENABLE_BYPASS)
  list(APPEND _install_targets csilk_bypass)
endif()
if(CSILK_ENABLE_PERM)
  list(APPEND _install_targets csilk_permission)
endif()
if(CSILK_ENABLE_DB)
  list(APPEND _install_targets csilk_db)
endif()
if(CSILK_ENABLE_VECTOR)
  list(APPEND _install_targets csilk_vector)
endif()
if(CSILK_ENABLE_AI)
  list(APPEND _install_targets csilk_ai)
endif()
if(CSILK_ENABLE_MQ)
  list(APPEND _install_targets csilk_mq)
endif()
if(CSILK_ENABLE_WORKFLOW)
  list(APPEND _install_targets csilk_workflow)
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  list(APPEND _install_targets csilk_observability)
endif()

if(CSILK_USE_URING)
  list(APPEND _install_targets uring)
endif()

if(CSILK_BUILD_SHARED)
  list(APPEND _install_targets
      csilk_shared
      csilk_base_shared
      csilk_crypto_shared
      csilk_runtime_shared
      csilk_json_shared
      csilk_http_shared
      csilk_tls_shared
      csilk_http2_shared
      csilk_protocols_shared
      csilk_middleware_shared
      csilk_reflection_shared
      csilk_app_shared)
  if(CSILK_ENABLE_HTTP3)
    list(APPEND _install_targets csilk_h3_shared)
  endif()
  if(CSILK_ENABLE_MCP)
    list(APPEND _install_targets csilk_mcp_shared)
  endif()
  if(CSILK_ENABLE_GRPC)
    list(APPEND _install_targets csilk_grpc_shared)
  endif()
  if(CSILK_ENABLE_WASM)
    list(APPEND _install_targets csilk_wasm_shared)
  endif()
  if(CSILK_ENABLE_BYPASS)
    list(APPEND _install_targets csilk_bypass_shared)
  endif()
  if(CSILK_ENABLE_PERM)
    list(APPEND _install_targets csilk_permission_shared)
  endif()
  if(CSILK_ENABLE_DB)
    list(APPEND _install_targets csilk_db_shared)
  endif()
  if(CSILK_ENABLE_VECTOR)
    list(APPEND _install_targets csilk_vector_shared)
  endif()
  if(CSILK_ENABLE_AI)
    list(APPEND _install_targets csilk_ai_shared)
  endif()
  if(CSILK_ENABLE_MQ)
    list(APPEND _install_targets csilk_mq_shared)
  endif()
  if(CSILK_ENABLE_WORKFLOW)
    list(APPEND _install_targets csilk_workflow_shared)
  endif()
  if(CSILK_ENABLE_OBSERVABILITY)
    list(APPEND _install_targets csilk_observability_shared)
  endif()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${CSILK_BUILD_DIR}" --parallel --target ${_install_targets}
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error
)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "csilk install target build failed:\n${_build_output}\n${_build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${CSILK_BUILD_DIR}" --prefix "${_prefix}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error
)
if(NOT _install_result EQUAL 0)
  message(FATAL_ERROR "csilk install failed:\n${_install_output}\n${_install_error}")
endif()

# Verify pkg-config manifests for all enabled modules and core isolation.
set(_pc_dir "")
file(GLOB _pc_dir_candidates LIST_DIRECTORIES true "${_prefix}/lib*/pkgconfig")
foreach(_cand IN LISTS _pc_dir_candidates)
  if(IS_DIRECTORY "${_cand}" AND EXISTS "${_cand}/csilk.pc")
    set(_pc_dir "${_cand}")
    break()
  endif()
endforeach()
if(NOT _pc_dir)
  message(FATAL_ERROR "missing installed pkgconfig directory with csilk.pc under ${_prefix}")
endif()

set(_expected_pc_modules
    csilk
    csilk-base
    csilk-core
    csilk-json
    csilk-crypto
    csilk-reflection
    csilk-runtime
    csilk-tls
    csilk-http2
    csilk-http
    csilk-protocols
    csilk-middleware
    csilk-app
)
set(_optional_pc_flags
    CSILK_ENABLE_HTTP3
    CSILK_ENABLE_MCP
    CSILK_ENABLE_GRPC
    CSILK_ENABLE_WASM
    CSILK_ENABLE_BYPASS
    CSILK_ENABLE_MQ
    CSILK_ENABLE_DB
    CSILK_ENABLE_VECTOR
    CSILK_ENABLE_AI
    CSILK_ENABLE_PERM
    CSILK_ENABLE_WORKFLOW
    CSILK_ENABLE_OBSERVABILITY
)
set(_optional_pc_names
    csilk-h3
    csilk-mcp
    csilk-grpc
    csilk-wasm
    csilk-bypass
    csilk-mq
    csilk-db
    csilk-vector
    csilk-ai
    csilk-permission
    csilk-workflow
    csilk-observability
)

foreach(_mod IN LISTS _expected_pc_modules)
  if(NOT EXISTS "${_pc_dir}/${_mod}.pc")
    message(FATAL_ERROR "missing expected pkg-config file: ${_pc_dir}/${_mod}.pc")
  endif()
endforeach()

file(READ "${_pc_dir}/csilk.pc" _csilk_pc_content)
if(NOT _csilk_pc_content MATCHES "Requires:([^\n]*)")
  message(FATAL_ERROR "csilk.pc is missing Requires line:\n${_csilk_pc_content}")
endif()
set(_csilk_pc_requires "${CMAKE_MATCH_1}")

foreach(_flag _pc_name IN ZIP_LISTS _optional_pc_flags _optional_pc_names)
  if(${_flag})
    if(NOT EXISTS "${_pc_dir}/${_pc_name}.pc")
      message(FATAL_ERROR "missing enabled module pkg-config file: ${_pc_dir}/${_pc_name}.pc")
    endif()
    if(NOT _csilk_pc_requires MATCHES "(^|[ \t])${_pc_name}([ \t]|$)")
      message(FATAL_ERROR "csilk.pc Requires missing enabled module '${_pc_name}': Requires:${_csilk_pc_requires}")
    endif()
  else()
    if(EXISTS "${_pc_dir}/${_pc_name}.pc")
      message(FATAL_ERROR "unexpected disabled module pkg-config file: ${_pc_dir}/${_pc_name}.pc")
    endif()
    if(_csilk_pc_requires MATCHES "(^|[ \t])${_pc_name}([ \t]|$)")
      message(FATAL_ERROR "csilk.pc Requires contains disabled module '${_pc_name}': Requires:${_csilk_pc_requires}")
    endif()
  endif()
endforeach()

if(NOT CSILK_ENABLE_DB)
  if(_csilk_pc_content MATCHES "sqlite3")
    message(FATAL_ERROR "csilk.pc must not reference sqlite3 when CSILK_ENABLE_DB=OFF:\n${_csilk_pc_content}")
  endif()
endif()
if(NOT CSILK_ENABLE_AI AND NOT CSILK_ENABLE_VECTOR AND NOT CSILK_ENABLE_MCP)
  if(_csilk_pc_content MATCHES "curl")
    message(FATAL_ERROR "csilk.pc must not reference curl when AI/Vector/MCP are OFF:\n${_csilk_pc_content}")
  endif()
endif()
if(CSILK_ENABLE_CSV)
  if(NOT _csilk_pc_content MATCHES "-DCSILK_HAS_CSV=1")
    message(FATAL_ERROR "csilk.pc missing -DCSILK_HAS_CSV=1 when CSILK_ENABLE_CSV=ON:\n${_csilk_pc_content}")
  endif()
else()
  if(_csilk_pc_content MATCHES "CSILK_HAS_CSV")
    message(FATAL_ERROR "csilk.pc must not define CSILK_HAS_CSV when CSILK_ENABLE_CSV=OFF:\n${_csilk_pc_content}")
  endif()
endif()

file(GLOB _installed_csilk_pcs "${_pc_dir}/csilk*.pc")
foreach(_pc_file IN LISTS _installed_csilk_pcs)
  file(READ "${_pc_file}" _pc_body)
  if(_pc_body MATCHES "@[A-Za-z0-9_]+@")
    message(FATAL_ERROR "unexpanded CMake placeholder in ${_pc_file}:\n${_pc_body}")
  endif()
endforeach()

find_program(_pkg_config_exe pkg-config)
if(_pkg_config_exe AND NOT CSILK_USE_URING)
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_pc_dir}" "${_pkg_config_exe}" --cflags --libs csilk
      RESULT_VARIABLE _pc_res
      OUTPUT_VARIABLE _pc_out
      ERROR_VARIABLE _pc_err
  )
  if(NOT _pc_res EQUAL 0)
    message(FATAL_ERROR "pkg-config --cflags --libs csilk failed:\n${_pc_out}\n${_pc_err}")
  endif()
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG_PATH=${_pc_dir}" "${_pkg_config_exe}" --static --libs csilk
      RESULT_VARIABLE _pc_static_res
      OUTPUT_VARIABLE _pc_static_out
      ERROR_VARIABLE _pc_static_err
  )
  if(NOT _pc_static_res EQUAL 0)
    message(FATAL_ERROR "pkg-config --static --libs csilk failed:\n${_pc_static_out}\n${_pc_static_err}")
  endif()
endif()

file(WRITE "${_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.17)
project(csilk_package_consumer C)

find_package(csilk CONFIG REQUIRED)

foreach(_cfg_flag IN ITEMS
    CSILK_BUILD_SHARED
    CSILK_USE_URING
    CSILK_ENABLE_DB
    CSILK_ENABLE_MQ
    CSILK_ENABLE_AI
    CSILK_ENABLE_VECTOR
    CSILK_ENABLE_MCP
    CSILK_ENABLE_WORKFLOW
    CSILK_ENABLE_GRPC
    CSILK_ENABLE_WASM
    CSILK_ENABLE_BYPASS
    CSILK_ENABLE_PERM
    CSILK_ENABLE_HTTP3
    CSILK_ENABLE_OBSERVABILITY
    CSILK_ENABLE_CSV
    CSILK_NEED_CURL)
  if(NOT DEFINED ${_cfg_flag})
    message(FATAL_ERROR "csilk-config.cmake did not define ${_cfg_flag}")
  endif()
  if(DEFINED CSILK_EXPECTED_${_cfg_flag})
    if((${_cfg_flag} AND NOT CSILK_EXPECTED_${_cfg_flag}) OR (NOT ${_cfg_flag} AND CSILK_EXPECTED_${_cfg_flag}))
      message(FATAL_ERROR "csilk-config.cmake ${_cfg_flag}='${${_cfg_flag}}' != expected '${CSILK_EXPECTED_${_cfg_flag}}'")
    endif()
  endif()
endforeach()

set(_required_targets
    csilk::csilk
    csilk::base
    csilk::core
    csilk::crypto
    csilk::runtime
    csilk::json
    csilk::tls
    csilk::http2
    csilk::http
    csilk::protocols
    csilk::middleware
    csilk::reflection
    csilk::app)
if(CSILK_BUILD_SHARED)
  list(APPEND _required_targets
      csilk::base_shared
      csilk::core_shared
      csilk::crypto_shared
      csilk::runtime_shared
      csilk::json_shared
      csilk::tls_shared
      csilk::http2_shared
      csilk::http_shared
      csilk::protocols_shared
      csilk::middleware_shared
      csilk::reflection_shared
      csilk::app_shared
      csilk::shared)
endif()

set(_optional_flags
    CSILK_ENABLE_PERM
    CSILK_ENABLE_DB
    CSILK_ENABLE_VECTOR
    CSILK_ENABLE_AI
    CSILK_ENABLE_MQ
    CSILK_ENABLE_WORKFLOW
    CSILK_ENABLE_HTTP3
    CSILK_ENABLE_MCP
    CSILK_ENABLE_GRPC
    CSILK_ENABLE_WASM
    CSILK_ENABLE_BYPASS
    CSILK_ENABLE_OBSERVABILITY)
set(_optional_aliases
    permission
    db
    vector
    ai
    mq
    workflow
    h3
    mcp
    grpc
    wasm
    bypass
    observability)

foreach(_flag _alias IN ZIP_LISTS _optional_flags _optional_aliases)
  if(${_flag})
    list(APPEND _required_targets csilk::${_alias})
    if(CSILK_BUILD_SHARED)
      list(APPEND _required_targets csilk::${_alias}_shared)
    endif()
  else()
    if(TARGET csilk::${_alias})
      message(FATAL_ERROR "unexpected disabled module target exposed: csilk::${_alias}")
    endif()
    if(TARGET csilk::${_alias}_shared)
      message(FATAL_ERROR "unexpected disabled shared module target exposed: csilk::${_alias}_shared")
    endif()
  endif()
endforeach()

if(CSILK_ENABLE_HTTP3)
  list(APPEND _required_targets csilk::ngtcp2 csilk::nghttp3)
endif()
if(CSILK_ENABLE_WASM)
  list(APPEND _required_targets csilk::vmlib vmlib)
endif()

foreach(_target IN LISTS _required_targets)
  if(NOT TARGET ${_target})
    message(FATAL_ERROR "missing exported target: ${_target}")
  endif()
endforeach()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/consumer.c" [==[
#include <stddef.h>
#include "csilk/core/server/server.h"

int main(void) {
    csilk_arena_t *arena = csilk_arena_new(0);
    if (arena == NULL) {
        return 1;
    }
    void *memory = csilk_arena_alloc(arena, sizeof(size_t));
    csilk_arena_free(arena);
    return memory == NULL;
}
]==])

add_executable(csilk_package_consumer "${CMAKE_CURRENT_BINARY_DIR}/consumer.c")
target_link_libraries(csilk_package_consumer PRIVATE csilk::base)
if(USE_ASAN)
  target_compile_options(csilk_package_consumer PRIVATE -fsanitize=address)
endif()
]=])

set(_consumer_cmake_args
    -S "${_consumer}"
    -B "${_consumer}/build"
)
if(DEFINED CMAKE_PREFIX_PATH AND CMAKE_PREFIX_PATH)
  list(APPEND _consumer_cmake_args "-DCMAKE_PREFIX_PATH=${_prefix};${CMAKE_PREFIX_PATH}")
else()
  list(APPEND _consumer_cmake_args "-DCMAKE_PREFIX_PATH=${_prefix}")
endif()
if(DEFINED CMAKE_C_COMPILER AND CMAKE_C_COMPILER)
  list(APPEND _consumer_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(DEFINED CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE)
  list(APPEND _consumer_cmake_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()
if(DEFINED USE_ASAN AND USE_ASAN)
  list(APPEND _consumer_cmake_args "-DUSE_ASAN=${USE_ASAN}")
endif()
foreach(_flag IN ITEMS
    CSILK_BUILD_SHARED
    CSILK_USE_URING
    CSILK_ENABLE_DB
    CSILK_ENABLE_MQ
    CSILK_ENABLE_AI
    CSILK_ENABLE_VECTOR
    CSILK_ENABLE_MCP
    CSILK_ENABLE_WORKFLOW
    CSILK_ENABLE_GRPC
    CSILK_ENABLE_WASM
    CSILK_ENABLE_BYPASS
    CSILK_ENABLE_PERM
    CSILK_ENABLE_HTTP3
    CSILK_ENABLE_OBSERVABILITY
    CSILK_ENABLE_CSV)
  if(DEFINED ${_flag})
    list(APPEND _consumer_cmake_args "-DCSILK_EXPECTED_${_flag}=${${_flag}}")
  endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_consumer_cmake_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error
)
if(NOT _configure_result EQUAL 0)
  message(FATAL_ERROR "package consumer configure failed:\n${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer}/build" --parallel
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error
)
if(NOT _build_result EQUAL 0)
  message(FATAL_ERROR "package consumer build failed:\n${_build_output}\n${_build_error}")
endif()

message(STATUS "installed csilk package target contract passed")
