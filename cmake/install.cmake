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
    csilk_http
    csilk_tls
    csilk_http2
    csilk_protocols
    csilk_middleware
    csilk_reflection
    csilk_app
)
foreach(_opt_mod
    csilk_h3
    csilk_mcp
    csilk_grpc
    csilk_wasm
    csilk_bypass
    csilk_permission
    csilk_db
    csilk_vector
    csilk_ai
    csilk_mq
    csilk_workflow
    csilk_observability
)
  if(TARGET ${_opt_mod})
    list(APPEND CSILK_INSTALL_TARGETS ${_opt_mod})
  endif()
endforeach()
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
        csilk_http_shared
        csilk_tls_shared
        csilk_http2_shared
        csilk_protocols_shared
        csilk_middleware_shared
        csilk_reflection_shared
        csilk_app_shared
    )
    foreach(_opt_mod_shared
        csilk_h3_shared
        csilk_mcp_shared
        csilk_grpc_shared
        csilk_wasm_shared
        csilk_bypass_shared
        csilk_permission_shared
        csilk_db_shared
        csilk_vector_shared
        csilk_ai_shared
        csilk_mq_shared
        csilk_workflow_shared
        csilk_observability_shared
    )
      if(TARGET ${_opt_mod_shared})
        list(APPEND CSILK_SHARED_INSTALL_TARGETS ${_opt_mod_shared})
      endif()
    endforeach()
    install(TARGETS ${CSILK_SHARED_INSTALL_TARGETS}
        EXPORT csilk-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(CSILK_USE_URING AND liburing_SOURCE_DIR AND liburing_BINARY_DIR)
    install(DIRECTORY ${liburing_SOURCE_DIR}/src/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY ${liburing_BINARY_DIR}/src/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
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
set(CSILK_RUNTIME_PC_REQUIRES "csilk-base csilk-crypto csilk-json csilk-reflection")
set(CSILK_RUNTIME_PC_REQUIRES_PRIVATE "")
set(CSILK_RUNTIME_PC_LIBS_PRIVATE "-luv -lyaml -lpthread -lm")
if(CSILK_USE_URING)
  set(CSILK_RUNTIME_PC_REQUIRES "csilk-base csilk-crypto csilk-json csilk-reflection liburing")
  set(CSILK_RUNTIME_PC_LIBS_PRIVATE "-luring -lyaml -lpthread -lm")
endif()

# Protocols
set(CSILK_PROTOCOLS_PC_REQUIRES "csilk-http csilk-http2 csilk-reflection")
if(CSILK_ENABLE_MQ)
  string(APPEND CSILK_PROTOCOLS_PC_REQUIRES " csilk-mq")
endif()
set(CSILK_PROTOCOLS_PC_REQUIRES_PRIVATE "")
set(CSILK_PROTOCOLS_PC_LIBS_PRIVATE "")

# Middleware
set(CSILK_MIDDLEWARE_PC_REQUIRES "csilk-http csilk-tls")
if(CSILK_ENABLE_MQ)
  string(APPEND CSILK_MIDDLEWARE_PC_REQUIRES " csilk-mq")
endif()
set(CSILK_MIDDLEWARE_PC_REQUIRES_PRIVATE "zlib")
set(CSILK_MIDDLEWARE_PC_LIBS_PRIVATE "-lz -lcrypto -lpthread")

# Permission
set(CSILK_PERMISSION_PC_REQUIRES "csilk-runtime csilk-json")
set(CSILK_PERMISSION_PC_REQUIRES_PRIVATE "")
set(CSILK_PERMISSION_PC_LIBS_PRIVATE "")

# Reflection
set(CSILK_REFLECTION_PC_REQUIRES "csilk-base csilk-json")
set(CSILK_REFLECTION_PC_REQUIRES_PRIVATE "")
set(CSILK_REFLECTION_PC_LIBS_PRIVATE "")

# Application
set(CSILK_APP_PC_REQUIRES "csilk-http csilk-protocols csilk-middleware csilk-reflection")
if(CSILK_ENABLE_DB)
  string(APPEND CSILK_APP_PC_REQUIRES " csilk-db")
endif()
if(CSILK_ENABLE_AI)
  string(APPEND CSILK_APP_PC_REQUIRES " csilk-ai")
endif()
if(CSILK_ENABLE_MQ)
  string(APPEND CSILK_APP_PC_REQUIRES " csilk-mq")
endif()
if(CSILK_ENABLE_WASM)
  string(APPEND CSILK_APP_PC_REQUIRES " csilk-wasm")
endif()
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
set(CSILK_HTTP_PC_REQUIRES "csilk-core csilk-tls csilk-http2")
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

# HTTP/3
set(CSILK_H3_PC_REQUIRES "csilk-runtime csilk-http csilk-tls")
set(CSILK_H3_PC_REQUIRES_PRIVATE "openssl")
set(CSILK_H3_PC_LIBS_PRIVATE "-lngtcp2 -lnghttp3")
if(NGTCP2_LIB AND NGHTTP3_LIB)
  string(APPEND CSILK_H3_PC_REQUIRES_PRIVATE " libngtcp2 libnghttp3")
  if(NGTCP2_CRYPTO_OSSL_LIB)
    string(APPEND CSILK_H3_PC_REQUIRES_PRIVATE " libngtcp2_crypto_ossl")
    string(APPEND CSILK_H3_PC_LIBS_PRIVATE " -lngtcp2_crypto_ossl")
  endif()
elseif(CSILK_HAS_NGTCP2)
  string(APPEND CSILK_H3_PC_LIBS_PRIVATE " -lngtcp2_crypto_ossl")
endif()
string(APPEND CSILK_H3_PC_LIBS_PRIVATE " -lssl -lcrypto")

# MCP
set(CSILK_MCP_PC_REQUIRES "csilk-protocols csilk-http csilk-runtime csilk-json csilk-ai")
set(CSILK_MCP_PC_REQUIRES_PRIVATE "libcurl")
set(CSILK_MCP_PC_LIBS_PRIVATE "-lcurl")

# gRPC
set(CSILK_GRPC_PC_REQUIRES "csilk-middleware csilk-http csilk-runtime csilk-json")
set(CSILK_GRPC_PC_REQUIRES_PRIVATE "")
set(CSILK_GRPC_PC_LIBS_PRIVATE "")

# Observability
set(CSILK_OBSERVABILITY_PC_REQUIRES "csilk-app csilk-middleware csilk-http csilk-runtime csilk-json")
if(CSILK_ENABLE_GRPC)
  string(APPEND CSILK_OBSERVABILITY_PC_REQUIRES " csilk-grpc")
endif()
if(CSILK_ENABLE_DB)
  string(APPEND CSILK_OBSERVABILITY_PC_REQUIRES " csilk-db")
endif()
if(CSILK_ENABLE_AI)
  string(APPEND CSILK_OBSERVABILITY_PC_REQUIRES " csilk-ai")
endif()
if(CSILK_ENABLE_MQ)
  string(APPEND CSILK_OBSERVABILITY_PC_REQUIRES " csilk-mq")
endif()
set(CSILK_OBSERVABILITY_PC_REQUIRES_PRIVATE "")
set(CSILK_OBSERVABILITY_PC_LIBS_PRIVATE "")
if(NOT APPLE AND NOT WIN32)
  set(CSILK_OBSERVABILITY_PC_LIBS_PRIVATE "-lpthread")
endif()

# Umbrella static closure. Requires lists pull the enabled CSilk archives;
# Libs.private supplies implementation dependencies for --static consumers.
set(CSILK_PC_REQUIRES "csilk-app csilk-http")
set(CSILK_PC_LIBS_PRIVATE "-lyyjson -lssl -lcrypto -lz -lnghttp2 -lyaml")
set(CSILK_PC_CFLAGS "")
if(CSILK_NEED_CURL)
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lcurl")
endif()
if(CSILK_ENABLE_DB)
  string(APPEND CSILK_PC_REQUIRES " csilk-db")
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lsqlite3")
  if(CSILK_HAS_MYSQL)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -lmysqlclient")
  endif()
  if(CSILK_HAS_POSTGRES)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -lpq")
  endif()
  if(CSILK_HAS_REDIS)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -lhiredis")
  endif()
  if(CSILK_HAS_MONGODB)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -lmongoc-1.0 -lbson-1.0")
  endif()
endif()
if(CSILK_ENABLE_MQ)
  string(APPEND CSILK_PC_REQUIRES " csilk-mq")
endif()
if(CSILK_ENABLE_AI)
  string(APPEND CSILK_PC_REQUIRES " csilk-ai")
endif()
if(CSILK_ENABLE_VECTOR)
  string(APPEND CSILK_PC_REQUIRES " csilk-vector")
endif()
if(CSILK_ENABLE_MCP)
  string(APPEND CSILK_PC_REQUIRES " csilk-mcp")
endif()
if(CSILK_ENABLE_WORKFLOW)
  string(APPEND CSILK_PC_REQUIRES " csilk-workflow")
endif()
if(CSILK_ENABLE_GRPC)
  string(APPEND CSILK_PC_REQUIRES " csilk-grpc")
endif()
if(CSILK_ENABLE_WASM)
  string(APPEND CSILK_PC_REQUIRES " csilk-wasm")
  if(CSILK_HAS_WAMR)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -lvmlib")
  endif()
endif()
if(CSILK_ENABLE_BYPASS)
  string(APPEND CSILK_PC_REQUIRES " csilk-bypass")
endif()
if(CSILK_ENABLE_PERM)
  string(APPEND CSILK_PC_REQUIRES " csilk-permission")
endif()
if(CSILK_ENABLE_HTTP3)
  string(APPEND CSILK_PC_REQUIRES " csilk-h3")
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lngtcp2 -lnghttp3")
  if(NGTCP2_CRYPTO_OSSL_LIB OR NOT NGTCP2_LIB)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -lngtcp2_crypto_ossl")
  endif()
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  string(APPEND CSILK_PC_REQUIRES " csilk-observability")
endif()
if(CSILK_USE_URING)
  string(APPEND CSILK_PC_LIBS_PRIVATE " -luring -lpthread -lm")
else()
  string(APPEND CSILK_PC_LIBS_PRIVATE " -luv -lpthread -lm")
endif()
if(NOT APPLE AND NOT WIN32 AND CMAKE_DL_LIBS)
  foreach(_dl_lib IN LISTS CMAKE_DL_LIBS)
    string(APPEND CSILK_PC_LIBS_PRIVATE " -l${_dl_lib}")
  endforeach()
endif()
if(LLHTTP_LIB)
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lllhttp")
else()
  string(APPEND CSILK_PC_LIBS_PRIVATE " -lcsilk-llhttp")
endif()

foreach(_opt_def IN ITEMS
    CSILK_ENABLE_DB:CSILK_HAS_DB
    CSILK_ENABLE_MQ:CSILK_HAS_MQ
    CSILK_ENABLE_AI:CSILK_HAS_AI
    CSILK_ENABLE_VECTOR:CSILK_HAS_VECTOR
    CSILK_ENABLE_MCP:CSILK_HAS_MCP
    CSILK_ENABLE_WORKFLOW:CSILK_HAS_WORKFLOW
    CSILK_ENABLE_GRPC:CSILK_HAS_GRPC
    CSILK_ENABLE_WASM:CSILK_HAS_WASM
    CSILK_ENABLE_BYPASS:CSILK_HAS_BYPASS
    CSILK_ENABLE_PERM:CSILK_HAS_PERM
    CSILK_ENABLE_HTTP3:CSILK_HAS_HTTP3
    CSILK_ENABLE_OBSERVABILITY:CSILK_HAS_OBSERVABILITY
    CSILK_ENABLE_CSV:CSILK_HAS_CSV
)
  string(REPLACE ":" ";" _pair "${_opt_def}")
  list(GET _pair 0 _opt_var)
  list(GET _pair 1 _macro_name)
  if(${_opt_var})
    string(APPEND CSILK_PC_CFLAGS " -D${_macro_name}=1")
  endif()
endforeach()

set(CSILK_PC_FILES
    csilk-base
    csilk-core
    csilk-json
    csilk-crypto
    csilk-runtime
    csilk-tls
    csilk-http2
    csilk-http
    csilk-protocols
    csilk-middleware
    csilk-reflection
    csilk-app
    csilk
)
if(CSILK_ENABLE_HTTP3)
  list(APPEND CSILK_PC_FILES csilk-h3)
endif()
if(CSILK_ENABLE_MCP)
  list(APPEND CSILK_PC_FILES csilk-mcp)
endif()
if(CSILK_ENABLE_GRPC)
  list(APPEND CSILK_PC_FILES csilk-grpc)
endif()
if(CSILK_ENABLE_WASM)
  list(APPEND CSILK_PC_FILES csilk-wasm)
endif()
if(CSILK_ENABLE_BYPASS)
  list(APPEND CSILK_PC_FILES csilk-bypass)
endif()
if(CSILK_ENABLE_MQ)
  list(APPEND CSILK_PC_FILES csilk-mq)
endif()
if(CSILK_ENABLE_PERM)
  list(APPEND CSILK_PC_FILES csilk-permission)
endif()
if(CSILK_ENABLE_DB)
  list(APPEND CSILK_PC_FILES csilk-db)
endif()
if(CSILK_ENABLE_VECTOR)
  list(APPEND CSILK_PC_FILES csilk-vector)
endif()
if(CSILK_ENABLE_AI)
  list(APPEND CSILK_PC_FILES csilk-ai)
endif()
if(CSILK_ENABLE_WORKFLOW)
  list(APPEND CSILK_PC_FILES csilk-workflow)
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  list(APPEND CSILK_PC_FILES csilk-observability)
endif()

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
