# cmake/sources.cmake — Source files organized by modular sub-library.
#
# Modular sub-libraries:
#   - CSILK_CORE_SOURCES     -> csilk_core     (libcsilk-core.a)
#   - CSILK_HTTP_SOURCES     -> csilk_http     (libcsilk-http.a)
#   - CSILK_TLS_SOURCES      -> csilk_tls      (libcsilk-tls.a)
#   - CSILK_HTTP2_SOURCES    -> csilk_http2    (libcsilk-http2.a)
#   - CSILK_DB_SOURCES       -> csilk_db       (libcsilk-db.a)
#   - CSILK_AI_SOURCES       -> csilk_ai       (libcsilk-ai.a)
#   - CSILK_MQ_SOURCES       -> csilk_mq       (libcsilk-mq.a)
#   - CSILK_WORKFLOW_SOURCES -> csilk_workflow (libcsilk-workflow.a)
#   - CSILK_SOURCES          -> csilk          (libcsilk.a / libcsilk.so)

# ── Minimal Core Module (arena, bounded_buf, config, logger, sync, crypto) ──
set(CSILK_CORE_SOURCES
    src/core/primitives/arena.c
    src/core/primitives/bounded_buf.c
    src/core/primitives/kv_store.c
    src/core/cache/mvcc_cache.c
    src/core/config/config.c
    src/core/config/logger.c
    src/core/config/hooks.c
    src/core/uring/uring_buf.c
    src/core/uring/uring_sqpoll.c
    src/core/uring/uring_vector.c
    src/crypto/base64.c
    src/crypto/sha1.c
    src/crypto/cipher_dispatch.c
    src/crypto/uuid.c
    src/crypto/crypto.c
    src/crypto/bcrypt.c
    src/drivers/cipher/openssl.c
    src/util/flamegraph.c
)

if(CSILK_USE_URING)
    list(APPEND CSILK_CORE_SOURCES
        src/core/uring/uring_thread_pool.c
        src/core/uring/uring_fs.c
        src/core/uring/uring_io.c
        src/core/uring/uring_loop.c
        src/core/uring/uring_handle.c
        src/core/uring/uring_tcp.c
        src/core/uring/uring_stream.c
        src/core/uring/uring_write.c
        src/core/uring/uring_close.c
        src/core/uring/uring_timer.c
        src/core/uring/uring_run.c
    )
endif()

# ── JSON Module (yyjson fast serialization engine) ──────────────────────
set(CSILK_JSON_SOURCES
    src/core/json/json_internal.c
    src/core/json/json_factory.c
    src/core/json/json_object.c
    src/core/json/json_array.c
    src/core/json/json_access.c
    src/core/json/json_type.c
    src/core/json/json_parse.c
    src/core/json/json_serialize.c
    src/core/json/json_free.c
    src/core/json/json_copy.c
    src/core/json/json_iterate.c
    src/core/json/json_mutate.c
    src/core/json/json.c
)

# ── WASM Module (WASM VM & WASI sandbox plugin engine) ───────────────────
set(CSILK_WASM_SOURCES
    src/core/plugin/wasm_plugin.c
    src/core/plugin/wasm_vm.c
    src/core/plugin/wasm_wasi.c
)

# ── Bypass Module (AF_XDP & DPDK kernel bypass drivers) ──────────────────
set(CSILK_BYPASS_SOURCES
    src/core/io/af_xdp.c
    src/core/io/af_xdp_zerocopy.c
    src/core/io/dpdk_pmd.c
    src/core/io/io_perf_probe.c
)

# ── HTTP Module (HTTP/1, context, router, connection, server, app, middleware) ─
set(CSILK_HTTP_SOURCES
    src/core/ctx/context.c
    src/core/ctx/ctx_accessors.c
    src/core/ctx/ctx_defer.c
    src/core/ctx/ctx_json.c
    src/core/config/hot_reload.c
    src/core/test_utils.c
    src/core/primitives/recovery.c
    src/core/primitives/header_map.c
    src/core/primitives/query.c
    src/core/primitives/url.c
    src/core/primitives/response.c
    src/core/primitives/router.c
    src/core/primitives/router_simd.c
    src/core/primitives/router_trie.c
    src/core/http/http1_parse.c
    src/core/http/http1_serialize.c
    src/core/http/http1_write.c
    src/core/http/http1_pipeline.c
    src/core/http/http1_response.c
    src/core/http/http1_zerocopy.c
    src/core/http/swar_http.c
    src/core/server/connection_pool.c
    src/core/server/connection_state.c
    src/core/server/timer_lifetime.c
    src/core/server/connection_timer.c
    src/core/server/connection_close.c

    src/core/server/connection_io.c
    src/core/server/connection.c
    src/core/server/server_lifecycle.c
    src/core/server/server_driver.c
    src/core/server/server_rcu.c
    src/core/server/server_shutdown.c
    src/core/server/server_worker.c
    src/app/app.c
    src/app/app_routes.c
    src/app/group.c
    src/app/admin.c
    src/middleware/auth.c
    src/middleware/circuit_breaker.c
    src/middleware/cors.c
    src/middleware/csrf.c
    src/middleware/grpc_gateway.c
    src/middleware/gzip.c
    src/middleware/jwt.c
    src/middleware/logger.c
    src/middleware/metrics.c
    src/middleware/multipart.c
    src/middleware/otlp_exporter.c
    src/middleware/otlp_trace.c
    src/middleware/ratelimit.c
    src/middleware/request_id.c
    src/middleware/session.c
    src/middleware/sliding_ratelimit.c
    src/middleware/sse.c
    src/middleware/static.c
    src/middleware/validate.c
    src/middleware/waf.c
    src/middleware/xdp_waf.c
    src/protocols/swagger.c
    src/protocols/websocket.c
    src/protocols/ws_room.c
    src/reflection/reflect.c
    src/reflection/reflect_marshal.c
    src/reflection/reflect_unmarshal.c
    src/reflection/reflect_free.c
    src/drivers/perm/perm.c
    src/drivers/perm/simple.c
)

# ── TLS Module (OpenSSL TLS engine & cipher drivers) ─────────────────────
set(CSILK_TLS_SOURCES
    src/core/http/tls.c
)

# ── HTTP/2 & HTTP/3 Module (nghttp2 & QUIC protocol adapters) ────────────
set(CSILK_HTTP2_SOURCES
    src/core/http/h2_callbacks.c
    src/core/http/h2_session.c
    src/core/http/h2_response.c
    src/core/http/h2.c
    src/protocols/h3.c
)

# ── Database & Vector Module ─────────────────────────────────────────────
set(CSILK_DB_SOURCES
    src/drivers/db/db.c
    src/drivers/db/sqlite.c
    src/drivers/vector/vector.c
    src/drivers/vector/vector_simd.c
    src/drivers/vector/vector_hnsw.c
    src/drivers/vector/qdrant.c
    src/drivers/vector/milvus.c
)

# ── AI Module (LLM client drivers) ───────────────────────────────────────
set(CSILK_AI_SOURCES
    src/drivers/ai/ai.c
    src/drivers/ai/ollama.c
    src/drivers/ai/openai.c
)

# ── MQ Module (Message queue & Raft consensus) ───────────────────────────
set(CSILK_MQ_SOURCES
    src/messaging/mq_core.c
    src/messaging/mq_pubsub.c
    src/messaging/mq_dispatch.c
    src/messaging/mq_context.c
    src/messaging/mq_offload.c
    src/messaging/mq_wal.c
    src/messaging/raft_wal.c
    src/messaging/raft_rpc.c
    src/messaging/raft_consensus.c
    src/messaging/raft_snapshot.c
)

# ── Workflow & MCP Module (Agent scheduler, DSL, MCP protocols) ──────────
set(CSILK_WORKFLOW_SOURCES
    src/workflow/wf_graph.c
    src/workflow/wf_distributed.c
    src/workflow/wf_monitor.c
    src/workflow/wf_ai_utils.c
    src/workflow/wf_ai_nodes.c
    src/workflow/wf_ai_agents.c
    src/workflow/wf_tools.c
    src/workflow/wf_scheduler.c
    src/workflow/wf_ctx.c
    src/workflow/wf_wal.c
    src/workflow/wf_node.c
    src/workflow/wf_run.c
    src/workflow/wf_resume.c
    src/workflow/wf_trace.c
    src/workflow/workflow_loader.c
    src/workflow/workflow_wal.c
    src/workflow/workflow_dsl.c
    src/workflow/workflow_manager.c
    src/workflow/workflow_debug.c
    src/workflow/wf_cluster_sm.c
    src/protocols/mcp/mcp_jsonrpc.c
    src/protocols/mcp/mcp_server.c
    src/protocols/mcp/mcp_client.c
)

# ── Combined Full Source List (Monolithic fallback & shared library) ──────
set(CSILK_SOURCES
    ${CSILK_CORE_SOURCES}
    ${CSILK_JSON_SOURCES}
    ${CSILK_HTTP_SOURCES}
    ${CSILK_TLS_SOURCES}
    ${CSILK_HTTP2_SOURCES}
    ${CSILK_DB_SOURCES}
    ${CSILK_AI_SOURCES}
    ${CSILK_MQ_SOURCES}
    ${CSILK_WORKFLOW_SOURCES}
    ${CSILK_WASM_SOURCES}
    ${CSILK_BYPASS_SOURCES}
)
