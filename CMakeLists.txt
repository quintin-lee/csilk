# CMakeLists.txt
cmake_minimum_required(VERSION 3.11)
project(c_csilk_framework C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# cJSON defines true/false macros conflicting with C23 keywords, and
# Apple's SDK deprecates sprintf. These flags suppress benign warnings
# from third-party code (cJSON, nghttp2, libuv).
# GCC and older Clang silently ignore unsupported -Wno-* flags.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-keyword-macro -Wno-deprecated-declarations")

# SIMD acceleration for router path matching (AVX2 on x86_64, NEON on aarch64)
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-mavx2")
check_c_source_compiles("
  #include <immintrin.h>
  int main(void) {
    __m256i v = _mm256_setzero_si256();
    (void)v;
    return 0;
  }
" CSILK_HAS_AVX2)
set(CMAKE_REQUIRED_FLAGS "")
if(CSILK_HAS_AVX2)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mavx2 -D__AVX2__")
  message(STATUS "AVX2 SIMD enabled for router path matching")
else()
  message(STATUS "AVX2 not available — using word-aligned fast path")
endif()

# Version — single source of truth
set(CSILK_VERSION_MAJOR 0)
set(CSILK_VERSION_MINOR 3)
set(CSILK_VERSION_PATCH 0)
set(CSILK_VERSION "${CSILK_VERSION_MAJOR}.${CSILK_VERSION_MINOR}.${CSILK_VERSION_PATCH}")

# Build acceleration: ccache (if available)
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Found ccache: ${CCACHE_PROGRAM}")
endif()

enable_testing()

option(USE_ASAN "Enable AddressSanitizer for memory leak detection" OFF)
if(USE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    # Use -static-libasan to avoid manual LD_PRELOAD issues in some environments.
    # Note: For clang, static linking is often the default or handled via -fsanitize=address.
    # For GCC, -static-libasan is explicit.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
    
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -static-libasan")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libasan")
    endif()
endif()

option(USE_COVERAGE "Enable code coverage with gcov" OFF)
if(USE_COVERAGE)
    message(STATUS "Code coverage enabled")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0 -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} --coverage")
endif()

# Option to enable OOM testing
option(ENABLE_OOM_TEST "Enable OOM (Out Of Memory) testing" OFF)
if(ENABLE_OOM_TEST)
    message(STATUS "OOM testing enabled")
    add_definitions(-DTEST_OOM)
endif()

# 查找 clang-format
find_program(CLANG_FORMAT_EXECUTABLE clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXECUTABLE}")
    
    add_custom_target(format ALL
        COMMAND ${CMAKE_COMMAND}
            -DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/format.cmake
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting C/H files with clang-format..."
    )

    # 设置 clang-format 选项
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")
endif()

# 查找 doxygen
find_program(DOXYGEN_EXECUTABLE doxygen)
find_program(CURL_EXECUTABLE curl)
if(DOXYGEN_EXECUTABLE)
    set(MERMAID_JS "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets/mermaid.min.js")
    set(MERMAID_URL "https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js")
    if(CURL_EXECUTABLE)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets"
            COMMAND ${CURL_EXECUTABLE} -fsSL --connect-timeout 10 --retry 3 -o "${MERMAID_JS}" "${MERMAID_URL}"
            COMMAND ${CMAKE_COMMAND} -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" -DBIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" -DCSILK_VERSION="${CSILK_VERSION}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-doxyfile.cmake
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (Mermaid diagrams enabled)"
        )
        message(STATUS "Found curl: ${CURL_EXECUTABLE}")
    else()
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -DSRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}" -DBIN_DIR="${CMAKE_CURRENT_BINARY_DIR}" -DCSILK_VERSION="${CSILK_VERSION}" -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen-doxyfile.cmake
            COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (curl not found, Mermaid may be disabled)"
        )
        message(WARNING "curl not found — Mermaid diagrams may not render in Doxygen output")
    endif()
endif()

include(FetchContent)

# Fetch libuv
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.48.0
)
FetchContent_MakeAvailable(libuv)

# Prefer system llhttp; fallback to FetchContent from source
find_library(LLHTTP_LIB NAMES llhttp libllhttp)
if(NOT LLHTTP_LIB)
  message(STATUS "System llhttp not found — fetching from source")
  FetchContent_Declare(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp.git
    GIT_TAG        release/v9.4.1
  )
  set(LLHTTP_BUILD_STATIC_LIBS ON)
  FetchContent_MakeAvailable(llhttp)
  # llhttp v9.4+ creates llhttp_shared / llhttp_static; alias llhttp::llhttp
  # points to whichever was defined last (static when both are built).
  set(LLHTTP_LIB llhttp::llhttp)
  if(llhttp_SOURCE_DIR)
    include_directories(${llhttp_SOURCE_DIR}/include)
  endif()
else()
  message(STATUS "Found system llhttp: ${LLHTTP_LIB}")
endif()

# Fetch cJSON
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(CJSON_INSTALL OFF CACHE BOOL "" FORCE)
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG v1.7.18
)
FetchContent_MakeAvailable(cjson)

# Fetch nghttp2
set(ENABLE_LIB_ONLY ON CACHE BOOL "" FORCE)
set(ENABLE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC_LIB ON CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  nghttp2
  GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
  GIT_TAG        v1.61.0
)
FetchContent_MakeAvailable(nghttp2)
if(nghttp2_SOURCE_DIR)
  include_directories(${nghttp2_SOURCE_DIR}/lib/includes)
  include_directories(${nghttp2_BINARY_DIR}/lib/includes)
endif()

# Find CURL (>= 7.80.0 for HTTP/2 support)
find_package(CURL 7.80.0 REQUIRED)

include_directories(include)
include_directories(src)   # for internal headers (ctx_types.h moved from include/)
include_directories(${CMAKE_CURRENT_BINARY_DIR})   # for generated version.h
include_directories(${cjson_SOURCE_DIR})
include_directories(${CURL_INCLUDE_DIRS})

# Generate version.h from CMake variable
configure_file(
  include/csilk/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/csilk/version.h
  @ONLY
)

# Find MySQL (optional)
find_library(MYSQL_LIB NAMES mysqlclient libmysqlclient)
if(MYSQL_LIB)
  message(STATUS "Found MySQL client library: ${MYSQL_LIB}")
  find_path(MYSQL_INCLUDE_DIR mysql.h PATHS /usr/include/mysql /usr/local/include/mysql)
  if(MYSQL_INCLUDE_DIR)
    include_directories(${MYSQL_INCLUDE_DIR})
    message(STATUS "Found MySQL include dir: ${MYSQL_INCLUDE_DIR}")
  endif()
else()
  message(STATUS "MySQL client library not found — mysql driver will not build")
endif()

# Find PostgreSQL (optional)
find_library(PQ_LIB NAMES pq libpq)
if(PQ_LIB)
  message(STATUS "Found PostgreSQL client library: ${PQ_LIB}")
  find_path(PQ_INCLUDE_DIR libpq-fe.h PATHS /usr/include/postgresql /usr/local/include/postgresql)
  if(PQ_INCLUDE_DIR)
    include_directories(${PQ_INCLUDE_DIR})
    message(STATUS "Found PostgreSQL include dir: ${PQ_INCLUDE_DIR}")
  endif()
else()
  message(STATUS "PostgreSQL client library not found — postgres driver will not build")
endif()

# Find MongoDB (optional)
find_library(MONGOC_LIB NAMES mongoc-1.0)
find_library(BSON_LIB NAMES bson-1.0)
if(MONGOC_LIB AND BSON_LIB)
  message(STATUS "Found MongoDB client library: ${MONGOC_LIB}")
  find_path(MONGOC_INCLUDE_DIR mongoc/mongoc.h PATHS /usr/include/libmongoc-1.0 /usr/local/include/libmongoc-1.0)
  find_path(BSON_INCLUDE_DIR bson/bson.h PATHS /usr/include/libbson-1.0 /usr/local/include/libbson-1.0)
  if(MONGOC_INCLUDE_DIR AND BSON_INCLUDE_DIR)
    include_directories(${MONGOC_INCLUDE_DIR})
    include_directories(${BSON_INCLUDE_DIR})
    message(STATUS "Found MongoDB include dir: ${MONGOC_INCLUDE_DIR}")
    set(HAS_MONGODB TRUE)
  endif()
else()
  message(STATUS "MongoDB client library not found — mongodb driver will not build")
endif()

# Source files — organized per module in cmake/sources.cmake
include(cmake/sources.cmake)

# Conditional driver sources
if(MYSQL_LIB AND MYSQL_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/mysql.c)
  add_definitions(-DHAS_MYSQL)
endif()

if(PQ_LIB AND PQ_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/postgres.c)
  add_definitions(-DHAS_POSTGRES)
endif()

if(HAS_MONGODB)
  list(APPEND CSILK_SOURCES src/drivers/mongodb.c)
  add_definitions(-DHAS_MONGODB)
endif()

# Find Redis (hiredis) (optional)
find_library(HIREDIS_LIB NAMES hiredis)
if(HIREDIS_LIB)
  message(STATUS "Found hiredis library: ${HIREDIS_LIB}")
  find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/include /usr/local/include)
  if(HIREDIS_INCLUDE_DIR)
    include_directories(${HIREDIS_INCLUDE_DIR})
    message(STATUS "Found hiredis include dir: ${HIREDIS_INCLUDE_DIR}")
    list(APPEND CSILK_SOURCES src/drivers/redis.c)
    add_definitions(-DHAS_REDIS)
  endif()
else()
  message(STATUS "hiredis library not found — redis driver will not build")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Find system libyaml (>= 0.2.0)
find_library(YAML_LIB NAMES yaml libyaml REQUIRED)
if(NOT YAML_LIB)
  message(FATAL_ERROR "libyaml library not found. Install libyaml-dev (>= 0.2.0)")
endif()
message(STATUS "Found libyaml: ${YAML_LIB}")

# Find system zlib (>= 1.2.0)
find_library(ZLIB_LIB NAMES z libz REQUIRED)
if(NOT ZLIB_LIB)
  message(FATAL_ERROR "zlib library not found. Install zlib1g-dev (>= 1.2.0)")
endif()
message(STATUS "Found zlib: ${ZLIB_LIB}")

# Find system sqlite3 (>= 3.20.0)
find_library(SQLITE3_LIB NAMES sqlite3 REQUIRED)
if(NOT SQLITE3_LIB)
  message(FATAL_ERROR "sqlite3 library not found. Install libsqlite3-dev (>= 3.20.0)")
endif()
message(STATUS "Found sqlite3: ${SQLITE3_LIB}")

# Find OpenSSL (>= 1.1.1 for TLS 1.3)
find_package(OpenSSL 1.1.1 REQUIRED)
message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")

# Option to also build as shared library
option(CSILK_BUILD_SHARED "Build shared library (in addition to static)" OFF)

# Build static library
add_library(csilk STATIC ${CSILK_SOURCES})
set_target_properties(csilk PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(csilk PUBLIC uv cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLITE3_LIB} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB} nghttp2)
if(MYSQL_LIB)
  target_link_libraries(csilk PRIVATE ${MYSQL_LIB})
endif()
if(PQ_LIB)
  target_link_libraries(csilk PRIVATE ${PQ_LIB})
endif()
if(HIREDIS_LIB)
  target_link_libraries(csilk PRIVATE ${HIREDIS_LIB})
endif()
target_include_directories(csilk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
)
target_compile_definitions(csilk PRIVATE
    CSILK_SWAGGER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui"
)
# Download Swagger UI distribution at configure time (not tracked in git).
# Only the 3 essential runtime files are fetched (~1.2 MB total);
# custom index.css stays in the repository.
# Primary CDN: jsdelivr; fallback: unpkg.
set(SWAGGER_UI_VERSION "5.17.14")
set(SWAGGER_UI_CDN "https://cdn.jsdelivr.net/npm/swagger-ui-dist@${SWAGGER_UI_VERSION}")
set(SWAGGER_UI_FALLBACK "https://unpkg.com/swagger-ui-dist@${SWAGGER_UI_VERSION}")
foreach(swagger_file swagger-ui.css swagger-ui-bundle.js swagger-ui-standalone-preset.js)
  set(swagger_dest "${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui/${swagger_file}")
  if(NOT EXISTS "${swagger_dest}")
    file(DOWNLOAD "${SWAGGER_UI_CDN}/${swagger_file}" "${swagger_dest}"
         STATUS swagger_dl_status
         TIMEOUT 15)
    list(GET swagger_dl_status 0 swagger_dl_code)
    if(NOT swagger_dl_code EQUAL 0)
      message(WARNING "jsdelivr failed for ${swagger_file}, trying unpkg...")
      file(DOWNLOAD "${SWAGGER_UI_FALLBACK}/${swagger_file}" "${swagger_dest}"
           STATUS swagger_dl_status
           TIMEOUT 30)
      list(GET swagger_dl_status 0 swagger_dl_code)
    endif()
    if(NOT swagger_dl_code EQUAL 0)
      message(WARNING "Failed to download ${swagger_file} — Swagger UI will be unavailable at runtime")
    else()
      message(STATUS "Downloaded ${swagger_file}")
    endif()
  endif()
endforeach()
set_target_properties(csilk PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
    VERSION "${CSILK_VERSION}"
    SOVERSION "${CSILK_VERSION_MAJOR}"
    OUTPUT_NAME csilk
)

# Precompiled Headers (PCH) — accelerate builds by pre-including stable headers
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
    target_precompile_headers(csilk PRIVATE
        <stdio.h>
        <stdlib.h>
        <string.h>
        <stdint.h>
        <time.h>
        <errno.h>
    )
endif()

# Build shared library if requested
if(CSILK_BUILD_SHARED)
    add_library(csilk_shared SHARED ${CSILK_SOURCES})
    target_link_libraries(csilk_shared PUBLIC uv cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads ${SQLITE3_LIB} ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB} nghttp2)
    if(MYSQL_LIB)
      target_link_libraries(csilk_shared PRIVATE ${MYSQL_LIB})
    endif()
    if(PQ_LIB)
      target_link_libraries(csilk_shared PRIVATE ${PQ_LIB})
    endif()
    if(HIREDIS_LIB)
      target_link_libraries(csilk_shared PRIVATE ${HIREDIS_LIB})
    endif()
    target_include_directories(csilk_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
    )
    set_target_properties(csilk_shared PROPERTIES
        PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
        VERSION "${CSILK_VERSION}"
        SOVERSION "${CSILK_VERSION_MAJOR}"
        OUTPUT_NAME csilk
    )
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_link_options(csilk_shared PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libcsilk.map")
    endif()
    target_compile_definitions(csilk_shared PRIVATE
        CSILK_SWAGGER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui"
    )
    message(STATUS "Shared library build enabled: libcsilk.so")
endif()

# Only build tests and examples when csilk is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Tests — organized per module in cmake/tests.cmake
  include(cmake/tests.cmake)

  # test_timeout needs an extended CTest timeout
  add_csilk_test(test_timeout tests/test_timeout.c)
  set_tests_properties(test_timeout PROPERTIES TIMEOUT 10)
  list(APPEND CSILK_ALL_TEST_NAMES test_timeout)

  # test_multi_worker needs a longer timeout for multi-threaded requests
  set_tests_properties(test_multi_worker PROPERTIES TIMEOUT 30)

  # Concurrent scenario tests involve thread spawning or multiple streams
  set_tests_properties(test_mq_concurrent PROPERTIES TIMEOUT 30)
  set_tests_properties(test_ws_concurrent PROPERTIES TIMEOUT 15)
  set_tests_properties(test_sse_concurrent PROPERTIES TIMEOUT 15)

  if(ENABLE_OOM_TEST)
      add_executable(test_oom tests/test_oom.c)
      target_compile_definitions(test_oom PRIVATE TEST_OOM)
      target_link_libraries(test_oom csilk pthread)
      add_test(NAME test_oom COMMAND test_oom)

      add_executable(test_oom_io tests/test_oom_io.c)
      target_compile_definitions(test_oom_io PRIVATE TEST_OOM)
      target_link_libraries(test_oom_io csilk pthread)
      add_test(NAME test_oom_io COMMAND test_oom_io)
  endif()

  option(USE_FUZZER "Enable libFuzzer" OFF)
  if(USE_FUZZER)
      add_executable(fuzz_test tests/fuzz_test.c)
      target_compile_options(fuzz_test PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_test csilk)

      add_executable(fuzz_yaml tests/fuzz_yaml.c)
      target_compile_options(fuzz_yaml PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_yaml PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_yaml csilk)

      add_executable(fuzz_url tests/fuzz_url.c)
      target_compile_options(fuzz_url PRIVATE -fsanitize=fuzzer -std=c23)
      target_link_options(fuzz_url PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_url csilk)
  endif()

  # Examples
  add_executable(example_server examples/example_server.c)
  target_link_libraries(example_server csilk)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config_multi.yaml ${CMAKE_CURRENT_BINARY_DIR}/config.yaml COPYONLY)

  add_executable(example_app examples/example_app.c)
  target_link_libraries(example_app csilk)

  add_executable(example_ai examples/example_ai.c)
  target_link_libraries(example_ai csilk)

  add_executable(example_ai_providers examples/example_ai_providers.c)
  target_link_libraries(example_ai_providers csilk)

  add_executable(example_ai_workflow examples/example_ai_workflow.c)
  target_link_libraries(example_ai_workflow csilk)

  add_executable(example_workflow_ui examples/example_workflow_ui.c)
  target_link_libraries(example_workflow_ui csilk)

  add_executable(example_admin_dashboard examples/example_admin_dashboard.c)
  target_link_libraries(example_admin_dashboard csilk)

  add_executable(example_db examples/example_db.c)
  target_link_libraries(example_db csilk)

  add_executable(example_ws_tls_mq examples/example_ws_tls_mq.c)
  target_link_libraries(example_ws_tls_mq csilk)

  add_executable(example_websocket examples/example_websocket.c)
  target_link_libraries(example_websocket csilk)

  add_executable(example_sse examples/example_sse.c)
  target_link_libraries(example_sse csilk)

  add_executable(example_tls examples/example_tls.c)
  target_link_libraries(example_tls csilk)

  add_executable(example_custom_driver examples/example_custom_driver.c)
  target_link_libraries(example_custom_driver csilk)

  # Scaffold Tool (TUI)
  file(GLOB SCAFFOLD_SOURCES tools/csilk-scaffold/*.c)
  if(SCAFFOLD_SOURCES)
      add_executable(csilk-scaffold ${SCAFFOLD_SOURCES})
      target_include_directories(csilk-scaffold PRIVATE tools/csilk-scaffold)
      message(STATUS "Built scaffold tool: csilk-scaffold")
  endif()
endif()

# Add run_tests custom target
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS csilk ${CSILK_ALL_TEST_NAMES}
    COMMENT "Running all tests with ctest"
)

if(USE_COVERAGE)
    find_program(GCOVR_EXECUTABLE gcovr)
    if(GCOVR_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR}
                --filter "src/"
                --exclude "tests/"
                --exclude "examples/"
                --exclude "build_release/"
                --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
                --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
                --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS run_tests
            COMMENT "Generating code coverage report"
        )
        add_custom_target(coverage_summary
            COMMAND ${GCOVR_EXECUTABLE} --root ${CMAKE_SOURCE_DIR}
                --filter "src/"
                --exclude "tests/"
                --exclude "examples/"
                --exclude "build_release/"
                --print-summary
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            DEPENDS run_tests
            COMMENT "Printing coverage summary"
        )
    else()
        message(WARNING "gcovr not found — install with 'pip install gcovr' for coverage reports")
        add_custom_target(coverage
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "gcovr not found — tests run without coverage report"
        )
    endif()
endif()


# Install targets
include(GNUInstallDirs)
install(TARGETS csilk
    EXPORT csilk-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CSILK_BUILD_SHARED)
    install(TARGETS csilk_shared
        EXPORT csilk-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY share/swagger-ui/ DESTINATION ${CMAKE_INSTALL_DATADIR}/csilk/swagger-ui)

install(EXPORT csilk-targets
    FILE csilk-config.cmake
    NAMESPACE csilk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/csilk
)

# Build-tree export disabled: csilk's FetchContent dependencies (uv, cjson)
# would also need export sets. Use add_subdirectory with CSILK_SOURCE_DIR
# or install csilk and use find_package instead.
# export(EXPORT csilk-targets FILE "${CMAKE_BINARY_DIR}/cmake/csilk-config.cmake" NAMESPACE csilk::)

# clang-tidy
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
    set(CSILK_TIDY_CHECKS "clang-analyzer-*,performance-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-performance-no-int-to-ptr")
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE}
            --quiet
            --checks=-*,${CSILK_TIDY_CHECKS}
            --warnings-as-errors="*"
            --extra-arg=-Wno-everything
            ${CSILK_SOURCES}
            -- -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -I${CMAKE_CURRENT_BINARY_DIR}
            -I${cjson_SOURCE_DIR}
            -I${cjson_BINARY_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on source files"
    )
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
else()
    message(STATUS "clang-tidy not found — skipping")
endif()

# Mermaid diagram validation
find_program(PYTHON3_EXECUTABLE python3)
if(PYTHON3_EXECUTABLE)
    add_custom_target(check-mermaid
        COMMAND ${PYTHON3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mermaid.py
            ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Validating Mermaid diagrams in documentation"
    )
    message(STATUS "Found python3: ${PYTHON3_EXECUTABLE}")
else()
    message(STATUS "python3 not found — skipping Mermaid validation target")
endif()

# Benchmark target
add_custom_target(bench
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_benchmarks.sh --save
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running performance benchmarks with wrk"
)

# Profiling target (perf + FlameGraph)
add_custom_target(profile
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/profile.sh --duration 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Profiling example_server with perf + FlameGraph"
)

# CPack packaging
set(CPACK_PACKAGE_NAME "csilk")
set(CPACK_PACKAGE_VERSION "${CSILK_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Csilk")
include(CPack)
