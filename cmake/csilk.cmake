# csilk cmake helper
# Include this file from a consuming project BEFORE calling add_subdirectory(csilk).
# It pre-configures FetchContent variables so csilk uses its pre-fetched
# dependencies instead of re-downloading.
#
# Usage:
#   list(APPEND CMAKE_MODULE_PATH /path/to/csilk/cmake)
#   include(csilk)
#   add_subdirectory(/path/to/csilk ${CMAKE_BINARY_DIR}/csilk)
#   target_link_libraries(myapp PRIVATE csilk)

if(NOT CSILK_SOURCE_DIR)
  message(FATAL_ERROR "csilk.cmake: CSILK_SOURCE_DIR must be set to the csilk source root")
endif()

set(CSILK_DEPS_DIR "${CSILK_SOURCE_DIR}/build/_deps")

# Pre-set FetchContent source directories so dependencies aren't re-downloaded
if(EXISTS "${CSILK_DEPS_DIR}/libuv-src/CMakeLists.txt")
  set(libuv_SOURCE_DIR "${CSILK_DEPS_DIR}/libuv-src" CACHE PATH "Pre-fetched libuv source")
  message(STATUS "csilk: using pre-fetched libuv at ${libuv_SOURCE_DIR}")
endif()

if(EXISTS "${CSILK_DEPS_DIR}/cjson-src/CMakeLists.txt")
  set(cjson_SOURCE_DIR "${CSILK_DEPS_DIR}/cjson-src" CACHE PATH "Pre-fetched cJSON source")
  message(STATUS "csilk: using pre-fetched cJSON at ${cjson_SOURCE_DIR}")
endif()

if(EXISTS "${CSILK_DEPS_DIR}/llhttp-src/CMakeLists.txt")
  set(llhttp_SOURCE_DIR "${CSILK_DEPS_DIR}/llhttp-src" CACHE PATH "Pre-fetched llhttp source")
  message(STATUS "csilk: using pre-fetched llhttp at ${llhttp_SOURCE_DIR}")
endif()
