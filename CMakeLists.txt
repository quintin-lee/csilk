# CMakeLists.txt
cmake_minimum_required(VERSION 3.11)
project(c_csilk_framework C)

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
find_program(CURL_EXECUTABLE curl)
if(DOXYGEN_EXECUTABLE)
    set(MERMAID_JS "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets/mermaid.min.js")
    set(MERMAID_URL "https://cdn.jsdelivr.net/npm/mermaid@11/dist/mermaid.min.js")
    if(CURL_EXECUTABLE)
        add_custom_target(docs
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/docs/assets"
            COMMAND ${CURL_EXECUTABLE} -fsSL --connect-timeout 10 --retry 3 -o "${MERMAID_JS}" "${MERMAID_URL}"
            COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating documentation with Doxygen (Mermaid diagrams enabled)"
        )
        message(STATUS "Found curl: ${CURL_EXECUTABLE}")
    else()
        add_custom_target(docs
            COMMAND ${DOXYGEN_EXECUTABLE} Doxyfile
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
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG        v1.7.18
)
FetchContent_MakeAvailable(cjson)

# Find CURL
find_package(CURL REQUIRED)

include_directories(include)
include_directories(${cjson_SOURCE_DIR})
include_directories(${CURL_INCLUDE_DIRS})

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

# Find system libyaml
find_library(YAML_LIB NAMES yaml libyaml REQUIRED)
if(NOT YAML_LIB)
  message(FATAL_ERROR "libyaml library not found. Install libyaml-dev")
endif()
message(STATUS "Found libyaml: ${YAML_LIB}")

# Find system zlib
find_library(ZLIB_LIB NAMES z libz REQUIRED)
if(NOT ZLIB_LIB)
  message(FATAL_ERROR "zlib library not found. Install zlib1g-dev")
endif()
message(STATUS "Found zlib: ${ZLIB_LIB}")

# Find OpenSSL
find_package(OpenSSL REQUIRED)
message(STATUS "Found OpenSSL: ${OPENSSL_VERSION}")

# Option to also build as shared library
option(CSILK_BUILD_SHARED "Build shared library (in addition to static)" OFF)

# Build static library
add_library(csilk STATIC ${CSILK_SOURCES})
target_link_libraries(csilk PUBLIC uv cjson OpenSSL::SSL OpenSSL::Crypto Threads::Threads sqlite3 ${CURL_LIBRARIES} PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB})
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
set_target_properties(csilk PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
    VERSION "0.2.3"
    SOVERSION "0"
    OUTPUT_NAME csilk
)

# Build shared library if requested
if(CSILK_BUILD_SHARED)
    add_library(csilk_shared SHARED ${CSILK_SOURCES})
    target_link_libraries(csilk_shared PUBLIC uv cjson Threads::Threads sqlite3 PRIVATE ${LLHTTP_LIB} ${YAML_LIB} ${ZLIB_LIB})
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
        VERSION "0.2.3"
        SOVERSION "0"
        OUTPUT_NAME csilk
    )
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

  if(ENABLE_OOM_TEST)
      add_executable(test_oom tests/test_oom.c)
      target_compile_definitions(test_oom PRIVATE TEST_OOM)
      target_link_libraries(test_oom csilk pthread)
      add_test(NAME test_oom COMMAND test_oom)
  endif()

  option(USE_FUZZER "Enable libFuzzer" OFF)
  if(USE_FUZZER)
      add_executable(fuzz_test tests/fuzz_test.c)
      target_compile_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_test csilk)
  endif()

  # Examples
  add_executable(example_server examples/example_server.c)
  target_link_libraries(example_server csilk)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config_multi.yaml ${CMAKE_CURRENT_BINARY_DIR}/config.yaml COPYONLY)

  add_executable(example_app examples/example_app.c)
  target_link_libraries(example_app csilk)

  add_executable(example_ai examples/example_ai.c)
  target_link_libraries(example_ai csilk)

  add_executable(example_ai_workflow examples/example_ai_workflow.c)
  target_link_libraries(example_ai_workflow csilk)

  add_executable(example_workflow_ui examples/example_workflow_ui.c)
  target_link_libraries(example_workflow_ui csilk)

  add_executable(example_admin_dashboard examples/example_admin_dashboard.c)
  target_link_libraries(example_admin_dashboard csilk)

  add_executable(example_db examples/example_db.c)
  target_link_libraries(example_db csilk)
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
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE}
            --quiet
            --checks="-*,clang-analyzer-*,performance-*,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling"
            --warnings-as-errors="*"
            ${CSILK_SOURCES}
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

# CPack packaging
set(CPACK_PACKAGE_NAME "csilk")
set(CPACK_PACKAGE_VERSION "0.2.3")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Csilk")
include(CPack)
