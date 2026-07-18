# Source files organized by module.
# Each section corresponds to a subdirectory under src/.
# Conditional driver sources (mysql.c, postgres.c, mongodb.c, redis.c)
# are appended by CMakeLists.txt after dependency detection.

set(CSILK_AI_SOURCES
    src/drivers/ai/ai.c
)

set(CSILK_APP_SOURCES
    src/app/app.c
    src/app/group.c
)

set(CSILK_CORE_SOURCES
    src/core/config/admin.c
    src/core/primitives/arena.c
    src/core/primitives/bounded_buf.c
    src/core/config/config.c
    src/core/ctx/context.c
    src/core/ctx/ctx_defer.c
    src/core/ctx/ctx_json.c
    src/core/http/h2.c
    src/core/http/http1.c
    src/core/config/logger.c
    src/core/primitives/recovery.c
    src/core/primitives/response.c
    src/core/primitives/router.c
    src/core/primitives/router_match.c
    src/core/test_utils.c
    src/core/http/tls.c
    src/core/server/url.c
    src/core/server/utils.c
    src/core/server/sha1.c
    src/core/server/base64.c
    src/core/server/uuid.c
    src/core/config/hot_reload.c
    src/core/primitives/header_map.c
    src/core/config/hooks.c
    src/core/primitives/kv_store.c
    src/core/primitives/query.c
    src/core/cache/mvcc_cache.c
    src/core/uring/uring_buf.c
    src/core/plugin/wasm_plugin.c
)

if(CSILK_USE_URING)
    list(APPEND CSILK_CORE_SOURCES
        src/core/uring/uring_server.c
        src/core/uring/uring_connection.c
        src/core/uring/uring_thread_pool.c
        src/core/uring/uring_fs.c
        src/core/uring/uv_stubs.c
    )
else()
    list(APPEND CSILK_CORE_SOURCES
        src/core/server/server.c
        src/core/server/connection.c
    )
endif()

set(CSILK_DATA_SOURCES
    src/drivers/db/db.c
)

set(CSILK_DRIVER_SOURCES
    src/drivers/ai/ollama.c
    src/drivers/ai/openai.c
    src/drivers/cipher/openssl.c
    src/drivers/perm/simple.c
    src/drivers/db/sqlite.c
    src/drivers/vector/vector.c
    src/drivers/vector/qdrant.c
    src/drivers/vector/milvus.c
)

set(CSILK_MESSAGING_SOURCES
    src/messaging/mq_core.c
    src/messaging/mq_pubsub.c
    src/messaging/mq_dispatch.c
    src/messaging/mq_context.c
    src/messaging/mq_offload.c
    src/messaging/mq_wal.c
)

set(CSILK_MIDDLEWARE_SOURCES
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
)

set(CSILK_PROTOCOL_SOURCES
    src/protocols/h3.c
    src/protocols/swagger.c
    src/protocols/websocket.c
    src/protocols/ws_room.c
)

set(CSILK_REFLECTION_SOURCES
    src/reflection/reflect.c
)

set(CSILK_SECURITY_SOURCES
    src/drivers/perm/perm.c
)

set(CSILK_UTIL_SOURCES
    src/util/flamegraph.c
)

set(CSILK_WORKFLOW_SOURCES
    src/workflow/wf_lifecycle.c
    src/workflow/wf_monitor.c
    src/workflow/wf_ai.c
    src/workflow/wf_tools.c
    src/workflow/wf_scheduler.c
    src/workflow/wf_resume.c
    src/workflow/wf_trace.c
    src/workflow/workflow_loader.c
    src/workflow/workflow_wal.c
)

# Combine into the main source list.
set(CSILK_SOURCES
    ${CSILK_AI_SOURCES}
    ${CSILK_APP_SOURCES}
    ${CSILK_CORE_SOURCES}
    ${CSILK_DATA_SOURCES}
    ${CSILK_DRIVER_SOURCES}
    ${CSILK_MESSAGING_SOURCES}
    ${CSILK_MIDDLEWARE_SOURCES}
    ${CSILK_PROTOCOL_SOURCES}
    ${CSILK_REFLECTION_SOURCES}
    ${CSILK_SECURITY_SOURCES}
    ${CSILK_UTIL_SOURCES}
    ${CSILK_WORKFLOW_SOURCES}
)
