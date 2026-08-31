# cmake/install.cmake — install targets, CMake package config, pkg-config, and CPack packaging

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CSILK_INSTALL_TARGETS
    csilk
    csilk_base
    csilk_crypto
    csilk_core
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
)
if(CSILK_USE_URING)
    list(APPEND CSILK_INSTALL_TARGETS uring)
endif()
if(TARGET yyjson)
    list(APPEND CSILK_INSTALL_TARGETS yyjson)
endif()
if(TARGET csilk_llhttp)
    list(APPEND CSILK_INSTALL_TARGETS csilk_llhttp)
endif()
if(TARGET nghttp2_static)
    list(APPEND CSILK_INSTALL_TARGETS nghttp2_static)
endif()

install(TARGETS ${CSILK_INSTALL_TARGETS}
    EXPORT csilk-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(CSILK_BUILD_SHARED)
    set(CSILK_SHARED_INSTALL_TARGETS
        csilk_shared
        csilk_base_shared
        csilk_crypto_shared
        csilk_core_shared
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
        csilk_workflow_shared
    )
    install(TARGETS ${CSILK_SHARED_INSTALL_TARGETS}
        EXPORT csilk-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# version.h is generated at configure time into the binary include/ tree,
# so it must be installed explicitly (the source include/ only holds the .in
# template).
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/csilk/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/csilk)
install(DIRECTORY share/swagger-ui/ DESTINATION ${CMAKE_INSTALL_DATADIR}/csilk/swagger-ui)

# ── CMake Package Configuration (csilk-targets.cmake & csilk-config.cmake) ──
install(EXPORT csilk-targets
    FILE csilk-targets.cmake
    NAMESPACE csilk::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/csilk
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/csilk-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/csilk"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config-version.cmake"
    VERSION "${CSILK_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/csilk-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/csilk"
)

# ── pkg-config (.pc files) ──────────────────────────────────────────────
# Base
set(CSILK_BASE_PC_LIBS_PRIVATE "")
if(NOT APPLE AND NOT WIN32)
  string(APPEND CSILK_BASE_PC_LIBS_PRIVATE "-lm -lpthread")
endif()

# Core
set(CSILK_CORE_PC_REQUIRES_PRIVATE "")
if(CSILK_USE_URING)
  set(CSILK_CORE_PC_REQUIRES_PRIVATE "liburing")
else()
  set(CSILK_CORE_PC_REQUIRES_PRIVATE "libuv")
endif()
string(APPEND CSILK_CORE_PC_REQUIRES_PRIVATE " libcrypto yaml-0.1")

set(CSILK_CORE_PC_LIBS_PRIVATE "")
if(CSILK_USE_URING)
  string(APPEND CSILK_CORE_PC_LIBS_PRIVATE " -luring")
else()
  string(APPEND CSILK_CORE_PC_LIBS_PRIVATE " -luv")
endif()
string(APPEND CSILK_CORE_PC_LIBS_PRIVATE " -lcrypto -lyaml")
if(NOT APPLE AND NOT WIN32)
  string(APPEND CSILK_CORE_PC_LIBS_PRIVATE " -lm -lpthread")
endif()

# JSON
set(CSILK_JSON_PC_REQUIRES "")
set(CSILK_JSON_PC_LIBS_PRIVATE "-lyyjson")

# Crypto
set(CSILK_CRYPTO_PC_REQUIRES "csilk-base openssl")
set(CSILK_CRYPTO_PC_LIBS_PRIVATE "-lcrypto")

# Runtime
set(CSILK_RUNTIME_PC_REQUIRES "csilk-base csilk-crypto csilk-json")
set(CSILK_RUNTIME_PC_REQUIRES_PRIVATE "")
set(CSILK_RUNTIME_PC_LIBS_PRIVATE "-luv -lyaml -lpthread -lm")
if(CSILK_USE_URING)
  set(CSILK_RUNTIME_PC_REQUIRES "csilk-base csilk-crypto csilk-json liburing")
  set(CSILK_RUNTIME_PC_LIBS_PRIVATE "-luring -lyaml -lpthread -lm")
endif()

# Protocols
set(CSILK_PROTOCOLS_PC_REQUIRES "csilk-http csilk-http2 csilk-reflection csilk-workflow csilk-mq")
set(CSILK_PROTOCOLS_PC_REQUIRES_PRIVATE "")
set(CSILK_PROTOCOLS_PC_LIBS_PRIVATE "")

# Middleware
set(CSILK_MIDDLEWARE_PC_REQUIRES "csilk-http csilk-tls")
set(CSILK_MIDDLEWARE_PC_REQUIRES_PRIVATE "zlib")
set(CSILK_MIDDLEWARE_PC_LIBS_PRIVATE "-lz -lcrypto -lpthread")

# Permission
set(CSILK_PERMISSION_PC_REQUIRES "csilk-runtime csilk-json")
set(CSILK_PERMISSION_PC_REQUIRES_PRIVATE "")
set(CSILK_PERMISSION_PC_LIBS_PRIVATE "")

# Reflection
set(CSILK_REFLECTION_PC_REQUIRES "csilk-runtime csilk-json")
set(CSILK_REFLECTION_PC_REQUIRES_PRIVATE "")
set(CSILK_REFLECTION_PC_LIBS_PRIVATE "")

# Application
set(CSILK_APP_PC_REQUIRES "csilk-http csilk-protocols csilk-middleware csilk-reflection csilk-db csilk-ai")
set(CSILK_APP_PC_REQUIRES_PRIVATE "")
set(CSILK_APP_PC_LIBS_PRIVATE "")

# Vector
set(CSILK_VECTOR_PC_REQUIRES "csilk-runtime csilk-json")
set(CSILK_VECTOR_PC_REQUIRES_PRIVATE "libcurl")
set(CSILK_VECTOR_PC_LIBS_PRIVATE "-lcurl")

# WASM
set(CSILK_WASM_PC_REQUIRES "csilk-core")
set(CSILK_WASM_PC_LIBS_PRIVATE "")

# Bypass
set(CSILK_BYPASS_PC_REQUIRES "csilk-core")
set(CSILK_BYPASS_PC_LIBS_PRIVATE "")

# TLS
set(CSILK_TLS_PC_REQUIRES_PRIVATE "openssl")
set(CSILK_TLS_PC_LIBS_PRIVATE "-lssl -lcrypto")

# MQ
set(CSILK_MQ_PC_REQUIRES_PRIVATE "")
set(CSILK_MQ_PC_LIBS_PRIVATE "")

# HTTP/2
set(CSILK_HTTP2_PC_REQUIRES_PRIVATE "libnghttp2")
set(CSILK_HTTP2_PC_LIBS_PRIVATE "-lnghttp2")

# HTTP
set(CSILK_HTTP_PC_REQUIRES "csilk-core csilk-tls csilk-http2 csilk-mq")
set(CSILK_HTTP_PC_REQUIRES_PRIVATE "zlib")
set(CSILK_HTTP_PC_LIBS_PRIVATE "-lz -lssl -lcrypto")
if(LLHTTP_LIB)
  string(APPEND CSILK_HTTP_PC_LIBS_PRIVATE " -lllhttp")
else()
  string(APPEND CSILK_HTTP_PC_LIBS_PRIVATE " -lcsilk-llhttp")
endif()

# DB
set(CSILK_DB_PC_REQUIRES_PRIVATE "sqlite3")
set(CSILK_DB_PC_LIBS_PRIVATE "-lsqlite3")
if(CSILK_HAS_MYSQL)
  string(APPEND CSILK_DB_PC_LIBS_PRIVATE " -lmysqlclient")
endif()
if(CSILK_HAS_POSTGRES)
  string(APPEND CSILK_DB_PC_REQUIRES_PRIVATE " libpq")
  string(APPEND CSILK_DB_PC_LIBS_PRIVATE " -lpq")
endif()
if(CSILK_HAS_REDIS)
  string(APPEND CSILK_DB_PC_REQUIRES_PRIVATE " hiredis")
  string(APPEND CSILK_DB_PC_LIBS_PRIVATE " -lhiredis")
endif()
if(CSILK_HAS_MONGODB)
  string(APPEND CSILK_DB_PC_REQUIRES_PRIVATE " libmongoc-1.0")
  string(APPEND CSILK_DB_PC_LIBS_PRIVATE " -lmongoc-1.0 -lbson-1.0")
endif()

# AI
set(CSILK_AI_PC_REQUIRES_PRIVATE "libcurl")
set(CSILK_AI_PC_LIBS_PRIVATE "-lcurl")

# Workflow
set(CSILK_WORKFLOW_PC_REQUIRES_PRIVATE "yaml-0.1")
set(CSILK_WORKFLOW_PC_LIBS_PRIVATE "-lyaml")

# Umbrella static closure. Requires lists pull the modular CSilk archives;
# Libs.private supplies implementation dependencies for --static consumers.
set(CSILK_PC_REQUIRES "csilk-http csilk-permission csilk-db csilk-workflow csilk-wasm")
set(CSILK_PC_LIBS_PRIVATE "-lyyjson -lssl -lcrypto -lz -lnghttp2 -lcurl -lsqlite3 -lyaml -luv -lpthread -lm")
if(LLHTTP_LIB)
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lllhttp")
else()
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lcsilk-llhttp")
endif()

set(CSILK_PC_FILES
    csilk-base
    csilk-core
    csilk-json
    csilk-crypto
    csilk-runtime
    csilk-wasm
    csilk-bypass
    csilk-tls
    csilk-mq
    csilk-http2
    csilk-http
    csilk-protocols
    csilk-middleware
    csilk-reflection
    csilk-permission
    csilk-app
    csilk-db
    csilk-vector
    csilk-ai
    csilk-workflow
    csilk
)

set(CSILK_GENERATED_PC_FILES "")
foreach(pc_mod ${CSILK_PC_FILES})
  configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkgconfig/${pc_mod}.pc.in"
      "${CMAKE_CURRENT_BINARY_DIR}/${pc_mod}.pc"
      @ONLY
  )
  list(APPEND CSILK_GENERATED_PC_FILES "${CMAKE_CURRENT_BINARY_DIR}/${pc_mod}.pc")
endforeach()

install(FILES ${CSILK_GENERATED_PC_FILES}
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)

# ── CPack Packaging ───────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "csilk")
set(CPACK_PACKAGE_VERSION "${CSILK_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A lightweight C HTTP framework inspired by Csilk")
include(CPack)
