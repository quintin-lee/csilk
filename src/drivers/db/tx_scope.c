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
    memset(scope, 0, sizeof(*scope));

    int in_tx = csilk_db_in_transaction(pool);
    if (in_tx > 0) {
        uint64_t seq = atomic_fetch_add(&g_csilk_sp_seq, 1);
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
        csilk_db_exec(pool, sql);
    } else {
        rc = csilk_db_exec(pool, "ROLLBACK;");
    }
    scope->active = false;
    return rc;
}
