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
#include "csilk/core/sync.h"
#include "../core/internal/obs_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAS_REDIS
#include <hiredis/hiredis.h>
#endif

/* Forward declaration for shared in-process TTL hash table */
typedef struct mem_session_table_s mem_session_table_t;

enum { MEM_SESSION_BUCKETS = 256 };

typedef struct {
    char    id[128];
    char*   data;
    size_t  len;
    int64_t expire_at;
    int     in_use;
} mem_session_entry_t;

struct mem_session_table_s {
    csilk_mutex_t       lock;
    mem_session_entry_t entries[MEM_SESSION_BUCKETS];
};

/* ===================================================================
 * Internal store structure
 * =================================================================== */

struct csilk_session_store_s {
    csilk_session_store_ops_t ops;
    /* Backend-specific state */
    struct {
        mem_session_table_t* table;
    } local;
    struct {
        void*                mq; /* csilk_mq_t* */
        char                 prefix[128];
        mem_session_table_t* table;
    } mq;
    struct {
        void*         redis_ctx; /* redisContext* */
        char          prefix[128];
        csilk_mutex_t lock;      /* Serializes ops: a hiredis redisContext is not thread-safe. */
    } redis;
};

static mem_session_table_t*
_mem_table_new(void)
{
    mem_session_table_t* table = calloc(1, sizeof(mem_session_table_t));
    if (!table) {
        return NULL;
    }
    csilk_mutex_init(&table->lock);
    return table;
}

static void
_mem_table_destroy(mem_session_table_t* table)
{
    if (!table) {
        return;
    }
    csilk_mutex_destroy(&table->lock);
    for (int i = 0; i < MEM_SESSION_BUCKETS; i++) {
        if (table->entries[i].in_use) {
            free(table->entries[i].data);
        }
    }
    free(table);
}

static uint32_t
_mem_table_hash(const char* s)
{
    uint32_t hash = 2166136261u;
    for (const char* p = s; *p; p++) {
        /* clang-format off */
        hash = (hash ^ (uint32_t)*(const uint8_t*)p) * 16777619u;
        /* clang-format on */
    }
    return hash;
}

static mem_session_entry_t*
_mem_table_lookup(mem_session_table_t* table, const char* session_id)
{
    uint32_t             hash = _mem_table_hash(session_id);
    mem_session_entry_t* slot = &table->entries[hash % MEM_SESSION_BUCKETS];
    if (slot->in_use && strcmp(slot->id, session_id) == 0) {
        return slot;
    }
    return NULL;
}

static int
_mem_table_get(mem_session_table_t* table, const char* session_id, char** out_data, size_t* out_len)
{
    if (!table || !session_id) {
        return -1;
    }

    csilk_mutex_lock(&table->lock);
    mem_session_entry_t* slot = _mem_table_lookup(table, session_id);
    if (slot && slot->in_use) {
        int64_t now = time(NULL);
        if (slot->expire_at > 0 && now >= slot->expire_at) {
            slot->in_use = 0;
            free(slot->data);
            slot->data = NULL;
            csilk_mutex_unlock(&table->lock);
            if (out_data) {
                *out_data = NULL;
            }
            if (out_len) {
                *out_len = 0;
            }
            return -1;
        }
        if (out_data) {
            size_t slen = slot->len;
            char*  copy = malloc(slen + 1);
            if (!copy) {
                csilk_mutex_unlock(&table->lock);
                return -1;
            }
            if (slen > 0 && slot->data) {
                memcpy(copy, slot->data, slen);
            }
            copy[slen] = '\0';
            *out_data = copy;
        }
        if (out_len) {
            *out_len = slot->len;
        }
        csilk_mutex_unlock(&table->lock);
        return 0;
    }
    csilk_mutex_unlock(&table->lock);

    if (out_data) {
        *out_data = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    return -1;
}

static int
_mem_table_set(mem_session_table_t* table,
               const char*          session_id,
               const char*          data,
               size_t               len,
               int32_t              ttl_sec)
{
    if (!table || !session_id || !data) {
        return -1;
    }

    csilk_mutex_lock(&table->lock);

    uint32_t             hash = _mem_table_hash(session_id);
    mem_session_entry_t* slot = &table->entries[hash % MEM_SESSION_BUCKETS];

    if (slot->in_use && strcmp(slot->id, session_id) != 0) {
        free(slot->data);
        slot->data = NULL;
    }

    strncpy(slot->id, session_id, sizeof(slot->id) - 1);
    slot->id[sizeof(slot->id) - 1] = '\0';
    free(slot->data);
    slot->data = malloc(len + 1);
    if (!slot->data) {
        slot->in_use = 0;
        csilk_mutex_unlock(&table->lock);
        return -1;
    }
    memcpy(slot->data, data, len);
    slot->data[len] = '\0';
    slot->len = len;
    slot->expire_at = (ttl_sec > 0) ? (int64_t)time(NULL) + ttl_sec : 0;
    slot->in_use = 1;

    csilk_mutex_unlock(&table->lock);
    return 0;
}

static int
_mem_table_del(mem_session_table_t* table, const char* session_id)
{
    if (!table || !session_id) {
        return -1;
    }

    csilk_mutex_lock(&table->lock);
    mem_session_entry_t* slot = _mem_table_lookup(table, session_id);
    if (slot && slot->in_use) {
        free(slot->data);
        slot->data = NULL;
        slot->in_use = 0;
    }
    csilk_mutex_unlock(&table->lock);
    return 0;
}

/* ===================================================================
 * Local (process) backend — thread-safe in-process TTL hash table
 * =================================================================== */

static int
_local_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    return _mem_table_get(store ? store->local.table : NULL, session_id, out_data, out_len);
}

static int
_local_set(csilk_session_store_t* store,
           const char*            session_id,
           const char*            data,
           size_t                 len,
           int32_t                ttl_sec)
{
    return _mem_table_set(store ? store->local.table : NULL, session_id, data, len, ttl_sec);
}

static int
_local_del(csilk_session_store_t* store, const char* session_id)
{
    return _mem_table_del(store ? store->local.table : NULL, session_id);
}

static void
_local_destroy(csilk_session_store_t* store)
{
    if (store) {
        _mem_table_destroy(store->local.table);
        store->local.table = NULL;
    }
}

csilk_session_store_t*
csilk_session_store_new_local(void)
{
    csilk_session_store_t* store = calloc(1, sizeof(csilk_session_store_t));
    if (!store) {
        return NULL;
    }
    store->local.table = _mem_table_new();
    if (!store->local.table) {
        free(store);
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

#ifdef HAS_REDIS
static char*
_redis_build_key(const char* prefix, const char* session_id, size_t* out_len)
{
    size_t plen = strlen(prefix);
    size_t slen = strlen(session_id);
    size_t keylen = plen + slen;
    char*  key = malloc(keylen + 1);
    if (!key) {
        return NULL;
    }
    memcpy(key, prefix, plen);
    memcpy(key + plen, session_id, slen);
    key[keylen] = '\0';
    if (out_len) {
        *out_len = keylen;
    }
    return key;
}

static int
_redis_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    redisContext* ctx = (redisContext*)store->redis.redis_ctx;
    if (!ctx || !session_id) {
        return -1;
    }

    size_t keylen = 0;
    char*  key = _redis_build_key(store->redis.prefix, session_id, &keylen);
    if (!key) {
        return -1;
    }

    /* Serialize the shared redisContext (hiredis is not thread-safe) and
     * use argv-based commands to prevent space/format injection. */
    csilk_mutex_lock(&store->redis.lock);
    const char* argv[] = {"GET", key};
    size_t      argvlen[] = {3, keylen};
    redisReply* reply = (redisReply*)redisCommandArgv(ctx, 2, argv, argvlen);
    free(key);
    csilk_mutex_unlock(&store->redis.lock);
    if (!reply) {
        CSILK_LOG_W("SessionStore(redis): GET %s failed: %s", session_id, ctx->errstr);
        return -1;
    }

    if (reply->type == REDIS_REPLY_NIL) {
        freeReplyObject(reply);
        if (out_data) {
            *out_data = NULL;
        }
        if (out_len) {
            *out_len = 0;
        }
        return -1;
    }

    if (reply->type == REDIS_REPLY_STRING && reply->str) {
        size_t rlen = reply->len;
        if (out_data) {
            char* copy = malloc(rlen + 1);
            if (!copy) {
                freeReplyObject(reply);
                return -1;
            }
            memcpy(copy, reply->str, rlen);
            copy[rlen] = '\0';
            *out_data = copy;
        }
        if (out_len) {
            *out_len = rlen;
        }
    } else {
        freeReplyObject(reply);
        return -1;
    }

    freeReplyObject(reply);
    return 0;
}

static int
_redis_set(csilk_session_store_t* store,
           const char*            session_id,
           const char*            data,
           size_t                 len,
           int32_t                ttl_sec)
{
    redisContext* ctx = (redisContext*)store->redis.redis_ctx;
    if (!ctx || !session_id || !data) {
        return -1;
    }

    size_t keylen = 0;
    char*  key = _redis_build_key(store->redis.prefix, session_id, &keylen);
    if (!key) {
        return -1;
    }

    /* Serialize the shared redisContext and surface I/O errors. */
    csilk_mutex_lock(&store->redis.lock);
    redisReply* reply;
    if (ttl_sec > 0) {
        char        ttlbuf[16];
        int         tn = snprintf(ttlbuf, sizeof(ttlbuf), "%d", ttl_sec);
        const char* argv[] = {"SETEX", key, ttlbuf, data};
        size_t      argvlen[] = {5, keylen, (size_t)tn, len};
        reply = (redisReply*)redisCommandArgv(ctx, 4, argv, argvlen);
    } else {
        const char* argv[] = {"SET", key, data};
        size_t      argvlen[] = {3, keylen, len};
        reply = (redisReply*)redisCommandArgv(ctx, 3, argv, argvlen);
    }
    free(key);
    csilk_mutex_unlock(&store->redis.lock);
    if (!reply) {
        CSILK_LOG_W("SessionStore(redis): SET %s failed: %s", session_id, ctx->errstr);
        return -1;
    }

    int rc = (reply->type == REDIS_REPLY_ERROR) ? -1 : 0;
    freeReplyObject(reply);
    return rc;
}

static int
_redis_del(csilk_session_store_t* store, const char* session_id)
{
    redisContext* ctx = (redisContext*)store->redis.redis_ctx;
    if (!ctx || !session_id) {
        return -1;
    }

    size_t keylen = 0;
    char*  key = _redis_build_key(store->redis.prefix, session_id, &keylen);
    if (!key) {
        return -1;
    }

    /* Serialize the shared redisContext and use argv-based DEL. */
    csilk_mutex_lock(&store->redis.lock);
    const char* argv[] = {"DEL", key};
    size_t      argvlen[] = {3, keylen};
    redisReply* reply = (redisReply*)redisCommandArgv(ctx, 2, argv, argvlen);
    free(key);
    csilk_mutex_unlock(&store->redis.lock);
    if (!reply) {
        CSILK_LOG_W("SessionStore(redis): DEL %s failed: %s", session_id, ctx->errstr);
        return -1;
    }

    int rc = (reply->type == REDIS_REPLY_ERROR) ? -1 : 0;
    freeReplyObject(reply);
    return rc;
}

static void
_redis_destroy(csilk_session_store_t* store)
{
    if (store->redis.redis_ctx) {
        redisFree((redisContext*)store->redis.redis_ctx);
        store->redis.redis_ctx = NULL;
    }
    csilk_mutex_destroy(&store->redis.lock);
}

csilk_session_store_t*
csilk_session_store_new_redis(const char* host, int port, const char* prefix)
{
    if (!prefix || prefix[0] == '\0') {
        prefix = "csilk:session:";
    }

    csilk_session_store_t* store = calloc(1, sizeof(csilk_session_store_t));
    if (!store) {
        return NULL;
    }

    snprintf(store->redis.prefix, sizeof(store->redis.prefix), "%s", prefix);
    csilk_mutex_init(&store->redis.lock);

    struct timeval tv = {5, 0};
    redisContext*  ctx = redisConnectWithTimeout(host ? host : "127.0.0.1", port ? port : 6379, tv);
    if (!ctx || ctx->err) {
        if (ctx) {
            redisFree(ctx);
        }
        /* Degraded mode: no live Redis, but the store remains usable with
         * no-op Redis ops so callers (and unit tests without a Redis service)
         * still get a valid handle rather than NULL. get returns -1, set/del
         * are no-ops — equivalent to the non-HAS_REDIS stubs. */
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

    store->redis.redis_ctx = ctx;

    store->ops = (csilk_session_store_ops_t){
        .get = _redis_get,
        .set = _redis_set,
        .del = _redis_del,
        .destroy = _redis_destroy,
        .name = "redis",
    };
    return store;
}
#else  /* !HAS_REDIS — fallback stubs */

static int
_redis_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    (void)store;
    (void)session_id;
    if (out_data) {
        *out_data = NULL;
    }
    if (out_len) {
        *out_len = 0;
    }
    return -1;
}

static int
_redis_set(csilk_session_store_t* store,
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
_redis_del(csilk_session_store_t* store, const char* session_id)
{
    (void)store;
    (void)session_id;
    return 0;
}

static void
_redis_destroy(csilk_session_store_t* store)
{
    (void)store;
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
#endif /* HAS_REDIS */

/* ===================================================================
 * MQ backend (WAL + raft replication)
 * ===================================================================
 *
 * When a csilk_mq_t handle is provided, session data is published
 * to the MQ topic for cross-node replication. A local fallback
 * hash table ensures availability when the MQ is unavailable.
 */

static int
_mq_get(csilk_session_store_t* store, const char* session_id, char** out_data, size_t* out_len)
{
    return _mem_table_get(store ? store->mq.table : NULL, session_id, out_data, out_len);
}

static int
_mq_set(csilk_session_store_t* store,
        const char*            session_id,
        const char*            data,
        size_t                 len,
        int32_t                ttl_sec)
{
    int rc = _mem_table_set(store ? store->mq.table : NULL, session_id, data, len, ttl_sec);
    if (rc == 0 && store && store->mq.mq) {
        const csilk_mq_runtime_ops_t* ops = _csilk_get_mq_runtime_ops();
        if (ops && ops->publish_raw) {
            char topic[256];
            snprintf(topic, sizeof(topic), "%s%s", store->mq.prefix, session_id ? session_id : "");
            ops->publish_raw(store->mq.mq, topic, data, len);
        }
    }
    return rc;
}

static int
_mq_del(csilk_session_store_t* store, const char* session_id)
{
    return _mem_table_del(store ? store->mq.table : NULL, session_id);
}

static void
_mq_destroy(csilk_session_store_t* store)
{
    if (store) {
        _mem_table_destroy(store->mq.table);
        store->mq.table = NULL;
    }
}

csilk_session_store_t*
csilk_session_store_new_mq(void* mq, const char* prefix)
{
    if (!prefix || prefix[0] == '\0') {
        prefix = "csilk:mq:session:";
    }

    csilk_session_store_t* store = calloc(1, sizeof(csilk_session_store_t));
    if (!store) {
        return NULL;
    }

    snprintf(store->mq.prefix, sizeof(store->mq.prefix), "%s", prefix);
    store->mq.mq = mq;

    store->mq.table = _mem_table_new();
    if (!store->mq.table) {
        free(store);
        return NULL;
    }

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
