/**
 * @file session_store.h
 * @brief Pluggable session storage backend abstraction.
 *
 * Defines a vtable interface that allows session data to be backed by
 * different storage engines (in-process sharded hash, Redis, csilk_mq)
 * while keeping the session middleware code path identical.
 *
 * @copyright MIT License
 */

#ifndef CSILK_CORE_SESSION_STORE_H
#define CSILK_CORE_SESSION_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for a session store backend.
 */
typedef struct csilk_session_store_s csilk_session_store_t;

/**
 * @brief Session store vtable.
 *
 * All functions return 0 on success, negative on failure.
 * The `ctx` argument is the owning csilk_session_store_t.
 */
typedef struct {
    /** Load session data from backend. @return 0 on success, -1 on not found. */
    int (*get)(csilk_session_store_t* store,
               const char*            session_id,
               char**                 out_data,
               size_t*                out_len);

    /** Save session data to backend. */
    int (*set)(csilk_session_store_t* store,
               const char*            session_id,
               const char*            data,
               size_t                 len,
               int32_t                ttl_sec);

    /** Delete a session from backend. */
    int (*del)(csilk_session_store_t* store, const char* session_id);

    /** Destroy the store and release resources. */
    void (*destroy)(csilk_session_store_t* store);

    /** Backend name for logging. */
    const char* name;
} csilk_session_store_ops_t;

/**
 * @brief Create a process-local session store (sharded hash table).
 *
 * @return Newly allocated store handle, or NULL on OOM.
 */
csilk_session_store_t* csilk_session_store_new_local(void);

/**
 * @brief Create a Redis-backed session store.
 *
 * @param host     Redis host (e.g. "127.0.0.1").
 * @param port     Redis port (e.g. 6379).
 * @param prefix   Key prefix for session keys (e.g. "csilk:session:").
 * @return Store handle, or NULL on failure.
 */
csilk_session_store_t*
csilk_session_store_new_redis(const char* host, int port, const char* prefix);

/**
 * @brief Create a csilk_mq-backed session store (WAL + raft replication).
 *
 * @param mq       Message queue handle.
 * @param prefix   Key prefix for session keys.
 * @return Store handle, or NULL on failure.
 */
csilk_session_store_t* csilk_session_store_new_mq(void* mq, const char* prefix);

/**
 * @brief Destroy a session store.
 */
void csilk_session_store_destroy(csilk_session_store_t* store);

/**
 * @brief Load session data from the store.
 *
 * @param store     Store handle.
 * @param session_id  Session ID.
 * @param out_data  [out] Allocated data buffer (caller frees).
 * @param out_len   [out] Data length.
 * @return 0 on success, -1 if session not found.
 */
int csilk_session_store_get(csilk_session_store_t* store,
                            const char*            session_id,
                            char**                 out_data,
                            size_t*                out_len);

/**
 * @brief Save session data to the store.
 *
 * @param store     Store handle.
 * @param session_id  Session ID.
 * @param data     Data to store.
 * @param len      Data length.
 * @param ttl_sec  TTL in seconds (0 = no expiry).
 * @return 0 on success.
 */
int csilk_session_store_set(csilk_session_store_t* store,
                            const char*            session_id,
                            const char*            data,
                            size_t                 len,
                            int32_t                ttl_sec);

/**
 * @brief Delete a session from the store.
 *
 * @param store     Store handle.
 * @param session_id  Session ID.
 * @return 0 on success.
 */
int csilk_session_store_del(csilk_session_store_t* store, const char* session_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CSILK_CORE_SESSION_STORE_H */
