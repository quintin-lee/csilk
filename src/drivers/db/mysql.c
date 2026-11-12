#ifdef HAS_MYSQL
/**
 * @file mysql.c
 * @brief MySQL database driver for the csilk pluggable DB interface.
 *
 * Implements the csilk_db_driver_t vtable using libmysqlclient.
 * Key design points:
 *   - DSN format: "host=...;port=...;user=...;password=...;dbname=..."
 *   - Uses mysql_real_connect for TCP connections (no Unix socket support yet).
 *   - Query results are fully buffered via mysql_store_result.
 *   - Transactions use raw SQL sentinel strings (BEGIN/COMMIT/ROLLBACK).
 *
 * @copyright MIT License
 */

#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>

/* MySQL 8.0+ removed `my_bool` in favour of standard C `bool`, while MariaDB
 * Connector/C still defines `my_bool` as `char`.  Infer the exact indicator
 * pointee type directly from MYSQL_BIND::is_null using C23 `typeof` so
 * mysql.c builds warning-free on both MySQL 8.0+ and MariaDB. */
typedef typeof(*((MYSQL_BIND*)0)->is_null) csilk_mysql_bool_t;

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "db_internal.h"

/* strndup is POSIX.1-2008 but may not be visible on musl-based systems
 * (Alpine Linux) without the correct feature test macros. We always
 * use this local wrapper instead of calling strndup directly. */
static inline char*
csilk_strndup(const char* s, size_t n)
{
    size_t len = strnlen(s, n);
    char*  copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

/** @brief Per-connection data for the MySQL driver. */
typedef struct {
    MYSQL* db; /**< libmysqlclient connection handle. */
} mysql_conn_t;

/**
 * @brief Parse a semicolon-delimited DSN string into connection parameters.
 *
 * Expected format (all fields optional except as noted):
 *   "host=...;port=...;user=...;password=...;dbname=..."
 * Port defaults to 3306 if omitted.  Unknown keys are silently ignored.
 * The input string is duplicated internally for tokenisation.
 *
 * @param dsn      Raw DSN string (may be NULL).
 * @param host     [out] Heap-allocated host string (or NULL).
 * @param port     [out] Port number (defaults to 3306).
 * @param user     [out] Heap-allocated username (or NULL).
 * @param password [out] Heap-allocated password (or NULL).
 * @param dbname   [out] Heap-allocated database name (or NULL).
 */
static void
mysql_parse_dsn(
    const char* dsn, char** host, int* port, char** user, char** password, char** dbname)
{
    *host = NULL;
    *port = 3306;
    *user = NULL;
    *password = NULL;
    *dbname = NULL;
    if (!dsn) {
        return;
    }

    char* buf = strdup(dsn);
    if (!buf) {
        return;
    }

    char* token = strtok(buf, ";");
    while (token) {
        while (*token == ' ') {
            token++;
        }
        char* eq = strchr(token, '=');
        if (!eq) {
            token = strtok(NULL, ";");
            continue;
        }
        *eq = '\0';
        const char* key = token;
        const char* val = eq + 1;

        if (strcmp(key, "host") == 0) {
            free(*host);
            *host = strdup(val);
        } else if (strcmp(key, "port") == 0) {
            *port = atoi(val);
        } else if (strcmp(key, "user") == 0) {
            free(*user);
            *user = strdup(val);
        } else if (strcmp(key, "password") == 0) {
            free(*password);
            *password = strdup(val);
        } else if (strcmp(key, "dbname") == 0) {
            free(*dbname);
            *dbname = strdup(val);
        }

        token = strtok(NULL, ";");
    }
    free(buf);
}

/**
 * @brief Open a MySQL connection using the given DSN.
 *
 * Allocates a connection struct, initialises the MYSQL handle, parses the DSN,
 * and calls mysql_real_connect.  On failure the connection and struct are
 * cleaned up before returning -1.
 */
static int
mysql_drv_connect(csilk_db_pool_t* pool, const char* dsn)
{
    if (!pool || !dsn) {
        return -1;
    }

    mysql_conn_t* conn = calloc(1, sizeof(mysql_conn_t));
    if (!conn) {
        return -1;
    }

    conn->db = mysql_init(NULL);
    if (!conn->db) {
        CSILK_LOG_E("csilk_db_mysql: mysql_init failed");
        free(conn);
        return -1;
    }

    char *host = NULL, *user = NULL, *password = NULL, *dbname = NULL;
    int   port = 3306;
    mysql_parse_dsn(dsn, &host, &port, &user, &password, &dbname);

    /* TCP connection; no client flags set */
    MYSQL* ret = mysql_real_connect(conn->db, host, user, password, dbname, port, NULL, 0);
    free(host);
    free(user);
    free(password);
    free(dbname);

    if (!ret) {
        CSILK_LOG_E("csilk_db_mysql: connect failed: %s", mysql_error(conn->db));
        mysql_close(conn->db);
        free(conn);
        return -1;
    }

    csilk_db_pool_set_connection(pool, conn);
    return 0;
}

/**
 * @brief Close the MySQL connection and free the connection struct.
 */
static int
mysql_drv_disconnect(csilk_db_pool_t* pool)
{
    if (!pool || !csilk_db_pool_get_connection(pool)) {
        return -1;
    }

    mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);
    if (conn->db) {
        mysql_close(conn->db);
    }
    free(conn);
    csilk_db_pool_set_connection(pool, NULL);
    return 0;
}

/**
 * @brief Free all memory associated with a query result set.
 *
 * Iterates over rows (freeing each value string, the values array, and the
 * row struct), then frees column names and the top-level arrays.  Counts
 * are reset to zero to prevent double-free.
 */
static void
mysql_free_csilk_result(csilk_db_result_t* result)
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

/**
 * @brief Execute a SELECT query and buffer the full result set.
 *
 * Flow:
 *   1. Execute via mysql_real_query.
 *   2. Buffer the entire result with mysql_store_result (all rows in memory).
 *      If the query produces no result set (e.g. a non-SELECT), return 0
 *      with an empty result.
 *   3. Extract column names from the MYSQL_FIELD metadata.
 *   4. Iterate mysql_fetch_row, allocating a csilk_db_row_t per row.
 *      Each cell is duplicated via strndup (respecting binary-safe lengths).
 *   5. On any allocation failure, partial results are freed immediately.
 *
 * @note Uses strndup with mysql_fetch_lengths for binary-safe field copies.
 *       NULL SQL values remain NULL in the result row.
 */
static int
mysql_drv_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result)
{
    if (!pool || !csilk_db_pool_get_connection(pool) || !sql || !result) {
        return -1;
    }

    mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);

    if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
        CSILK_LOG_E("csilk_db_mysql: query failed: %s", mysql_error(conn->db));
        return -1;
    }

    /* Buffer the entire result set client-side */
    MYSQL_RES* mysql_res = mysql_store_result(conn->db);
    if (!mysql_res) {
        return 0;
    }

    /* --- Extract column metadata --- */
    MYSQL_FIELD* fields = mysql_fetch_fields(mysql_res);
    unsigned int num_fields = mysql_num_fields(mysql_res);

    result->column_count = (int)num_fields;
    result->column_names = calloc(num_fields, sizeof(char*));
    if (!result->column_names) {
        mysql_free_result(mysql_res);
        return -1;
    }
    for (unsigned int i = 0; i < num_fields; i++) {
        result->column_names[i] = strdup(fields[i].name);
    }

    /* --- Iterate rows --- */
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(mysql_res))) {
        unsigned long* lengths = mysql_fetch_lengths(mysql_res);

        csilk_db_row_t* drow = calloc(1, sizeof(csilk_db_row_t));
        if (!drow) {
            mysql_free_result(mysql_res);
            mysql_free_csilk_result(result);
            return -1;
        }

        drow->count = (int)num_fields;
        drow->values = calloc(num_fields, sizeof(char*));
        if (!drow->values) {
            free(drow);
            mysql_free_result(mysql_res);
            mysql_free_csilk_result(result);
            return -1;
        }

        /* Copy each field using the binary-safe length from mysql_fetch_lengths */
        for (unsigned int i = 0; i < num_fields; i++) {
            drow->values[i] = row[i] ? csilk_strndup(row[i], lengths[i]) : NULL;
        }

        /* Append row to the result array */
        csilk_db_row_t** new_rows =
            realloc(result->rows, (result->row_count + 1) * sizeof(csilk_db_row_t*));
        if (!new_rows) {
            free(drow->values);
            free(drow);
            mysql_free_result(mysql_res);
            mysql_free_csilk_result(result);
            return -1;
        }
        result->rows = new_rows;
        result->rows[result->row_count++] = drow;
    }

    mysql_free_result(mysql_res);
    return 0;
}

/**
 * @brief Execute a SQL statement that returns no result rows.
 *
 * After execution, any result set is consumed and discarded via
 * mysql_store_result + mysql_free_result.  This handles cases where
 * the statement produces rows even though the caller expects none.
 */
static int
mysql_drv_exec(csilk_db_pool_t* pool, const char* sql)
{
    if (!pool || !csilk_db_pool_get_connection(pool) || !sql) {
        return -1;
    }

    mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);

    if (mysql_real_query(conn->db, sql, strlen(sql)) != 0) {
        CSILK_LOG_E("csilk_db_mysql: exec failed: %s", mysql_error(conn->db));
        return -1;
    }

    /* Drain any remaining result (important for multi-statement or
   * statements that unexpectedly produce rows) */
    MYSQL_RES* res = mysql_store_result(conn->db);
    if (res) {
        mysql_free_result(res);
    }

    return 0;
}

/** @brief Begin a transaction (sends "BEGIN"). */
static int
mysql_drv_transaction_begin(csilk_db_pool_t* pool)
{
    return mysql_drv_exec(pool, "BEGIN");
}

/** @brief Commit the current transaction (sends "COMMIT"). */
static int
mysql_drv_transaction_commit(csilk_db_pool_t* pool)
{
    return mysql_drv_exec(pool, "COMMIT");
}

/** @brief Rollback the current transaction (sends "ROLLBACK"). */
static int
mysql_drv_transaction_rollback(csilk_db_pool_t* pool)
{
    return mysql_drv_exec(pool, "ROLLBACK");
}

/** @brief Check whether the connection currently has an active transaction. */
static int
mysql_drv_in_transaction(csilk_db_pool_t* pool)
{
    if (!pool || !csilk_db_pool_get_connection(pool)) {
        return 0;
    }
    mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);
    return (conn->db && (conn->db->server_status & SERVER_STATUS_IN_TRANS)) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Native Prepared Statements (MYSQL_STMT + MYSQL_BIND)
 * ----------------------------------------------------------------------- */

typedef struct {
    enum enum_field_types type;
    long long             i64;
    double                dbl;
    char*                 str;
    unsigned long         str_len;
    csilk_mysql_bool_t    is_null;
} mysql_param_slot_t;

typedef struct {
    MYSQL*              db;
    MYSQL_STMT*         stmt;
    unsigned int        param_count;
    MYSQL_BIND*         binds;
    mysql_param_slot_t* slots;
} mysql_stmt_wrap_t;

static void
mysql_stmt_free_param_storage(mysql_stmt_wrap_t* w)
{
    if (!w || !w->slots) {
        return;
    }
    for (unsigned int i = 0; i < w->param_count; i++) {
        free(w->slots[i].str);
        w->slots[i].str = NULL;
    }
}

static int
mysql_drv_stmt_prepare(csilk_db_pool_t* pool, const char* sql, csilk_stmt_t** out_stmt)
{
    if (!pool || !sql || !out_stmt) {
        return -1;
    }
    mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);
    if (!conn || !conn->db) {
        return -1;
    }

    MYSQL_STMT* mstmt = mysql_stmt_init(conn->db);
    if (!mstmt) {
        return -1;
    }
    if (mysql_stmt_prepare(mstmt, sql, (unsigned long)strlen(sql)) != 0) {
        CSILK_LOG_E("csilk_db_mysql: mysql_stmt_prepare failed: %s", mysql_stmt_error(mstmt));
        mysql_stmt_close(mstmt);
        return -1;
    }

    unsigned int       param_count = mysql_stmt_param_count(mstmt);
    mysql_stmt_wrap_t* wrap = calloc(1, sizeof(mysql_stmt_wrap_t));
    if (!wrap) {
        mysql_stmt_close(mstmt);
        return -1;
    }
    wrap->db = conn->db;
    wrap->stmt = mstmt;
    wrap->param_count = param_count;
    if (param_count > 0) {
        wrap->binds = calloc(param_count, sizeof(MYSQL_BIND));
        wrap->slots = calloc(param_count, sizeof(mysql_param_slot_t));
        if (!wrap->binds || !wrap->slots) {
            free(wrap->binds);
            free(wrap->slots);
            free(wrap);
            mysql_stmt_close(mstmt);
            return -1;
        }
        for (unsigned int i = 0; i < param_count; i++) {
            wrap->slots[i].is_null = 1;
            wrap->binds[i].buffer_type = MYSQL_TYPE_NULL;
            wrap->binds[i].is_null = &wrap->slots[i].is_null;
        }
    }

    csilk_stmt_t* stmt = calloc(1, sizeof(csilk_stmt_t));
    if (!stmt) {
        free(wrap->binds);
        free(wrap->slots);
        free(wrap);
        mysql_stmt_close(mstmt);
        return -1;
    }
    stmt->pool = pool;
    stmt->handle = wrap;
    *out_stmt = stmt;
    return 0;
}

static int
mysql_drv_stmt_bind_int64(csilk_stmt_t* stmt, int index, int64_t val)
{
    if (!stmt || !stmt->handle || index < 1) {
        return -1;
    }
    mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
    unsigned int       i = (unsigned int)(index - 1);
    if (i >= w->param_count) {
        return -1;
    }
    free(w->slots[i].str);
    w->slots[i].str = NULL;
    w->slots[i].i64 = (long long)val;
    w->slots[i].is_null = 0;
    memset(&w->binds[i], 0, sizeof(MYSQL_BIND));
    w->binds[i].buffer_type = MYSQL_TYPE_LONGLONG;
    w->binds[i].buffer = &w->slots[i].i64;
    w->binds[i].is_unsigned = 0;
    w->binds[i].is_null = &w->slots[i].is_null;
    return 0;
}

static int
mysql_drv_stmt_bind_double(csilk_stmt_t* stmt, int index, double val)
{
    if (!stmt || !stmt->handle || index < 1) {
        return -1;
    }
    mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
    unsigned int       i = (unsigned int)(index - 1);
    if (i >= w->param_count) {
        return -1;
    }
    free(w->slots[i].str);
    w->slots[i].str = NULL;
    w->slots[i].dbl = val;
    w->slots[i].is_null = 0;
    memset(&w->binds[i], 0, sizeof(MYSQL_BIND));
    w->binds[i].buffer_type = MYSQL_TYPE_DOUBLE;
    w->binds[i].buffer = &w->slots[i].dbl;
    w->binds[i].is_null = &w->slots[i].is_null;
    return 0;
}

static int
mysql_drv_stmt_bind_text(csilk_stmt_t* stmt, int index, const char* val)
{
    if (!stmt || !stmt->handle || index < 1) {
        return -1;
    }
    mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
    unsigned int       i = (unsigned int)(index - 1);
    if (i >= w->param_count) {
        return -1;
    }
    free(w->slots[i].str);
    w->slots[i].str = val ? strdup(val) : NULL;
    if (val && !w->slots[i].str) {
        return -1;
    }
    memset(&w->binds[i], 0, sizeof(MYSQL_BIND));
    if (val) {
        w->slots[i].str_len = (unsigned long)strlen(w->slots[i].str);
        w->slots[i].is_null = 0;
        w->binds[i].buffer_type = MYSQL_TYPE_STRING;
        w->binds[i].buffer = w->slots[i].str;
        w->binds[i].buffer_length = w->slots[i].str_len;
        w->binds[i].length = &w->slots[i].str_len;
    } else {
        w->slots[i].str_len = 0;
        w->slots[i].is_null = 1;
        w->binds[i].buffer_type = MYSQL_TYPE_NULL;
    }
    w->binds[i].is_null = &w->slots[i].is_null;
    return 0;
}

static int
mysql_drv_stmt_bind_null(csilk_stmt_t* stmt, int index)
{
    if (!stmt || !stmt->handle || index < 1) {
        return -1;
    }
    mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
    unsigned int       i = (unsigned int)(index - 1);
    if (i >= w->param_count) {
        return -1;
    }
    free(w->slots[i].str);
    w->slots[i].str = NULL;
    w->slots[i].str_len = 0;
    w->slots[i].is_null = 1;
    memset(&w->binds[i], 0, sizeof(MYSQL_BIND));
    w->binds[i].buffer_type = MYSQL_TYPE_NULL;
    w->binds[i].is_null = &w->slots[i].is_null;
    return 0;
}

static int
mysql_drv_stmt_execute(csilk_stmt_t* stmt, int64_t* out_affected_rows)
{
    if (!stmt || !stmt->handle) {
        return -1;
    }
    mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
    if (w->param_count > 0 && mysql_stmt_bind_param(w->stmt, w->binds) != 0) {
        CSILK_LOG_E("csilk_db_mysql: mysql_stmt_bind_param failed: %s", mysql_stmt_error(w->stmt));
        return -1;
    }
    if (mysql_stmt_execute(w->stmt) != 0) {
        CSILK_LOG_E("csilk_db_mysql: mysql_stmt_execute failed: %s", mysql_stmt_error(w->stmt));
        return -1;
    }
    if (out_affected_rows) {
        *out_affected_rows = (int64_t)mysql_stmt_affected_rows(w->stmt);
    }
    mysql_stmt_reset(w->stmt);
    return 0;
}

static csilk_json_t*
mysql_drv_stmt_query_json(csilk_stmt_t* stmt)
{
    if (!stmt || !stmt->handle) {
        return NULL;
    }
    mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
    if (w->param_count > 0 && mysql_stmt_bind_param(w->stmt, w->binds) != 0) {
        CSILK_LOG_E("csilk_db_mysql: mysql_stmt_bind_param failed: %s", mysql_stmt_error(w->stmt));
        return NULL;
    }
    if (mysql_stmt_execute(w->stmt) != 0) {
        CSILK_LOG_E("csilk_db_mysql: mysql_stmt_execute failed: %s", mysql_stmt_error(w->stmt));
        return NULL;
    }

    csilk_json_t* arr = csilk_json_array();
    if (!arr) {
        mysql_stmt_reset(w->stmt);
        return NULL;
    }

    MYSQL_RES* meta = mysql_stmt_result_metadata(w->stmt);
    if (!meta) {
        mysql_stmt_reset(w->stmt);
        return arr;
    }

    unsigned int num_fields = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);
    if (num_fields == 0) {
        mysql_free_result(meta);
        mysql_stmt_reset(w->stmt);
        return arr;
    }

    MYSQL_BIND*         res_binds = calloc(num_fields, sizeof(MYSQL_BIND));
    char**              col_bufs = calloc(num_fields, sizeof(char*));
    unsigned long*      col_lens = calloc(num_fields, sizeof(unsigned long));
    csilk_mysql_bool_t* col_nulls = calloc(num_fields, sizeof(csilk_mysql_bool_t));
    csilk_mysql_bool_t* col_errs = calloc(num_fields, sizeof(csilk_mysql_bool_t));
    if (!res_binds || !col_bufs || !col_lens || !col_nulls || !col_errs) {
        free(res_binds);
        free(col_bufs);
        free(col_lens);
        free(col_nulls);
        free(col_errs);
        mysql_free_result(meta);
        csilk_json_free(arr);
        mysql_stmt_reset(w->stmt);
        return NULL;
    }

    size_t* buf_caps = calloc(num_fields, sizeof(size_t));
    if (!buf_caps) {
        free(res_binds);
        free(col_bufs);
        free(col_lens);
        free(col_nulls);
        free(col_errs);
        mysql_free_result(meta);
        csilk_json_free(arr);
        mysql_stmt_reset(w->stmt);
        return NULL;
    }

    for (unsigned int i = 0; i < num_fields; i++) {
        size_t cap = 256;
        col_bufs[i] = malloc(cap);
        if (!col_bufs[i]) {
            for (unsigned int k = 0; k < i; k++) {
                free(col_bufs[k]);
            }
            free(buf_caps);
            free(res_binds);
            free(col_bufs);
            free(col_lens);
            free(col_nulls);
            free(col_errs);
            mysql_free_result(meta);
            csilk_json_free(arr);
            mysql_stmt_reset(w->stmt);
            return NULL;
        }
        buf_caps[i] = cap;
        res_binds[i].buffer_type = MYSQL_TYPE_STRING;
        res_binds[i].buffer = col_bufs[i];
        res_binds[i].buffer_length = (unsigned long)cap;
        res_binds[i].length = &col_lens[i];
        res_binds[i].is_null = &col_nulls[i];
        res_binds[i].error = &col_errs[i];
    }

    if (mysql_stmt_bind_result(w->stmt, res_binds) != 0 || mysql_stmt_store_result(w->stmt) != 0) {
        CSILK_LOG_E("csilk_db_mysql: bind/store result failed: %s", mysql_stmt_error(w->stmt));
        for (unsigned int i = 0; i < num_fields; i++) {
            free(col_bufs[i]);
        }
        free(buf_caps);
        free(res_binds);
        free(col_bufs);
        free(col_lens);
        free(col_nulls);
        free(col_errs);
        mysql_free_result(meta);
        csilk_json_free(arr);
        mysql_stmt_reset(w->stmt);
        return NULL;
    }

    for (;;) {
        int fr = mysql_stmt_fetch(w->stmt);
        if (fr == MYSQL_NO_DATA) {
            break;
        }
        if (fr == 1) {
            CSILK_LOG_E("csilk_db_mysql: mysql_stmt_fetch failed: %s", mysql_stmt_error(w->stmt));
            break;
        }

        csilk_json_t* row = csilk_json_object();
        for (unsigned int i = 0; i < num_fields; i++) {
            const char* cname = fields[i].name ? fields[i].name : "";
            if (col_nulls[i]) {
                csilk_json_add_null(row, cname);
                continue;
            }
            size_t cap_now = buf_caps[i];
            if (fr == MYSQL_DATA_TRUNCATED && col_lens[i] >= cap_now &&
                col_lens[i] < (unsigned long)(16 * 1024 * 1024)) {
                size_t new_cap = (size_t)col_lens[i] + 1;
                char*  nb = malloc(new_cap);
                if (nb) {
                    free(col_bufs[i]);
                    col_bufs[i] = nb;
                    buf_caps[i] = new_cap;
                    cap_now = new_cap;
                    res_binds[i].buffer = nb;
                    res_binds[i].buffer_length = (unsigned long)new_cap;
                    MYSQL_BIND fetch_one = res_binds[i];
                    mysql_stmt_fetch_column(w->stmt, &fetch_one, i, 0);
                }
            }
            char*  cbuf = col_bufs[i];
            size_t flen = (size_t)col_lens[i];
            if (!cbuf || cap_now == 0) {
                continue;
            }
            if (flen >= cap_now) {
                flen = cap_now - 1;
            }
            memset(cbuf + flen, 0, 1);

            enum enum_field_types ft = fields[i].type;
            if (ft == MYSQL_TYPE_TINY || ft == MYSQL_TYPE_SHORT || ft == MYSQL_TYPE_LONG ||
                ft == MYSQL_TYPE_LONGLONG || ft == MYSQL_TYPE_INT24) {
                csilk_json_add_int(row, cname, strtoll(cbuf, NULL, 10));
            } else if (ft == MYSQL_TYPE_FLOAT || ft == MYSQL_TYPE_DOUBLE ||
                       ft == MYSQL_TYPE_DECIMAL || ft == MYSQL_TYPE_NEWDECIMAL) {
                csilk_json_add_number(row, cname, strtod(cbuf, NULL));
            } else {
                csilk_json_add_string(row, cname, cbuf);
            }
        }
        csilk_json_array_append(arr, row);
    }

    for (unsigned int i = 0; i < num_fields; i++) {
        free(col_bufs[i]);
    }
    free(buf_caps);
    free(res_binds);
    free(col_bufs);
    free(col_lens);
    free(col_nulls);
    free(col_errs);
    mysql_free_result(meta);
    mysql_stmt_free_result(w->stmt);
    mysql_stmt_reset(w->stmt);
    return arr;
}

static void
mysql_drv_stmt_close(csilk_stmt_t* stmt)
{
    if (!stmt) {
        return;
    }
    if (stmt->handle) {
        mysql_stmt_wrap_t* w = (mysql_stmt_wrap_t*)stmt->handle;
        mysql_stmt_free_param_storage(w);
        free(w->binds);
        free(w->slots);
        if (w->stmt) {
            mysql_stmt_close(w->stmt);
        }
        free(w);
        stmt->handle = NULL;
    }
    free(stmt);
}

static int
mysql_drv_last_error(csilk_db_pool_t* pool, csilk_db_err_t* out_err)
{
    if (!pool || !out_err) {
        return -1;
    }
    mysql_conn_t* conn = (mysql_conn_t*)csilk_db_pool_get_connection(pool);
    if (!conn || !conn->db) {
        *out_err = CSILK_DB_ERR_CONN;
        return 0;
    }
    unsigned int code = mysql_errno(conn->db);
    if (code == 0) {
        *out_err = CSILK_DB_OK;
    } else if (code == 1205 || code == 1213) { /* ER_LOCK_WAIT_TIMEOUT / ER_LOCK_DEADLOCK */
        *out_err = CSILK_DB_ERR_BUSY;
    } else if (code == 2002 || code == 2003 || code == 2006 || code == 2013) {
        *out_err = CSILK_DB_ERR_CONN;
    } else {
        *out_err = CSILK_DB_ERR_SQL;
    }
    return 0;
}

/** @brief Pre-built driver vtable for MySQL. */
csilk_db_driver_t csilk_db_mysql_driver = {
    .name = "mysql",
    .connect = mysql_drv_connect,
    .disconnect = mysql_drv_disconnect,
    .query = mysql_drv_query,
    .exec = mysql_drv_exec,
    .transaction_begin = mysql_drv_transaction_begin,
    .transaction_commit = mysql_drv_transaction_commit,
    .transaction_rollback = mysql_drv_transaction_rollback,
    .in_transaction = mysql_drv_in_transaction,
    .stmt_prepare = mysql_drv_stmt_prepare,
    .stmt_bind_int64 = mysql_drv_stmt_bind_int64,
    .stmt_bind_double = mysql_drv_stmt_bind_double,
    .stmt_bind_text = mysql_drv_stmt_bind_text,
    .stmt_bind_null = mysql_drv_stmt_bind_null,
    .stmt_execute = mysql_drv_stmt_execute,
    .stmt_query_json = mysql_drv_stmt_query_json,
    .stmt_close = mysql_drv_stmt_close,
    .free_result = mysql_free_csilk_result,
    .last_error = mysql_drv_last_error,
};

/**
 * @brief Register the MySQL driver with the database subsystem.
 * Only compiled when HAS_MYSQL is defined.
 */
void
csilk_db_mysql_init(void)
{
    csilk_db_register_driver("mysql", &csilk_db_mysql_driver);
}

#endif /* HAS_MYSQL */
