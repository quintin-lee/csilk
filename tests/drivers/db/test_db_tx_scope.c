#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

int
main(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != NULL);

    int rc = csilk_db_exec(pool, "CREATE TABLE items (id INTEGER PRIMARY KEY, val TEXT);");
    assert(rc == 0);

    /* Initially not in transaction */
    assert(csilk_db_in_transaction(pool) == 0);

    /* 1. Begin outer transaction */
    csilk_db_tx_scope_t outer_scope;
    assert(csilk_db_tx_scope_begin(pool, "outer", &outer_scope) == 0);
    assert(outer_scope.is_savepoint == false);
    assert(outer_scope.active == true);
    assert(csilk_db_in_transaction(pool) == 1);

    /* Insert 1 into items */
    assert(csilk_db_exec(pool, "INSERT INTO items (id, val) VALUES (1, 'one');") == 0);

    /* 2. Begin inner savepoint */
    csilk_db_tx_scope_t inner_scope;
    assert(csilk_db_tx_scope_begin(pool, "inner", &inner_scope) == 0);
    assert(inner_scope.is_savepoint == true);
    assert(inner_scope.active == true);
    assert(csilk_db_in_transaction(pool) == 1);

    /* Insert 2 into items */
    assert(csilk_db_exec(pool, "INSERT INTO items (id, val) VALUES (2, 'two');") == 0);

    /* Rollback inner savepoint */
    assert(csilk_db_tx_scope_rollback(pool, &inner_scope) == 0);
    assert(inner_scope.active == false);
    assert(csilk_db_in_transaction(pool) == 1); /* Still in outer tx */

    /* Commit outer transaction */
    assert(csilk_db_tx_scope_commit(pool, &outer_scope) == 0);
    assert(outer_scope.active == false);
    assert(csilk_db_in_transaction(pool) == 0);

    /* Verify items table: item 1 exists, item 2 does not */
    csilk_json_t* res = csilk_db_query_json(pool, "SELECT * FROM items ORDER BY id;");
    assert(res != NULL);
    assert(csilk_json_array_size(res) == 1);
    csilk_json_t* row = csilk_json_array_get(res, 0);
    assert(strcmp(csilk_json_get_string(row, "val"), "one") == 0);
    csilk_json_free(res);

    /* 3. Test full rollback of outer transaction */
    assert(csilk_db_tx_scope_begin(pool, "to_abort", &outer_scope) == 0);
    assert(csilk_db_exec(pool, "INSERT INTO items (id, val) VALUES (3, 'three');") == 0);
    assert(csilk_db_tx_scope_rollback(pool, &outer_scope) == 0);

    res = csilk_db_query_json(pool, "SELECT * FROM items WHERE id = 3;");
    assert(res != NULL);
    assert(csilk_json_array_size(res) == 0);
    csilk_json_free(res);

    /* 4. Regression: an oversized savepoint base name must be rejected
     *    (fail-fast) rather than silently truncated into a malformed /
     *    colliding savepoint identifier used by begin/commit/rollback. */
    assert(csilk_db_tx_scope_begin(pool, "oversized_base", &outer_scope) == 0);
    csilk_db_tx_scope_t sp;
    char                  long_name[200];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    /* Inside an active transaction, the savepoint path composes "name_<seq>"
     * into scope->name[64]; a 199-char base cannot fit -> -1. */
    assert(csilk_db_tx_scope_begin(pool, long_name, &sp) == -1);
    assert(sp.active == false);
    /* A normal-length name still works in the same transaction. */
    assert(csilk_db_tx_scope_begin(pool, "good_sp", &sp) == 0);
    assert(sp.is_savepoint == true);
    assert(csilk_db_tx_scope_rollback(pool, &sp) == 0);
    assert(csilk_db_tx_scope_rollback(pool, &outer_scope) == 0);

    csilk_db_pool_free(pool);
    printf("test_db_tx_scope passed!\n");
    return 0;
}
