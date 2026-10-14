#include "csilk/drivers/db/migration.h"
#include "csilk/core/json.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct csilk_migration_engine_s {
    csilk_db_pool_t* pool;
    char             base_dir[256];
    char             dialect_dir[256];
    int              baseline_version;
    char             sentinel_table[128];
};

static const char* s_create_schema_migrations_sql =
    "CREATE TABLE IF NOT EXISTS schema_migrations ("
    "    version INTEGER PRIMARY KEY,"
    "    name TEXT NOT NULL,"
    "    checksum TEXT NOT NULL,"
    "    applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
    "    execution_time_ms INTEGER NOT NULL DEFAULT 0,"
    "    execution_time INTEGER NOT NULL DEFAULT 0"
    ");";

int
csilk_migration_engine_new(csilk_db_pool_t*           pool,
                           const char*                migrations_base_dir,
                           csilk_migration_engine_t** out_engine)
{
    if (!pool || !out_engine) {
        return -1;
    }

    csilk_migration_engine_t* engine = calloc(1, sizeof(*engine));
    if (!engine) {
        return -1;
    }

    engine->pool = pool;

    const char* base =
        (migrations_base_dir && migrations_base_dir[0]) ? migrations_base_dir : "sql/migrations";
    strncpy(engine->base_dir, base, sizeof(engine->base_dir) - 1);

    size_t len = strlen(engine->base_dir);
    while (len > 1 && engine->base_dir[len - 1] == '/') {
        engine->base_dir[len - 1] = '\0';
        len--;
    }

    const char* driver_name = csilk_db_pool_get_driver_name(pool);
    const char* dialect =
        (driver_name && strcmp(driver_name, "postgres") == 0) ? "postgres" : "sqlite";

    char dialect_subdir[512];
    snprintf(dialect_subdir, sizeof(dialect_subdir), "%s/%s", engine->base_dir, dialect);

    if (access(dialect_subdir, R_OK | X_OK) == 0) {
        snprintf(engine->dialect_dir, sizeof(engine->dialect_dir), "%s", dialect_subdir);
    } else {
        snprintf(engine->dialect_dir, sizeof(engine->dialect_dir), "%s", engine->base_dir);
    }

    *out_engine = engine;
    return 0;
}

int
csilk_migration_engine_set_baseline(csilk_migration_engine_t* engine,
                                    int                       baseline_version,
                                    const char*               sentinel_table)
{
    if (!engine) {
        return -1;
    }
    engine->baseline_version = baseline_version;
    if (sentinel_table) {
        strncpy(engine->sentinel_table, sentinel_table, sizeof(engine->sentinel_table) - 1);
    } else {
        engine->sentinel_table[0] = '\0';
    }
    return 0;
}

void
csilk_migration_engine_free(csilk_migration_engine_t* engine)
{
    if (engine) {
        free(engine);
    }
}

static bool
parse_migration_filename(const char* filename, int* out_version, char* out_name, size_t name_size)
{
    if (!filename || (filename[0] != 'V' && filename[0] != 'v')) {
        return false;
    }
    size_t len = strlen(filename);
    if (len < 8 || strcmp(filename + len - 4, ".sql") != 0) {
        return false;
    }

    const char* sep = strstr(filename, "__");
    if (!sep || sep == filename + 1) {
        return false;
    }

    for (const char* p = filename + 1; p < sep; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    int version = atoi(filename + 1);
    if (version <= 0) {
        return false;
    }

    const char* desc_start = sep + 2;
    size_t      desc_len = (filename + len - 4) - desc_start;
    if (desc_len == 0 || desc_len >= name_size) {
        return false;
    }

    if (out_version) {
        *out_version = version;
    }
    if (out_name) {
        strncpy(out_name, desc_start, desc_len);
        out_name[desc_len] = '\0';
    }
    return true;
}

static int
compare_migration_items(const void* a, const void* b)
{
    const csilk_migration_item_t* item_a = (const csilk_migration_item_t*)a;
    const csilk_migration_item_t* item_b = (const csilk_migration_item_t*)b;
    if (item_a->version < item_b->version) {
        return -1;
    }
    if (item_a->version > item_b->version) {
        return 1;
    }
    return 0;
}

int
csilk_migration_discover(csilk_migration_engine_t* engine,
                         csilk_migration_item_t**  out_items,
                         int*                      out_count)
{
    if (!engine || !out_items || !out_count) {
        return -1;
    }
    *out_items = NULL;
    *out_count = 0;

    DIR* dir = opendir(engine->dialect_dir);
    if (!dir) {
        return -1;
    }

    size_t                  capacity = 16;
    size_t                  count = 0;
    csilk_migration_item_t* items = calloc(capacity, sizeof(csilk_migration_item_t));
    if (!items) {
        closedir(dir);
        return -1;
    }

    struct dirent* ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        int  version = 0;
        char name[128] = {0};
        if (!parse_migration_filename(ent->d_name, &version, name, sizeof(name))) {
            continue;
        }

        if (count >= capacity) {
            capacity *= 2;
            csilk_migration_item_t* new_items =
                realloc(items, capacity * sizeof(csilk_migration_item_t));
            if (!new_items) {
                free(items);
                closedir(dir);
                return -1;
            }
            items = new_items;
        }

        csilk_migration_item_t* cur = &items[count];
        cur->version = version;
        snprintf(cur->name, sizeof(cur->name), "%s", name);
        snprintf(cur->filepath, sizeof(cur->filepath), "%s/%s", engine->dialect_dir, ent->d_name);
        cur->is_applied = false;
        cur->execution_time_ms = 0;

        if (csilk_migration_checksum_file(cur->filepath, cur->checksum) != 0) {
            free(items);
            closedir(dir);
            return -1;
        }

        count++;
    }
    closedir(dir);

    if (count > 1) {
        qsort(items, count, sizeof(csilk_migration_item_t), compare_migration_items);

        for (size_t i = 0; i < count - 1; i++) {
            if (items[i].version == items[i + 1].version) {
                free(items);
                return -1;
            }
        }
    }

    *out_items = items;
    *out_count = (int)count;
    return 0;
}

static bool
table_exists(csilk_db_pool_t* pool, const char* table_name)
{
    if (!pool || !table_name) {
        return false;
    }

    const char* driver_name = csilk_db_pool_get_driver_name(pool);
    const char* sql = (driver_name && strcmp(driver_name, "postgres") == 0)
                          ? "SELECT 1 FROM information_schema.tables WHERE table_name = ?;"
                          : "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ?;";

    csilk_stmt_t* stmt = NULL;
    if (csilk_stmt_prepare(pool, sql, &stmt) != 0 || !stmt) {
        return false;
    }

    csilk_stmt_bind_text(stmt, 1, table_name);
    csilk_json_t* res = csilk_stmt_query_json(stmt);
    csilk_stmt_close(stmt);

    bool exists = false;
    if (res) {
        if (csilk_json_is_array(res) && csilk_json_array_size(res) > 0) {
            exists = true;
        }
        csilk_json_free(res);
    }
    return exists;
}

static int
ensure_schema_migrations_and_baseline(csilk_migration_engine_t* engine)
{
    bool has_migrations_table = table_exists(engine->pool, "schema_migrations");
    if (has_migrations_table) {
        return 0;
    }

    bool should_baseline = false;
    if (engine->baseline_version > 0) {
        if (engine->sentinel_table[0] != '\0') {
            should_baseline = table_exists(engine->pool, engine->sentinel_table);
        } else {
            should_baseline = true;
        }
    }

    if (csilk_db_exec(engine->pool, s_create_schema_migrations_sql) != 0) {
        return -1;
    }

    if (!should_baseline) {
        return 0;
    }

    csilk_migration_item_t* items = NULL;
    int                     count = 0;
    if (csilk_migration_discover(engine, &items, &count) != 0) {
        return -1;
    }

    for (int i = 0; i < count; i++) {
        if (items[i].version <= engine->baseline_version) {
            csilk_stmt_t* stmt = NULL;
            int           rc = csilk_stmt_prepare(
                engine->pool,
                "INSERT INTO schema_migrations (version, name, checksum, applied_at, "
                          "execution_time_ms, execution_time) "
                          "VALUES (?, ?, ?, CURRENT_TIMESTAMP, 0, 0);",
                &stmt);
            if (rc != 0 || !stmt) {
                free(items);
                return -1;
            }
            csilk_stmt_bind_int64(stmt, 1, items[i].version);
            csilk_stmt_bind_text(stmt, 2, items[i].name);
            csilk_stmt_bind_text(stmt, 3, items[i].checksum);
            rc = csilk_stmt_execute(stmt, NULL);
            csilk_stmt_close(stmt);
            if (rc != 0) {
                free(items);
                return -1;
            }
        }
    }

    free(items);
    return 0;
}

int
csilk_migration_validate(csilk_migration_engine_t* engine)
{
    if (!engine) {
        return -1;
    }

    if (!table_exists(engine->pool, "schema_migrations")) {
        return 0;
    }

    csilk_migration_item_t* disk_items = NULL;
    int                     disk_count = 0;
    if (csilk_migration_discover(engine, &disk_items, &disk_count) != 0) {
        return -1;
    }

    csilk_stmt_t* stmt = NULL;
    int           rc = csilk_stmt_prepare(
        engine->pool,
        "SELECT version, name, checksum FROM schema_migrations ORDER BY version ASC;",
        &stmt);
    if (rc != 0 || !stmt) {
        free(disk_items);
        return -1;
    }

    csilk_json_t* res = csilk_stmt_query_json(stmt);
    csilk_stmt_close(stmt);

    if (!res || !csilk_json_is_array(res)) {
        if (res) {
            csilk_json_free(res);
        }
        free(disk_items);
        return -1;
    }

    int    max_applied_version = 0;
    bool   valid = true;
    size_t row_count = csilk_json_array_size(res);

    for (size_t r = 0; r < row_count; r++) {
        csilk_json_t* row = csilk_json_array_get(res, r);
        if (!row) {
            continue;
        }

        int64_t     db_version = csilk_json_as_int64(csilk_json_get(row, "version"), 0);
        const char* db_checksum = csilk_json_get_string(row, "checksum");
        if (!db_checksum) {
            db_checksum = "";
        }
        if (db_version > max_applied_version) {
            max_applied_version = (int)db_version;
        }

        bool found = false;
        for (int i = 0; i < disk_count; i++) {
            if (disk_items[i].version == (int)db_version) {
                found = true;
                if (!db_checksum[0] || strcmp(disk_items[i].checksum, db_checksum) != 0) {
                    valid = false;
                }
                break;
            }
        }
        if (!found) {
            valid = false;
        }
        if (!valid) {
            break;
        }
    }

    csilk_json_free(res);

    if (valid) {
        for (int i = 0; i < disk_count; i++) {
            if (disk_items[i].version < max_applied_version) {
                csilk_stmt_t* chk_stmt = NULL;
                if (csilk_stmt_prepare(engine->pool,
                                       "SELECT 1 FROM schema_migrations WHERE version = ?;",
                                       &chk_stmt) == 0 &&
                    chk_stmt) {
                    csilk_stmt_bind_int64(chk_stmt, 1, disk_items[i].version);
                    csilk_json_t* chk_res = csilk_stmt_query_json(chk_stmt);
                    csilk_stmt_close(chk_stmt);
                    bool was_applied = false;
                    if (chk_res) {
                        if (csilk_json_is_array(chk_res) && csilk_json_array_size(chk_res) > 0) {
                            was_applied = true;
                        }
                        csilk_json_free(chk_res);
                    }
                    if (!was_applied) {
                        valid = false;
                        break;
                    }
                }
            }
        }
    }

    free(disk_items);
    return valid ? 0 : -1;
}

int
csilk_migration_status(csilk_migration_engine_t* engine,
                       csilk_migration_item_t**  out_items,
                       int*                      out_count)
{
    if (!engine || !out_items || !out_count) {
        return -1;
    }

    csilk_migration_item_t* items = NULL;
    int                     count = 0;
    if (csilk_migration_discover(engine, &items, &count) != 0) {
        return -1;
    }

    if (table_exists(engine->pool, "schema_migrations")) {
        csilk_stmt_t* stmt = NULL;
        if (csilk_stmt_prepare(engine->pool,
                               "SELECT version, execution_time_ms FROM schema_migrations;",
                               &stmt) == 0 &&
            stmt) {
            csilk_json_t* res = csilk_stmt_query_json(stmt);
            csilk_stmt_close(stmt);
            if (res && csilk_json_is_array(res)) {
                size_t row_count = csilk_json_array_size(res);
                for (size_t r = 0; r < row_count; r++) {
                    csilk_json_t* row = csilk_json_array_get(res, r);
                    int64_t       v = csilk_json_as_int64(csilk_json_get(row, "version"), 0);
                    int64_t t = csilk_json_as_int64(csilk_json_get(row, "execution_time_ms"), 0);
                    for (int i = 0; i < count; i++) {
                        if (items[i].version == (int)v) {
                            items[i].is_applied = true;
                            items[i].execution_time_ms = (int)t;
                            break;
                        }
                    }
                }
                csilk_json_free(res);
            }
        }
    }

    *out_items = items;
    *out_count = count;
    return 0;
}

static char*
read_file_to_string(const char* filepath)
{
    FILE* f = fopen(filepath, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char* buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read_bytes = fread(buf, 1, (size_t)size, f);
    buf[read_bytes] = '\0';
    fclose(f);
    return buf;
}

int
csilk_migration_apply(csilk_migration_engine_t* engine, int* out_applied_count)
{
    if (!engine) {
        return -1;
    }

    if (csilk_migration_lock_acquire(engine->pool, "csilk_engine", 10) != 0) {
        return -1;
    }

    if (ensure_schema_migrations_and_baseline(engine) != 0) {
        csilk_migration_lock_release(engine->pool, "csilk_engine");
        return -1;
    }

    if (csilk_migration_validate(engine) != 0) {
        csilk_migration_lock_release(engine->pool, "csilk_engine");
        return -1;
    }

    csilk_migration_item_t* items = NULL;
    int                     count = 0;
    if (csilk_migration_discover(engine, &items, &count) != 0) {
        csilk_migration_lock_release(engine->pool, "csilk_engine");
        return -1;
    }

    int applied_count = 0;
    for (int i = 0; i < count; i++) {
        csilk_stmt_t* stmt = NULL;
        bool          already_applied = false;
        if (csilk_stmt_prepare(
                engine->pool, "SELECT 1 FROM schema_migrations WHERE version = ?;", &stmt) == 0 &&
            stmt) {
            csilk_stmt_bind_int64(stmt, 1, items[i].version);
            csilk_json_t* res = csilk_stmt_query_json(stmt);
            csilk_stmt_close(stmt);
            if (res) {
                if (csilk_json_is_array(res) && csilk_json_array_size(res) > 0) {
                    already_applied = true;
                }
                csilk_json_free(res);
            }
        }

        if (already_applied) {
            continue;
        }

        char* sql_content = read_file_to_string(items[i].filepath);
        if (!sql_content) {
            free(items);
            csilk_migration_lock_release(engine->pool, "csilk_engine");
            return -1;
        }

        csilk_db_tx_scope_t scope;
        if (csilk_db_tx_scope_begin(engine->pool, "migration", &scope) != 0) {
            free(sql_content);
            free(items);
            csilk_migration_lock_release(engine->pool, "csilk_engine");
            return -1;
        }

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        if (csilk_db_exec(engine->pool, sql_content) != 0) {
            csilk_db_tx_scope_rollback(engine->pool, &scope);
            free(sql_content);
            free(items);
            csilk_migration_lock_release(engine->pool, "csilk_engine");
            return -1;
        }
        free(sql_content);

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        int elapsed_ms = (int)((t_end.tv_sec - t_start.tv_sec) * 1000 +
                               (t_end.tv_nsec - t_start.tv_nsec) / 1000000);
        if (elapsed_ms < 0) {
            elapsed_ms = 0;
        }

        csilk_stmt_t* ins_stmt = NULL;
        int           ins_rc = csilk_stmt_prepare(
            engine->pool,
            "INSERT INTO schema_migrations (version, name, checksum, applied_at, "
                      "execution_time_ms, execution_time) "
                      "VALUES (?, ?, ?, CURRENT_TIMESTAMP, ?, ?);",
            &ins_stmt);
        if (ins_rc == 0 && ins_stmt) {
            csilk_stmt_bind_int64(ins_stmt, 1, items[i].version);
            csilk_stmt_bind_text(ins_stmt, 2, items[i].name);
            csilk_stmt_bind_text(ins_stmt, 3, items[i].checksum);
            csilk_stmt_bind_int64(ins_stmt, 4, elapsed_ms);
            csilk_stmt_bind_int64(ins_stmt, 5, elapsed_ms);
            ins_rc = csilk_stmt_execute(ins_stmt, NULL);
            csilk_stmt_close(ins_stmt);
        }

        if (ins_rc != 0) {
            csilk_db_tx_scope_rollback(engine->pool, &scope);
            free(items);
            csilk_migration_lock_release(engine->pool, "csilk_engine");
            return -1;
        }

        if (csilk_db_tx_scope_commit(engine->pool, &scope) != 0) {
            free(items);
            csilk_migration_lock_release(engine->pool, "csilk_engine");
            return -1;
        }

        applied_count++;
    }

    free(items);
    csilk_migration_lock_release(engine->pool, "csilk_engine");

    if (out_applied_count) {
        *out_applied_count = applied_count;
    }
    return 0;
}
