#ifdef HAS_POSTGRES
/**
 * @file postgres.c
 * @brief PostgreSQL driver for the csilk pluggable DB interface.
 *
 * Implements the csilk_db_driver_t vtable using libpq.
 * Key design points:
 *   - DSN is passed directly to PQconnectdb in standard PostgreSQL
 *     key=value format (e.g., "host=... port=... dbname=...").
 *   - Queries use PQexec (synchronous, full result buffering).
 *   - NULL SQL values are detected via PQgetisnull and stored as NULL
 *     pointers in the result row.
 *   - Transactions use raw SQL sentinel strings (BEGIN/COMMIT/ROLLBACK).
 *   - Prepared statements: ? placeholders are rewritten to $1, $2, …
 *     at prepare time (PQprepare / PQexecPrepared).
 *
 * @copyright MIT License
 */

#include <libpq-fe.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/json/json.h"
#include "csilk/drivers/db.h"
#include "db_internal.h"

/** @brief Per-connection data for the PostgreSQL driver. */
typedef struct {
    PGconn*  db;          /**< libpq connection handle. */
    uint64_t stmt_serial; /**< Counter for unique prepared-statement names. */
} pg_conn_t;

/** @brief Maximum parameters supported per prepared statement. */
#define PG_MAX_PARAMS 64

/**
 * @brief Internal state for a PostgreSQL prepared statement.
 *
 * Holds the server-side prepared-statement name, the bound parameter values
 * (heap-allocated copies), and their count.  NULL parameters are stored as
 * NULL pointers so that PQexecPrepared passes them correctly.
 */
typedef struct {
    PGconn* db;                           /**< Borrowed connection (no ownership). */
    char    stmt_name[64];                /**< Unique name registered with PostgreSQL. */
    int     param_count;                  /**< Number of parameters bound so far. */
    char*   param_values[PG_MAX_PARAMS];  /**< Heap copies of each parameter value. */
    int     param_lengths[PG_MAX_PARAMS]; /**< Byte lengths (text mode — not used). */
    int     param_formats[PG_MAX_PARAMS]; /**< 0 = text. Always text here. */
} pg_stmt_t;

/* -----------------------------------------------------------------------
 * Helper: rewrite ? placeholders to $1, $2, … for PostgreSQL protocol.
 *
 * Returns a heap-allocated string (caller must free), or NULL on OOM.
 * Also writes the number of placeholders found to *out_param_count.
 * ----------------------------------------------------------------------- */
static char*
pg_rewrite_placeholders(const char* sql, int* out_param_count)
{
    if (!sql || !out_param_count) {
        return NULL;
    }

    /* First pass: count ? occurrences to size the output buffer. */
    int    param_count = 0;
    size_t extra = 0; /* extra bytes needed for $N vs ? */
    for (const char* p = sql; *p; p++) {
        if (*p == '?') {
            param_count++;
            /* $N takes 2 + digits chars; ? takes 1.  Worst case: $64 (3 digits). */
            extra += 3; /* over-estimate; we'll trim with realloc if needed */
        }
    }

    size_t src_len = strlen(sql);
    char*  out = malloc(src_len + extra * 3 + 1);
    if (!out) {
        return NULL;
    }

    char* dst = out;
    int   idx = 1;
    for (const char* p = sql; *p; p++) {
        if (*p == '?') {
            dst += sprintf(dst, "$%d", idx++);
        } else {
            *dst++ = *p;
        }
    }
    *dst = '\0';

    *out_param_count = param_count;
    return out;
}

/* -----------------------------------------------------------------------
 * Helper: build a csilk_json_t* array from a PGresult with TUPLES_OK.
 * ----------------------------------------------------------------------- */
static csilk_json_t*
pg_result_to_json(PGresult* pg_res)
{
    csilk_json_t* arr = csilk_json_array();
    if (!arr) {
        return NULL;
    }

    int ntuples = PQntuples(pg_res);
    int nfields = PQnfields(pg_res);

    for (int i = 0; i < ntuples; i++) {
        csilk_json_t* row = csilk_json_object();
        if (!row) {
            csilk_json_free(arr);
            return NULL;
        }
        for (int j = 0; j < nfields; j++) {
            const char* col = PQfname(pg_res, j);
            if (PQgetisnull(pg_res, i, j)) {
                csilk_json_add_null(row, col);
            } else {
                csilk_json_add_string(row, col, PQgetvalue(pg_res, i, j));
            }
        }
        csilk_json_array_append(arr, row);
    }
    return arr;
}

/* -----------------------------------------------------------------------
 * connect / disconnect
 * ----------------------------------------------------------------------- */

/**
 * @brief Open a PostgreSQL connection using the given DSN.
 *
 * The DSN is passed as-is to PQconnectdb, which accepts the standard
 * libpq key=value format.  Connection status is verified via PQstatus
 * before returning success.
 */
static int
postgres_connect(csilk_db_pool_t* pool, const char* dsn)
{
    if (!pool || !dsn) {
        return -1;
    }

    pg_conn_t* conn = calloc(1, sizeof(pg_conn_t));
    if (!conn) {
        return -1;
    }

    conn->db = PQconnectdb(dsn);
    if (!conn->db) {
        CSILK_LOG_E("csilk_db_postgres: PQconnectdb failed (OOM)");
        free(conn);
        return -1;
    }

    if (PQstatus(conn->db) != CONNECTION_OK) {
        CSILK_LOG_E("csilk_db_postgres: connect failed: %s", PQerrorMessage(conn->db));
        PQfinish(conn->db);
        free(conn);
        return -1;
    }

    csilk_db_pool_set_connection(pool, conn);
    return 0;
}

/**
 * @brief Close the PostgreSQL connection and free the connection struct.
 */
static int
postgres_disconnect(csilk_db_pool_t* pool)
{
    if (!pool || !csilk_db_pool_get_connection(pool)) {
        return -1;
    }

    pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);
    if (conn->db) {
        PQfinish(conn->db);
    }
    free(conn);
    csilk_db_pool_set_connection(pool, NULL);
    return 0;
}

/* -----------------------------------------------------------------------
 * free_result
 * ----------------------------------------------------------------------- */

/**
 * @brief Free all memory associated with a query result set.
 * Iterates rows (freeing each value, the values array, and the row),
 * column names, and top-level arrays.  Resets counts to prevent double-free.
 */
static void
postgres_free_result(csilk_db_result_t* result)
{
    if (!result) {
        return;
    }
    for (int i = 0; i < result->row_count; i++) {
        csilk_db_row_t* row = result->rows[i];
        if (row) {
            for (int j = 0; j < row->count; j++) {
                free(row->values[j]);
            }
            free(row->values);
            free(row);
        }
    }
    free(result->rows);
    for (int i = 0; i < result->column_count; i++) {
        free(result->column_names[i]);
    }
    free(result->column_names);
    result->rows = NULL;
    result->column_names = NULL;
    result->row_count = 0;
    result->column_count = 0;
}

/* -----------------------------------------------------------------------
 * query (SELECT → csilk_db_result_t)
 * ----------------------------------------------------------------------- */

/**
 * @brief Execute a SELECT query and buffer the full result set.
 *
 * Flow:
 *   1. Send the query via PQexec.
 *   2. Check result status: PGRES_TUPLES_OK indicates a query return,
 *      PGRES_COMMAND_OK means no result rows (return empty).
 *   3. Extract column names via PQfname.
 *   4. Iterate rows with PQgetvalue, using PQgetisnull to detect SQL NULLs.
 *   5. On any allocation failure, partial results are freed immediately.
 *
 * @note NULL SQL values are stored as NULL pointers in the row's values
 *       array, not as empty strings.
 */
static int
postgres_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
    if (!pool || !csilk_db_pool_get_connection(pool) || !sql || !result) {
        return -1;
    }

    pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);

    PGresult* pg_res = PQexec(conn->db, sql);
    if (!pg_res) {
        return -1;
    }

    ExecStatusType status = PQresultStatus(pg_res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        CSILK_LOG_E("csilk_db_postgres: query failed: %s", PQresultErrorMessage(pg_res));
        PQclear(pg_res);
        return -1;
    }

    /* Non-SELECT commands produce no tuples */
    if (status == PGRES_COMMAND_OK) {
        PQclear(pg_res);
        return 0;
    }

    int ntuples = PQntuples(pg_res);
    int nfields = PQnfields(pg_res);

    /* --- Extract column metadata --- */
    result->column_count = nfields;
    result->column_names = calloc(nfields, sizeof(char*));
    if (!result->column_names && nfields > 0) {
        PQclear(pg_res);
        return -1;
    }
    for (int i = 0; i < nfields; i++) {
        result->column_names[i] = strdup(PQfname(pg_res, i));
    }

    /* --- Iterate rows --- */
    for (int i = 0; i < ntuples; i++) {
        csilk_db_row_t* drow = calloc(1, sizeof(csilk_db_row_t));
        if (!drow) {
            PQclear(pg_res);
            postgres_free_result(result);
            return -1;
        }

        drow->count = nfields;
        drow->values = calloc(nfields, sizeof(char*));
        if (!drow->values) {
            free(drow);
            PQclear(pg_res);
            postgres_free_result(result);
            return -1;
        }

        /* Copy each field; NULL SQL values remain NULL pointers */
        for (int j = 0; j < nfields; j++) {
            drow->values[j] = PQgetisnull(pg_res, i, j) ? NULL : strdup(PQgetvalue(pg_res, i, j));
        }

        /* Append row to result array */
        csilk_db_row_t** new_rows =
            realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
        if (!new_rows) {
            free(drow->values);
            free(drow);
            PQclear(pg_res);
            postgres_free_result(result);
            return -1;
        }
        result->rows = new_rows;
        result->rows[result->row_count++] = drow;
    }

    PQclear(pg_res);
    return 0;
}

/* -----------------------------------------------------------------------
 * exec (DDL / DML, no result rows)
 * ----------------------------------------------------------------------- */

/**
 * @brief Execute a SQL statement that returns no result rows.
 *
 * Sends the statement via PQexec and checks for error status.  Unlike the
 * MySQL driver, no extra result draining is needed since PQexec handles
 * single-statement execution atomically.
 */
static int
postgres_exec(csilk_db_pool_t* pool, const char* sql)
{
    if (!pool || !csilk_db_pool_get_connection(pool) || !sql) {
        return -1;
    }

    pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);

    PGresult* pg_res = PQexec(conn->db, sql);
    if (!pg_res) {
        return -1;
    }

    ExecStatusType status = PQresultStatus(pg_res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        CSILK_LOG_E("csilk_db_postgres: exec failed: %s", PQresultErrorMessage(pg_res));
        PQclear(pg_res);
        return -1;
    }

    PQclear(pg_res);
    return 0;
}

/* -----------------------------------------------------------------------
 * Transaction helpers
 * ----------------------------------------------------------------------- */

/** @brief Begin a transaction (sends "BEGIN"). */
static int
postgres_transaction_begin(csilk_db_pool_t* pool)
{
    return postgres_exec(pool, "BEGIN");
}

/** @brief Commit the current transaction (sends "COMMIT"). */
static int
postgres_transaction_commit(csilk_db_pool_t* pool)
{
    return postgres_exec(pool, "COMMIT");
}

/** @brief Rollback the current transaction (sends "ROLLBACK"). */
static int
postgres_transaction_rollback(csilk_db_pool_t* pool)
{
    return postgres_exec(pool, "ROLLBACK");
}

/** @brief Check if the connection is inside an active transaction. */
static int
postgres_in_transaction(csilk_db_pool_t* pool)
{
    if (!pool || !csilk_db_pool_get_connection(pool)) {
        return -1;
    }
    pg_conn_t*              conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);
    PGTransactionStatusType ts = PQtransactionStatus(conn->db);
    return (ts == PQTRANS_INTRANS || ts == PQTRANS_INERROR) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Prepared-statement API
 *
 * PostgreSQL uses $1, $2, … as parameter placeholders, not ?.
 * We rewrite the SQL at prepare time, then store bound values in a
 * pg_stmt_t and execute with PQexecPrepared.
 * ----------------------------------------------------------------------- */

/**
 * @brief Prepare a SQL statement (rewriting ? → $N placeholders).
 *
 * Uses PQprepare to register a named plan on the server.  The statement
 * name is unique per connection using an atomic serial counter to avoid
 * conflicts when multiple migrations run sequentially.
 */
static int
postgres_stmt_prepare(csilk_db_pool_t* pool, const char* sql, csilk_stmt_t** out_stmt)
{
    if (!pool || !sql || !out_stmt) {
        return -1;
    }
    pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);
    if (!conn || !conn->db) {
        return -1;
    }

    /* Rewrite ? → $1, $2, … */
    int   param_count = 0;
    char* pg_sql = pg_rewrite_placeholders(sql, &param_count);
    if (!pg_sql) {
        return -1;
    }

    /* Generate a unique statement name for this connection */
    char stmt_name[64];
    snprintf(
        stmt_name, sizeof(stmt_name), "csilk_stmt_%llu", (unsigned long long)++conn->stmt_serial);

    /* PQprepare: nParams=0 means PostgreSQL infers param types */
    PGresult* res = PQprepare(conn->db, stmt_name, pg_sql, 0, NULL);
    free(pg_sql);

    if (!res || PQresultStatus(res) != PGRES_COMMAND_OK) {
        CSILK_LOG_E("postgres_stmt_prepare: PQprepare failed: %s",
                    res ? PQresultErrorMessage(res) : "(null)");
        if (res) {
            PQclear(res);
        }
        return -1;
    }
    PQclear(res);

    /* Allocate pg_stmt_t */
    pg_stmt_t* ps = calloc(1, sizeof(pg_stmt_t));
    if (!ps) {
        /* Deallocate the plan to avoid leaking server-side resources */
        char deallocate_sql[128];
        snprintf(deallocate_sql, sizeof(deallocate_sql), "DEALLOCATE \"%s\";", stmt_name);
        PGresult* dr = PQexec(conn->db, deallocate_sql);
        if (dr) {
            PQclear(dr);
        }
        return -1;
    }
    ps->db = conn->db;
    ps->param_count = 0; /* params bound via bind_* calls */
    strncpy(ps->stmt_name, stmt_name, sizeof(ps->stmt_name) - 1);

    /* Wrap in csilk_stmt_t */
    csilk_stmt_t* stmt = calloc(1, sizeof(csilk_stmt_t));
    if (!stmt) {
        char deallocate_sql[128];
        snprintf(deallocate_sql, sizeof(deallocate_sql), "DEALLOCATE \"%s\";", stmt_name);
        PGresult* dr = PQexec(conn->db, deallocate_sql);
        if (dr) {
            PQclear(dr);
        }
        free(ps);
        return -1;
    }
    stmt->pool = pool;
    stmt->handle = ps;
    *out_stmt = stmt;
    return 0;
}

/** @brief Bind a 64-bit integer parameter (converted to text for libpq). */
static int
postgres_stmt_bind_int64(csilk_stmt_t* stmt, int index, int64_t val)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;
    int        i = index - 1; /* csilk uses 1-based indexing */
    if (i < 0 || i >= PG_MAX_PARAMS) {
        return -1;
    }
    free(ps->param_values[i]);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    ps->param_values[i] = strdup(buf);
    if (!ps->param_values[i]) {
        return -1;
    }
    ps->param_lengths[i] = 0;
    ps->param_formats[i] = 0;
    if (i + 1 > ps->param_count) {
        ps->param_count = i + 1;
    }
    return 0;
}

/** @brief Bind a double parameter (converted to text for libpq). */
static int
postgres_stmt_bind_double(csilk_stmt_t* stmt, int index, double val)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;
    int        i = index - 1;
    if (i < 0 || i >= PG_MAX_PARAMS) {
        return -1;
    }
    free(ps->param_values[i]);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", val);
    ps->param_values[i] = strdup(buf);
    if (!ps->param_values[i]) {
        return -1;
    }
    ps->param_lengths[i] = 0;
    ps->param_formats[i] = 0;
    if (i + 1 > ps->param_count) {
        ps->param_count = i + 1;
    }
    return 0;
}

/** @brief Bind a text parameter. */
static int
postgres_stmt_bind_text(csilk_stmt_t* stmt, int index, const char* val)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;
    int        i = index - 1;
    if (i < 0 || i >= PG_MAX_PARAMS) {
        return -1;
    }
    free(ps->param_values[i]);
    ps->param_values[i] = val ? strdup(val) : NULL;
    if (val && !ps->param_values[i]) {
        return -1;
    }
    ps->param_lengths[i] = 0;
    ps->param_formats[i] = 0;
    if (i + 1 > ps->param_count) {
        ps->param_count = i + 1;
    }
    return 0;
}

/** @brief Bind a NULL parameter. */
static int
postgres_stmt_bind_null(csilk_stmt_t* stmt, int index)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;
    int        i = index - 1;
    if (i < 0 || i >= PG_MAX_PARAMS) {
        return -1;
    }
    free(ps->param_values[i]);
    ps->param_values[i] = NULL; /* NULL → SQL NULL in PQexecPrepared */
    ps->param_lengths[i] = 0;
    ps->param_formats[i] = 0;
    if (i + 1 > ps->param_count) {
        ps->param_count = i + 1;
    }
    return 0;
}

/**
 * @brief Execute a prepared statement (DML path — returns affected rows).
 *
 * Uses PQexecPrepared with the bound parameter array.  PGRES_COMMAND_OK
 * indicates success; affected row count is parsed from PQcmdTuples.
 */
static int
postgres_stmt_execute(csilk_stmt_t* stmt, int64_t* out_affected_rows)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;

    PGresult* res = PQexecPrepared(ps->db,
                                   ps->stmt_name,
                                   ps->param_count,
                                   (const char* const*)ps->param_values,
                                   ps->param_lengths,
                                   ps->param_formats,
                                   0 /* results in text format */);
    if (!res) {
        CSILK_LOG_E("postgres_stmt_execute: PQexecPrepared returned NULL");
        return -1;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        CSILK_LOG_E("postgres_stmt_execute failed: %s", PQresultErrorMessage(res));
        PQclear(res);
        return -1;
    }

    if (out_affected_rows) {
        const char* tuples = PQcmdTuples(res);
        *out_affected_rows = (tuples && tuples[0]) ? (int64_t)atoll(tuples) : 0;
    }

    PQclear(res);
    return 0;
}

/**
 * @brief Execute a prepared statement (SELECT path — returns JSON array).
 *
 * Uses PQexecPrepared and converts the result set to a csilk_json_t* array
 * of objects, mirroring the sqlite driver's behaviour.
 */
static csilk_json_t*
postgres_stmt_query_json(csilk_stmt_t* stmt)
{
    if (!stmt || !stmt->handle) {
        return NULL;
    }
    pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;

    PGresult* res = PQexecPrepared(ps->db,
                                   ps->stmt_name,
                                   ps->param_count,
                                   (const char* const*)ps->param_values,
                                   ps->param_lengths,
                                   ps->param_formats,
                                   0);
    if (!res) {
        CSILK_LOG_E("postgres_stmt_query_json: PQexecPrepared returned NULL");
        return NULL;
    }

    ExecStatusType status = PQresultStatus(res);
    if (status == PGRES_COMMAND_OK) {
        /* No rows (e.g. UPDATE/DELETE called via query path) */
        PQclear(res);
        return csilk_json_array();
    }
    if (status != PGRES_TUPLES_OK) {
        CSILK_LOG_E("postgres_stmt_query_json failed: %s", PQresultErrorMessage(res));
        PQclear(res);
        return NULL;
    }

    csilk_json_t* arr = pg_result_to_json(res);
    PQclear(res);
    return arr;
}

/**
 * @brief Close a prepared statement: free bound params, deallocate the
 *        server-side plan, and release the pg_stmt_t + csilk_stmt_t.
 */
static void
postgres_stmt_close(csilk_stmt_t* stmt)
{
    if (!stmt) {
        return;
    }
    if (stmt->handle) {
        pg_stmt_t* ps = (pg_stmt_t*)stmt->handle;

        /* Free all bound parameter copies */
        for (int i = 0; i < PG_MAX_PARAMS; i++) {
            free(ps->param_values[i]);
            ps->param_values[i] = NULL;
        }

        /* Ask the server to discard the prepared plan */
        if (ps->db && ps->stmt_name[0]) {
            char deallocate_sql[128];
            snprintf(deallocate_sql, sizeof(deallocate_sql), "DEALLOCATE \"%s\";", ps->stmt_name);
            PGresult* dr = PQexec(ps->db, deallocate_sql);
            if (dr) {
                PQclear(dr);
            }
        }

        free(ps);
        stmt->handle = NULL;
    }
    free(stmt);
}

/* -----------------------------------------------------------------------
 * Driver vtable
 * ----------------------------------------------------------------------- */

/** @brief Pre-built driver vtable for PostgreSQL. */
csilk_db_driver_t csilk_db_postgres_driver = {
    .name = "postgres",
    .connect = postgres_connect,
    .disconnect = postgres_disconnect,
    .query = postgres_query,
    .exec = postgres_exec,
    .transaction_begin = postgres_transaction_begin,
    .transaction_commit = postgres_transaction_commit,
    .transaction_rollback = postgres_transaction_rollback,
    .in_transaction = postgres_in_transaction,
    .free_result = postgres_free_result,
    .stmt_prepare = postgres_stmt_prepare,
    .stmt_bind_int64 = postgres_stmt_bind_int64,
    .stmt_bind_double = postgres_stmt_bind_double,
    .stmt_bind_text = postgres_stmt_bind_text,
    .stmt_bind_null = postgres_stmt_bind_null,
    .stmt_execute = postgres_stmt_execute,
    .stmt_query_json = postgres_stmt_query_json,
    .stmt_close = postgres_stmt_close,
};

/**
 * @brief Register the PostgreSQL driver with the database subsystem.
 * Only compiled when HAS_POSTGRES is defined.
 */
void
csilk_db_postgres_init(void)
{
    csilk_db_register_driver("postgres", &csilk_db_postgres_driver);
}

#endif /* HAS_POSTGRES */
