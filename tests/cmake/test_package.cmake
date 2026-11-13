# Verify that an installed csilk package exposes the phase-one module targets.

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
    csilk_wasm
    csilk_bypass
    csilk_http
    csilk_tls
    csilk_http2
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_permission
    csilk_app
    csilk_db
    csilk_vector
    csilk_ai
    csilk_mq
    csilk_workflow
    yyjson)

if(CSILK_BUILD_SHARED)
  list(APPEND _install_targets
      csilk_shared
      csilk_base_shared
      csilk_crypto_shared
      csilk_runtime_shared
      csilk_json_shared
      csilk_wasm_shared
      csilk_bypass_shared
      csilk_http_shared
      csilk_tls_shared
      csilk_http2_shared
      csilk_protocols_shared
      csilk_middleware_shared
      csilk_reflection_shared
      csilk_permission_shared
      csilk_app_shared
      csilk_db_shared
      csilk_vector_shared
      csilk_ai_shared
      csilk_mq_shared
      csilk_workflow_shared)
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

file(WRITE "${_consumer}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.11)
project(csilk_package_consumer C)

find_package(csilk CONFIG REQUIRED)

set(_required_targets
    csilk::base
    csilk::core
    csilk::crypto
    csilk::runtime
    csilk::http
    csilk::protocols
    csilk::middleware
    csilk::reflection
    csilk::permission
    csilk::app
    csilk::db
    csilk::vector
    csilk::ai
    csilk::mq
    csilk::workflow)
if(CSILK_BUILD_SHARED)
  list(APPEND _required_targets
      csilk::crypto_shared
      csilk::runtime_shared
      csilk::protocols_shared
      csilk::middleware_shared
      csilk::reflection_shared
      csilk::permission_shared
      csilk::app_shared
      csilk::vector_shared
      csilk::shared)
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
