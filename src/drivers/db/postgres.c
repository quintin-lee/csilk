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
#include <poll.h>
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
    return csilk_sql_rewrite_placeholders(sql, out_param_count);
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

/*
 * Map the most recent PostgreSQL failure to a csilk_db_err_t (csilk#8).
 * pg_conn_t does not store the last PQstatus, so this falls back to a
 * connection liveness check.
 */
static int
postgres_last_error(csilk_db_pool_t* pool, csilk_db_err_t* out_err)
{
    if (!pool || !out_err) {
        return -1;
    }
    pg_conn_t* conn = (pg_conn_t*)csilk_db_pool_get_connection(pool);
    if (!conn) {
        *out_err = CSILK_DB_ERR_CONN;
        return 0;
    }
    *out_err = CSILK_DB_ERR_SQL;
    return 0;
}

/*
 * Apply a dialect-specific optimisation on PostgreSQL (csilk#4).  Most
 * SQLite PRAGMA kinds have no PG equivalent: they are a no-op that logs a
 * CSILK_LOG_W.  CSILK_DB_PRAGMA_CUSTOM executes the raw SQL verbatim.
 */
static int
postgres_set_pragma(csilk_db_pool_t* pool, int kind, const char* value)
{
    if (!pool || !csilk_db_pool_get_connection(pool)) {
        return -1;
    }
    switch (kind) {
    case CSILK_DB_PRAGMA_CUSTOM:
        return postgres_exec(pool, value ? value : "");
    case CSILK_DB_PRAGMA_JOURNAL_MODE:
    case CSILK_DB_PRAGMA_BUSY_TIMEOUT:
    case CSILK_DB_PRAGMA_SYNCHRONOUS:
    case CSILK_DB_PRAGMA_FOREIGN_KEYS:
    default:
        CSILK_LOG_W("postgres_set_pragma: kind %d has no PostgreSQL equivalent; no-op "
                    "(use CSILK_DB_PRAGMA_CUSTOM with a raw SET statement if needed)",
                    kind);
        return 0; /* no-op, NOT an error */
    }
}

static int
postgres_set_nonblocking(csilk_db_pool_t* pool, int enable)
{
    if (!pool) {
        return -1;
    }
    int n = pool->max_conns > 0 ? pool->max_conns : 1;
    for (int i = 0; i < n; i++) {
        if (pool->connections[i]) {
            pg_conn_t* conn = (pg_conn_t*)pool->connections[i];
            if (conn->db) {
                PQsetnonblocking(conn->db, enable ? 1 : 0);
            }
        }
    }
    return 0;
}

typedef enum { PG_ASYNC_FLUSHING = 0, PG_ASYNC_READING, PG_ASYNC_CLOSING } pg_async_state_t;

typedef struct {
    csilk_db_pool_t*        pool;
    void*                   raw_conn;
    int                     lease_id;
    pg_conn_t*              conn;
    csilk_io_loop_t*        loop;
    csilk_io_poll_t         poll;
    csilk_io_timer_t        timer;
    pg_async_state_t        state;
    bool                    is_query;
    csilk_db_query_async_cb query_cb;
    csilk_db_exec_async_cb  exec_cb;
    void*                   user_data;
    bool                    poll_started;
    bool                    timer_started;
    int                     closing_count;
    csilk_json_t*           result_json;
    csilk_db_err_t          result_err;
    int                     result_exec_rc;
} pg_async_op_t;

static void
on_pg_handle_closed(csilk_io_handle_t* handle)
{
    pg_async_op_t* op = (pg_async_op_t*)handle->data;
    if (!op) {
        return;
    }
    op->closing_count--;
    if (op->closing_count == 0) {
        if (op->is_query) {
            if (op->query_cb) {
                op->query_cb(op->result_json, op->result_err, op->user_data);
            } else if (op->result_json) {
                csilk_json_free(op->result_json);
            }
        } else {
            if (op->exec_cb) {
                op->exec_cb(op->result_exec_rc, op->user_data);
            }
        }
        free(op);
    }
}

static void
pg_async_finish(
    pg_async_op_t* op, csilk_json_t* json, csilk_db_err_t err, int exec_rc, bool is_healthy)
{
    if (!op || op->state == PG_ASYNC_CLOSING) {
        return;
    }
    op->state = PG_ASYNC_CLOSING;

    op->result_json = json;
    op->result_err = err;
    op->result_exec_rc = exec_rc;

    if (op->timer_started) {
        csilk_io_timer_stop(&op->timer);
        op->timer_started = false;
    }
    if (op->poll_started) {
        csilk_io_poll_stop(&op->poll);
        op->poll_started = false;
    }

    if (op->raw_conn) {
        csilk_db_pool_put_async(op->pool, op->raw_conn, op->lease_id, is_healthy);
        op->raw_conn = NULL;
    }

    op->closing_count = 2;
    op->poll.data = op;
    op->timer.data = op;
    csilk_io_close((csilk_io_handle_t*)&op->poll, on_pg_handle_closed);
    csilk_io_close((csilk_io_handle_t*)&op->timer, on_pg_handle_closed);
}

static void
on_pg_async_timeout(csilk_io_timer_t* timer)
{
    pg_async_op_t* op = (pg_async_op_t*)timer->data;
    if (!op || op->state == PG_ASYNC_CLOSING) {
        return;
    }

    if (op->conn && op->conn->db) {
        PGcancel* cancel = PQgetCancel(op->conn->db);
        if (cancel) {
            char errbuf[256];
            (void)PQcancel(cancel, errbuf, sizeof(errbuf));
            PQfreeCancel(cancel);
        }
    }
    pg_async_finish(op, NULL, CSILK_DB_ERR_BUSY, -1, false);
}

static void
on_pg_async_poll(csilk_io_poll_t* handle, int status, int events)
{
    (void)events;
    pg_async_op_t* op = (pg_async_op_t*)handle->data;
    if (!op || op->state == PG_ASYNC_CLOSING) {
        return;
    }

    if (status < 0) {
        pg_async_finish(op, NULL, CSILK_DB_ERR_CONN, -1, false);
        return;
    }

    if (op->state == PG_ASYNC_FLUSHING) {
        int flush_rc = PQflush(op->conn->db);
        if (flush_rc < 0) {
            pg_async_finish(op, NULL, CSILK_DB_ERR_CONN, -1, false);
            return;
        }
        if (flush_rc == 0) {
            op->state = PG_ASYNC_READING;
            csilk_io_poll_start(&op->poll, CSILK_IO_READABLE, on_pg_async_poll);
            return;
        }
        return;
    }

    if (op->state == PG_ASYNC_READING) {
        if (PQconsumeInput(op->conn->db) == 0) {
            pg_async_finish(op, NULL, CSILK_DB_ERR_CONN, -1, false);
            return;
        }
        if (PQisBusy(op->conn->db)) {
            return;
        }

        PGresult* last_res = NULL;
        while (1) {
            PGresult* r = PQgetResult(op->conn->db);
            if (!r) {
                break;
            }
            if (last_res) {
                PQclear(last_res);
            }
            last_res = r;
        }

        csilk_json_t*  json = NULL;
        csilk_db_err_t err = CSILK_DB_OK;
        int            rc = -1;

        if (last_res) {
            ExecStatusType st = PQresultStatus(last_res);
            if (st == PGRES_TUPLES_OK) {
                if (op->is_query) {
                    json = pg_result_to_json(last_res);
                }
                rc = 0;
            } else if (st == PGRES_COMMAND_OK) {
                rc = 0;
            } else {
                err = CSILK_DB_ERR_SQL;
            }
            PQclear(last_res);
        } else {
            err = CSILK_DB_ERR_CONN;
        }

        pg_async_finish(op, json, err, rc, err != CSILK_DB_ERR_CONN);
    }
}

static int
postgres_submit_async_op(csilk_db_pool_t*        pool,
                         const char*             sql,
                         const char**            params,
                         csilk_io_loop_t*        loop,
                         bool                    is_query,
                         csilk_db_query_async_cb query_cb,
                         csilk_db_exec_async_cb  exec_cb,
                         void*                   user_data)
{
    if (!pool || !sql) {
        return -1;
    }
    if (is_query && !query_cb) {
        return -1;
    }
    if (!is_query && !exec_cb) {
        return -1;
    }

    void* raw_conn = NULL;
    int   lease_id = 0;
    if (csilk_db_pool_acquire_async(pool, &raw_conn, &lease_id) != 0 || !raw_conn) {
        if (is_query) {
            query_cb(NULL, CSILK_DB_ERR_CONN, user_data);
        } else {
            exec_cb(-1, user_data);
        }
        return -1;
    }

    pg_conn_t* conn = (pg_conn_t*)raw_conn;
    PQsetnonblocking(conn->db, 1);

    int ok = 0;
    if (params && params[0]) {
        int   pcount = 0;
        char* rw = pg_rewrite_placeholders(sql, &pcount);
        if (!rw) {
            csilk_db_pool_put_async(pool, raw_conn, lease_id, true);
            if (is_query) {
                query_cb(NULL, CSILK_DB_ERR_OOM, user_data);
            } else {
                exec_cb(-1, user_data);
            }
            return -1;
        }
        ok = PQsendQueryParams(conn->db, rw, pcount, NULL, params, NULL, NULL, 0);
        free(rw);
    } else {
        ok = PQsendQuery(conn->db, sql);
    }

    if (!ok) {
        csilk_db_pool_put_async(pool, raw_conn, lease_id, false);
        if (is_query) {
            query_cb(NULL, CSILK_DB_ERR_SQL, user_data);
        } else {
            exec_cb(-1, user_data);
        }
        return -1;
    }

    pg_async_op_t* op = calloc(1, sizeof(*op));
    if (!op) {
        csilk_db_pool_put_async(pool, raw_conn, lease_id, true);
        if (is_query) {
            query_cb(NULL, CSILK_DB_ERR_OOM, user_data);
        } else {
            exec_cb(-1, user_data);
        }
        return -1;
    }

    op->pool = pool;
    op->raw_conn = raw_conn;
    op->lease_id = lease_id;
    op->conn = conn;
    op->loop = loop ? loop : csilk_io_default_loop();
    op->is_query = is_query;
    op->query_cb = query_cb;
    op->exec_cb = exec_cb;
    op->user_data = user_data;

    int sock = PQsocket(conn->db);
    if (sock < 0) {
        csilk_db_pool_put_async(pool, raw_conn, lease_id, false);
        free(op);
        if (is_query) {
            query_cb(NULL, CSILK_DB_ERR_CONN, user_data);
        } else {
            exec_cb(-1, user_data);
        }
        return -1;
    }

    csilk_io_poll_init_socket(op->loop, &op->poll, sock);
    op->poll.data = op;

    csilk_io_timer_init(op->loop, &op->timer);
    op->timer.data = op;
    csilk_io_timer_start(&op->timer, on_pg_async_timeout, 30000, 0);
    op->timer_started = true;

    int flush_rc = PQflush(conn->db);
    if (flush_rc < 0) {
        pg_async_finish(op, NULL, CSILK_DB_ERR_CONN, -1, false);
        return 0;
    }

    if (flush_rc > 0) {
        op->state = PG_ASYNC_FLUSHING;
        csilk_io_poll_start(&op->poll, CSILK_IO_WRITABLE, on_pg_async_poll);
    } else {
        op->state = PG_ASYNC_READING;
        csilk_io_poll_start(&op->poll, CSILK_IO_READABLE, on_pg_async_poll);
    }
    op->poll_started = true;
    return 0;
}

static int
postgres_query_async(csilk_db_pool_t*        pool,
                     const char*             sql,
                     const char**            params,
                     csilk_io_loop_t*        loop,
                     csilk_db_query_async_cb cb,
                     void*                   user_data)
{
    return postgres_submit_async_op(pool, sql, params, loop, true, cb, NULL, user_data);
}

static int
postgres_exec_async(csilk_db_pool_t*       pool,
                    const char*            sql,
                    const char**           params,
                    csilk_io_loop_t*       loop,
                    csilk_db_exec_async_cb cb,
                    void*                  user_data)
{
    return postgres_submit_async_op(pool, sql, params, loop, false, NULL, cb, user_data);
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
    .last_error = postgres_last_error, /* csilk#8 */
    .set_pragma = postgres_set_pragma, /* csilk#4 */
    .set_nonblocking = postgres_set_nonblocking,
    .query_async = postgres_query_async,
    .exec_async = postgres_exec_async,
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
