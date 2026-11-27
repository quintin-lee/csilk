# cmake/sources.cmake — source ownership for modular sub-libraries.
#
# Every implementation source belongs to one module list below.  The full
# CSILK_SOURCES list at the end is the only aggregate closure.

# ── Foundation primitives ────────────────────────────────────────────────
set(CSILK_BASE_SOURCES
    src/core/primitives/arena.c
    src/core/primitives/bounded_buf.c
    src/core/primitives/kv_store.c
    src/core/primitives/header_map.c
    src/core/primitives/url.c
    src/core/primitives/ssrf.c
)

# ── Crypto and cipher implementations ────────────────────────────────────
set(CSILK_CRYPTO_SOURCES
    src/crypto/base64.c
    src/crypto/sha1.c
    src/crypto/cipher_dispatch.c
    src/crypto/uuid.c
    src/crypto/crypto.c
    src/crypto/bcrypt.c
    src/crypto/totp.c
    src/crypto/cipher_openssl.c
)

# Compatibility source list.  csilk_core is an interface compatibility target.
set(CSILK_CORE_SOURCES)

# ── Runtime: context, router, server, config and backend glue ─────────────
set(CSILK_RUNTIME_SOURCES
    src/core/config/config.c
    src/core/config/logger.c
    src/core/config/hooks.c
    src/core/config/hot_reload.c
    src/core/config/secret.c
    src/core/ctx/context.c
    src/core/ctx/ctx_accessors.c
    src/core/ctx/ctx_async.c
    src/core/ctx/ctx_defer.c
    src/core/ctx/ctx_json.c
    src/core/primitives/recovery.c
    src/core/primitives/query.c
    src/core/primitives/response.c
    src/core/primitives/router.c
    src/core/primitives/router_simd.c
    src/core/primitives/router_trie.c
    src/core/server/connection_pool.c
    src/core/server/connection_state.c
    src/core/server/timer_lifetime.c
    src/core/server/connection_timer.c
    src/core/server/connection_close.c
    src/core/server/connection_io.c
    src/core/server/server_lifecycle.c
    src/core/server/server_driver.c
    src/core/server/server_rcu.c
    src/core/server/server_shutdown.c
    src/core/server/server_worker.c
    src/core/plugin/loader.c
)

if(CSILK_ENABLE_CSV)
    list(APPEND CSILK_RUNTIME_SOURCES src/modules/csv/csv.c)
endif()

if(CSILK_USE_URING)
    list(APPEND CSILK_RUNTIME_SOURCES
        src/core/uring/uring_buf.c
        src/core/uring/uring_sqpoll.c
        src/core/uring/uring_thread_pool.c
        src/core/uring/uring_fs.c
        src/core/uring/uring_loop.c
        src/core/uring/uring_handle.c
        src/core/uring/uring_tcp.c
        src/core/uring/uring_stream.c
        src/core/uring/uring_udp.c
        src/core/uring/uring_run.c
    )
endif()

# Test helpers are linked only by test executables.
set(CSILK_TEST_SUPPORT_SOURCES
    src/core/test_utils.c
)

# ── JSON Module (yyjson fast serialization engine) ──────────────────────
set(CSILK_JSON_SOURCES
    src/core/json/json_internal.c
    src/core/json/json_doc.c
    src/core/json/json_access.c
    src/core/json/json_mutate.c
)

# ── WASM Module (WASM VM & WASI sandbox plugin engine) ───────────────────
set(CSILK_WASM_SOURCES
    src/modules/wasm/wasm_plugin.c
    src/modules/wasm/wasm_vm.c
    src/modules/wasm/wasm_wasi.c
)

# ── HTTP/1 Module ─────────────────────────────────────────────────────────
set(CSILK_HTTP_SOURCES
    src/core/http/http1_parse.c
    src/core/http/http1_serialize.c
    src/core/http/http1_write.c
    src/core/http/http1_pipeline.c
    src/core/http/http1_zerocopy.c
    src/core/http/swar_http.c
)

# ── TLS Module (OpenSSL TLS engine & cipher drivers) ─────────────────────
set(CSILK_TLS_SOURCES
    src/core/http/tls.c
)

# ── HTTP/2 Module (nghttp2 protocol adapter) ──────────────────────────────
set(CSILK_HTTP2_SOURCES
    src/core/http/h2_callbacks.c
    src/core/http/h2_session.c
    src/core/http/h2_response.c
)

# ── HTTP/3 Module (ngtcp2 & nghttp3 QUIC adapter, optional) ───────────────
set(CSILK_HTTP3_SOURCES
    src/modules/http3/h3.c
    src/modules/http3/h3_session.c
    src/modules/http3/http3.c
)

# ── Vector Module (HNSW + vector DB drivers) ───────────────────────────
set(CSILK_VECTOR_SOURCES
    src/modules/vector/vector.c
    src/modules/vector/vector_hnsw.c
    src/modules/vector/vector_simd.c
    src/modules/vector/milvus.c
    src/modules/vector/qdrant.c
)

# ── Database Module ───────────────────────────────────────────────────────
set(CSILK_DB_SOURCES
    src/modules/db/db.c
    src/modules/db/sqlite.c
    src/modules/db/tx_scope.c
    src/modules/db/migration/migration_checksum.c
    src/modules/db/migration/migration_engine.c
    src/modules/db/migration/migration_lock.c
)

# ── Web Protocol Module (WebSocket, OpenAPI, Swagger — Web Core) ─────────
set(CSILK_PROTOCOLS_SOURCES
    src/protocols/websocket.c
    src/protocols/ws_room.c
    src/protocols/openapi_gen.c
    src/protocols/swagger_serve.c
)

# ── MCP Module (Model Context Protocol, optional) ────────────────────────
set(CSILK_MCP_SOURCES
    src/modules/mcp/mcp_jsonrpc.c
    src/modules/mcp/mcp_server.c
    src/modules/mcp/mcp_client.c
    src/modules/mcp/mcp_stdio_pool.c
)

# ── gRPC Module (gRPC wire codec & gateway, optional) ────────────────────
set(CSILK_GRPC_SOURCES
    src/modules/grpc/grpc.c
    src/modules/grpc/grpc_gateway.c
)

# ── Permission Module ─────────────────────────────────────────────────────
set(CSILK_PERMISSION_SOURCES
    src/modules/permission/perm.c
    src/modules/permission/simple.c
)

# ── Middleware Module ─────────────────────────────────────────────────────
set(CSILK_MIDDLEWARE_SOURCES
    src/middleware/auth.c
    src/middleware/circuit_breaker.c
    src/middleware/cors.c
    src/middleware/csrf.c
    src/middleware/gzip.c
    src/middleware/jwt.c
    src/middleware/logger.c
    src/middleware/metrics.c
    src/middleware/multipart.c
    src/middleware/ratelimit.c
    src/middleware/request_id.c
    src/middleware/security_headers.c
    src/middleware/session.c
    src/middleware/session_store.c
    src/middleware/sse.c
    src/middleware/static.c
    src/middleware/validate.c
    src/middleware/waf.c
)

# ── Reflection Module ────────────────────────────────────────────────────
set(CSILK_REFLECTION_SOURCES
    src/reflection/reflect.c
    src/reflection/reflect_marshal.c
    src/reflection/reflect_unmarshal.c
    src/reflection/reflect_free.c
)

# ── Application Module ───────────────────────────────────────────────────
set(CSILK_APP_SOURCES
    src/app/app.c
    src/app/app_routes.c
    src/app/group.c
)

# ── Observability & Admin Module (optional) ──────────────────────────────
set(CSILK_OBSERVABILITY_SOURCES
    src/modules/observability/otlp_exporter.c
    src/modules/observability/otlp_trace.c
    src/modules/observability/flamegraph.c
    src/modules/observability/admin.c
)

# ── AI Module (LLM client drivers) ───────────────────────────────────────
set(CSILK_AI_SOURCES
    src/modules/ai/ai.c
    src/modules/ai/ai_trace.c
    src/modules/ai/openai.c
    src/modules/ai/ollama.c
    src/modules/ai/anthropic.c
    src/modules/ai/tool_registry.c
)

# ── MQ Module (Message queue & Raft consensus) ───────────────────────────
set(CSILK_MQ_SOURCES
    src/modules/mq/mq/mq_context.c
    src/modules/mq/mq/mq_core.c
    src/modules/mq/mq/mq_dispatch.c
    src/modules/mq/mq/mq_offload.c
    src/modules/mq/mq/mq_pubsub.c
    src/modules/mq/mq/mq_wal.c
    src/modules/common/wal_common.c
    src/modules/common/wal_uring.c
    src/modules/mq/raft/raft_wal.c
    src/modules/mq/raft/raft_consensus.c
    src/modules/mq/raft/raft_rpc.c
    src/modules/mq/raft/raft_snapshot.c
    src/modules/mq/raft/raft_membership.c
)

# ── Workflow & MCP Module (Agent scheduler, DSL, MCP protocols) ──────────
set(CSILK_WORKFLOW_SOURCES
    src/modules/workflow/wf_graph.c
    src/modules/workflow/wf_distributed.c
    src/modules/workflow/wf_grid.c
    src/modules/workflow/wf_monitor.c
    src/modules/workflow/wf_ai_utils.c
    src/modules/workflow/wf_ai_nodes.c
    src/modules/workflow/wf_ai_agents.c
    src/modules/workflow/wf_ai_react_loop.c
    src/modules/workflow/skill.c
    src/modules/workflow/wf_tools.c
    src/modules/workflow/wf_ctx.c
    src/modules/workflow/wf_node.c
    src/modules/workflow/wf_run.c
    src/modules/workflow/wf_resume.c
    src/modules/workflow/wf_trace.c
    src/modules/workflow/workflow_loader.c
    src/modules/workflow/workflow_wal.c
    src/modules/workflow/workflow_dsl.c
    src/modules/workflow/workflow_manager.c
    src/modules/workflow/workflow_debug.c
    src/modules/workflow/wf_cluster_sm.c
)

# ── Combined Source List (Built conditionally from enabled modules) ──────
set(CSILK_SOURCES
    ${CSILK_BASE_SOURCES}
    ${CSILK_CRYPTO_SOURCES}
    ${CSILK_JSON_SOURCES}
    ${CSILK_RUNTIME_SOURCES}
    ${CSILK_TLS_SOURCES}
    ${CSILK_HTTP_SOURCES}
    ${CSILK_HTTP2_SOURCES}
    ${CSILK_PROTOCOLS_SOURCES}
    ${CSILK_MIDDLEWARE_SOURCES}
    ${CSILK_REFLECTION_SOURCES}
    ${CSILK_APP_SOURCES}
)
if(CSILK_ENABLE_HTTP3)
  list(APPEND CSILK_SOURCES ${CSILK_HTTP3_SOURCES})
endif()
if(CSILK_ENABLE_MCP)
  list(APPEND CSILK_SOURCES ${CSILK_MCP_SOURCES})
endif()
if(CSILK_ENABLE_GRPC)
  list(APPEND CSILK_SOURCES ${CSILK_GRPC_SOURCES})
endif()
if(CSILK_ENABLE_PERM)
  list(APPEND CSILK_SOURCES ${CSILK_PERMISSION_SOURCES})
endif()
if(CSILK_ENABLE_DB)
  list(APPEND CSILK_SOURCES ${CSILK_DB_SOURCES})
endif()
if(CSILK_ENABLE_VECTOR)
  list(APPEND CSILK_SOURCES ${CSILK_VECTOR_SOURCES})
endif()
if(CSILK_ENABLE_AI)
  list(APPEND CSILK_SOURCES ${CSILK_AI_SOURCES})
endif()
if(CSILK_ENABLE_MQ)
  list(APPEND CSILK_SOURCES ${CSILK_MQ_SOURCES})
endif()
if(CSILK_ENABLE_WORKFLOW)
  list(APPEND CSILK_SOURCES ${CSILK_WORKFLOW_SOURCES})
endif()
if(CSILK_ENABLE_WASM)
  list(APPEND CSILK_SOURCES ${CSILK_WASM_SOURCES})
endif()
if(CSILK_ENABLE_OBSERVABILITY)
  list(APPEND CSILK_SOURCES ${CSILK_OBSERVABILITY_SOURCES})
endif()
