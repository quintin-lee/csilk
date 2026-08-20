# HTTP/2 Stream Map Optimization Design

## 1. Objective

Refactor `client->h2_streams` from a linked list to an adaptive power-of-two chained hash map in `src/core/http/h2_session.c` and `src/core/http/h2_callbacks.c`.

### Requirements
1. Support 100 / 1,000 / 10,000 concurrent multiplexed HTTP/2 streams with constant $O(1)$ lookup time.
2. Timely and leak-free removal when a stream closes (`on_stream_close_callback`).
3. Explicit stream context and arena ownership lifecycle.
4. Zero heap allocation overhead for typical stream counts ($\le 16$) by using inline bucket storage in `csilk_client_t`.
5. Benchmark stream lookup, insertion, deletion across 100 / 1K / 10K concurrent streams.
6. Clean ASAN (Address/Leak Sanitizer) and TSAN execution.

---

## 2. Architecture & Data Structures

### 2.1 Hash Map Representation in `csilk_client_t`
In `src/core/internal/srv_internal.h`:

```c
#define CSILK_H2_INLINE_BUCKETS 16

typedef struct csilk_h2_stream_map_s {
    csilk_ctx_t** buckets;                              /**< Pointer to active bucket array (inline or heap). */
    uint32_t      capacity;                             /**< Total bucket capacity (power of two). */
    uint32_t      mask;                                 /**< Bitmask for fast modulo (capacity - 1). */
    uint32_t      count;                                /**< Active stream count. */
    csilk_ctx_t*  inline_buckets[CSILK_H2_INLINE_BUCKETS]; /**< Embedded fast-path storage (zero malloc). */
} csilk_h2_stream_map_t;
```

In `struct csilk_client_s`:
Replace `csilk_ctx_t* h2_streams;` with:
```c
    csilk_h2_stream_map_t h2_stream_map; /**< Adaptive hash table for active HTTP/2 streams. */
```

### 2.2 Hash Function
Use Knuth's multiplicative hashing on the 31-bit stream ID:
```c
static inline uint32_t
_csilk_h2_stream_hash(int32_t stream_id, uint32_t mask)
{
    /* Golden ratio multiplier for 32-bit integers */
    return (uint32_t)(((uint32_t)stream_id * 2654435761u) & mask);
}
```

### 2.3 Collision & Storage Management
- `csilk_ctx_t` retains `struct csilk_ctx_s* next_stream;` as its collision chain pointer in the bucket.
- **Zero wrapper allocation**: Every `csilk_ctx_t` inserted into the hash table is directly chained via its own `next_stream` pointer.

### 2.4 Adaptive Dynamic Resizing
- Initial capacity: `CSILK_H2_INLINE_BUCKETS` (16), using `inline_buckets`. `buckets = inline_buckets`.
- Threshold: When `count >= capacity` and `capacity < 65536`:
  - Double capacity (`new_cap = capacity * 2`).
  - Allocate `new_buckets = malloc(sizeof(csilk_ctx_t*) * new_cap)`.
  - Rehash all existing active contexts into `new_buckets`.
  - If previous `buckets != inline_buckets`, `free(old_buckets)`.
  - Update `buckets = new_buckets; capacity = new_cap; mask = new_cap - 1;`.

---

## 3. Lifecycle & Operations

### 3.1 Initialization (`csilk_h2_init_stream_map` / in `connection_pool.c`)
- `map->capacity = CSILK_H2_INLINE_BUCKETS;`
- `map->mask = CSILK_H2_INLINE_BUCKETS - 1;`
- `map->count = 0;`
- `map->buckets = map->inline_buckets;`
- `memset(map->inline_buckets, 0, sizeof(map->inline_buckets));`

### 3.2 Stream Lookup & Creation (`csilk_h2_get_or_create_stream`)
1. Compute `idx = _csilk_h2_stream_hash(stream_id, map->mask);`
2. Walk bucket list `for (csilk_ctx_t* c = map->buckets[idx]; c; c = c->next_stream)`:
   - If `c->stream_id == stream_id`, return `c` immediately ($O(1)$ fast path).
3. If not found:
   - Check if resizing is needed (`count >= capacity`); if so, expand table.
   - Recompute `idx = _csilk_h2_stream_hash(stream_id, map->mask);`
   - Allocate `csilk_ctx_t* ctx = malloc(sizeof(csilk_ctx_t));`
   - Initialize `_csilk_ctx_init(ctx, client->server, client);`
   - Set `ctx->stream_id = stream_id;`
   - Create arena: `ctx->arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);`
   - Insert into bucket head: `ctx->next_stream = map->buckets[idx]; map->buckets[idx] = ctx;`
   - `map->count++;`
   - Return `ctx`.

### 3.3 Stream Closure (`csilk_h2_remove_stream` / `on_stream_close_callback`)
1. Compute `idx = _csilk_h2_stream_hash(stream_id, map->mask);`
2. Walk bucket `csilk_ctx_t** curr = &map->buckets[idx];`:
   - If `(*curr)->stream_id == stream_id`:
     - Unlink: `csilk_ctx_t* found = *curr; *curr = found->next_stream; found->next_stream = NULL;`
     - Decrement count: `map->count--;`
     - Free context resources:
       ```c
       csilk_ctx_cleanup(found);
       if (found->arena) {
           csilk_arena_free(found->arena);
           found->arena = NULL;
       }
       free(found);
       ```
     - Return 0 (success).
3. Return -1 if not found.

### 3.4 Connection Teardown (`csilk_h2_free_streams`)
1. Iterate `for (uint32_t i = 0; i < map->capacity; i++)`:
   - Walk bucket chain, calling `csilk_ctx_cleanup()`, `csilk_arena_free()`, `free(ctx)` for each node.
2. If `map->buckets != map->inline_buckets`, `free(map->buckets)`.
3. Reset map to initial empty inline state.

---

## 4. Verification & Testing

1. **Unit & Benchmark Test**: `tests/core/test_h2_stream_bench.c`
   - Test correctness: insert, lookup, close, multiple streams, collisions, resize.
   - Benchmark: 100, 1,000, 10,000 concurrent streams:
     - Stream lookup latency (cycles / ns per lookup).
     - Stream creation / close throughput.
2. **Protocol Integration**: `tests/protocols/test_h2.c`
   - Ensure GET, POST, Server Push over TLS with HTTP/2 function identically.
3. **Full Sanitizer Matrix**:
   - `ctest` under standard Debug build.
   - `ctest` under ASAN (Address + Leak sanitizer).
