# Tests organized by module.
# This file defines the add_csilk_test helper and registers all test executables.
# Only include when csilk is the top-level project.

# Test helpers are compiled into a test-only support library. They remain out
# of every production module and are linked transitively by test executables.
add_library(csilk_test_support STATIC ${CSILK_TEST_SUPPORT_SOURCES})
add_library(csilk_test_support_shared SHARED ${CSILK_TEST_SUPPORT_SOURCES})
csilk_target_setup(csilk_test_support PRIVATE STATIC)
csilk_target_setup(csilk_test_support_shared PRIVATE SHARED)
target_link_libraries(csilk_test_support PRIVATE csilk)
target_link_libraries(csilk_test_support_shared PRIVATE csilk_runtime csilk_base csilk_http csilk_middleware csilk_app csilk_shared)

# Helper to add a test executable and register with CTest
function(add_csilk_test name source)
  add_executable(${name} ${source})
  if(name STREQUAL "test_pool_economics")
    target_compile_definitions(${name} PRIVATE CSILK_POOL_STATS)
    target_sources(${name} PRIVATE tests/core/pool_stats.c)
  endif()
  # Link csilk_test_support before csilk so the RESCAN static archive group
  # precedes external shared libraries (libuv, OpenSSL, etc.) under --as-needed.
  # CMake releases before 4.x drop transitive INTERFACE libraries referenced only
  # through a $<LINK_GROUP> genex, so link the I/O backend explicitly after csilk.
  if(TARGET csilk::libuv)
    target_link_libraries(${name} csilk_test_support csilk pthread m csilk::libuv)
  elseif(TARGET uring)
    target_link_libraries(${name} csilk_test_support csilk pthread m uring)
  else()
    target_link_libraries(${name} csilk_test_support csilk pthread m)
  endif()
  target_compile_features(${name} PRIVATE c_std_23)
  target_compile_options(${name} PRIVATE
      "${CSILK_COMMON_FLAGS}"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
      "$<$<BOOL:${USE_COVERAGE}>:--coverage;-O0;-g>"
      -UNDEBUG
  )
  target_link_options(${name} PRIVATE
      "$<$<BOOL:${USE_COVERAGE}>:--coverage>"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
  )
  target_compile_definitions(${name} PRIVATE
      "$<$<BOOL:${ENABLE_OOM_TEST}>:TEST_OOM>"
      "$<$<BOOL:${CSILK_HAS_MYSQL}>:HAS_MYSQL>"
      "$<$<BOOL:${CSILK_HAS_POSTGRES}>:HAS_POSTGRES>"
      "$<$<BOOL:${HAS_MONGODB}>:HAS_MONGODB>"
      "$<$<BOOL:${CSILK_HAS_REDIS}>:HAS_REDIS>"
  )
  add_test(NAME ${name} COMMAND ${name})
  if(name STREQUAL "test_performance_model")
    set_tests_properties(${name} PROPERTIES TIMEOUT 600)
  elseif(name STREQUAL "test_logger_async_bench")
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
  elseif(name STREQUAL "test_openai_mock" OR name STREQUAL "test_ai_anthropic" OR name STREQUAL "test_ollama_mock" OR name STREQUAL "test_vector_milvus_qdrant_mock" OR name STREQUAL "test_agent_memory")
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
  elseif(name STREQUAL "test_mcp_stdio_pool")
    # Forks real subprocesses, waits on a TTL boundary and runs RPC round-trips
    # over a genuinely dead pipe: the liveness and eviction assertions need
    # far more than the 10s default budget.
    set_tests_properties(${name} PROPERTIES TIMEOUT 30)
  endif()
endfunction()

# -- Core tests --
set(CSILK_CORE_TESTS
    test_arena
    test_arena_bench
    test_arena_overflow
    test_body_pool
    test_config
    test_config_comprehensive
    test_config_validate
    test_config_load
    test_config_tls
    test_connection
    test_client_lifetime_stress
    test_context
    test_context_ext
    test_context_sizeof
    test_context_reflect_ext
    test_async_op
    test_edge
    test_get_param
    test_headers
    test_https
    test_ip
    test_json
    test_json_as
    test_json_ext
    test_json_accessor_bench
    test_json_mutate
    test_json_ai_request
    test_keepalive
    test_async_keepalive
    test_multi_worker
    test_next_null
    test_params_limit
    test_query
    test_radix
    test_redirect
    test_response
    test_response_envelope
    test_response_ownership
    test_router
    test_middleware_chain
    test_server
    test_server_ext
    test_server_limits
    test_storage
    test_storage_limit
    test_url_decode
    test_url_ext
    test_utils
    test_utils_ext
    test_zerocopy_lifecycle
    test_headers_fragmented
    test_hot_reload
    test_hot_reload_stress
    test_hot_reload_10k_stress
    test_hot_reload_null
    test_dispatch_bench
    test_lfqueue_stress
    test_router_bench
    test_router_ordering
    test_router_iterative_bench
    test_router_simd_fuzz_bench
    test_header_map_bench
    test_codec_prop
    test_hash_prop
    test_simd_router_arena
    test_server_stats_bench
    test_swar_http
    test_http1_zerocopy
    test_ctx_json
    test_io_op_model
    test_h2_stream_bench
    test_pool_economics
    test_performance_model
    test_http1_pipeline_bench
    test_http1_e2e_bench
    test_http1_parser_pmu
    test_logger_async_bench
    test_server_config_race
    test_core_concurrency_stress
    test_atomic_lifecycle
    test_rcu_lifecycle_stress
    test_sendfile_workers
    test_bounded_buf
    test_cpu_hardware_profile
    test_io_backend
    test_io_equivalence
    test_secret
    test_python_abi
    test_hot_update_atomic
    test_lazy_arena
    test_io_poll
)
if(CSILK_USE_URING)
  list(APPEND CSILK_CORE_TESTS
      test_uring_recv_bench
      test_uring_stale_stress
      test_uring_buf
      test_uring_sqpoll
      test_uring_vector
      test_uring_io
      test_uring_fs
  )
endif()
if(CSILK_ENABLE_MQ AND CSILK_ENABLE_AI)
  list(APPEND CSILK_CORE_TESTS test_ctx_lifecycle_async)
endif()

# -- Application tests --
set(CSILK_APP_TESTS
    test_app
    test_app_ext
    test_app_integration
    test_cookie
    test_form
    test_group
    test_group_ext
    test_hooks
    test_hooks_rcu
    test_jsonrpc_frames
)

# -- Workflow tests --
set(CSILK_WORKFLOW_TESTS)
if(CSILK_ENABLE_WORKFLOW)
  set(CSILK_WORKFLOW_TESTS
      test_workflow_agentic
      test_workflow_agent_engine
      test_workflow_agent_multi
      test_workflow_agent_hitl
      test_workflow_budget
      test_workflow_context
      test_workflow_control
      test_workflow_distributed
      test_workflow_dx
      test_workflow_exec
      test_workflow_filters
      test_workflow_graph
      test_workflow_interactive
      test_workflow_jsonpath
      test_workflow_lifecycle
      test_workflow_loader
      test_workflow_monitor
      test_workflow_parallel
      test_workflow_parallel_tools
      test_workflow_persistence
      test_workflow_retry
      test_workflow_schema
      test_workflow_streaming
      test_workflow_timeout
      test_workflow_tools
      test_workflow_tracing
      test_workflow_dsl
      test_workflow_hotreload
      test_wf_cluster_sm
      test_wf_wasm_node
      test_ai_react_loop
      test_agent_memory
      test_wf_tools_monitor
      test_wf_grid
      test_wf_grid_integration
      test_wf_grid_raft
  )
endif()

# -- Middleware tests --
set(CSILK_MIDDLEWARE_TESTS
    test_auth
    test_cors
    test_cors_ext
    test_csrf
    test_csrf_ext
    test_csrf_whitelist
    test_gzip
    test_jwt
    test_logger
    test_logger_ext
    test_metrics
    test_multipart
    test_ratelimit
    test_sliding_ratelimit
    test_ratelimit_distributed
    test_keyed_ratelimit_security_headers
    test_security_headers
    test_recovery
    test_recovery_ext
    test_request_id
    test_session
    test_session_ext
    test_session_store
    test_sse
    test_sse_concurrent
    test_static
    test_circuit_breaker
    test_file
    test_validate
    test_waf
    test_waf_filter
)

# -- Protocol tests --
if(NOT TARGET CURL::libcurl)
  find_package(CURL QUIET)
endif()
set(CSILK_PROTOCOL_TESTS
    test_swagger
    test_ws
    test_ws_room
    test_ws_concurrent
    test_h2
    test_h2_dispatch_lifecycle
    test_h2_header_bench
)

# -- Security tests --
set(CSILK_SECURITY_TESTS
    test_crypto_primitives
    test_jwt_security
    test_uuid
    test_bcrypt
    test_cipher
    test_crypto_driver
    test_crypto
    test_totp
    test_ssrf
)
# Build matching directory list dynamically so ZIP_LISTS never drifts.
set(CSILK_SECURITY_TEST_DIRS
    security;security;security;security;security;security;crypto;crypto;security
)

# -- Data / DB module tests --
set(CSILK_DATA_TESTS)
if(CSILK_ENABLE_DB)
  set(CSILK_DATA_TESTS
      test_db_sqlite
      test_db_registry
      test_db_tx_scope
      test_db_stmt
      test_db_migration
      test_db_pool_concurrent
      test_db_typed_binding
      test_db_error_codes
      test_db_postgres
      test_db_mysql
      test_db_redis
      test_db_abstraction
      test_db_nonblocking
      test_db
      test_mongodb
  )
endif()

# -- AI & Vector module tests --
set(CSILK_AI_TESTS)
if(CSILK_ENABLE_AI)
  list(APPEND CSILK_AI_TESTS
      test_ai
      test_ai_ext
      test_ai_driver_mock
      test_openai_mock
      test_ollama_mock
      test_ai_anthropic
      test_ai_trace
  )
  if(CSILK_ENABLE_OBSERVABILITY)
    list(APPEND CSILK_AI_TESTS test_ai_ctx_trace)
  endif()
  if(CSILK_ENABLE_WORKFLOW)
    list(APPEND CSILK_AI_TESTS test_ai_skill)
  endif()
endif()

set(CSILK_VECTOR_TESTS)
if(CSILK_ENABLE_VECTOR)
  set(CSILK_VECTOR_TESTS
      test_vector_db
      test_vector_simd
      test_vector_hnsw
      test_vector_db_embedded
      test_vector_milvus_qdrant_mock
  )
endif()

# -- Reflection tests --
set(CSILK_REFLECTION_TESTS
    test_reflect
)

# -- Messaging (MQ & Raft) module tests --
set(CSILK_MESSAGING_TESTS)
if(CSILK_ENABLE_MQ)
  set(CSILK_MESSAGING_TESTS
      test_mq
      test_mq_integration
      test_mq_monitor
      test_mq_persistence
      test_mq_recovery
      test_mq_wal
      test_mq_wal_write
      test_mq_concurrent
      test_raft_wal
      test_raft_rpc
      test_raft_consensus
      test_raft_failover
      test_raft_snapshot
      test_raft_membership
      test_wal_uring
  )
endif()

# -- Optional module tests (Observability, WASM, gRPC, HTTP/3, MCP, Perm, CSV) --
set(CSILK_OBSERVABILITY_TESTS)
if(CSILK_ENABLE_OBSERVABILITY)
  set(CSILK_OBSERVABILITY_TESTS
      test_admin
      test_admin_auth
      test_otlp_exporter
      test_otlp_trace_span
      test_apm_dashboard_route
      test_trace_circuit_breaker
  )
  if(CSILK_ENABLE_AI)
    list(APPEND CSILK_OBSERVABILITY_TESTS test_otlp_ai)
  endif()
endif()

set(CSILK_WASM_TESTS)
if(CSILK_ENABLE_WASM)
  set(CSILK_WASM_TESTS
      test_wasm_plugin
      test_wasm_vm
      test_wasm_fuel
      test_wasm_wasi
      test_wasm_real
  )
  if(CSILK_ENABLE_WORKFLOW)
    list(APPEND CSILK_WASM_TESTS test_app_wasm)
  endif()
endif()

set(CSILK_GRPC_TESTS)
if(CSILK_ENABLE_GRPC)
  set(CSILK_GRPC_TESTS test_grpc test_grpc_gateway)
endif()

set(CSILK_HTTP3_TESTS)
if(CSILK_ENABLE_HTTP3)
  set(CSILK_HTTP3_TESTS test_h3 test_h3_worker test_h3_e2e test_http3_skeleton)
endif()

set(CSILK_MCP_TESTS)
if(CSILK_ENABLE_MCP)
  set(CSILK_MCP_TESTS
      test_mcp_jsonrpc
      test_mcp_server_client
      test_mcp_server_core
      test_mcp_stdio
      test_mcp_stdio_pool
      test_mcp_sse_ext
  )
endif()

set(CSILK_PERMISSION_TESTS)
if(CSILK_ENABLE_PERM)
  set(CSILK_PERMISSION_TESTS test_perm test_perm_ext)
endif()

set(CSILK_CSV_TESTS)
if(CSILK_ENABLE_CSV)
  set(CSILK_CSV_TESTS test_csv)
endif()

# -- Extra / integration tests --
set(CSILK_EXTRA_TESTS
    test_extra
    test_integration
    test_session_integration
    test_session_store_lifecycle
    test_middleware_chain_integration
    test_openapi_integration
    test_sse_integration
    test_ws_integration
)
if(CSILK_ENABLE_OBSERVABILITY)
  list(APPEND CSILK_EXTRA_TESTS test_admin_integration)
endif()
if(CSILK_ENABLE_WORKFLOW AND CSILK_ENABLE_OBSERVABILITY)
  list(APPEND CSILK_EXTRA_TESTS test_integration_ext)
endif()

# Collect all test names for run_tests DEPENDS
set(CSILK_ALL_TEST_NAMES
    ${CSILK_CORE_TESTS}
    ${CSILK_APP_TESTS}
    ${CSILK_WORKFLOW_TESTS}
    ${CSILK_MIDDLEWARE_TESTS}
    ${CSILK_PROTOCOL_TESTS}
    ${CSILK_SECURITY_TESTS}
    ${CSILK_DATA_TESTS}
    ${CSILK_AI_TESTS}
    ${CSILK_VECTOR_TESTS}
    ${CSILK_REFLECTION_TESTS}
    ${CSILK_MESSAGING_TESTS}
    ${CSILK_OBSERVABILITY_TESTS}
    ${CSILK_WASM_TESTS}
    ${CSILK_GRPC_TESTS}
    ${CSILK_HTTP3_TESTS}
    ${CSILK_MCP_TESTS}
    ${CSILK_PERMISSION_TESTS}
    ${CSILK_CSV_TESTS}
    ${CSILK_EXTRA_TESTS}
)

# Register tests using per-module source directories
foreach(_name IN LISTS CSILK_CORE_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/core/${_name}.c")
endforeach()

# Standalone core tests registered outside the list loop
# plugin.h pulls in server.h -> router.h -> context.h -> sys_io.h, which needs
# <uv.h> on the libuv backend. These dlopen test plugins are the only targets
# with no library dependencies, so they must inherit csilk::libuv's include
# directories (e.g. Homebrew's /opt/homebrew/include on macOS, where uv.h is
# not on the default include path). In native io_uring builds csilk::libuv is
# not defined at all and sys_io.h does not include <uv.h>, so guard on TARGET.
add_library(csilk_test_plugin SHARED tests/core/test_plugin_mod.c)
set_target_properties(csilk_test_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins"
    POSITION_INDEPENDENT_CODE ON)
csilk_target_setup(csilk_test_plugin PRIVATE SHARED)
if(TARGET csilk::libuv)
  target_link_libraries(csilk_test_plugin PRIVATE csilk::libuv)
endif()

add_library(csilk_test_plugin_bad_abi SHARED tests/core/test_plugin_bad_abi.c)
set_target_properties(csilk_test_plugin_bad_abi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins"
    POSITION_INDEPENDENT_CODE ON)
csilk_target_setup(csilk_test_plugin_bad_abi PRIVATE SHARED)
if(TARGET csilk::libuv)
  target_link_libraries(csilk_test_plugin_bad_abi PRIVATE csilk::libuv)
endif()

add_csilk_test(test_plugin "${CMAKE_CURRENT_SOURCE_DIR}/tests/core/test_plugin.c")
set_target_properties(test_plugin PROPERTIES ENABLE_EXPORTS TRUE)
add_dependencies(test_plugin csilk_test_plugin csilk_test_plugin_bad_abi)
target_compile_definitions(test_plugin PRIVATE
    CSILK_TEST_PLUGIN_PATH="$<TARGET_FILE:csilk_test_plugin>"
    CSILK_TEST_BAD_ABI_PLUGIN_PATH="$<TARGET_FILE:csilk_test_plugin_bad_abi>"
)
list(APPEND CSILK_ALL_TEST_NAMES test_plugin)

if(TARGET CURL::libcurl)
  add_csilk_test(test_openapi "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocols/test_openapi.c")
  target_link_libraries(test_openapi CURL::libcurl)
  list(APPEND CSILK_ALL_TEST_NAMES test_openapi)
endif()

foreach(_name IN LISTS CSILK_APP_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/app/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_WORKFLOW_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/workflow/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_MIDDLEWARE_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/middleware/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_PROTOCOL_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocols/${_name}.c")
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_SECURITY_TESTS CSILK_SECURITY_TEST_DIRS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_dir}/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_DATA_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/db/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_AI_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/ai/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_VECTOR_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/vector/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_REFLECTION_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/reflection/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_MESSAGING_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/mq/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_OBSERVABILITY_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/observability/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_WASM_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/wasm/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_GRPC_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/grpc/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_HTTP3_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/http3/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_MCP_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/mcp/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_PERMISSION_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/permission/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_CSV_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/csv/${_name}.c")
endforeach()

foreach(_name IN LISTS CSILK_EXTRA_TESTS)
    add_csilk_test(${_name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/${_name}.c")
endforeach()

