#ifndef CSILK_DB_INTERNAL_H
#define CSILK_DB_INTERNAL_H

/**
 * @file db_internal.h
 * @brief Internal definitions for the unified database layer.
 *
 * Declares the private connection-pool struct shared between the core DB
 * registry (db.c) and the individual driver implementations (sqlite.c,
 * mysql.c, postgres.c, redis.c, mongodb.c).
 *
 * @copyright MIT License
 */

#include <csilk/core/sys_io.h>
#include "csilk/core/sync.h"

#include <stdbool.h>

#include "csilk/drivers/db.h"

/** @brief Maximum connections per pool (csilk#6). */
#define CSILK_DB_POOL_MAX_CONNS 32

struct csilk_db_pool_s {
    csilk_db_driver_t* driver;
    void*              connections[CSILK_DB_POOL_MAX_CONNS]; /**< Connection array (csilk#6). */
    int                max_conns;  /**< Number of connections in this pool. */
    void*              connection; /**< = connections[0], backward compat. */
    csilk_mutex_t      mutex;
    /*
     * Per-connection holder tracking (csilk#6).
     * conn_holder[i] / conn_holder_set[i] track which thread owns connections[i].
     * For single-connection pools (max_conns == 1), conn_holder and conn_holder_set
     * remain as the primary fields (backward compat with all existing code).
     * For multi-connection pools, the arrays are used by acquire/put to track
     * per-connection ownership.
     */
    csilk_thread_t conn_holder;     /**< Holder of connections[0] (primary). */
    bool           conn_holder_set; /**< True when connections[0] is held. */
    csilk_thread_t conn_holders[CSILK_DB_POOL_MAX_CONNS]; /**< Per-conn holder (multi-conn mode). */
    bool           conn_holder_flags[CSILK_DB_POOL_MAX_CONNS]; /**< Per-conn held flag. */
    /*
     * Most recent error category (csilk#8).  Set by the core layer when a
     * high-level call (csilk_db_exec / csilk_db_query_json / ...) fails,
     * using either the driver's last_error callback (when present) or the
     * generic fallback.  Read by csilk_db_last_error().
     */
    csilk_db_err_t last_err;
    /*
     * Optional on-open hook (csilk#4).  If non-NULL, invoked by the
     * driver's connect() callback right after the connection is established,
     * letting the downstream apply dialect optimisations (PRAGMA / SET) at
     * connect time rather than in a separate db_init step.
     */
    void (*on_open_cb)(csilk_db_pool_t* pool, void* ctx);
    void* on_open_ctx;
};

struct csilk_stmt_s {
    csilk_db_pool_t* pool;
    void*            handle;
};

/**
 * @brief Rewrite standard ? SQL placeholders to PostgreSQL $1, $2, ... format.
 * Skips ? characters contained within single-quoted string literals.
 * @param sql Input SQL query string.
 * @param out_param_count Receives count of placeholders rewritten.
 * @return Heap-allocated rewritten SQL string (caller frees), or NULL on OOM.
 */
char* csilk_sql_rewrite_placeholders(const char* sql, int* out_param_count);

#endif
