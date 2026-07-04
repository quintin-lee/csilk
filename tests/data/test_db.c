#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

void
test_db_init(void)
{
    csilk_db_init();
    printf("test_db_init passed\n");
}

void
test_db_sqlite_create_table(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_db.db");
    assert(pool != nullptr);

    int rc = csilk_db_exec(pool,
                           "CREATE TABLE IF NOT EXISTS test_items (id INTEGER "
                           "PRIMARY KEY, value TEXT)");
    assert(rc == 0);

    csilk_db_pool_free(pool);

    /* Clean up test database */
    remove("test_db.db");
    remove("test_db.db-journal");
    printf("test_db_sqlite_create_table passed\n");
}

void
test_db_sqlite_insert_query(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_db.db");
    assert(pool != nullptr);

    /* Create table */
    csilk_db_exec(pool,
                  "CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY "
                  "KEY, name TEXT)");

    /* Insert */
    const char* params[] = {"Alice", nullptr};
    cJSON* result = csilk_db_query_param_json(pool, "INSERT INTO users (name) VALUES (?)", params);
    if (result) {
        cJSON_Delete(result);
    }

    /* Query */
    cJSON* users = csilk_db_query_json(pool, "SELECT * FROM users");
    assert(users != nullptr);
    assert(cJSON_GetArraySize(users) == 1);

    cJSON* user = cJSON_GetArrayItem(users, 0);
    cJSON* name = cJSON_GetObjectItem(user, "name");
    assert(name != nullptr);
    assert(strcmp(name->valuestring, "Alice") == 0);

    cJSON_Delete(users);
    csilk_db_pool_free(pool);

    /* Clean up */
    remove("test_db.db");
    remove("test_db.db-journal");
    printf("test_db_sqlite_insert_query passed\n");
}

void
test_db_sqlite_transactions(void)
{
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "test_db.db");
    assert(pool != nullptr);

    csilk_db_exec(pool,
                  "CREATE TABLE IF NOT EXISTS accounts (id INTEGER PRIMARY KEY, "
                  "balance INTEGER)");

    /* Begin transaction */
    csilk_db_exec(pool, "BEGIN");

    /* Insert multiple rows */
    csilk_db_exec(pool, "INSERT INTO accounts (balance) VALUES (100)");
    csilk_db_exec(pool, "INSERT INTO accounts (balance) VALUES (200)");

    /* Commit */
    csilk_db_exec(pool, "COMMIT");

    /* Verify */
    cJSON* accounts = csilk_db_query_json(pool, "SELECT SUM(balance) as total FROM accounts");
    assert(accounts != nullptr);
    cJSON* acc = cJSON_GetArrayItem(accounts, 0);
    cJSON* total = cJSON_GetObjectItem(acc, "total");
    assert(total != nullptr);
    assert(strcmp(total->valuestring, "300") == 0);

    cJSON_Delete(accounts);
    csilk_db_pool_free(pool);

    /* Clean up */
    remove("test_db.db");
    remove("test_db.db-journal");
    printf("test_db_sqlite_transactions passed\n");
}

int
main(void)
{
    test_db_init();
    test_db_sqlite_create_table();
    test_db_sqlite_insert_query();
    test_db_sqlite_transactions();
    printf("All db tests passed\n");
    return 0;
}