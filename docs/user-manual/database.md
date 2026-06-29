# 数据库驱动使用指南

> **Version**: 0.5.0-dev | **Last updated**: 2026-06-29

csilk 提供了一个统一的数据库驱动接口，支持多种后端通过相同的 API 操作。内置驱动包括 **SQLite**、**MySQL**、**PostgreSQL**、**MongoDB** 和 **Redis**，也支持通过 `csilk_db_register_driver` 注册自定义驱动。

---

## 1. 初始化

所有数据库操作前必须调用 `csilk_db_init()` 注册内置驱动。该函数线程安全，可多次调用。

```c
#include "csilk/drivers/db.h"

int main() {
    // 注册所有内置驱动（SQLite、MySQL、PostgreSQL、MongoDB、Redis）
    csilk_db_init();
    // ...
}
```

---

## 2. 创建连接池

`csilk_db_pool_new(driver_name, dsn)` 创建并连接一个数据库连接池。

### 2.1 SQLite

```c
csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", "/data/app.db");
if (!pool) {
    CSILK_LOG_E("Failed to connect to SQLite");
    return 1;
}
```

DSN 格式：文件路径（`:memory:` 支持内存数据库）

### 2.2 MySQL

```c
// 构建时需启用 -DCSILK_USE_MYSQL=ON
csilk_db_pool_t* pool = csilk_db_pool_new("mysql",
    "user:password@tcp(localhost:3306)/dbname");
```

DSN 格式：`user:password@tcp(host:port)/dbname`

### 2.3 PostgreSQL

```c
// 构建时需启用 -DCSILK_USE_POSTGRES=ON
csilk_db_pool_t* pool = csilk_db_pool_new("postgres",
    "host=localhost port=5432 dbname=app user=user password=pass");
```

DSN 格式：PostgreSQL 标准键值对连接字符串

### 2.4 MongoDB

```c
// 构建时需启用 -DCSILK_USE_MONGODB=ON
csilk_db_pool_t* pool = csilk_db_pool_new("mongodb",
    "mongodb://localhost:27017");
```

DSN 格式：标准 MongoDB URI

### 2.5 Redis

```c
// 构建时需启用 -DCSILK_USE_REDIS=ON
csilk_db_pool_t* pool = csilk_db_pool_new("redis",
    "redis://localhost:6379");
```

DSN 格式：`redis://host:port` 或 `redis://user:password@host:port/db`

---

## 3. 执行查询

### 3.1 SELECT 查询

使用 `csilk_db_query_json` 获取结果为 JSON 数组：

```c
void list_users_handler(csilk_ctx_t* c) {
    csilk_db_pool_t* pool = (csilk_db_pool_t*)csilk_get(c, "db_pool");

    // 执行查询，返回 cJSON 数组
    cJSON* result = csilk_db_query_json(pool, "SELECT id, name, email FROM users");
    if (result) {
        csilk_json(c, 200, result);
        cJSON_Delete(result);
    } else {
        csilk_json_error(c, 500, "Query failed");
    }
}
```

### 3.2 参数化查询

使用 `?` 占位符防止 SQL 注入：

```c
void get_user_handler(csilk_ctx_t* c) {
    csilk_db_pool_t* pool = (csilk_db_pool_t*)csilk_get(c, "db_pool");
    const char* user_id = csilk_get_param(c, "id");

    const char* params[] = {user_id, nullptr};  // nullptr 终止
    cJSON* result = csilk_db_query_param_json(pool,
        "SELECT * FROM users WHERE id = ?", params);

    if (result && cJSON_GetArraySize(result) > 0) {
        csilk_json(c, 200, cJSON_GetArrayItem(result, 0));
    } else {
        csilk_json_error(c, 404, "User not found");
    }

    cJSON_Delete(result);
}
```

### 3.3 非查询语句（INSERT / UPDATE / DELETE）

```c
void create_user_handler(csilk_ctx_t* c) {
    csilk_db_pool_t* pool = (csilk_db_pool_t*)csilk_get(c, "db_pool");

    int rc = csilk_db_exec(pool,
        "INSERT INTO users (name, email) VALUES ('Alice', 'alice@example.com')");

    if (rc == 0) {
        csilk_json_string(c, 201, "{\"status\":\"created\"}");
    } else {
        csilk_json_error(c, 500, "Insert failed");
    }
}
```

---

## 4. 事务管理

```c
void transfer_handler(csilk_ctx_t* c) {
    csilk_db_pool_t* pool = (csilk_db_pool_t*)csilk_get(c, "db_pool");

    // 获取原生连接（用于事务操作）
    void* conn = csilk_db_pool_get_connection(pool);

    // 使用 csilk_db_exec 执行 SQL（事务由 SQL 语句控制）
    int rc = 0;
    rc |= csilk_db_exec(pool, "BEGIN IMMEDIATE TRANSACTION");
    rc |= csilk_db_exec(pool,
        "UPDATE accounts SET balance = balance - 100 WHERE id = 1");
    rc |= csilk_db_exec(pool,
        "UPDATE accounts SET balance = balance + 100 WHERE id = 2");

    if (rc == 0) {
        csilk_db_exec(pool, "COMMIT");
        csilk_json_string(c, 200, "{\"status\":\"ok\"}");
    } else {
        csilk_db_exec(pool, "ROLLBACK");
        csilk_json_error(c, 500, "Transfer failed");
    }
}
```

---

## 5. 原生结果集遍历

需要直接访问行数据时，使用驱动层的 `query`/`exec` 接口：

```c
void query_raw_example(csilk_db_pool_t* pool) {
    csilk_db_result_t result = {0};

    // 调用驱动层的 query 函数
    csilk_db_driver_t* driver = csilk_db_get_driver("sqlite");
    if (driver && driver->query(pool, "SELECT id, name FROM users", &result) == 0) {
        for (int i = 0; i < result.row_count; i++) {
            csilk_db_row_t* row = result.rows[i];
            for (int j = 0; j < result.column_count; j++) {
                printf("%s: %s\n",
                    result.column_names[j],
                    row->values[j] ? row->values[j] : "NULL");
            }
        }
        driver->free_result(&result);
    }
}
```

---

## 6. Redis 存储驱动

Redis 连接池可作为 `csilk_storage_driver_t` 接口的存储后端，用于分布式 Session 管理或限流：

```c
#ifdef HAS_REDIS
void setup_redis_storage(void) {
    csilk_db_pool_t* pool = csilk_db_pool_new("redis", "redis://localhost:6379");

    // 创建基于 Redis 的存储驱动
    csilk_storage_driver_t* storage = csilk_redis_storage_driver_new(pool);

    // 设置 server 的存储驱动
    csilk_server_set_storage_driver(server, storage);
}
#endif
```

---

## 7. 统计与监控

```c
void db_stats_handler(csilk_ctx_t* c) {
    csilk_db_stats_t stats;
    csilk_db_get_stats(&stats);

    CSILK_LOG_I("DB Stats:");
    CSILK_LOG_I("  Queries: %lu", stats.queries_total);
    CSILK_LOG_I("  Execs:   %lu", stats.execs_total);
    CSILK_LOG_I("  Errors:  %lu", stats.errors_total);
    CSILK_LOG_I("  Duration: %lu µs", stats.duration_us_total);
}
```

Admin Dashboard 自动显示数据库统计面板。

---

## 8. 完整示例

一个带数据库 CRUD 的 REST API 服务：

```c
#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

static csilk_db_pool_t* g_db;

// CREATE
void create_user(csilk_ctx_t* c) {
    cJSON* body = csilk_get_json(c);
    const char* name  = cJSON_GetObjectItem(body, "name")->valuestring;
    const char* email = cJSON_GetObjectItem(body, "email")->valuestring;

    const char* params[] = {name, email, nullptr};
    cJSON* result = csilk_db_query_param_json(g_db,
        "INSERT INTO users (name, email) VALUES (?, ?) RETURNING id", params);

    if (result) {
        csilk_json(c, 201, result);
        cJSON_Delete(result);
    } else {
        csilk_json_error(c, 500, "Create failed");
    }
    cJSON_Delete(body);
}

// READ
void list_users(csilk_ctx_t* c) {
    cJSON* result = csilk_db_query_json(g_db, "SELECT * FROM users");
    if (result) {
        csilk_json(c, 200, result);
        cJSON_Delete(result);
    }
}

// UPDATE
void update_user(csilk_ctx_t* c) {
    const char* id = csilk_get_param(c, "id");
    cJSON* body = csilk_get_json(c);
    const char* email = cJSON_GetObjectItem(body, "email")->valuestring;

    const char* params[] = {email, id, nullptr};
    csilk_db_query_param_json(g_db,
        "UPDATE users SET email = ? WHERE id = ?", params);

    csilk_json_string(c, 200, "{\"status\":\"updated\"}");
    cJSON_Delete(body);
}

// DELETE
void delete_user(csilk_ctx_t* c) {
    const char* id = csilk_get_param(c, "id");
    const char* params[] = {id, nullptr};

    csilk_db_query_param_json(g_db,
        "DELETE FROM users WHERE id = ?", params);
    csilk_json_string(c, 200, "{\"status\":\"deleted\"}");
}

int main() {
    csilk_db_init();
    g_db = csilk_db_pool_new("sqlite", "app.db");

    // 初始化表
    csilk_db_exec(g_db,
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL, email TEXT)");

    csilk_app_t* app = csilk_app_new("config.yaml");
    csilk_app_use(app, csilk_recovery_handler);

    csilk_app_post(app,   "/users",        create_user);
    csilk_app_get(app,    "/users",        list_users);
    csilk_app_get(app,    "/users/:id",    get_user);
    csilk_app_put(app,    "/users/:id",    update_user);
    csilk_app_delete(app, "/users/:id",    delete_user);

    csilk_app_run(app, 8080);
    csilk_db_pool_free(g_db);
    csilk_app_free(app);
    return 0;
}
```

---

## 9. 注册自定义驱动

对接其他数据库（如 Oracle、ClickHouse）：

```c
// 实现驱动接口
int my_connect(csilk_db_pool_t* pool, const char* dsn) {
    pool->connection = my_db_connect(dsn);
    return pool->connection ? 0 : -1;
}

int my_disconnect(csilk_db_pool_t* pool) {
    my_db_close(pool->connection);
    return 0;
}

int my_query(csilk_db_pool_t* pool, const char* sql, csilk_db_result_t* result) {
    // 实现查询，填充 result->rows / column_names / row_count / column_count
    return 0;
}

int my_exec(csilk_db_pool_t* pool, const char* sql) {
    return my_db_exec(pool->connection, sql);
}

void my_free_result(csilk_db_result_t* result) {
    free(result->rows);
    free(result->column_names);
}

static csilk_db_driver_t my_driver = {
    .name              = "my-db",
    .connect           = my_connect,
    .disconnect        = my_disconnect,
    .query             = my_query,
    .exec              = my_exec,
    .transaction_begin = my_transaction_begin,
    .transaction_commit = my_transaction_commit,
    .transaction_rollback = my_transaction_rollback,
    .free_result       = my_free_result,
};

void register_my_db(void) {
    csilk_db_register_driver("my-db", &my_driver);
}
```

---

## 10. 最佳实践

| 实践 | 说明 |
|:-----|:------|
| **MUST** 先调用 `csilk_db_init()` | 注册所有内置驱动 |
| **SHOULD** 使用参数化查询 | `csilk_db_query_param_json` 防止 SQL 注入 |
| **SHOULD** 事务使用 IMMEDIATE | SQLite 使用 `BEGIN IMMEDIATE` 避免并发死锁 |
| **SHOULD** 长事务在后台线程执行 | 事务期间持有连接锁，应尽量短 |
| **MUST** 释放 JSON 结果 | `csilk_db_query_json` 返回的 cJSON 需 `cJSON_Delete` |
| **MAY** 使用 Redis 做分布式存储 | `csilk_redis_storage_driver_new` 支持分布式分享 |
| **MAY** 通过 Admin Dashboard 监控 | 数据库统计自动在 `/admin` 面板显示 |

---

## 延伸阅读

| 文档 | 内容 |
|:-----|:------|
| [模块设计 — 数据层](../../docs/module-design/data.md) | 数据库内部架构、驱动注册、连接池实现 |
| [配置指南](./configuration.md) | 数据库 DSN 和连接池 YAML 配置 |
| [高级用法 — 事务管理](./advanced-usage.md#database-transaction-management-数据库事务管理) | 数据库事务管理进阶示例 |
| [性能调优指南](../performance-tuning.md) | 连接池大小调优和查询优化 |
