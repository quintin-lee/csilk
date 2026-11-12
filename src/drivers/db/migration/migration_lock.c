#include "csilk/drivers/db/migration.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Distributed migration lock with lease timeout.
 *
 * Schema (schema_migration_lock):
 *   id                INTEGER PRIMARY KEY
 *   is_locked         INTEGER NOT NULL DEFAULT 0
 *   locked_at         TIMESTAMP
 *   locked_by         TEXT
 *   lease_expires_at  TIMESTAMP   -- auto-expiry: a crashed holder's lock
 *                                   is reclaimable once this timestamp has
 *                                   passed.
 *
 * The lease makes the lock "true mutex" semantics:
 *
 *   - acquire() writes lease_expires_at (an ISO-8601 string computed on
 *     the csilk side, driver-agnostic), so a crashed holder's lock
 *     auto-expires without a manual table clear.
 *   - acquire() succeeds on `is_locked = 0 OR lease_expires_at <= ?`,
 *     where the parameter is "now + lease" — a timed-out lease is
 *     treated as free.
 *   - release() validates `locked_by = ?` — only the holder may release.
 *     A release by any other instance affects 0 rows and returns -1.
 *     This eliminates the "release someone else's lock" data-corruption
 *     path reported in csilk#7.
 *
 * The lease timestamp is computed on the C side (time() + strftime into
 * an ISO-8601 UTC string) and bound as a parameter.  Both SQLite
 * (TIMESTAMP is TEXT) and PostgreSQL (TIMESTAMP is a native type that
 * accepts an ISO-8601 literal) accept the same string, so the SQL is
 * fully dialect-agnostic — no `CURRENT_TIMESTAMP + N` arithmetic that
 * would need per-driver rendering.
 *
 * The previous implementation had two bugs (both fixed here):
 *
 *   1. release() used `WHERE id = 1` with no locked_by check when
 *      locked_by was NULL/empty, so ANY instance could clear the lock
 *      regardless of ownership.
 *   2. There was no lease_expires_at, so a crashed holder left
 *      is_locked = 1 forever; the lock could only be cleared by manually
 *      deleting the row.
 *
 * @copyright MIT License
 */

#define CSILK_MIGRATION_LOCK_DEFAULT_LEASE_SECONDS 300

static bool
is_mysql_pool(csilk_db_pool_t* pool)
{
    const char* drv = csilk_db_pool_get_driver_name(pool);
    return drv && strcmp(drv, "mysql") == 0;
}

static int
_csilk_migration_lock_ensure_lease_column(csilk_db_pool_t* pool)
{
    /* Add lease_expires_at column (epoch seconds) if missing (idempotent;
     * tolerate the "already exists" failure). */
    int rc = csilk_db_exec(pool,
                           "ALTER TABLE schema_migration_lock ADD COLUMN lease_expires_at BIGINT;");
    return rc; /* caller ignores non-zero (column already present) */
}

int
csilk_migration_lock_init(csilk_db_pool_t* pool)
{
    if (!pool) {
        return -1;
    }

    const char* ddl = is_mysql_pool(pool) ? "CREATE TABLE IF NOT EXISTS schema_migration_lock ("
                                            "id INT PRIMARY KEY, "
                                            "is_locked INT NOT NULL DEFAULT 0, "
                                            "locked_at TIMESTAMP NULL, "
                                            "locked_by VARCHAR(255), "
                                            "lease_expires_at BIGINT"
                                            ");"
                                          : "CREATE TABLE IF NOT EXISTS schema_migration_lock ("
                                            "id INTEGER PRIMARY KEY, "
                                            "is_locked INTEGER NOT NULL DEFAULT 0, "
                                            "locked_at TIMESTAMP, "
                                            "locked_by TEXT, "
                                            "lease_expires_at INTEGER" /* epoch seconds */
                                            ");";
    if (csilk_db_exec(pool, ddl) != 0) {
        return -1;
    }

    /* Tolerate the column already being present on a previously-initialized
     * table (created by the old schema without lease_expires_at). */
    (void)_csilk_migration_lock_ensure_lease_column(pool);

    const char* init_row =
        is_mysql_pool(pool)
            ? "INSERT IGNORE INTO schema_migration_lock (id, is_locked, locked_by) VALUES (1, 0, "
              "'');"
            : "INSERT INTO schema_migration_lock (id, is_locked, locked_by) VALUES (1, 0, '') "
              "ON CONFLICT(id) DO NOTHING;";
    return csilk_db_exec(pool, init_row);
}

/*
 * Acquire with lease.
 *
 * lease_seconds controls how long a holder's lock is valid before another
 * instance may preempt it.  A value of 0 or negative uses the default
 * 300s.
 *
 * The acquire loop:
 *   1. prepare the UPDATE once (outside the loop) — avoids the PG
 *      PQprepare round-trip on every poll (csilk#7 performance fix).
 *   2. bind locked_by = who, lease_expires_at = now_iso + lease_seconds.
 *   3. execute; if 1 row affected, the lock is ours.
 *   4. sleep 100ms and retry up to timeout_seconds * 10 attempts.
 *
 * The WHERE clause accepts both `is_locked = 0` (idle) and
 * `lease_expires_at < <now_iso>` (timed-out lease from a crashed
 * holder), so a dead holder's lock auto-expires.
 */
int
csilk_migration_lock_acquire_lease(csilk_db_pool_t* pool,
                                   const char*      locked_by,
                                   int              timeout_seconds,
                                   int              lease_seconds)
{
    if (!pool) {
        return -1;
    }
    if (csilk_migration_lock_init(pool) != 0) {
        return -1;
    }
    if (lease_seconds <= 0) {
        lease_seconds = CSILK_MIGRATION_LOCK_DEFAULT_LEASE_SECONDS;
    }

    const char* who = locked_by ? locked_by : "csilk_instance";
    int         max_attempts = timeout_seconds > 0 ? (timeout_seconds * 10) : 1;

    /* now_epoch: "now" as a Unix epoch seconds integer.  Used both to
     * store lease_expires_at and to test for expiry, so the comparison
     * is integer arithmetic (dialect-agnostic: both SQLite and PG
     * accept an INTEGER column compared to an integer parameter).  Using
     * epoch seconds avoids the string-dictionary-order trap that ISO-8601
     * strings with sub-second precision would introduce. */
    long long now_epoch = (long long)time(NULL);
    long long expires_epoch = now_epoch + (long long)lease_seconds;

    /* Prepare once, reuse across all attempts. */
    const char*   sql = "UPDATE schema_migration_lock "
                        "SET is_locked = 1, "
                        "    locked_at = CURRENT_TIMESTAMP, "
                        "    locked_by = ?, "
                        "    lease_expires_at = ? "
                        "WHERE id = 1 AND (is_locked = 0 "
                        "                   OR lease_expires_at IS NULL "
                        "                   OR lease_expires_at <= ?);";
    csilk_stmt_t* stmt = NULL;
    int           rc = csilk_stmt_prepare(pool, sql, &stmt);
    if (rc != 0 || !stmt) {
        return -1;
    }

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        csilk_stmt_bind_text(stmt, 1, who);            /* locked_by */
        csilk_stmt_bind_int64(stmt, 2, expires_epoch); /* lease_expires_at (epoch s) */
        csilk_stmt_bind_int64(stmt, 3, now_epoch);     /* WHERE lease_expires_at < now */

        int64_t aff = 0;
        rc = csilk_stmt_execute(stmt, &aff);

        if (rc == 0 && aff == 1) {
            csilk_stmt_close(stmt);
            return 0; /* successfully acquired */
        }

        if (attempt + 1 < max_attempts) {
            usleep(100000); /* 100 ms backoff */
        }
    }

    csilk_stmt_close(stmt);
    return -1; /* timed out or failed */
}

/*
 * Backwards-compatible acquire without an explicit lease.  Uses the
 * default 300s lease.
 */
int
csilk_migration_lock_acquire(csilk_db_pool_t* pool, const char* locked_by, int timeout_seconds)
{
    return csilk_migration_lock_acquire_lease(
        pool, locked_by, timeout_seconds, CSILK_MIGRATION_LOCK_DEFAULT_LEASE_SECONDS);
}

/*
 * Release with holder validation.
 *
 * Only the instance whose identifier matches `locked_by` may release.
 * A release by any other instance (or a release with NULL/empty
 * locked_by) is refused — the WHERE clause `locked_by = ?` only matches
 * the actual holder, so a foreign release affects 0 rows and returns -1.
 * This eliminates the "release someone else's lock" data-corruption path
 * from csilk#7.
 *
 * Callers who never acquired (locked_by is NULL or empty) should NOT
 * call release; there is nothing to release.
 */
int
csilk_migration_lock_release(csilk_db_pool_t* pool, const char* locked_by)
{
    if (!pool) {
        return -1;
    }
    if (!locked_by || !locked_by[0]) {
        /* No holder identifier — refuse.  The old implementation would
         * blindly clear the lock here (WHERE id = 1), which was the root
         * of the data-corruption bug.  Now we require a holder
         * identifier to perform any release. */
        return -1;
    }

    const char*   sql = "UPDATE schema_migration_lock "
                        "SET is_locked = 0, locked_at = NULL, locked_by = '', "
                        "    lease_expires_at = NULL "
                        "WHERE id = 1 AND locked_by = ?;";
    csilk_stmt_t* stmt = NULL;
    int           rc = csilk_stmt_prepare(pool, sql, &stmt);
    if (rc != 0 || !stmt) {
        return -1;
    }
    csilk_stmt_bind_text(stmt, 1, locked_by);

    int64_t aff = 0;
    rc = csilk_stmt_execute(stmt, &aff);
    csilk_stmt_close(stmt);

    if (rc != 0) {
        return -1;
    }
    /* 0 rows affected means the lock was not held by `locked_by` — either
     * it was already released, or it is held by a different instance.
     * Both are errors for the caller. */
    if (aff == 0) {
        return -1;
    }
    return 0;
}
