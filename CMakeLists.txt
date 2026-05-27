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

# Source files
set(CSILK_SOURCES
    src/core/context.c
    src/core/router.c
    src/app/group.c
    src/core/server.c
    src/core/url.c
    src/core/arena.c
    src/core/logger.c
    src/data/db.c
    src/app/app.c
    src/core/config.c
    src/reflection/reflect.c
    src/core/utils.c
    src/protocols/websocket.c
    src/messaging/mq.c
    src/middleware/logger.c
    src/middleware/recovery.c
    src/middleware/auth.c
    src/middleware/static.c
    src/middleware/cors.c
    src/middleware/ratelimit.c
    src/middleware/csrf.c
    src/middleware/sse.c
    src/middleware/gzip.c
    src/middleware/jwt.c
    src/middleware/multipart.c
    src/middleware/request_id.c
    src/middleware/session.c
    src/middleware/validate.c
    src/middleware/metrics.c
    src/protocols/swagger.c
    src/security/perm.c
    src/crypto/cipher.c
    src/ai/ai.c
    src/drivers/sqlite.c
    src/drivers/perm_simple.c
    src/drivers/ai_openai.c
    src/drivers/ai_ollama.c
    src/app/workflow.c
)

if(MYSQL_LIB AND MYSQL_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/mysql.c)
  add_definitions(-DHAS_MYSQL)
endif()

if(PQ_LIB AND PQ_INCLUDE_DIR)
  list(APPEND CSILK_SOURCES src/drivers/postgres.c)
  add_definitions(-DHAS_POSTGRES)
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
    VERSION "0.2.1"
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
    target_include_directories(csilk_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
    )
    set_target_properties(csilk_shared PROPERTIES
        PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/csilk.h"
        VERSION "0.2.1"
        SOVERSION "0"
        OUTPUT_NAME csilk
    )
    target_compile_definitions(csilk_shared PRIVATE
        CSILK_SWAGGER_UI_DIR="${CMAKE_CURRENT_SOURCE_DIR}/share/swagger-ui"
    )
    message(STATUS "Shared library build enabled: libcsilk.so")
endif()

# Helper to add a test executable and register with CTest
function(add_csilk_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} csilk pthread)
  target_compile_options(${name} PRIVATE $<$<CONFIG:Release>:-UNDEBUG>)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Only build tests and examples when csilk is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Unit tests
  add_csilk_test(test_gzip tests/test_gzip.c)
  add_csilk_test(test_logger tests/test_logger.c)
  add_csilk_test(test_static tests/test_static.c)
  add_csilk_test(test_context tests/test_context.c)
  add_csilk_test(test_router tests/test_router.c)
  add_csilk_test(test_radix tests/test_radix.c)
  add_csilk_test(test_group tests/test_group.c)
  add_csilk_test(test_headers tests/test_headers.c)
  add_csilk_test(test_server tests/test_server.c)
  add_csilk_test(test_query tests/test_query.c)
  add_csilk_test(test_json tests/test_json.c)
  add_csilk_test(test_recovery tests/test_recovery.c)
  add_csilk_test(test_auth tests/test_auth.c)
  add_csilk_test(test_ws tests/test_ws.c)
  add_csilk_test(test_config tests/test_config.c)
  add_csilk_test(test_config_validate tests/test_config_validate.c)
  add_csilk_test(test_csrf tests/test_csrf.c)
  add_csilk_test(test_redirect tests/test_redirect.c)
  add_csilk_test(test_cookie tests/test_cookie.c)
  add_csilk_test(test_reflect tests/test_reflect.c)
  add_csilk_test(test_keepalive tests/test_keepalive.c)
  add_csilk_test(test_edge tests/test_edge.c)
  add_csilk_test(test_timeout tests/test_timeout.c)
  add_csilk_test(test_arena tests/test_arena.c)
  add_csilk_test(test_url_decode tests/test_url_decode.c)
  add_csilk_test(test_utils tests/test_utils.c)
  add_csilk_test(test_form tests/test_form.c)
  add_csilk_test(test_ip tests/test_ip.c)
  add_csilk_test(test_cors tests/test_cors.c)
  add_csilk_test(test_multipart tests/test_multipart.c)
  add_csilk_test(test_sse tests/test_sse.c)
  add_csilk_test(test_ratelimit tests/test_ratelimit.c)
  add_csilk_test(test_app tests/test_app.c)
  add_csilk_test(test_swagger tests/test_swagger.c)
  add_csilk_test(test_session tests/test_session.c)
  add_csilk_test(test_validate tests/test_validate.c)
  add_csilk_test(test_jwt tests/test_jwt.c)
  add_csilk_test(test_hooks tests/test_hooks.c)
  add_csilk_test(test_crypto_driver tests/test_crypto_driver.c)
  add_csilk_test(test_cipher tests/test_cipher.c)
  add_csilk_test(test_storage tests/test_storage.c)
  add_csilk_test(test_perm tests/test_perm.c)
  add_csilk_test(test_db tests/test_db.c)
  add_csilk_test(test_ai tests/test_ai.c)
  add_csilk_test(test_extra tests/test_extra.c)
  add_csilk_test(test_metrics tests/test_metrics.c)
  add_csilk_test(test_workflow_lifecycle tests/test_workflow_lifecycle.c)
  add_csilk_test(test_workflow_graph tests/test_workflow_graph.c)
  add_csilk_test(test_mq_integration tests/test_mq_integration.c)
  add_csilk_test(test_mq tests/test_mq.c)
  add_csilk_test(test_mq_persistence tests/test_mq_persistence.c)
  add_csilk_test(test_mq_wal_write tests/test_mq_wal_write.c)
  add_csilk_test(test_mq_recovery tests/test_mq_recovery.c)
  add_csilk_test(test_mq_wal tests/test_mq_wal.c)
  if(ENABLE_OOM_TEST)
      add_executable(test_oom tests/test_oom.c)
      target_compile_definitions(test_oom PRIVATE TEST_OOM)
      target_link_libraries(test_oom csilk pthread)
      add_test(NAME test_oom COMMAND test_oom)
  endif()

  # Extended coverage tests
  add_csilk_test(test_context_ext tests/test_context_ext.c)
  add_csilk_test(test_url_ext tests/test_url_ext.c)
  add_csilk_test(test_utils_ext tests/test_utils_ext.c)
  add_csilk_test(test_cors_ext tests/test_cors_ext.c)
  add_csilk_test(test_recovery_ext tests/test_recovery_ext.c)
  add_csilk_test(test_request_id tests/test_request_id.c)
  add_csilk_test(test_csrf_ext tests/test_csrf_ext.c)
  add_csilk_test(test_perm_ext tests/test_perm_ext.c)
  add_csilk_test(test_logger_ext tests/test_logger_ext.c)
  add_csilk_test(test_app_ext tests/test_app_ext.c)
  add_csilk_test(test_group_ext tests/test_group_ext.c)
  add_csilk_test(test_config_comprehensive tests/test_config_comprehensive.c)
  add_csilk_test(test_session_ext tests/test_session_ext.c)
  add_csilk_test(test_server_ext tests/test_server_ext.c)
  add_csilk_test(test_ai_ext tests/test_ai_ext.c)

  add_csilk_test(test_async_keepalive tests/test_async_keepalive.c)
  add_csilk_test(test_https tests/test_https.c)
  set_tests_properties(test_timeout PROPERTIES TIMEOUT 10)

  option(USE_FUZZER "Enable libFuzzer" OFF)
  if(USE_FUZZER)
      add_executable(fuzz_test tests/fuzz_test.c)
      target_compile_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz_test PRIVATE -fsanitize=fuzzer)
      target_link_libraries(fuzz_test csilk)
  endif()

  # Integration tests
 add_executable(test_integration tests/test_integration.c)
 target_link_libraries(test_integration csilk)
 add_test(NAME test_integration COMMAND test_integration)

 add_executable(test_app_integration tests/test_app_integration.c)
 target_link_libraries(test_app_integration csilk)
 add_test(NAME test_app_integration COMMAND test_app_integration)

  # Example server
  add_executable(example_server examples/example_server.c)
  target_link_libraries(example_server csilk)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config_multi.yaml ${CMAKE_CURRENT_BINARY_DIR}/config.yaml COPYONLY)

  # Easy API example
  add_executable(example_app examples/example_app.c)
  target_link_libraries(example_app csilk)

  # AI example
  add_executable(example_ai examples/example_ai.c)
  target_link_libraries(example_ai csilk)

  # Database example
  add_executable(example_db examples/example_db.c)
  target_link_libraries(example_db csilk)
endif()

# Add run_tests custom target
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS csilk test_gzip test_logger test_static test_context test_router test_radix test_group test_headers test_server test_query test_json test_recovery test_auth test_ws test_config test_config_validate test_cookie test_reflect test_keepalive test_edge test_timeout test_arena test_url_decode test_utils test_form test_ip test_cors test_multipart test_sse test_ratelimit test_app test_csrf test_redirect test_session test_validate test_jwt test_hooks test_crypto_driver test_cipher test_storage test_perm test_db test_ai test_extra test_async_keepalive test_https test_integration test_app_integration test_swagger test_metrics test_workflow_lifecycle test_workflow_graph test_mq_integration test_mq test_mq_persistence test_mq_wal_write test_mq_recovery test_mq_wal test_context_ext test_url_ext test_utils_ext test_cors_ext test_recovery_ext test_request_id test_csrf_ext test_perm_ext test_logger_ext test_app_ext test_group_ext test_config_comprehensive test_session_ext test_server_ext test_ai_ext
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

# CPack packaging
set(CPACK_PACKAGE_NAME "csilk")
set(CPACK_PACKAGE_VERSION "0.2.1")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Csilk")
include(CPack)
