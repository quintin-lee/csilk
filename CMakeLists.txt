# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(c_gin_framework C)

set(CMAKE_C_STANDARD 11)

# Include directories
include_directories(include)

# For now, just a dummy executable to ensure build works.
add_executable(run_tests tests/test_main.c)
