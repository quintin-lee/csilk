# HTTP/2 Per-Connection Stream Pool Design

## 1. Overview & Objective

Eliminate per-stream `malloc(sizeof(csilk_ctx_t))` and `csilk_arena_new()` overhead in HTTP/2 request processing by implementing a per-connection stream context and arena pool (`csilk_h2_stream_map_t`).

### Key Requirements
1. $O(1)$ lock-free stream context acquire and release within the owning worker thread.
2. Arena pooling and resetting via `csilk_arena_reset()`, eliminating dynamic memory allocation system calls on stream reuse.
3. Clean reset of request/response headers, bodies, state flags, and storage items on stream reuse.
4. Safe concurrent and high-frequency stream closures (RST_STREAM / END_STREAM).
5. Comprehensive single-pass teardown on connection close.
6. Zero impact on HTTP/1 performance or memory footprint.
7. High-concurrency benchmarks across 100 and 1,000 streams comparing pooled vs unpooled allocation.

---

## 2. Architecture & Data Structures

### 2.1 Per-Connection Stream Map & Pool
In `src/core/internal/srv_internal.h`:

```c
#define CSILK_H2_INLINE_BUCKETS 16
#define CSILK_H2_STREAM_POOL_MAX 64

typedef struct csilk_h2_stream_map_s {
    csilk_ctx_t** buckets;                                 /**< Active bucket array (points to inline_buckets or heap). */
    uint32_t      capacity;                                /**< Bucket capacity (power of 2). */
    uint32_t      mask;                                    /**< Mask (capacity - 1). */
    uint32_t      count;                                   /**< Number of currently active streams. */
    csilk_ctx_t*  inline_buckets[CSILK_H2_INLINE_BUCKETS]; /**< Embedded fast-path buckets (0 malloc). */
    csilk_ctx_t*  free_list;                               /**< LIFO free list of idle stream contexts. */
    uint32_t      pool_count;                              /**< Current number of pooled stream contexts. */
    uint32_t      pool_max;                                /**< Maximum pool capacity (defaults to 64). */
} csilk_h2_stream_map_t;
```

### 2.2 Lifecycle & State Transitions

```
[ New Stream Needed (get_or_create) ]
                 │
      ┌──────────┴──────────┐
 [free_list != NULL]   [free_list == NULL]
      │                     │
 Pop context from      malloc(csilk_ctx_t) +
 free_list             csilk_arena_new()
      │                     │
 csilk_arena_reset()        │
      │                     │
      └──────────┬──────────┘
                 │
      _csilk_ctx_init(ctx, server, client)
      ctx->stream_id = stream_id
      Insert into active hash map
                 │
                 ▼
       [ Stream Active / Running ]
                 │
                 ▼
    [ Stream Closes / RST_STREAM ]
                 │
       Unlink from active hash map
       csilk_ctx_cleanup(ctx)
                 │
      ┌──────────┴──────────┐
 [pool_count < pool_max] [pool_count >= pool_max]
      │                     │
 csilk_arena_reset(arena) csilk_arena_free(arena)
 Push to free_list        free(ctx)
 (O(1) recycling)
```

---

## 3. Implementation Details

### 3.1 Stream Map Initialisation
- `map->free_list = NULL;`
- `map->pool_count = 0;`
- `map->pool_max = CSILK_H2_STREAM_POOL_MAX;`

### 3.2 Stream Acquire (`csilk_h2_get_or_create_stream`)
1. Compute bucket index with Knuth hash: `idx = _csilk_h2_stream_hash(stream_id, map->mask)`.
2. Check if stream is already in active bucket chain: if found, return immediately.
3. Auto-resize active bucket table if `map->count >= map->capacity`.
4. If `map->free_list`:
   - Pop `ctx = map->free_list; map->free_list = ctx->next_stream; map->pool_count--;`
   - Reset arena: `csilk_arena_reset(ctx->arena);`
5. Else:
   - `ctx = malloc(sizeof(csilk_ctx_t));`
   - `ctx->arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);`
   - If server config specifies arena alignment, set alignment.
6. Initialize context: `_csilk_ctx_init(ctx, client->server, client);`
7. Set `ctx->stream_id = stream_id;`
8. Insert into active bucket: `ctx->next_stream = map->buckets[idx]; map->buckets[idx] = ctx; map->count++;`
9. Return `ctx`.

### 3.3 Stream Release (`csilk_h2_remove_stream`)
1. Unlink `ctx` from active hash map bucket. `map->count--;`
2. Perform complete resource release: `csilk_ctx_cleanup(ctx);`
3. If `map->pool_count < map->pool_max`:
   - `csilk_arena_reset(ctx->arena);`
   - `ctx->next_stream = map->free_list;`
   - `map->free_list = ctx;`
   - `map->pool_count++;`
4. Else:
   - `if (ctx->arena) csilk_arena_free(ctx->arena);`
   - `free(ctx);`

### 3.4 Connection Teardown (`csilk_h2_free_streams`)
1. Free all remaining active streams in `map->buckets`:
   `csilk_ctx_cleanup()` + `csilk_arena_free()` + `free()`.
2. Free all pooled idle streams in `map->free_list`:
   `csilk_arena_free()` + `free()`.
3. Free dynamic bucket array if allocated.
4. Reset map to zeroed inline state.

---

## 4. Verification & Testing

1. Add tests in `tests/core/test_h2_stream_bench.c`:
   - Verify pooled context reuse and memory stability across thousands of stream lifecycles.
   - Verify arena allocation and reset across repeated stream creations.
   - Benchmark throughput and cycles/op comparison with pooling enabled.
2. Full test suite and ASAN verification (100% pass).
