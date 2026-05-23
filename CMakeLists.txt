# CMakeLists.txt
cmake_minimum_required(VERSION 3.11)
project(c_gin_framework C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 查找 clang-format
find_program(CLANG_FORMAT_EXECUTABLE clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXECUTABLE}")
    
    # 查找所有源文件和头文件
    file(GLOB_RECURSE ALL_SOURCE_FILES
        "src/*.c" "src/*.h"
        "include/*.h"
        "tests/*.c" "tests/*.h"
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${ALL_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-format on source files"
    )

    # 设置 clang-format 选项
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")
endif()

# 查找 doxygen
find_program(DOXYGEN_EXECUTABLE doxygen)
if(DOXYGEN_EXECUTABLE)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating documentation with Doxygen"
    )
endif()

include(FetchContent)

# Fetch libuv
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.48.0
)
FetchContent_MakeAvailable(libuv)

# Fetch llhttp - replaced with system library to avoid ABI mismatch
# System has libllhttp.so.9.3 at /usr/lib/libllhttp.so with headers at /usr/include/llhttp.h
# If system llhttp is not available, uncomment the following to fetch from source:
# FetchContent_Declare(
#   llhttp
#   GIT_REPOSITORY https://github.com/nodejs/llhttp.git
#   GIT_TAG        release/v9.2.1
# )
# FetchContent_MakeAvailable(llhttp)

# Fetch cJSON
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG        v1.7.18
)
FetchContent_MakeAvailable(cjson)

# Include directories
include_directories(include)
include_directories(${cjson_SOURCE_DIR})

# Source files
set(GIN_SOURCES
    src/context.c
    src/router.c
    src/group.c
    src/server.c
    src/url_parser.c
    src/middleware/logger.c
    src/middleware/recovery.c
    src/middleware/auth.c
    src/middleware/static.c
    src/middleware/cors.c
)

# Find system llhttp (prefer system library to match installed headers)
find_library(LLHTTP_LIB NAMES llhttp libllhttp REQUIRED)
if(NOT LLHTTP_LIB)
  message(FATAL_ERROR "llhttp library not found. Install libllhttp-dev or adjust FetchContent in CMakeLists.txt")
endif()
message(STATUS "Found llhttp: ${LLHTTP_LIB}")

# Build library
add_library(gin ${GIN_SOURCES})
target_link_libraries(gin uv ${LLHTTP_LIB} cjson)

# Test executables
add_executable(test_logger tests/test_logger.c)
target_link_libraries(test_logger gin)

add_executable(test_static tests/test_static.c)
target_link_libraries(test_static gin)

add_executable(test_context tests/test_context.c)
target_link_libraries(test_context gin)

add_executable(test_router tests/test_router.c)
target_link_libraries(test_router gin)

add_executable(test_radix tests/test_radix.c)
target_link_libraries(test_radix gin)

add_executable(test_group tests/test_group.c)
target_link_libraries(test_group gin)

add_executable(test_headers tests/test_headers.c)
target_link_libraries(test_headers gin)

add_executable(test_server tests/test_server.c)
target_link_libraries(test_server gin)

add_executable(test_query tests/test_query.c)
target_link_libraries(test_query gin)

add_executable(test_json tests/test_json.c)
target_link_libraries(test_json gin)

add_executable(test_recovery tests/test_recovery.c)
target_link_libraries(test_recovery gin)

add_executable(test_auth tests/test_auth.c)
target_link_libraries(test_auth gin)

add_executable(test_keepalive tests/test_keepalive.c)
target_link_libraries(test_keepalive gin)

add_executable(test_timeout tests/test_timeout.c)
target_link_libraries(test_timeout gin)

add_executable(test_integration tests/test_integration.c)
target_link_libraries(test_integration gin)

# Example server
add_executable(example_server example_server.c)
target_link_libraries(example_server gin)

# For now, just a dummy executable to ensure build works.
add_executable(run_tests tests/test_main.c)
