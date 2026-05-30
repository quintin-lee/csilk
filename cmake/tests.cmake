# Tests organized by module.
# This file defines the add_csilk_test helper and registers all test executables.
# Only include when csilk is the top-level project.

# Helper to add a test executable and register with CTest
function(add_csilk_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} csilk pthread)
  target_compile_options(${name} PRIVATE $<$<CONFIG:Release>:-UNDEBUG>)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# -- Core tests --
set(CSILK_CORE_TESTS
    test_arena
    test_config
    test_config_comprehensive
    test_config_validate
    test_config_load
    test_config_tls
    test_context
    test_context_ext
    test_context_reflect_ext
    test_edge
    test_get_param
    test_headers
    test_https
    test_ip
    test_json
    test_keepalive
    test_async_keepalive
    test_multi_worker
    test_next_null
    test_params_limit
    test_query
    test_radix
    test_redirect
    test_router
    test_server
    test_server_ext
    test_server_limits
    test_storage
    test_storage_limit
    test_url_decode
    test_url_ext
    test_utils
    test_utils_ext
)

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
)

# -- Workflow tests --
set(CSILK_WORKFLOW_TESTS
    test_workflow_agentic
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
    test_recovery
    test_recovery_ext
    test_request_id
    test_session
    test_session_ext
    test_sse
    test_static
    test_file
    test_validate
    test_waf
)

# -- Protocol tests --
set(CSILK_PROTOCOL_TESTS
    test_swagger
    test_ws
    test_ws_room
    test_h2
)

# -- Security tests --
set(CSILK_SECURITY_TESTS
    test_perm
    test_perm_ext
)

# -- Data / driver tests --
set(CSILK_DATA_TESTS
    test_cipher
    test_crypto_driver
    test_db
    test_mongodb
)

# -- AI tests --
set(CSILK_AI_TESTS
    test_ai
    test_ai_ext
)

# -- Reflection tests --
set(CSILK_REFLECTION_TESTS
    test_reflect
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
)

# -- Extra / integration tests --
set(CSILK_EXTRA_TESTS
    test_extra
    test_integration
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

# Register all tests — each follows tests/<name>.c convention
foreach(name ${CSILK_ALL_TEST_NAMES})
    add_csilk_test(${name} tests/${name}.c)
endforeach()
