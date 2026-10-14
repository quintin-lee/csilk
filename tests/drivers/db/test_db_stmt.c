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

    int rc = csilk_db_exec(pool,
                           "CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price REAL, "
                           "stock INTEGER, note TEXT);");
    assert(rc == 0);

    /* 1. Prepare INSERT statement */
    csilk_stmt_t* ins_stmt = NULL;
    assert(csilk_stmt_prepare(
               pool,
               "INSERT INTO products (id, name, price, stock, note) VALUES (?, ?, ?, ?, ?);",
               &ins_stmt) == 0);
    assert(ins_stmt != NULL);

    /* Bind and execute row 1 */
    assert(csilk_stmt_bind_int64(ins_stmt, 1, 1) == 0);
    assert(csilk_stmt_bind_text(ins_stmt, 2, "Laptop") == 0);
    assert(csilk_stmt_bind_double(ins_stmt, 3, 1299.99) == 0);
    assert(csilk_stmt_bind_int64(ins_stmt, 4, 10) == 0);
    assert(csilk_stmt_bind_null(ins_stmt, 5) == 0);

    int64_t affected = 0;
    assert(csilk_stmt_execute(ins_stmt, &affected) == 0);
    assert(affected == 1);

    /* Re-bind same prepared statement and execute row 2 */
    assert(csilk_stmt_bind_int64(ins_stmt, 1, 2) == 0);
    assert(csilk_stmt_bind_text(ins_stmt, 2, "Mouse") == 0);
    assert(csilk_stmt_bind_double(ins_stmt, 3, 29.50) == 0);
    assert(csilk_stmt_bind_int64(ins_stmt, 4, 50) == 0);
    assert(csilk_stmt_bind_text(ins_stmt, 5, "Wireless") == 0);

    assert(csilk_stmt_execute(ins_stmt, &affected) == 0);
    assert(affected == 1);

    csilk_stmt_close(ins_stmt);

    /* 2. Prepare parameterized query */
    csilk_stmt_t* q_stmt = NULL;
    assert(csilk_stmt_prepare(
               pool,
               "SELECT id, name, price, stock, note FROM products WHERE price > ? ORDER BY id;",
               &q_stmt) == 0);
    assert(q_stmt != NULL);

    assert(csilk_stmt_bind_double(q_stmt, 1, 100.0) == 0);

    csilk_json_t* res = csilk_stmt_query_json(q_stmt);
    assert(res != NULL);
    assert(csilk_json_array_size(res) == 1);

    csilk_json_t* row = csilk_json_array_get(res, 0);
    assert(csilk_json_get_int(row, "id") == 1);
    assert(strcmp(csilk_json_get_string(row, "name"), "Laptop") == 0);
    assert(csilk_json_get_number(row, "price") > 1299.0);
    assert(csilk_json_is_null(csilk_json_get(row, "note")) == true);

    csilk_json_free(res);
    csilk_stmt_close(q_stmt);

    csilk_db_pool_free(pool);
    printf("test_db_stmt passed!\n");
    return 0;
}
