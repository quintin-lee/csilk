# Server Stats Aggregation & Data Race Elimination Design

## 1. Problem Audit & Data Race Analysis

### 1.1 Current Implementation in `server_lifecycle.c`
```c
void
csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn)
{
    if (!server) {
        return;
    }
    if (active_conn) {
        *active_conn = atomic_load(&server->active_connections);
    }
    if (pooled_conn) {
        int total = 0;
        for (int w = 0; w < server->worker_pool_count; w++) {
            total += server->worker_pools[w].client_pool_count; // DATA RACE!
        }
        *pooled_conn = total;
    }
}
```

### 1.2 Identified Data Race & Bottlenecks
1. **Unsynchronized Read/Write of `client_pool_count`**:
   - Worker event-loop threads concurrently mutate `wp->client_pool_count` during connection acquire (`pool_get`) and release (`pool_put`) without atomic operations or memory barriers.
   - Any external thread (metrics middleware, health check timer, admin dashboard handler) calling `csilk_server_get_stats()` reads `wp->client_pool_count` directly.
   - This causes an undefined-behavior **Data Race** detected by ThreadSanitizer (TSAN).
2. **Global Active Connection Contention**:
   - `server->active_connections` was a single shared atomic variable updated across all worker threads, causing CPU cache-line bouncing.

---

## 2. Target Design: Worker-Local Stats & Aggregation

### 2.1 Per-Worker Cache-Isolated Counters (`worker_pool_t`)
In `src/core/internal/srv_internal.h`:
```c
typedef struct CSILK_CACHE_ALIGNED {
    csilk_server_t*  server;
    ...
    csilk_client_t*  client_pool[CSILK_CLIENT_POOL_SIZE];
    _Atomic(int)     client_pool_count;   /**< Atomic count of free clients in pool */
    _Atomic(int)     active_connections;  /**< Worker-local active connections count */
    ...
} worker_pool_t;
```

### 2.2 Relaxed Atomic Updates in Hot Path
- `pool_get(wp)`:
  - Decrements `wp->client_pool_count` via `atomic_store_explicit(..., memory_order_relaxed)`.
  - Increments `wp->active_connections` via `atomic_fetch_add_explicit(..., memory_order_relaxed)`.
- `pool_put(wp, client)`:
  - Decrements `wp->active_connections` via `atomic_fetch_sub_explicit(..., memory_order_relaxed)`.
  - Increments `wp->client_pool_count` via `atomic_store_explicit(..., memory_order_relaxed)`.

### 2.3 Safe Snapshot Aggregation in `csilk_server_get_stats()`
```c
void
csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn)
{
    if (!server) {
        return;
    }
    int active_total = 0;
    int pooled_total = 0;
    int n = server->worker_pool_count;

    if (server->worker_pools && n > 0) {
        for (int w = 0; w < n; w++) {
            worker_pool_t* wp = &server->worker_pools[w];
            pooled_total += atomic_load_explicit(&wp->client_pool_count, memory_order_relaxed);
            active_total += atomic_load_explicit(&wp->active_connections, memory_order_relaxed);
        }
    } else {
        active_total = atomic_load_explicit(&server->active_connections, memory_order_relaxed);
    }

    if (active_conn) {
        *active_conn = active_total > 0 ? active_total : 0;
    }
    if (pooled_conn) {
        *pooled_conn = pooled_total > 0 ? pooled_total : 0;
    }
}
```

---

## 3. Guarantees & Verification
1. **0 Global Lock in Hot Path**: Only relaxed atomics on worker-local cache lines.
2. **Arbitrary Thread Safety**: Can be called concurrently by any thread at any time.
3. **TSAN Clean**: Zero data races between readers and worker writers.
4. **Benchmark**: `tests/core/test_server_stats_bench.c` measuring query throughput and latency under multi-threaded load.
