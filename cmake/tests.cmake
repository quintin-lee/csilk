# Tests organized by module.
# This file defines the add_csilk_test helper and registers all test executables.
# Only include when csilk is the top-level project.

# Helper to add a test executable and register with CTest
function(add_csilk_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} csilk pthread m)
  target_compile_features(${name} PRIVATE c_std_23)
  target_compile_options(${name} PRIVATE
      "${CSILK_COMMON_FLAGS}"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
      "$<$<BOOL:${USE_COVERAGE}>:--coverage;-O0;-g>"
  )
  target_link_options(${name} PRIVATE
      "$<$<BOOL:${USE_COVERAGE}>:--coverage>"
      "$<$<BOOL:${USE_ASAN}>:${CSILK_ASAN_FLAGS}>"
      "$<$<BOOL:${USE_TSAN}>:${CSILK_TSAN_FLAGS}>"
  )
  target_compile_definitions(${name} PRIVATE
      "$<$<BOOL:${ENABLE_OOM_TEST}>:TEST_OOM>"
  )
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# -- Core tests --
set(CSILK_CORE_TESTS
    test_arena
    test_arena_bench
    test_body_pool
    test_config
    test_config_comprehensive
    test_config_validate
    test_config_load
    test_config_tls
    test_connection
    test_context
    test_context_ext
    test_context_reflect_ext
    test_ctx_lifecycle_async
    test_edge
    test_get_param
    test_mvcc_cache
    test_headers
    test_https
    test_ip
    test_json
    test_json_ext
    test_json_accessor_bench
    test_keepalive
    test_async_keepalive
    test_multi_worker
    test_next_null
    test_params_limit
    test_query
    test_radix
    test_redirect
    test_response
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
    test_dispatch_bench
    test_lfqueue_stress
    test_uring_recv_bench
    test_uring_stale_stress
    test_router_bench
    test_router_ordering
    test_router_iterative_bench
    test_router_simd_fuzz_bench
    test_header_map_bench
    test_codec_prop

    test_hash_prop
    test_simd_router_arena
    test_uring_buf
    test_wasm_plugin
    test_server_stats_bench
    test_swar_http
    test_af_xdp
    test_dpdk_pmd
    test_http1_zerocopy
    test_af_xdp_zerocopy
    test_uring_sqpoll
    test_uring_io
    test_io_backend
    test_io_perf_fallback
    test_wasm_vm
    test_wasm_fuel
    test_wasm_wasi
    test_h2_stream_bench
    test_logger_async_bench
    test_server_config_race
    test_core_concurrency_stress
    test_atomic_lifecycle
    test_rcu_lifecycle_stress
)
set(CSILK_CORE_TEST_DIRS
    core;core;core;core;core;core;core;core;core;core
    core;core;core;core;core;core;core;core;core;core
    core;core;core;core;core;core;core;core;core;core
    core;core;core;core;core;core;core;core;core;core
    core;core;core;core;core;core;core;core;core;core
    core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core;core
)










# -- Application tests --
set(CSILK_APP_TESTS
    test_app
    test_app_ext
    test_app_integration
    test_admin
    test_cookie
    test_form
    test_group
    test_group_ext
    test_hooks
    test_hooks_rcu
)
set(CSILK_APP_TEST_DIRS
    app;app;app;app;app;app;app;app;app;app
)

# -- Workflow tests --
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
)
set(CSILK_WORKFLOW_TEST_DIRS
    workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow
    workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow
    workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow;workflow
)

# -- Middleware tests --
set(CSILK_MIDDLEWARE_TESTS
    test_auth
    test_cors
    test_cors_ext
    test_csrf
    test_csrf_ext
    test_gzip
    test_jwt
    test_logger
    test_logger_ext
    test_metrics
    test_multipart
    test_ratelimit
    test_sliding_ratelimit
    test_recovery
    test_recovery_ext
    test_request_id
    test_session
    test_session_ext
    test_sse
    test_sse_concurrent
    test_static
    test_trace_circuit_breaker
    test_otlp_exporter
    test_grpc_gateway
    test_file
    test_validate
    test_waf
    test_xdp_waf
    test_xdp_waf_rules
    test_otlp_trace_span
    test_apm_dashboard_route
)
set(CSILK_MIDDLEWARE_TEST_DIRS
    middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware
    middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware
    middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware;middleware
    middleware
)

# -- Protocol tests --
set(CSILK_PROTOCOL_TESTS
    test_swagger
    test_openapi
    test_ws
    test_ws_room
    test_ws_concurrent
    test_h2
    test_h3
    test_mcp_jsonrpc
    test_mcp_server_client
)
set(CSILK_PROTOCOL_TEST_DIRS
    protocols
    protocols
    protocols
    protocols
    protocols
    protocols
    protocols
    protocols
    protocols
)

# -- Security tests --
set(CSILK_SECURITY_TESTS
    test_perm
    test_perm_ext
    test_crypto_primitives
    test_jwt_security
    test_uuid
    test_bcrypt
    test_cipher
    test_crypto_driver
    test_crypto
)
set(CSILK_SECURITY_TEST_DIRS
    security;security;security;security;security;security;security;security;crypto
)

# -- Data / driver tests --
set(CSILK_DATA_TESTS
    test_db_sqlite
    test_db_registry
)
set(CSILK_DATA_TEST_DIRS
    drivers;drivers
)

# -- AI / driver tests --
set(CSILK_AI_TESTS
    test_ai
    test_ai_ext
    test_vector_db
    test_vector_simd
    test_vector_hnsw
    test_vector_db_embedded
    test_db
    test_mongodb
)
set(CSILK_AI_TEST_DIRS
    drivers;drivers;drivers;drivers/vector;drivers/vector;drivers/vector
    drivers/db;drivers/db
)

# -- Reflection tests --
set(CSILK_REFLECTION_TESTS
    test_reflect
)
set(CSILK_REFLECTION_TEST_DIRS
    reflection
)

# -- Messaging tests --
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
)
set(CSILK_MESSAGING_TEST_DIRS
    mq;mq;mq;mq;mq;mq;mq;mq;mq;messaging;messaging;messaging
)

# -- Extra / integration tests --
set(CSILK_EXTRA_TESTS
    test_extra
    test_integration
    test_integration_ext
)
set(CSILK_EXTRA_TEST_DIRS
    integration;integration;integration
)

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
    ${CSILK_REFLECTION_TESTS}
    ${CSILK_MESSAGING_TESTS}
    ${CSILK_EXTRA_TESTS}
)

# Register tests using per-module source directories
foreach(_name _dir IN ZIP_LISTS CSILK_CORE_TESTS CSILK_CORE_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_APP_TESTS CSILK_APP_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_WORKFLOW_TESTS CSILK_WORKFLOW_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_MIDDLEWARE_TESTS CSILK_MIDDLEWARE_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_PROTOCOL_TESTS CSILK_PROTOCOL_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_SECURITY_TESTS CSILK_SECURITY_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_DATA_TESTS CSILK_DATA_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_AI_TESTS CSILK_AI_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_REFLECTION_TESTS CSILK_REFLECTION_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_MESSAGING_TESTS CSILK_MESSAGING_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

foreach(_name _dir IN ZIP_LISTS CSILK_EXTRA_TESTS CSILK_EXTRA_TEST_DIRS)
    add_csilk_test(${_name} tests/${_dir}/${_name}.c)
endforeach()

