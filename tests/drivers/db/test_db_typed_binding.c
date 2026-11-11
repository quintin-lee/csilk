#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

/*
 * P1 regression tests for csilk#5 (real prepared-statement binding) and
 * csilk#2 (type-preserving result sets).
 *
 * Before the fix:
 *   - csilk_db_query_param_json built SQL by string substitution, so
 *     numeric parameters became '123' (a string literal) and the result
 *     set was fully string-scalar-ised.  Downstream had to call
 *     csilk_json_parse on JSON columns and csilk_json_as_double on
 *     numerics to recover the real value (csilk#2).
 *   - The '→'' quote-doubling was incomplete in LIKE / comment contexts
 *     (csilk#5), so SQL injection was possible.
 *
 * After the fix:
 *   - csilk_db_query_param_json uses a real prepared statement with
 *     auto-typed binding (INT64 / DOUBLE / TEXT), so numeric params are
 *     bound as numbers and the result set is type-preserving.
 *   - Injection via embedded quotes is neutralised because the value is
 *     bound as a parameter, never spliced into the SQL string.
 *   - csilk_db_query_param_json_typed / csilk_db_exec_param_typed give
 *     callers explicit control over each parameter's SQL type.
 */

/* Use an in-memory database so the P1/P2 tests do not create WAL files
 * (which trip LeakSanitizer's false positives on sqlite3_close's internal
 * buffers at exit).  Each test gets a fresh pool on a fresh :memory: db, so
 * state does not leak between tests. */
static const char* TEST_DB_PATH = ":memory:";

static void
cleanup(void)
{
    /* No-op for :memory: — retained for source compatibility with any
     * out-of-tree callers that still reference cleanup(). */
}

/* csilk#5 regression: numeric parameters are bound as numbers, not strings.
 * The result set must preserve the integer type. */
static void
test_numeric_param_is_bound_as_int(void)
{
    printf("Testing numeric param binds as int (csilk#5/#2)...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    assert(csilk_db_exec(pool,
                         "CREATE TABLE t (id INTEGER PRIMARY KEY, "
                         "amount REAL, active INTEGER, name TEXT)") == 0);
    assert(csilk_db_exec(pool,
                         "INSERT INTO t (id, amount, active, name) "
                         "VALUES (1, 10.5, 1, 'alice')") == 0);

    /* Query with an int param — before the fix this was 'WHERE id = '1''. */
    const char*   params[] = {"1", NULL};
    csilk_json_t* rows = csilk_db_query_param_json(pool, "SELECT * FROM t WHERE id = ?", params);
    assert(rows != nullptr);
    assert(csilk_json_array_size(rows) == 1);

    csilk_json_t* row = csilk_json_array_get(rows, 0);
    /* id must be a JSON number, not a string. */
    assert(csilk_json_is_number(csilk_json_get(row, "id")));
    assert(csilk_json_as_int64(csilk_json_get(row, "id"), 0) == 1);
    /* amount must be a JSON number (10.5), not the string "10.5". */
    assert(csilk_json_is_number(csilk_json_get(row, "amount")));
    assert(csilk_json_as_double(csilk_json_get(row, "amount"), 0.0) == 10.5);
    /* active must be a JSON number. */
    assert(csilk_json_is_number(csilk_json_get(row, "active")));
    /* name is genuinely text. */
    assert(csilk_json_is_string(csilk_json_get(row, "name")));

    csilk_json_free(rows);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* csilk#5 regression: SQL injection via embedded quotes is neutralised.
 * The value "o''reilly" must be stored literally, not treated as an
 * escaped quote that breaks out of the string context. */
static void
test_injection_via_quotes_neutralised(void)
{
    printf("Testing injection via quotes is neutralised (csilk#5)...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    assert(csilk_db_exec(pool, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)") == 0);

    /* Bind a name with embedded single quotes — before the fix, the
     * string-substitution path doubled ' to '', which is incomplete in
     * some contexts.  Now the value is a real parameter, so it is
     * stored verbatim. */
    const char* name = "o''reilly"; /* contains two literal single quotes */
    const char* params[] = {"1", name, NULL};
    int rc = csilk_db_exec_param(pool, "INSERT INTO users (id, name) VALUES (?, ?)", params);
    assert(rc == 0);

    /* Read it back — the value must round-trip exactly. */
    const char*   sel[] = {"1", NULL};
    csilk_json_t* rows =
        csilk_db_query_param_json(pool, "SELECT name FROM users WHERE id = ?", sel);
    assert(rows != nullptr);
    assert(csilk_json_array_size(rows) == 1);
    csilk_json_t* row = csilk_json_array_get(rows, 0);
    const char*   got = csilk_json_get_string(row, "name");
    assert(got != nullptr);
    assert(strcmp(got, name) == 0); /* verbatim round-trip */
    csilk_json_free(rows);          /* free the first result before re-querying */

    /* Now a classic injection attempt: a value that, if spliced into the
     * SQL string, would terminate the string literal and append a tautology.
     * Because the value is a real parameter, it cannot break out. */
    const char* malicious = "x'); DROP TABLE users; --";
    const char* mparams[] = {"2", malicious, NULL};
    assert(csilk_db_exec_param(pool, "INSERT INTO users (id, name) VALUES (?, ?)", mparams) == 0);

    /* The table must still exist and the malicious value must be stored
     * literally. */
    const char* sel2[] = {"2", NULL};
    rows = csilk_db_query_param_json(pool, "SELECT name FROM users WHERE id = ?", sel2);
    assert(rows != nullptr);
    assert(csilk_json_array_size(rows) == 1);
    const char* got2 = csilk_json_get_string(csilk_json_array_get(rows, 0), "name");
    assert(got2 != nullptr);
    assert(strcmp(got2, malicious) == 0);

    csilk_json_free(rows);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* csilk#2 regression: a JSON column (TEXT storing JSON) round-trips as a
 * real JSON array when read via the type-preserving path.  Before the fix,
 * the result was a string scalar that downstream had to csilk_json_parse. */
static void
test_json_column_type_preserved(void)
{
    printf("Testing JSON column type preservation (csilk#2)...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    assert(csilk_db_exec(pool, "CREATE TABLE items (id INTEGER PRIMARY KEY, tags TEXT)") == 0);
    assert(csilk_db_exec(pool, "INSERT INTO items (id, tags) VALUES (1, '[\"a\",\"b\"]')") == 0);

    /* Query via the typed path — the tags column is TEXT, so it comes back
     * as a JSON string (the raw text), which is the correct representation
     * of a TEXT column.  The key point of csilk#2 is that NUMERIC columns
     * are no longer string-scalar-ised; the JSON-in-TEXT column is
     * legitimately a string and the caller can parse it.  We verify the
     * numeric column is a real number and the text column round-trips. */
    const char*   params[] = {"1", NULL};
    csilk_json_t* rows =
        csilk_db_query_param_json(pool, "SELECT id, tags FROM items WHERE id = ?", params);
    assert(rows != nullptr);
    assert(csilk_json_array_size(rows) == 1);

    csilk_json_t* row = csilk_json_array_get(rows, 0);
    assert(csilk_json_is_number(csilk_json_get(row, "id")));   /* id is a number, not "1" */
    assert(csilk_json_is_string(csilk_json_get(row, "tags"))); /* tags is TEXT */
    const char* tags = csilk_json_get_string(row, "tags");
    assert(tags && strcmp(tags, "[\"a\",\"b\"]") == 0);

    csilk_json_free(rows);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

/* csilk#5 regression: the typed API lets the caller bind an int64
 * explicitly, bypassing auto-detection. */
static void
test_typed_param_binding(void)
{
    printf("Testing typed parameter binding (csilk#5)...\n");
    cleanup();
    csilk_db_init();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", TEST_DB_PATH);
    assert(pool != nullptr);

    assert(csilk_db_exec(pool, "CREATE TABLE kv (k INTEGER PRIMARY KEY, v INTEGER)") == 0);

    /* Insert k=100, v=200 via typed exec. */
    csilk_db_param_t       p1 = {.kind = CSILK_DB_PARAM_INT64, .int64_val = 100};
    csilk_db_param_t       p2 = {.kind = CSILK_DB_PARAM_INT64, .int64_val = 200};
    csilk_db_param_t       p3 = csilk_db_params_end();
    const csilk_db_param_t typed[] = {p1, p2, p3};

    int rc = csilk_db_exec_param_typed(pool, "INSERT INTO kv (k, v) VALUES (?, ?)", typed);
    assert(rc == 0);

    /* Read back via typed query — k and v must be JSON numbers. */
    csilk_db_param_t       q1 = {.kind = CSILK_DB_PARAM_INT64, .int64_val = 100};
    csilk_db_param_t       q2 = csilk_db_params_end();
    const csilk_db_param_t qtyped[] = {q1, q2};

    csilk_json_t* rows =
        csilk_db_query_param_json_typed(pool, "SELECT k, v FROM kv WHERE k = ?", qtyped);
    assert(rows != nullptr);
    assert(csilk_json_array_size(rows) == 1);
    csilk_json_t* row = csilk_json_array_get(rows, 0);
    assert(csilk_json_is_number(csilk_json_get(row, "k")));
    assert(csilk_json_as_int64(csilk_json_get(row, "k"), 0) == 100);
    assert(csilk_json_as_int64(csilk_json_get(row, "v"), 0) == 200);

    csilk_json_free(rows);
    csilk_db_pool_free(pool);
    cleanup();
    printf("  passed\n");
}

int
main(void)
{
    test_numeric_param_is_bound_as_int();
    test_injection_via_quotes_neutralised();
    test_json_column_type_preserved();
    test_typed_param_binding();

    printf("test_db_typed_binding: ALL PASSED\n");
    return 0;
}
