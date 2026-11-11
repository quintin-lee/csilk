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

struct csilk_db_pool_s {
    csilk_db_driver_t* driver;
    void*              connection;
    csilk_mutex_t      mutex;
    /*
     * Owner-thread identifier for the connection. Set by csilk_db_pool_acquire
     * (or implicitly by csilk_db_with_conn) to the thread that currently holds
     * the raw connection pointer; reset to NULL on csilk_db_pool_put.
     * Under the libuv backend this is a uv_thread_t / csilk_thread_t key; under
     * the uring backend it is a pthread_t. For pools that never use the
     * acquire/with_conn primitives (i.e. only the high-level csilk_db_query_* /
     * csilk_db_exec_* / csilk_stmt_* APIs which always hold pool->mutex), this
     * field stays NULL and acts as a no-op guard.
     *
     * Purpose: enables csilk_db_pool_put to validate that the releasing thread
     * is actually the one that acquired the connection (holder check),
     * preventing "release someone else's conn" bugs that caused the
     * multi-thread UAF reported in csilk#1.
     */
    csilk_thread_t conn_holder;
    bool           conn_holder_set;
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

#endif
