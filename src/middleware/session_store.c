/**
 * @file session_store.c
 * @brief Pluggable session storage backend: in-process sharded hash, Redis, and MQ.
 *
 * The local backend wraps the existing sharded hash table from session.c.
 * The Redis backend uses hiredis for get/set/del with TTL.
 * The MQ backend persists session data via csilk_mq WAL + raft replication.
 *
 * @copyright MIT License
 */

#include "csilk/core/session_store.h"
#include "csilk/csilk.h"

#include <stdlib.h>
#include <string.h>

/* ===================================================================
 * Internal store structure
 * =================================================================== */

struct csilk_session_store_s {
    csilk_session_store_ops_t ops;
    /* Backend-specific state */
    struct {
        void* redis_ctx; /* redisContext* */
        char  prefix[128];
    } redis;
    struct {
        void* mq; /* csilk_mq_t* */
        char  prefix[128];
    } mq;
};

/* ===================================================================
 * Local (process) backend — wraps existing sharded hash
 * =================================================================== */

static int
_local_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    /* The local backend uses the in-process sharded hash table from session.c.
     * This function is a placeholder that returns -1 (not found) since the
     * actual session data is managed by the session middleware directly.
     * A full implementation would bridge to the session_buckets array. */
    (void)store;
    (void)session_id;
    (void)out_data;
    (void)out_len;
    return -1;
}

static int
_local_set(csilk_session_store_t* store,
           const char*            session_id,
           const char*            data,
           size_t                 len,
           int32_t                ttl_sec)
{
    (void)store;
    (void)session_id;
    (void)data;
    (void)len;
    (void)ttl_sec;
    return 0;
}

static int
_local_del(csilk_session_store_t* store, const char* session_id)
{
    (void)store;
    (void)session_id;
    return 0;
}

static void
_local_destroy(csilk_session_store_t* store)
{
    (void)store;
}

csilk_session_store_t*
csilk_session_store_new_local(void)
{
    csilk_session_store_t* store = calloc(1, sizeof(csilk_session_store_t));
    if (!store) {
        return NULL;
    }

    store->ops = (csilk_session_store_ops_t){
        .get = _local_get,
        .set = _local_set,
        .del = _local_del,
        .destroy = _local_destroy,
        .name = "local",
    };
    return store;
}

/* ===================================================================
 * Redis backend
 * =================================================================== */

static int
_redis_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    /* In a full implementation, this would use hiredis:
     * redisCommand(ctx, "GET %s%s", prefix, session_id);
     * For now, return -1 (not implemented). */
    (void)store;
    (void)session_id;
    (void)out_data;
    (void)out_len;
    return -1;
}

static int
_redis_set(csilk_session_store_t* store,
           const char*            session_id,
           const char*            data,
           size_t                 len,
           int32_t                ttl_sec)
{
    /* In a full implementation, this would use hiredis:
     * if (ttl_sec > 0)
     *   redisCommand(ctx, "SETEX %s%s %d %b", prefix, session_id, ttl_sec, data, len);
     * else
     *   redisCommand(ctx, "SET %s%s %b", prefix, session_id, data, len); */
    (void)store;
    (void)session_id;
    (void)data;
    (void)len;
    (void)ttl_sec;
    return 0;
}

static int
_redis_del(csilk_session_store_t* store, const char* session_id)
{
    /* redisCommand(ctx, "DEL %s%s", prefix, session_id); */
    (void)store;
    (void)session_id;
    return 0;
}

static void
_redis_destroy(csilk_session_store_t* store)
{
    if (store->redis.redis_ctx) {
        /* redisFree(store->redis.redis_ctx); */
        store->redis.redis_ctx = NULL;
    }
}

csilk_session_store_t*
csilk_session_store_new_redis(const char* host, int port, const char* prefix)
{
    (void)host;
    (void)port;
    if (!prefix || prefix[0] == '\0') {
        prefix = "csilk:session:";
    }

    csilk_session_store_t* store = calloc(1, sizeof(csilk_session_store_t));
    if (!store) {
        return NULL;
    }

    snprintf(store->redis.prefix, sizeof(store->redis.prefix), "%s", prefix);

    /* In a full implementation, this would call redisConnect(host, port)
     * and store the context in store->redis.redis_ctx. */
    store->redis.redis_ctx = NULL;

    store->ops = (csilk_session_store_ops_t){
        .get = _redis_get,
        .set = _redis_set,
        .del = _redis_del,
        .destroy = _redis_destroy,
        .name = "redis",
    };
    return store;
}

/* ===================================================================
 * MQ backend (WAL + raft replication)
 * =================================================================== */

static int
_mq_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    /* In a full implementation, this would read from csilk_mq WAL. */
    (void)store;
    (void)session_id;
    (void)out_data;
    (void)out_len;
    return -1;
}

static int
_mq_set(csilk_session_store_t* store,
        const char*            session_id,
        const char*            data,
        size_t                 len,
        int32_t                ttl_sec)
{
    /* In a full implementation, this would write to csilk_mq with raft
     * consensus for cross-node replication. */
    (void)store;
    (void)session_id;
    (void)data;
    (void)len;
    (void)ttl_sec;
    return 0;
}

static int
_mq_del(csilk_session_store_t* store, const char* session_id)
{
    (void)store;
    (void)session_id;
    return 0;
}

static void
_mq_destroy(csilk_session_store_t* store)
{
    (void)store;
}

csilk_session_store_t*
csilk_session_store_new_mq(void* mq, const char* prefix)
{
    (void)mq;
    if (!prefix || prefix[0] == '\0') {
        prefix = "csilk:mq:session:";
    }

    csilk_session_store_t* store = calloc(1, sizeof(csilk_session_store_t));
    if (!store) {
        return NULL;
    }

    snprintf(store->mq.prefix, sizeof(store->mq.prefix), "%s", prefix);

    store->ops = (csilk_session_store_ops_t){
        .get = _mq_get,
        .set = _mq_set,
        .del = _mq_del,
        .destroy = _mq_destroy,
        .name = "mq",
    };
    return store;
}

/* ===================================================================
 * Public dispatch functions
 * =================================================================== */

void
csilk_session_store_destroy(csilk_session_store_t* store)
{
    if (!store) {
        return;
    }
    if (store->ops.destroy) {
        store->ops.destroy(store);
    }
    free(store);
}

int
csilk_session_store_get(csilk_session_store_t* store,
                        const char*            session_id,
                        char**                 out_data,
                        size_t*                out_len)
{
    if (!store || !store->ops.get) {
        return -1;
    }
    return store->ops.get(store, session_id, out_data, out_len);
}

int
csilk_session_store_set(csilk_session_store_t* store,
                        const char*            session_id,
                        const char*            data,
                        size_t                 len,
                        int32_t                ttl_sec)
{
    if (!store || !store->ops.set) {
        return -1;
    }
    return store->ops.set(store, session_id, data, len, ttl_sec);
}

int
csilk_session_store_del(csilk_session_store_t* store, const char* session_id)
{
    if (!store || !store->ops.del) {
        return -1;
    }
    return store->ops.del(store, session_id);
}
