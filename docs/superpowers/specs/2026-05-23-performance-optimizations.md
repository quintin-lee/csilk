# Performance Optimizations Design Spec

Date: 2026-05-23
Status: Approved

## Goals
1. Offload CPU-bound and I/O-bound tasks to the libuv thread pool.
2. Optimize header lookup and memory management.
3. Improve keep-alive performance via arena reuse.
4. Scale to multiple CPU cores via multi-loop architecture.
5. Fix networking and logging performance bottlenecks.

## 1. Async Work Offloading (uv_queue_work)

### Gzip Middleware
- Current: Synchronous `deflate` in the middleware handler.
- New:
  - Add a flag `is_async` and `uv_work_t work_req` to `csilk_ctx_t`.
  - The `gzip` middleware will move the `deflate` logic to a work callback.
  - The handler chain will be "paused" or marked as async.
  - Since `gzip` usually wraps the entire request (calls `csilk_next` then compresses), it will capture the response body, offload compression to the worker thread, and then the completion callback will trigger the final response sending.

### Static File Serving
- Current: Synchronous `realpath`, `open`, `stat`, `read` using `NULL` loop.
- New:
  - Move the file I/O operations to a worker thread via `uv_queue_work`.
  - The `after_work_cb` will populate the context and trigger the response.

## 2. Header Hash Table & Arena Refactoring

### Data Structures
- Modify `csilk_header_t` to be part of a hash table.
- Use a fixed number of buckets (e.g., 16) in the request and response structures.
- All header keys and values MUST be allocated from the `arena`.
- No more `strdup` or `free` for individual headers.

### APIs
- `csilk_get_header`: Case-insensitive hash lookup.
- `csilk_set_header`: Hash insert/update.
- `csilk_add_header`: Handle multiple headers with same key (linked list within the bucket).

## 3. Arena Reuse (csilk_arena_reset)

### New Function
- `void csilk_arena_reset(csilk_arena_t* arena)`:
  - Instead of freeing chunks, it resets the `used` counter on all chunks to zero.
  - It keeps the chunk list intact for reuse in the next request of the same connection.

### Integration
- In `on_message_complete` (server.c), if it's a keep-alive connection, call `csilk_arena_reset` on the client context's arena.
- `csilk_ctx_cleanup` will now call `csilk_arena_reset` instead of `csilk_arena_free` if reuse is enabled.

## 4. Multi-loop Server (SO_REUSEPORT)

### Configuration
- Add `int worker_threads` to `csilk_config_t`.

### Implementation
- `csilk_server_run` will spawn `n-1` pthreads.
- Each thread creates its own `uv_loop_t`.
- Each loop creates its own `uv_tcp_t`.
- `uv_tcp_bind` with `UV_TCP_REUSEPORT`.
- Each loop runs `uv_run`.

## 5. Logger Optimization
- Remove `fflush(g_logger.fp)` from `log_text` and `log_json`.
- This relies on OS buffering and improves performance significantly under high load.

## 6. TCP_NODELAY Fix
- Move `uv_tcp_nodelay(client_handle, 1)` to `on_new_connection` in `src/core/server.c`.

## Verification Strategy
- **Benchmarks**: Compare RPS and latency before and after (especially for multi-core and gzip).
- **Unit Tests**:
  - `test_arena`: Verify `csilk_arena_reset` doesn't leak or corrupt memory.
  - `test_headers`: Verify hash table lookup and multiple headers.
- **Integration Tests**: Ensure keep-alive, static files, and gzip still work correctly.
