# 数据 / 数据库抽象层

## 1. 概述

数据层提供了一种 **可插拔、基于驱动的数据库抽象**，灵感来自 Go 的 `database/sql`。它将统一的查询/执行 API 与后端特定的线路协议分离，使得 SQLite、MySQL、PostgreSQL、MongoDB 和 Redis 可以通过单一接口互换使用。所有数据库驱动 **MUST** 实现 `csilk_db_driver_t` vtable。连接池操作 **MUST** 是线程安全的。查询结果 **MUST** 以 arena 管理的行形式返回 `csilk_db_result_t` — 调用者 **MUST NOT** 释放单个单元格。从池中获取连接 **SHOULD** 在 ≤ 100ns 内完成（无锁链表弹出）。

**文件**: `src/drivers/db/db.c`, `src/drivers/db/db_internal.h`, `include/csilk/drivers/db.h`

---

## 2. 架构

```
┌─────────────────────────────────────────────────────────┐
│                    HTTP / App Layer                      │
│         csilk_db_query_json / csilk_db_exec              │
└─────────────────────┬───────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────┐
│              Database Pool (csilk_db_pool_t)             │
│  ┌────────────────────────────────────────────────────┐ │
│  │  uv_mutex_t  │  csilk_db_driver_t*  │  void* conn  │ │
│  └────────────────────────────────────────────────────┘ │
└──────────┬──────────────────────────────┬───────────────┘
           │                              │
┌──────────▼──────────┐    ┌──────────────▼──────────────┐
│   Driver Registry   │    │   csilk_db_driver_t vtable  │
│   (static array)    │    │  ┌──────────────────────┐   │
│                     │    │  │ connect( )            │   │
│   drivers[16]       │    │  │ disconnect( )         │   │
│   driver_count      │    │  │ query( )              │   │
│   registry_mutex    │    │  │ exec( )               │   │
│                     │    │  │ transaction_begin( )  │   │
│                     │    │  │ transaction_commit( ) │   │
│                     │    │  │ transaction_rollback( )│  │
│                     │    │  │ free_result( )        │   │
│                     │    │  └──────────────────────┘   │
│                     │    └─────────────────────────────┘
└─────────────────────┘
```

### 关键设计决策

1. **每个连接池单个连接** — 每个 `csilk_db_pool_t` 只持有确切一个后端连接，由 `uv_mutex_t` 序列化。这避免了连接池管理的复杂性。需要并发的调用者创建多个池。

2. **驱动 vtable + 注册表** — 驱动在启动期间通过 `csilk_db_register_driver()` 自注册。注册表是固定大小的数组（最多 16 个驱动）由互斥锁保护。在池创建时间按名称查找驱动。

3. **结果为 cJSON** — 查询结果从表格格式转换为 cJSON 数组对象。所有值表示为字符串（无类型推断）。这与 JSON-first HTTP 响应路径保持一致，并消除调用者每个驱动的序列化逻辑。

4. **内置与条件编译驱动** — 只有 SQLite 总是包含。MySQL、PostgreSQL、MongoDB 和 Redis 通过 `HAS_MYSQL`、`HAS_POSTGRES`、`HAS_MONGODB`、`HAS_REDIS` 宏条件编译。

---

## 3. 核心数据结构

### 3.1 `csilk_db_pool_t` (不透明)

```c
// src/drivers/db/db_internal.h
struct csilk_db_pool_s {
    csilk_db_driver_t* driver;   // 指向全局注册表
    void* connection;             // 驱动特定句柄
    uv_mutex_t mutex;             // 序列化对连接的访问
};
```

内部是一个薄包装：存储驱动指针（来自全局注册表）、不透明连接句柄（由驱动的 `connect` 回调分配）和用于线程安全查询/执行序列化的互斥锁。

### 3.2 `csilk_db_driver_t` (vtable)

| 字段 | 类型 | 目的 |
|---|---|---|
| `name` | `const char*` | 驱动标识符（例如 `"sqlite3"`） |
| `connect` | `int (*)(pool, dsn)` | 打开连接，填充 pool->connection |
| `disconnect` | `int (*)(pool)` | 关闭连接，释放 pool->connection |
| `query` | `int (*)(pool, sql, result*)` | SELECT → 表格结果集 |
| `exec` | `int (*)(pool, sql)` | INSERT/UPDATE/DELETE/DDL |
| `transaction_begin` | `int (*)(pool)` | BEGIN |
| `transaction_commit` | `int (*)(pool)` | COMMIT |
| `transaction_rollback` | `int (*)(pool)` | ROLLBACK |
| `free_result` | `void (*)(result*)` | 释放驱动分配的结果内存 |

### 3.3 结果类型

```
csilk_db_result_t
  ├── rows[]       → csilk_db_row_t
  │                   └── values[]  → char*  (每个列，SQL NULL 为 NULL)
  ├── column_names[] → char*        (可为 NULL — 驱动可能省略)
  ├── row_count    → int
  └── column_count → int
```

行和列名由驱动堆分配；`free_result()` 返回所有内存。

---

## 4. API 界面

### 池生命周期

| 函数 | 角色 |
|---|---|
| `csilk_db_pool_new(driver_name, dsn)` | 查找驱动 → 分配池 → 初始化互斥锁 → 调用 driver->connect |
| `csilk_db_pool_free(pool)` | 调用 driver->disconnect → 销毁互斥锁 → 释放池 |
| `csilk_db_pool_get_connection(pool)` | 返回不透明驱动句柄（调用者需持有互斥锁） |
| `csilk_db_pool_set_connection(pool, conn)` | 存储驱动句柄（通常在 connect 回调中调用） |

### 查询 / Exec

| 函数 | 角色 |
|---|---|
| `csilk_db_query_json(pool, sql)` | SELECT → 互斥锁加锁 → driver->query → cJSON 转换 → 互斥锁解锁 |
| `csilk_db_query_param_json(pool, sql, params)` | 参数化查询，SQL 注入保护（见 §5.2） |
| `csilk_db_exec(pool, sql)` | INSERT/UPDATE/DELETE → 互�ixie锁加锁 → driver->exec → 互斥锁解锁 |

### 驱动注册

| 函数 | 角色 |
|---|---|
| `csilk_db_init()` | 注册所有内置驱动（SQLite 始终，其他条件性） |
| `csilk_db_register_driver(name, vtable)` | 将驱动添加到全局注册表 |
| `csilk_db_get_driver(name)` | 按名称线性扫描注册表 |

### 指标

| 函数 | 角色 |
|---|---|
| `csilk_db_get_stats(stats*)` | 原子读取 querys_total / execs_total / errors_total / duration_us_total |

指标是全局原子 (`atomic_uint_fast64_t`) 在每次查询/exec 时更新。Prometheus 中间件通过 `csilk_db_get_stats()` 读取。

---

## 5. 关键算法

### 5.1 行到 JSON 转换

```
csilk_db_query_json_locked(pool, sql):
  1. 调用 driver->query() → 表格 csilk_db_result_t
  2. cJSON_CreateArray()
  3. For each row i:
     a. cJSON_CreateObject()
     b. For each column j:
        - 如果 row->values[j] 不是 NULL:
          添加 (column_names[j] | "col", value) 作为字符串
     c. 将对象附加到数组
  4. driver->free_result()
  5. 返回 cJSON 数组（调用者必须 cJSON_Delete）
```

所有值都是 cJSON 字符串 — 无类型推断。调用者可使用 `cJSON_GetNumberValue()` 等进行转换。

### 5.2 参数化查询 (SQL 注入保护)

```
csilk_db_query_param_json(pool, sql, params[]):
  1. 预扫描: 计算输出缓冲区大小 =
     len(sql) + 2*N_params + escape_overhead (' → '', \ → \\)
  2. 分配缓冲区
  3. 遍历 sql 字符:
     - '?' → 写入 "'escaped_value'" (带 '→'', \→\\)
     - 其他 → 逐字符复制
  4. 通过 csilk_db_query_json_locked() 执行构造的 SQL
  5. 释放缓冲区
```

这 **不是预处理语句绑定** — 它是安全字符串插值。转义覆盖单引号和反斜线，击败简单的注入向量。对于真正的隔离，需要驱动特定 API 的预处理语句变体。

### 5.3 池生命周期

```
csilk_db_pool_new(name, dsn)
  ├── csilk_db_get_driver(name)      // 线性扫描注册表
  ├── calloc csilk_db_pool_t
  ├── uv_mutex_init(&pool->mutex)
  ├── driver->connect(pool, dsn)     // 驱动调用 csilk_db_pool_set_connection
  ├── on failure: uv_mutex_destroy + free → return NULL
  └── on success: return pool

csilk_db_pool_free(pool)
  ├── driver->disconnect(pool)       // 驱动释放其句柄
  ├── uv_mutex_destroy(&pool->mutex)
  └── free(pool)
```

---

## 6. 内置驱动

| 驱动 | C 模块 | 要求 | 连接 |
|---|---|---|---|
| **sqlite3** | `src/drivers/sqlite.c` | 始终构建 | 文件路径 (DSN = `/path/to/db.sqlite`) |
| **mysql** | `src/drivers/mysql.c` | `libmysqlclient`, `HAS_MYSQL` | MySQL 连接字符串 |
| **postgres** | `src/drivers/postgres.c` | `libpq`, `HAS_POSTGRES` | PostgreSQL 连接字符串 |
| **mongodb** | `src/drivers/mongodb.c` | `libmongoc`, `HAS_MONGODB` | MongoDB URI |
| **redis** | `src/drivers/redis.c` | `hiredis`, `HAS_REDIS` | Redis host:port |

### 6.1 Redis 双重角色

Redis 在 csilk 中扮演两个角色：

1. **数据库驱动** (`csilk_db_driver_t`) — Redis 命令通过标准查询/exec 接口。用于直接 Redis 命令执行。

2. **存储驱动** (`csilk_storage_driver_t`) — 通过 `csilk_redis_storage_driver_new(pool)` 创建。提供键值存储（set/get/clear/incr）用于会话管理和速率限制。存储驱动包装 DB 池并将存储操作转换为 Redis 命令。

### 6.2 事务支持

SQLite、MySQL、PostgreSQL 和 MongoDB 驱动实现完整的事务 vtable (`begin` / `commit` / `rollback`)。事务方法委托到相应的 SQL 命令 (`BEGIN`、`COMMIT`、`ROLLBACK`) 通过驱动的 `exec` 函数。MongoDB 使用 `mongoc_client_session_start/commit/abort_transaction`。

Redis 事务使用 `MULTI/EXEC/DISCARD` 在其事务回调中（与 db_driver_t vtable 分离；在 `redis_drv_transaction_begin/commit/rollback` 处理）。

---

## 7. 向量数据库

除了 SQL/NoSQL 驱动外，csilk 还有单独的 **向量数据库** 插件系统：

**文件**: `src/drivers/vector/vector.c`, `include/csilk/drivers/vector.h`

| 函数 | 角色 |
|---|---|
| `csilk_vector_db_register_driver()` | 按名称注册向量 DB 驱动 |
| `csilk_vector_db_new(name, endpoint, api_key)` | 创建向量 DB 实例 |
| `csilk_vector_db_upsert(db, collection, points, count)` | 插入/更新向量 |
| `csilk_vector_db_search(db, collection, vector, dim, limit, res*)` | ANN 搜索 |
| `csilk_vector_db_free(db)` | 销毁实例 |

**支持的后端**（条件编译）:
- **Qdrant** — 基于 REST/gRPC 的向量搜索引擎
- **Milvus** — 分布式向量数据库

向量 DB 系统镜像 DB 层的插件模式但使用单独的注册表具有自己的驱动生命周期（init/free + state 指针），因为向量 DB 是 HTTP/gRPC 客户端而不是 SQL 连接。

---

## 8. 并发模型

```
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│  Worker Thread  │  │  Worker Thread  │  │  Worker Thread  │
│  publish()      │  │  publish()      │  │  publish()      │
└────────┬────────┘  └────────┬────────┘  └────────┬────────┘
         │                    │                    │
         │    queue_mutex     │                    │
         └────────────────────┼────────────────────┘
                            │
                            ▼
               ┌──────────────────┐
               │  Pending Queue   │
               │  (linked list)    │
               └────────┬─────────┘
                        │ uv_async_send()
                        ▼
               ┌──────────────────┐
               │  Main Loop       │
               │  on_mq_async()   │
               │  └─ dispatch()   │
               └──────────────────┘
```

- **驱动注册**: 由 `registry_mutex` 保护。仅在启动期间（注册）和查找期间（池创建）持有锁。从不持有锁在 I/O 期间。
- **每个池**: 每个池有自己的 `uv_mutex_t`。加锁持续每个查询/exec 调用。序列化对单个后端连接的访问。
- **指标**: 原子操作上的 `atomic_uint_fast64_t` 计数器 — 无锁争用。

---

## 9. 错误处理 & 指标

每个查询/exec 调用遵循相同的检测模式：

```
1. 记录开始时间 (uv_hrtime)
2. 加锁池互斥锁
3. 调用驱动函数
4. 解锁池互斥锁
5. 计算持续时间
6. 原子加到 db_duration_us_total
7. 成功时: atomic-inc db_queries_total 或 db_execs_total
8. 失败时: atomic-inc db_errors_total，记录错误和持续时间
```

这为所有驱动后端提供一致的可观测性。

---

## 10. 使用示例

```c
// 初始化
csilk_db_init();

// 创建池 (单个连接，互料锁序列化)
csilk_db_pool_t* db = csilk_db_pool_new("sqlite3", "/tmp/app.db");

// 查询 → JSON 数组
cJSON* users = csilk_db_query_json(db, "SELECT * FROM users LIMIT 10");
cJSON_Delete(users);

// 参数化查询
const char* params[] = {"admin", NULL};
cJSON* result = csilk_db_query_param_json(db, "SELECT * FROM users WHERE role = ?", params);
cJSON_Delete(result);

// Exec
csilk_db_exec(db, "INSERT INTO log (msg) VALUES ('startup')");

// 清理
csilk_db_pool_free(db);
```

---

## 11. 相关文件

| 文件 | 角色 |
|---|---|
| `src/drivers/db/db.c` | 池生命周期，查询/exec，驱动注册，指标 |
| `src/drivers/db/db_internal.h` | 内部池结构定义 |
| `include/csilk/drivers/db.h` | 公共 API — 驱动 vtable，池函数，注册 |
| `src/drivers/sqlite.c` | SQLite3 驱动实现 |
| `src/drivers/mysql.c` | MySQL 驱动（条件性） |
| `src/drivers/postgres.c` | PostgreSQL 驱动（条件性） |
| `src/drivers/mongodb.c` | MongoDB 驱动（条件性） |
| `src/drivers/redis.c` | Redis 驱动 + 存储驱动（条件性） |
| `src/drivers/vector/vector.c` | 向量 DB 插件系统（单独注册表） |
| `include/csilk/drivers/vector.h` | 向量 DB 公共 API |
| `examples/advanced/example_custom_driver.c` | 自定义驱动注册示例 + 测试 |
| `python/csilk/db.py` | Python ctypes 绑定的 DB 层 |

---

## 12. 设计理由

**为什么单个连接池而不是多连接池？**  
简单性和可预测性。多连接池需要连接健康检查、最大生命周期管理、关闭时排空、和复杂的驱逐逻辑 — 所有这些都会增加延迟和故障模式。通过暴露单个连接池，我们让 *调用者* 决定池化策略（每个工作线程一个池，每个数据库角色一个池等），匹配 libuv 工作线程架构而不引入冗余抽象。

**为什么 cJSON 用于查询结果而不是行回调？**  
查询结果的主要消费者是 JSON API 响应。将行转换为 cJSON 数组消除了每个处理器的中间转换，减少内存副本，并保持 HTTP 响应路径简单。对于不需要 JSON 的工作负载，可通过自定义驱动查询使用原始 `csilk_db_result_t`。

**为什么默认不使用预处理语句？**  
预处理语句语义在驱动之间显著不同（SQLite 的 `sqlite3_prepare`、MySQL 的 `mysql_stmt_*`、PostgreSQL 的 `PQexecParams`）。统一的预处理语句 API 将需要最常见的公共分母，降低插件架构的价值。参数化 `csilk_db_query_param_json()` 提供简单的注入保护；在需要时，可通过驱动特定代码使用预处理语句。