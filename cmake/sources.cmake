# Source files organized by module.
# Each section corresponds to a subdirectory under src/.
# Conditional driver sources (mysql.c, postgres.c, mongodb.c, redis.c)
# are appended by CMakeLists.txt after dependency detection.

set(CSILK_AI_SOURCES
    src/ai/ai.c
)

set(CSILK_APP_SOURCES
    src/app/app.c
    src/app/group.c
)

set(CSILK_CORE_SOURCES
    src/core/admin.c
    src/core/arena.c
    src/core/config.c
    src/core/connection.c
    src/core/context.c
    src/core/h2.c
    src/core/http1.c
    src/core/logger.c
    src/core/recovery.c
    src/core/response.c
    src/core/router.c
    src/core/server.c
    src/core/test_utils.c
    src/core/tls.c
    src/core/url.c
    src/core/utils.c
    src/core/sha1.c
    src/core/base64.c
    src/core/uuid.c
    src/core/hot_reload.c
)

set(CSILK_DATA_SOURCES
    src/data/db.c
)

set(CSILK_DRIVER_SOURCES
    src/drivers/ai/ollama.c
    src/drivers/ai/openai.c
    src/drivers/cipher/openssl.c
    src/drivers/perm/simple.c
    src/drivers/sqlite.c
    src/drivers/vector/vector.c
    src/drivers/vector/qdrant.c
    src/drivers/vector/milvus.c
)

set(CSILK_MESSAGING_SOURCES
    src/messaging/mq.c
    src/messaging/mq_context.c
    src/messaging/mq_offload.c
    src/messaging/mq_wal.c
)

set(CSILK_MIDDLEWARE_SOURCES
    src/middleware/auth.c
    src/middleware/cors.c
    src/middleware/csrf.c
    src/middleware/gzip.c
    src/middleware/jwt.c
    src/middleware/logger.c
    src/middleware/metrics.c
    src/middleware/multipart.c
    src/middleware/ratelimit.c
    src/middleware/request_id.c
    src/middleware/session.c
    src/middleware/sse.c
    src/middleware/static.c
    src/middleware/validate.c
    src/middleware/waf.c
)

set(CSILK_PROTOCOL_SOURCES
    src/protocols/swagger.c
    src/protocols/websocket.c
    src/protocols/ws_room.c
)

set(CSILK_REFLECTION_SOURCES
    src/reflection/reflect.c
)

set(CSILK_SECURITY_SOURCES
    src/security/perm.c
)

set(CSILK_UTIL_SOURCES
    src/util/flamegraph.c
)

set(CSILK_WORKFLOW_SOURCES
    src/workflow/workflow.c
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
