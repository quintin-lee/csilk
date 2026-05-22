# CMakeLists.txt
cmake_minimum_required(VERSION 3.11)
project(c_gin_framework C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Fetch libuv
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.48.0
)
FetchContent_MakeAvailable(libuv)

# Fetch llhttp
FetchContent_Declare(
  llhttp
  GIT_REPOSITORY https://github.com/nodejs/llhttp.git
  GIT_TAG        release/v9.2.1
)
FetchContent_MakeAvailable(llhttp)

# Fetch cJSON
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG        v1.7.18
)
FetchContent_MakeAvailable(cjson)

# Include directories
include_directories(include)

# Source files
set(GIN_SOURCES
    src/context.c
    src/router.c
    src/group.c
    src/server.c
    src/url_parser.c
)

# Build library
add_library(gin ${GIN_SOURCES})
target_link_libraries(gin uv llhttp cjson)

# Test executables
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

# For now, just a dummy executable to ensure build works.
add_executable(run_tests tests/test_main.c)
