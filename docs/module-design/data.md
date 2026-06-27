# Data / Database Abstraction Layer

## 1. Overview

The Data layer provides a **pluggable, driver-based database abstraction** inspired by Go's `database/sql`. It separates the unified query/exec API from backend-specific wire protocols, enabling SQLite, MySQL, PostgreSQL, MongoDB, and Redis to be used interchangeably through a single interface. All DB drivers **MUST** implement the `csilk_db_driver_t` vtable. Connection pool operations **MUST** be thread-safe. Query results **MUST** be returned as `csilk_db_result_t` with arena-managed rows — callers **MUST NOT** free individual cells. Connection acquisition from pool **SHOULD** complete in ≤ 100ns (lock-free linked list pop).

**Files**: `src/data/db.c`, `src/data/db_internal.h`, `include/csilk/drivers/db.h`

---

## 2. Architecture

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
└─────────────────────┘    └─────────────────────────────┘
```

### Key Design Decisions

1. **Single connection per pool** — Each `csilk_db_pool_t` holds exactly one backend connection, serialized by a `uv_mutex_t`. This avoids the complexity of internal connection pooling. Callers who need concurrency create multiple pools.

2. **Driver vtable + registry** — Drivers self-register at startup via `csilk_db_register_driver()`. The registry is a fixed-size array (max 16 drivers) protected by a mutex. Drivers are looked up by name at pool-creation time.

3. **Results as cJSON** — Query results are converted from tabular format to cJSON arrays of objects. All values are represented as strings (no type inference). This aligns with the JSON-first HTTP response path and eliminates per-driver serialization logic in callers.

4. **Built-in vs. compile-time drivers** — Only SQLite is always included. MySQL, PostgreSQL, MongoDB, and Redis are conditionally compiled via `HAS_MYSQL`, `HAS_POSTGRES`, `HAS_MONGODB`, `HAS_REDIS` macros.

---

## 3. Core Data Structures

### 3.1 `csilk_db_pool_t` (opaque)

```c
// src/data/db_internal.h
struct csilk_db_pool_s {
    csilk_db_driver_t* driver;   // Points into global registry
    void* connection;             // Driver-specific handle
    uv_mutex_t mutex;             // Serializes access to connection
};
```

Internally a thin wrapper: stores a driver pointer (from the global registry), an opaque connection handle (allocated by the driver's `connect` callback), and a mutex for thread-safe query/exec serialization.

### 3.2 `csilk_db_driver_t` (vtable)

| Field | Type | Purpose |
|---|---|---|
| `name` | `const char*` | Driver identifier (e.g. `"sqlite3"`) |
| `connect` | `int (*)(pool, dsn)` | Open connection, populate pool->connection |
| `disconnect` | `int (*)(pool)` | Close connection, free pool->connection |
| `query` | `int (*)(pool, sql, result*)` | SELECT → tabular result set |
| `exec` | `int (*)(pool, sql)` | INSERT/UPDATE/DELETE/DDL |
| `transaction_begin` | `int (*)(pool)` | BEGIN |
| `transaction_commit` | `int (*)(pool)` | COMMIT |
| `transaction_rollback` | `int (*)(pool)` | ROLLBACK |
| `free_result` | `void (*)(result*)` | Release driver-allocated result memory |

### 3.3 Result Types

```
csilk_db_result_t
  ├── rows[]       → csilk_db_row_t
  │                   └── values[]  → char*  (per column, NULL for SQL NULL)
  ├── column_names[] → char*        (nullable — driver may omit)
  ├── row_count    → int
  └── column_count → int
```

Rows and column names are heap-allocated by the driver; `free_result()` returns all memory.

---

## 4. API Surface

### Pool Lifecycle

| Function | Role |
|---|---|
| `csilk_db_pool_new(driver_name, dsn)` | Look up driver → alloc pool → init mutex → call driver->connect |
| `csilk_db_pool_free(pool)` | Call driver->disconnect → destroy mutex → free pool |
| `csilk_db_pool_get_connection(pool)` | Return opaque driver handle (caller must hold mutex) |
| `csilk_db_pool_set_connection(pool, conn)` | Store driver handle (typically called in connect callback) |

### Query / Exec

| Function | Role |
|---|---|
| `csilk_db_query_json(pool, sql)` | SELECT → mutex-lock → driver->query → cJSON conversion → mutex-unlock |
| `csilk_db_query_param_json(pool, sql, params)` | Parameterized query with SQL-injection escaping (see §5.2) |
| `csilk_db_exec(pool, sql)` | INSERT/UPDATE/DELETE → mutex-lock → driver->exec → mutex-unlock |

### Driver Registry

| Function | Role |
|---|---|
| `csilk_db_init()` | Register all built-in drivers (SQLite always, others conditional) |
| `csilk_db_register_driver(name, vtable)` | Add a driver to the global registry |
| `csilk_db_get_driver(name)` | Linear search of registry by name |

### Metrics

| Function | Role |
|---|---|
| `csilk_db_get_stats(stats*)` | Atomic read of queries_total / execs_total / errors_total / duration_us_total |

Metrics are global atomics (`atomic_uint_fast64_t`) updated on every query/exec. The Prometheus middleware reads them via `csilk_db_get_stats()`.

---

## 5. Key Algorithms

### 5.1 Row-to-JSON Conversion

```
csilk_db_query_json_locked(pool, sql):
  1. Call driver->query() → tabular csilk_db_result_t
  2. cJSON_CreateArray()
  3. For each row i:
     a. cJSON_CreateObject()
     b. For each column j:
        - If row->values[j] is not NULL:
          Add (column_names[j] | "col", value) as string
     c. Append object to array
  4. driver->free_result()
  5. Return cJSON array (caller must cJSON_Delete)
```

All values are cJSON strings — no type inference. The caller may use `cJSON_GetNumberValue()` etc. to convert.

### 5.2 Parameterised Query (SQL Injection Protection)

```
csilk_db_query_param_json(pool, sql, params[]):
  1. Pre-scan: compute output buffer size =
     len(sql) + 2*N_params + escape_overhead (' → '', \ → \\)
  2. Allocate buffer
  3. Walk sql char-by-char:
     - '?' → write "'escaped_value'" (with '→'', \→\\)
     - other → copy verbatim
  4. Execute constructed SQL via csilk_db_query_json_locked()
  5. Free buffer
```

This is **NOT a prepared-statement binding** — it's safe string interpolation. The escaping covers single quotes and backslashes, which defeats simple injection vectors. For true isolation, a prepared-statement variant would require driver-specific APIs.

### 5.3 Pool Lifecycle

```
csilk_db_pool_new(name, dsn)
  ├── csilk_db_get_driver(name)      // linear scan of registry
  ├── calloc csilk_db_pool_t
  ├── uv_mutex_init(&pool->mutex)
  ├── driver->connect(pool, dsn)     // driver calls csilk_db_pool_set_connection
  ├── on failure: uv_mutex_destroy + free → return NULL
  └── on success: return pool

csilk_db_pool_free(pool)
  ├── driver->disconnect(pool)       // driver frees its handle
  ├── uv_mutex_destroy(&pool->mutex)
  └── free(pool)
```

---

## 6. Built-in Drivers

| Driver | C Module | Requires | Connection |
|---|---|---|---|
| **sqlite3** | `src/drivers/sqlite.c` | Always built | File path (DSN = `/path/to/db.sqlite`) |
| **mysql** | `src/drivers/mysql.c` | `libmysqlclient`, `HAS_MYSQL` | MySQL connection string |
| **postgres** | `src/drivers/postgres.c` | `libpq`, `HAS_POSTGRES` | PostgreSQL connection string |
| **mongodb** | `src/drivers/mongodb.c` | `libmongoc`, `HAS_MONGODB` | MongoDB URI |
| **redis** | `src/drivers/redis.c` | `hiredis`, `HAS_REDIS` | Redis host:port |

### 6.1 Redis Dual Role

Redis serves two roles in csilk:

1. **Database driver** (`csilk_db_driver_t`) — Redis commands via the standard query/exec interface. Used for direct Redis command execution.

2. **Storage driver** (`csilk_storage_driver_t`) — Created via `csilk_redis_storage_driver_new(pool)`. Provides key-value storage (set/get/clear/incr) for session management and rate limiting. The storage driver wraps a DB pool and translates storage operations to Redis commands.

### 6.2 Transaction Support

SQLite, MySQL, PostgreSQL, and MongoDB drivers implement the full transaction vtable (`begin` / `commit` / `rollback`). The transaction methods delegate to the corresponding SQL commands (`BEGIN`, `COMMIT`, `ROLLBACK`) sent through the driver's `exec` function. MongoDB uses `mongoc_client_session_start/commit/abort_transaction`.

Redis transactions use `MULTI/EXEC/DISCARD` in its transaction callbacks (separate from the db_driver_t vtable; handled in `redis_drv_transaction_begin/commit/rollback`).

---

## 7. Vector Database

In addition to the SQL/NoSQL drivers, csilk has a separate **vector database** plug-in system:

**File**: `src/drivers/vector/vector.c`, `include/csilk/drivers/vector.h`

| Function | Role |
|---|---|
| `csilk_vector_db_register_driver()` | Register a vector DB driver by name |
| `csilk_vector_db_new(name, endpoint, api_key)` | Create a vector DB instance |
| `csilk_vector_db_upsert(db, collection, points, count)` | Insert/update vectors |
| `csilk_vector_db_search(db, collection, vector, dim, limit, res*)` | ANN search |
| `csilk_vector_db_free(db)` | Destroy instance |

**Supported backends** (conditionally compiled):
- **Qdrant** — REST/gRPC-based vector search engine
- **Milvus** — Distributed vector database

The vector DB system mirrors the DB layer's plug-in pattern but uses a separate registry with its own driver lifecycle (init/free + state pointer), since vector DBs are HTTP/gRPC clients rather than SQL connections.

---

## 8. Concurrency Model

```
┌─────────────────────────────────────────────────────┐
│                  Worker Thread N                     │
│  ┌─────────┐  ┌─────────┐  ┌────────────────────┐   │
│  │Pool A   │  │Pool B   │  │Pool C (shared)      │   │
│  │mutex    │  │mutex    │  │mutex                │   │
│  │conn 1   │  │conn 2   │  │conn 3 (serialized)  │   │
│  └─────────┘  └─────────┘  └────────────────────┘   │
└──────────┬──────────────────────────────────────────┘
           │
┌──────────▼──────────────────────────────────────────┐
│              Global Driver Registry                  │
│  [sqlite3] [mysql] [postgres] [mongodb] [redis]     │
│  registry_mutex (only during register/get, not I/O)  │
└─────────────────────────────────────────────────────┘
```

- **Driver registry**: Protected by `registry_mutex`. Locked only during registration (startup) and lookup (pool creation). Never held during I/O.
- **Per-pool**: Each pool has its own `uv_mutex_t`. Locked for the duration of each query/exec call. Serializes access to the single backend connection.
- **Metrics**: Atomic operations on `atomic_uint_fast64_t` counters — no lock contention.

---

## 9. Error Handling & Metrics

Every query/exec call follows the same instrumentation pattern:

```
1. Record start time (uv_hrtime)
2. Lock pool mutex
3. Call driver function
4. Unlock pool mutex
5. Compute duration
6. Atomic-add to db_duration_us_total
7. On success: atomic-inc db_queries_total or db_execs_total
8. On failure: atomic-inc db_errors_total, log error with duration
```

This provides consistent observability regardless of driver backend.

---

## 10. Usage Example

```c
// Initialization
csilk_db_init();

// Create a pool (single connection, mutex-serialized)
csilk_db_pool_t* db = csilk_db_pool_new("sqlite3", "/tmp/app.db");

// Query → JSON array
cJSON* users = csilk_db_query_json(db, "SELECT * FROM users LIMIT 10");
cJSON_Delete(users);

// Parameterized query
const char* params[] = {"admin", NULL};
cJSON* result = csilk_db_query_param_json(db, "SELECT * FROM users WHERE role = ?", params);
cJSON_Delete(result);

// Exec
csilk_db_exec(db, "INSERT INTO log (msg) VALUES ('startup')");

// Cleanup
csilk_db_pool_free(db);
```

---

## 11. Related Files

| File | Role |
|---|---|
| `src/data/db.c` | Pool lifecycle, query/exec, driver registry, metrics |
| `src/data/db_internal.h` | Internal pool struct definition |
| `include/csilk/drivers/db.h` | Public API — driver vtable, pool functions, registration |
| `src/drivers/sqlite.c` | SQLite3 driver implementation |
| `src/drivers/mysql.c` | MySQL driver (conditional) |
| `src/drivers/postgres.c` | PostgreSQL driver (conditional) |
| `src/drivers/mongodb.c` | MongoDB driver (conditional) |
| `src/drivers/redis.c` | Redis driver + storage driver (conditional) |
| `src/drivers/vector/vector.c` | Vector DB plug-in system (separate registry) |
| `include/csilk/drivers/vector.h` | Vector DB public API |
| `examples/example_custom_driver.c` | Custom driver registration example + test |
| `python/csilk/db.py` | Python ctypes bindings for the DB layer |

---

## 12. Design Rationale

**Why single-connection pools instead of a multi-connection pool?**  
Simplicity and predictability. A multi-connection pool would require connection health-checking, max-lifetime management, drain-on-shutdown, and complex eviction logic — all of which add latency and failure modes. By exposing single-connection pools, we let the *caller* decide the pooling strategy (one pool per worker thread, one pool per database role, etc.), matching the libuv worker-thread architecture without redundant abstraction.

**Why cJSON for query results instead of row callbacks?**  
The primary consumer of query results is JSON API responses. Converting rows to cJSON at the DB layer avoids per-handler result marshalling, reduces memory copies, and keeps the HTTP response path simple. For workloads that don't need JSON, the raw `csilk_db_result_t` can be consumed via a custom driver query.

**Why not prepared statements as the default?**  
Prepared statement semantics differ significantly across drivers (SQLite's `sqlite3_prepare`, MySQL's `mysql_stmt_*`, PostgreSQL's `PQexecParams`). A unified prepared-statement API would require the most restrictive common denominator, reducing the value of the pluggable architecture. The parameterized `csilk_db_query_param_json()` provides safe string interpolation for common cases; prepared statements are available through driver-specific code when needed.
