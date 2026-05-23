# CMakeLists.txt
cmake_minimum_required(VERSION 3.11)
project(c_gin_framework C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

enable_testing()

option(USE_ASAN "Enable AddressSanitizer for memory leak detection" OFF)
if(USE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
endif()

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
    src/arena.c
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

# Build library (static by default; set -DBUILD_SHARED_LIBS=ON for shared)
add_library(gin ${GIN_SOURCES})
target_link_libraries(gin PUBLIC uv ${LLHTTP_LIB} cjson)
target_include_directories(gin PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(gin PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/gin.h"
    VERSION "0.1.0"
    SOVERSION "0"
)

# Helper to add a test executable and register with CTest
function(add_gin_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} gin)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Unit tests
add_gin_test(test_logger tests/test_logger.c)
add_gin_test(test_static tests/test_static.c)
add_gin_test(test_context tests/test_context.c)
add_gin_test(test_router tests/test_router.c)
add_gin_test(test_radix tests/test_radix.c)
add_gin_test(test_group tests/test_group.c)
add_gin_test(test_headers tests/test_headers.c)
add_gin_test(test_server tests/test_server.c)
add_gin_test(test_query tests/test_query.c)
add_gin_test(test_json tests/test_json.c)
add_gin_test(test_recovery tests/test_recovery.c)
add_gin_test(test_auth tests/test_auth.c)
add_gin_test(test_keepalive tests/test_keepalive.c)
add_gin_test(test_edge tests/test_edge.c)
add_gin_test(test_timeout tests/test_timeout.c)
set_tests_properties(test_timeout PROPERTIES TIMEOUT 10)

option(USE_FUZZER "Enable libFuzzer" OFF)
if(USE_FUZZER)
    add_executable(fuzz_test tests/fuzz_test.c)
    target_compile_options(fuzz_test PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_test PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_test gin)
endif()

# Integration tests (may have pre-existing failures, not run by default)
add_executable(test_integration tests/test_integration.c)
target_link_libraries(test_integration gin)
add_test(NAME test_integration COMMAND test_integration)

# Example server
add_executable(example_server example_server.c)
target_link_libraries(example_server gin)

# Install targets
include(GNUInstallDirs)
install(TARGETS gin
    EXPORT gin-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT gin-targets
    FILE gin-config.cmake
    NAMESPACE gin::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gin
)

# clang-tidy
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE}
            --quiet
            --checks="-*,clang-analyzer-*,performance-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling"
            --warnings-as-errors="*"
            ${GIN_SOURCES}
            -- -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${cjson_SOURCE_DIR}
            -I${cjson_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files"
    )
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
else()
    message(STATUS "clang-tidy not found — skipping")
endif()

# CPack packaging
set(CPACK_PACKAGE_NAME "c-gin-framework")
set(CPACK_PACKAGE_VERSION "0.1.0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Gin")
include(CPack)
