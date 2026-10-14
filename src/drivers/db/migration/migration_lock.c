#include "csilk/drivers/db/migration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
csilk_migration_lock_init(csilk_db_pool_t* pool)
{
    if (!pool) {
        return -1;
    }

    const char* ddl = "CREATE TABLE IF NOT EXISTS schema_migration_lock ("
                      "id INTEGER PRIMARY KEY, "
                      "is_locked INTEGER NOT NULL DEFAULT 0, "
                      "locked_at TIMESTAMP, "
                      "locked_by TEXT"
                      ");";
    if (csilk_db_exec(pool, ddl) != 0) {
        return -1;
    }

    const char* init_row =
        "INSERT INTO schema_migration_lock (id, is_locked, locked_by) VALUES (1, 0, '') "
        "ON CONFLICT(id) DO NOTHING;";
    return csilk_db_exec(pool, init_row);
}

int
csilk_migration_lock_acquire(csilk_db_pool_t* pool, const char* locked_by, int timeout_seconds)
{
    if (!pool) {
        return -1;
    }
    if (csilk_migration_lock_init(pool) != 0) {
        return -1;
    }

    const char* who = locked_by ? locked_by : "csilk_instance";
    int         max_attempts = timeout_seconds > 0 ? (timeout_seconds * 10) : 1;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        csilk_stmt_t* stmt = NULL;
        int           rc =
            csilk_stmt_prepare(pool,
                               "UPDATE schema_migration_lock "
                               "SET is_locked = 1, locked_at = CURRENT_TIMESTAMP, locked_by = ? "
                               "WHERE id = 1 AND is_locked = 0;",
                               &stmt);
        if (rc == 0 && stmt) {
            csilk_stmt_bind_text(stmt, 1, who);
            int64_t aff = 0;
            rc = csilk_stmt_execute(stmt, &aff);
            csilk_stmt_close(stmt);

            if (rc == 0 && aff == 1) {
                return 0; /* Successfully acquired */
            }
        }

        if (attempt + 1 < max_attempts) {
            usleep(100000); /* 100ms backoff */
        }
    }

    return -1; /* Timed out or failed */
}

int
csilk_migration_lock_release(csilk_db_pool_t* pool, const char* locked_by)
{
    if (!pool) {
        return -1;
    }

    csilk_stmt_t* stmt = NULL;
    int           rc = 0;
    if (locked_by && locked_by[0]) {
        rc = csilk_stmt_prepare(
            pool,
            "UPDATE schema_migration_lock SET is_locked = 0, locked_at = NULL, locked_by = '' "
            "WHERE id = 1 AND (locked_by = ? OR locked_by = '' OR is_locked = 0);",
            &stmt);
        if (rc == 0 && stmt) {
            csilk_stmt_bind_text(stmt, 1, locked_by);
        }
    } else {
        rc = csilk_stmt_prepare(
            pool,
            "UPDATE schema_migration_lock SET is_locked = 0, locked_at = NULL, locked_by = '' "
            "WHERE id = 1;",
            &stmt);
    }

    if (rc != 0 || !stmt) {
        return -1;
    }

    int64_t aff = 0;
    rc = csilk_stmt_execute(stmt, &aff);
    csilk_stmt_close(stmt);
    return rc;
}
