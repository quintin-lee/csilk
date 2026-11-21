/**
 * @file src/drivers/db/tx_scope.c
 * @brief Nested transaction scope and savepoint guard implementation.
 * @copyright MIT License
 */

#include "csilk/drivers/db.h"
#include "db_internal.h"
#include "csilk/csilk.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_uint_fast64_t g_csilk_sp_seq = 0;

/* Maximum savepoint/transaction name length accepted by the SQL layer.  The
 * name is concatenated into SAVEPOINT/RELEASE/ROLLBACK statements; a longer
 * name risks truncation (silent collision) and, if unvalidated, SQL-injection
 * via embedded delimiters/quotes. */
#define CSILK_TX_NAME_MAX 32

/** @brief Validate that `name` is a SQL-safe identifier.
 *
 * Only `[A-Za-z0-9_]` is permitted, bounded to CSILK_TX_NAME_MAX.  This blocks
 * any delimiter/quote injection into the SAVEPOINT/RELEASE/ROLLBACK SQL that
 * the scope guard builds by string concatenation.
 *
 * @return 0 if valid, -1 otherwise. */
static int
_csilk_tx_name_valid(const char* name)
{
    if (!name || !name[0]) {
        return -1;
    }
    size_t len = strlen(name);
    if (len == 0 || len >= CSILK_TX_NAME_MAX) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = name[i];
        if ((ch < '0' || ch > '9') && (ch < 'a' || ch > 'z') && (ch < 'A' || ch > 'Z') &&
            ch != '_') {
            return -1;
        }
    }
    return 0;
}

int
csilk_db_in_transaction(csilk_db_pool_t* pool)
{
    if (!pool) {
        return 0;
    }
    if (pool->driver && pool->driver->in_transaction) {
        return pool->driver->in_transaction(pool);
    }
    return 0;
}

int
csilk_db_tx_scope_begin(csilk_db_pool_t* pool, const char* name, csilk_db_tx_scope_t* scope)
{
    if (!pool || !scope || !name || !name[0]) {
        return -1;
    }
    /* Zero-init first so every rejection path below leaves the scope in a
     * known state (active == false) rather than leaving stack garbage. */
    memset(scope, 0, sizeof(*scope));
    /* Reject names that are not safe SQL identifiers: this guards the
     * SAVEPOINT/RELEASE/ROLLBACK concatenation against injection and bounds
     * the length so scope->name can never be truncated. */
    if (_csilk_tx_name_valid(name) != 0) {
        return -1;
    }

    int in_tx = csilk_db_in_transaction(pool);
    if (in_tx > 0) {
        uint64_t seq = atomic_fetch_add(&g_csilk_sp_seq, 1);
        /* Reserve capacity for the "_<seq>" suffix; if the base name is too
         * long to fit, reject up front rather than silently truncating
         * scope->name (begin/commit/rollback would then run against a
         * truncated, possibly-colliding or malformed savepoint name). */
        int needed = (int)strlen(name) + 32; /* "+_" + up to 20 digits + slack */
        if (needed >= (int)sizeof(scope->name)) {
            return -1;
        }
        snprintf(scope->name, sizeof(scope->name), "%s_%llu", name, (unsigned long long)seq);
        scope->is_savepoint = true;

        char sql[128];
        snprintf(sql, sizeof(sql), "SAVEPOINT %s;", scope->name);
        if (csilk_db_exec(pool, sql) != 0) {
            return -1;
        }
    } else {
        scope->is_savepoint = false;
        snprintf(scope->name, sizeof(scope->name), "%s", name);
        if (csilk_db_exec(pool, "BEGIN TRANSACTION;") != 0) {
            return -1;
        }
    }
    scope->active = true;
    return 0;
}

int
csilk_db_tx_scope_commit(csilk_db_pool_t* pool, csilk_db_tx_scope_t* scope)
{
    if (!pool || !scope || !scope->active) {
        return -1;
    }
    int rc = 0;
    if (scope->is_savepoint) {
        char sql[128];
        snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT %s;", scope->name);
        rc = csilk_db_exec(pool, sql);
    } else {
        rc = csilk_db_exec(pool, "COMMIT;");
    }
    scope->active = false;
    return rc;
}

int
csilk_db_tx_scope_rollback(csilk_db_pool_t* pool, csilk_db_tx_scope_t* scope)
{
    if (!pool || !scope || !scope->active) {
        return -1;
    }
    int rc = 0;
    if (scope->is_savepoint) {
        char sql[128];
        snprintf(sql, sizeof(sql), "ROLLBACK TO SAVEPOINT %s;", scope->name);
        rc = csilk_db_exec(pool, sql);
        snprintf(sql, sizeof(sql), "RELEASE SAVEPOINT %s;", scope->name);
        int rel_rc = csilk_db_exec(pool, sql);
        if (rc == 0 && rel_rc != 0) {
            rc = rel_rc;
        }
    } else {
        rc = csilk_db_exec(pool, "ROLLBACK;");
    }
    scope->active = false;
    return rc;
}
