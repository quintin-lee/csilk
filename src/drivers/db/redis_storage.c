#ifdef HAS_REDIS
/**
 * @file redis_storage.c
 * @brief Redis-backed implementation of the csilk storage driver interface.
 *
 * Implements the csilk_storage_driver_t vtable using a Redis connection pool.
 * Supports SET/GET/INCR/EXPIRE commands for key-value storage with optional
 * TTL and atomic increment.
 *
 * @copyright MIT License
 */

#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "core/ctx/ctx_internal.h"
#include "drivers/db/redis_internal.h"

typedef struct {
    csilk_storage_driver_t base;
    csilk_db_pool_t*       pool;
} redis_storage_driver_t;

/** @brief Store a value under a key in Redis (SET).
 *
 * @param c     Request context carrying the storage driver reference.
 * @param key   Null-terminated key string.
 * @param value Value pointer (assumed to be a null-terminated string). */
static void
redis_storage_set(csilk_ctx_t* c, const char* key, void* value)
{
    if (!c || !key || !value) {
        return;
    }
    redis_storage_driver_t* drv = (redis_storage_driver_t*)c->storage_driver;
    if (!drv || !drv->pool) {
        return;
    }

    /* Assumes value is a null-terminated string */
    redis_conn_t* conn = (redis_conn_t*)csilk_db_pool_get_connection(drv->pool);
    if (!conn || !conn->c) {
        return;
    }

    /* Pass argv to avoid % format-string interpretation and delimiter injection. */
    const char* argv[] = {"SET", key, value};
    size_t      lens[] = {3, strlen(key), strlen(value)};
    redisReply* reply = redisCommandArgv(conn->c, 3, argv, lens);
    if (reply) {
        freeReplyObject(reply);
    }
}

/** @brief Retrieve a value by key from Redis (GET).
 *
 * The returned string is arena-allocated (request-scoped) and must not be
 * freed by the caller.
 *
 * @param c   Request context.
 * @param key Null-terminated key.
 * @return Value string, or NULL if the key does not exist. */
static void*
redis_storage_get(csilk_ctx_t* c, const char* key)
{
    if (!c || !key) {
        return NULL;
    }
    redis_storage_driver_t* drv = (redis_storage_driver_t*)c->storage_driver;
    if (!drv || !drv->pool) {
        return NULL;
    }

    redis_conn_t* conn = (redis_conn_t*)csilk_db_pool_get_connection(drv->pool);
    if (!conn || !conn->c) {
        return NULL;
    }

    void*       result = NULL;
    const char* argv[] = {"GET", key};
    size_t      lens[] = {3, strlen(key)};
    redisReply* reply = redisCommandArgv(conn->c, 2, argv, lens);
    if (reply) {
        if (reply->type == REDIS_REPLY_STRING && reply->str && c->arena) {
            result = csilk_arena_strdup(c->arena, reply->str);
        }
        freeReplyObject(reply);
    }
    return result;
}

/** @brief Clear all stored data — unsupported for Redis.
 *
 * A global FLUSHDB is intentionally omitted because it is dangerous in a
 * shared-Redis environment.  This function is a no-op. */
static void
redis_storage_clear(csilk_ctx_t* c)
{
    /* Unsupported globally, would require FLUSHDB which is dangerous. */
    (void)c;
}

/** @brief Store a string value with optional TTL (SET … EX).
 *
 * Returns -1 on null parameters or on a Redis error reply.
 *
 * @param c       Request context.
 * @param key     Null-terminated key.
 * @param value   Null-terminated value.
 * @param ttl_sec Time-to-live in seconds (≤ 0 means no expiry).
 * @return 0 on success, -1 on error. */
static int
redis_storage_set_string(csilk_ctx_t* c, const char* key, const char* value, int ttl_sec)
{
    if (!c || !key || !value) {
        return -1;
    }
    redis_storage_driver_t* drv = (redis_storage_driver_t*)c->storage_driver;
    if (!drv || !drv->pool) {
        return -1;
    }

    redis_conn_t* conn = (redis_conn_t*)csilk_db_pool_get_connection(drv->pool);
    if (!conn || !conn->c) {
        return -1;
    }

    /* Pass argv to avoid % format-string interpretation and delimiter injection. */
    char        ttl_buf[32];
    int         ttl_len = snprintf(ttl_buf, sizeof(ttl_buf), "%d", ttl_sec);
    redisReply* reply = NULL;
    if (ttl_sec > 0) {
        const char* argv[] = {"SET", key, value, "EX", ttl_buf};
        size_t      lens[] = {3, strlen(key), strlen(value), 2, (size_t)ttl_len};
        reply = redisCommandArgv(conn->c, 5, argv, lens);
    } else {
        const char* argv[] = {"SET", key, value};
        size_t      lens[] = {3, strlen(key), strlen(value)};
        reply = redisCommandArgv(conn->c, 3, argv, lens);
    }

    int rc = 0;
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        rc = -1;
    }
    if (reply) {
        freeReplyObject(reply);
    }
    return rc;
}

/** @brief Retrieve a string value by key from Redis (GET).
 *
 * The returned string is heap-allocated via strdup(); the caller must
 * free() it.
 *
 * @param c   Request context.
 * @param key Null-terminated key.
 * @return Heap-allocated value string, or NULL if the key does not
 *         exist. */
static char*
redis_storage_get_string(csilk_ctx_t* c, const char* key)
{
    if (!c || !key) {
        return NULL;
    }
    redis_storage_driver_t* drv = (redis_storage_driver_t*)c->storage_driver;
    if (!drv || !drv->pool) {
        return NULL;
    }

    redis_conn_t* conn = (redis_conn_t*)csilk_db_pool_get_connection(drv->pool);
    if (!conn || !conn->c) {
        return NULL;
    }

    char*       result = NULL;
    const char* argv[] = {"GET", key};
    size_t      lens[] = {3, strlen(key)};
    redisReply* reply = redisCommandArgv(conn->c, 2, argv, lens);
    if (reply) {
        if (reply->type == REDIS_REPLY_STRING && reply->str) {
            result = strdup(reply->str); /* Heap-allocated as per API */
        }
        freeReplyObject(reply);
    }
    return result;
}

/** @brief Atomically increment a key (INCR) with exact remaining TTL via Lua script.
 *
 * Executes an atomic Lua script:
 *   local v = redis.call('INCR', KEYS[1])
 *   if v == 1 and tonumber(ARGV[1]) > 0 then
 *       redis.call('EXPIRE', KEYS[1], tonumber(ARGV[1]))
 *   end
 *   local p = redis.call('PTTL', KEYS[1])
 *   return {v, p}
 *
 * @param c          Request context.
 * @param key        Null-terminated key.
 * @param ttl_sec    TTL in seconds for newly created key (≤ 0 means none).
 * @param out_ttl_ms Optional pointer to store remaining window TTL in ms.
 * @return The incremented value, or -1 on error. */
static long long
redis_storage_incr_with_ttl(csilk_ctx_t* c, const char* key, int ttl_sec, long long* out_ttl_ms)
{
    if (!c || !key) {
        return -1;
    }
    redis_storage_driver_t* drv = (redis_storage_driver_t*)c->storage_driver;
    if (!drv || !drv->pool) {
        return -1;
    }

    redis_conn_t* conn = (redis_conn_t*)csilk_db_pool_get_connection(drv->pool);
    if (!conn || !conn->c) {
        return -1;
    }

    static const char lua_incr_ttl[] = "local v = redis.call('INCR', KEYS[1]); "
                                       "if v == 1 and tonumber(ARGV[1]) > 0 then "
                                       "redis.call('EXPIRE', KEYS[1], tonumber(ARGV[1])); "
                                       "end; "
                                       "local p = redis.call('PTTL', KEYS[1]); "
                                       "return {v, p};";

    long long val = -1;
    long long ttl_ms = (long long)ttl_sec * 1000;

    /* Pass argv to avoid % format-string interpretation and delimiter injection.
     * EVAL script 1 <key> <ttl>: 5 argv elements. */
    char        ttl_buf[32];
    int         ttl_len = snprintf(ttl_buf, sizeof(ttl_buf), "%d", ttl_sec);
    const char* argv[] = {"EVAL", lua_incr_ttl, "1", key, ttl_buf};
    size_t      lens[] = {4, strlen(lua_incr_ttl), 1, strlen(key), (size_t)ttl_len};
    redisReply* reply = redisCommandArgv(conn->c, 5, argv, lens);
    if (reply) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
            if (reply->element[0]->type == REDIS_REPLY_INTEGER) {
                val = reply->element[0]->integer;
            }
            if (reply->element[1]->type == REDIS_REPLY_INTEGER) {
                long long pttl = reply->element[1]->integer;
                if (pttl > 0) {
                    ttl_ms = pttl;
                }
            }
        } else if (reply->type == REDIS_REPLY_INTEGER) {
            val = reply->integer;
        }
        freeReplyObject(reply);
    }

    /* Fallback if EVAL failed (e.g. redis permissions or older server) */
    if (val < 0) {
        const char* argv[] = {"INCR", key};
        size_t      lens[] = {4, strlen(key)};
        reply = redisCommandArgv(conn->c, 2, argv, lens);
        if (reply) {
            if (reply->type == REDIS_REPLY_INTEGER) {
                val = reply->integer;
                if (val == 1 && ttl_sec > 0) {
                    char        ttl2_buf[32];
                    int         ttl2_len = snprintf(ttl2_buf, sizeof(ttl2_buf), "%d", ttl_sec);
                    const char* exp_argv[] = {"EXPIRE", key, ttl2_buf};
                    size_t      exp_lens[] = {6, strlen(key), (size_t)ttl2_len};
                    redisReply* exp_reply = redisCommandArgv(conn->c, 3, exp_argv, exp_lens);
                    if (exp_reply) {
                        freeReplyObject(exp_reply);
                    }
                }
            }
            freeReplyObject(reply);
        }
    }

    if (out_ttl_ms) {
        *out_ttl_ms = ttl_ms;
    }
    return val;
}

/** @brief Atomically increment a key (INCR) and optionally set a TTL.
 *
 * @param c       Request context.
 * @param key     Null-terminated key.
 * @param ttl_sec TTL to set on a newly-created counter (≤ 0 means none).
 * @return The incremented value, or -1 on error. */
static long long
redis_storage_incr(csilk_ctx_t* c, const char* key, int ttl_sec)
{
    return redis_storage_incr_with_ttl(c, key, ttl_sec, NULL);
}

/** @brief Persist an integer state value for a key (SET, optional TTL).
 * Used for distributed state/flags; returns -1 on null args or a Redis error. */
static int
redis_storage_sync_state(csilk_ctx_t* c, const char* key, int state, int ttl_sec)
{
    if (!c || !key) {
        return -1;
    }
    redis_storage_driver_t* drv = (redis_storage_driver_t*)c->storage_driver;
    if (!drv || !drv->pool) {
        return -1;
    }

    redis_conn_t* conn = (redis_conn_t*)csilk_db_pool_get_connection(drv->pool);
    if (!conn || !conn->c) {
        return -1;
    }

    /* Pass argv to avoid % format-string interpretation and delimiter injection. */
    char        state_buf[32], ttl_buf[32];
    int         state_len = snprintf(state_buf, sizeof(state_buf), "%d", state);
    int         ttl_len = snprintf(ttl_buf, sizeof(ttl_buf), "%d", ttl_sec);
    redisReply* reply = NULL;
    if (ttl_sec > 0) {
        const char* argv[] = {"SET", key, state_buf, "EX", ttl_buf};
        size_t      lens[] = {3, strlen(key), (size_t)state_len, 2, (size_t)ttl_len};
        reply = redisCommandArgv(conn->c, 5, argv, lens);
    } else {
        const char* argv[] = {"SET", key, state_buf};
        size_t      lens[] = {3, strlen(key), (size_t)state_len};
        reply = redisCommandArgv(conn->c, 3, argv, lens);
    }

    int rc = 0;
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        rc = -1;
    }
    if (reply) {
        freeReplyObject(reply);
    }
    return rc;
}

/** @brief Create a new Redis-based storage driver instance.
 *
 * The returned driver wraps the given database pool and implements the
 * csilk_storage_driver_t vtable via Redis SET/GET/INCR commands.
 *
 * @param pool An already-connected Redis database pool.
 * @return A heap-allocated storage driver, or NULL on allocation
 *         failure or null pool. */
csilk_storage_driver_t*
csilk_redis_storage_driver_new(csilk_db_pool_t* pool)
{
    if (!pool) {
        return NULL;
    }
    redis_storage_driver_t* drv = calloc(1, sizeof(redis_storage_driver_t));
    if (!drv) {
        return NULL;
    }

    drv->base.set = redis_storage_set;
    drv->base.get = redis_storage_get;
    drv->base.clear = redis_storage_clear;
    drv->base.set_string = redis_storage_set_string;
    drv->base.get_string = redis_storage_get_string;
    drv->base.incr = redis_storage_incr;
    drv->base.incr_with_ttl = redis_storage_incr_with_ttl;
    drv->base.sync_state = redis_storage_sync_state;
    drv->pool = pool;

    return (csilk_storage_driver_t*)drv;
}

#endif /* HAS_REDIS */
