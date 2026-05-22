# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(c_gin_framework C)

set(CMAKE_C_STANDARD 11)

# Include directories
include_directories(include)

# Source files
set(GIN_SOURCES
    src/context.c
    src/router.c
)

# Build library
add_library(gin ${GIN_SOURCES})

# Test executables
add_executable(test_context tests/test_context.c)
target_link_libraries(test_context gin)

add_executable(test_router tests/test_router.c)
target_link_libraries(test_router gin)

# For now, just a dummy executable to ensure build works.
add_executable(run_tests tests/test_main.c)
