# RCU & Copy-On-Write Lifecycle Hook System Design

## 1. Objective

Refactor `src/core/config/hooks.c` from a mutable linked-list structure to an immutable contiguous handler array architecture powered by Copy-On-Write (CoW) and Epoch-Based Reclamation (EBR / RCU).

### Key Requirements
1. **Mutex-Free Request Hot Path**: `_csilk_trigger_hooks()` executes purely lock-free with a single atomic pointer load (`memory_order_acquire`).
2. **$O(N)$ Contiguous Array Traversal**: Handlers are stored sequentially in flat memory (`csilk_hook_array_t`), maximizing CPU cache-line efficiency and instruction prefetching.
3. **Non-Blocking Registration**: `csilk_server_add_hook()` operates via Copy-On-Write, publishing new arrays atomically without blocking active request processing.
4. **Safe Hook Removal & Grace Period Reclamation**: `csilk_server_remove_hook()` removes handlers, atomically publishes the updated array, and safely waits for in-flight reader epochs before freeing the retired array.
5. **Full Lifecycle Hook Support**:
   - Server-level: `CSILK_HOOK_SERVER_START`, `CSILK_HOOK_SERVER_STOP`.
   - Context/Request-level: `CSILK_HOOK_CONN_OPEN`, `CSILK_HOOK_CONN_CLOSE`, `CSILK_HOOK_REQUEST_BEGIN`, `CSILK_HOOK_REQUEST_END`.
6. **ThreadSanitizer (TSAN) Clean**: 0 data races under high-concurrency runtime hook modifications.

---

## 2. Architecture & Data Structures

### 2.1 Data Layout
In `src/core/internal/srv_internal.h`:

```c
typedef struct csilk_hook_array_s {
    size_t count;
    void*  handlers[]; /**< Flexible array member of handler pointers */
} csilk_hook_array_t;

struct csilk_server_s {
    ...
    _Atomic(csilk_hook_array_t*) hooks[CSILK_HOOK_COUNT]; /**< Atomic pointer to immutable handler array */
    csilk_mutex_t                hook_mutex;               /**< Protects writer CoW operations */
    ...
};
```

### 2.2 Reader Trigger Path ($O(N)$ Lock-Free)
In `src/core/config/hooks.c`:

```c
CSILK_INTERNAL void
_csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type)
{
    if (!s || (unsigned)type >= CSILK_HOOK_COUNT) {
        return;
    }

    csilk_hook_array_t* arr = atomic_load_explicit(&s->hooks[type], memory_order_acquire);
    if (!arr || arr->count == 0) {
        return;
    }

    size_t count = arr->count;
    void** handlers = arr->handlers;

    if (type <= CSILK_HOOK_SERVER_STOP) {
        for (size_t i = 0; i < count; i++) {
            ((csilk_server_hook_handler_t)handlers[i])(s);
        }
    } else if (c) {
        for (size_t i = 0; i < count; i++) {
            ((csilk_ctx_hook_handler_t)handlers[i])(c);
        }
    }
}
```

### 2.3 Writer CoW & RCU Reclamation
1. **`csilk_server_add_hook(s, type, handler)`**:
   - Acquire `s->hook_mutex`.
   - Sample `old_arr = atomic_load_explicit(&s->hooks[type], memory_order_relaxed)`.
   - Allocate `new_arr` for `old_count + 1` entries.
   - Insert new handler at index 0 (preserving LIFO semantics) and copy previous handlers.
   - Atomically store `atomic_store_explicit(&s->hooks[type], new_arr, memory_order_release)`.
   - If `old_arr` exists, wait for grace period (`csilk_server_wait_grace_period(s)`) and `free(old_arr)`.
   - Release `s->hook_mutex`.

2. **`csilk_server_remove_hook(s, type, handler)`**:
   - Acquire `s->hook_mutex`.
   - Sample `old_arr`. Find occurrences of `handler`.
   - If found, allocate `new_arr` with `old_count - removed` entries.
   - Copy non-matching handlers.
   - Atomically store `new_arr`.
   - Wait for grace period via `csilk_server_wait_grace_period(s)` and `free(old_arr)`.
   - Release `s->hook_mutex`.

3. **`csilk_server_free(s)`**:
   - Iterates through all `CSILK_HOOK_COUNT` arrays and frees any active `csilk_hook_array_t`.
   - Destroys `s->hook_mutex`.

---

## 3. Verification & Testing

- Correctness & Multi-threaded RCU test in `tests/app/test_hooks_rcu.c`:
  - Rapid parallel execution of `_csilk_trigger_hooks` across multiple worker threads.
  - Concurrent runtime additions and removals of hooks.
  - Verification of handler execution order, argument passing, and clean memory teardown.
- Full test suite passing under AddressSanitizer and ThreadSanitizer.
