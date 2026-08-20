# JSON Subsystem Audit & Value Object Architecture Design

## 1. Audit & Problem Assessment

### 1.1 Current Architecture
In `src/core/json/json_internal.h` and `src/core/json/json_internal.c`:
```c
#define CSILK_JSON_VIEW_RING_SIZE 65536
__thread csilk_json_t tls_view_ring[CSILK_JSON_VIEW_RING_SIZE];
__thread size_t       tls_view_ring_idx = 0;
```
Every root document creation and child view lookup (`csilk_json_get`, `csilk_json_array_get`, `csilk_json_object_next`, etc.) indexes into `tls_view_ring` with `(tls_view_ring_idx++) % 65536`, marking `is_static = true` to prevent `csilk_json_free` from calling `free()`.

---

### 1.2 Evaluation of the 6 Dimensions

#### 1. TLS Memory Footprint
- `sizeof(csilk_json_t)` is 24 bytes (32 bytes with alignment).
- Ring footprint: $65,536 \times 32\text{ bytes} = \mathbf{2.0\text{ MB}}$ **per thread**.
- A standard server running 64 worker threads + 32 IO/MQ/offload threads consumes **$96 \times 2\text{ MB} = \mathbf{192\text{ MB}}$** of memory purely in static thread-local storage.
- High thread creation/destruction costs and high risk of allocation failure on resource-constrained or embedded environments.

#### 2. Pointer Lifetime
- Pointers returned from `csilk_json_get()` reference transient slots in `tls_view_ring`.
- The lifetime is implicitly governed by subsequent operations on the same thread rather than document ownership.

#### 3. Ring Overwrite (Silent Data Corruption)
- After 65,536 lookups on a single thread (e.g. iterating a 70,000-element array or handling thousands of requests on a persistent connection), index wrap-around silently overwrites previously returned `csilk_json_t` structs in place.
- Callers holding previously saved pointers read corrupted data without any warning or crash detection.

#### 4. Nested JSON Operations
- Deep recursive tree traversal or nested multi-level object processing that triggers $> 65,536$ sub-lookups will overwrite parent node pointers on the active call stack, leading to invalid memory dereferencing when the call stack unwinds.

#### 5. Async Handler Holding JSON View
- In asynchronous or event-driven execution (e.g. `csilk_dispatch`, thread pool offload, or coroutines), passing a `csilk_json_t*` pointer to a callback allows the owning worker thread to continue running, rapidly overwriting the TLS slot before the callback executes.

#### 6. Cross-Thread JSON View (TSAN Violations & UAF)
- `__thread` memory is unmapped when its owning thread terminates. Passing a TLS view pointer to another thread results in a Use-After-Free (UAF) segfault upon thread exit, or severe data races under TSAN when both threads read/write the slot unsynchronized.

---

## 2. Target Design: 16-Byte Small Value Object

### 2.1 Struct Layout
```c
typedef struct csilk_json_s {
    union {
        void*           raw;
        yyjson_val*     ival; /**< Immutable yyjson value */
        yyjson_mut_val* mval; /**< Mutable yyjson value */
    } u;
    union {
        void*           raw;
        yyjson_doc*     idoc; /**< Immutable document reference */
        yyjson_mut_doc* mdoc; /**< Mutable document reference */
    } doc;
    uint32_t flags; /**< Bit 0: is_owner, Bit 1: is_mutable, Bit 2: is_heap */
} csilk_json_t;
```
- Total size: Exactly **16 bytes** (passed and returned directly via 2 CPU registers `rax:rdx` / `x0:x1`).
- **0 heap allocation, 0 TLS memory, 0 ring overwrite**.

---

## 3. Public API Compatibility & Migration Plan

### 3.1 Dual-Track API Design
1. **Value Object Fast Path (Zero-Allocation, Modern C)**:
   - `csilk_json_t csilk_json_get_v(csilk_json_t obj, const char* key);`
   - `csilk_json_t csilk_json_array_get_v(csilk_json_t arr, size_t index);`
   - `bool         csilk_json_is_valid(csilk_json_t v);`
   - Returns by value on the stack/register.
2. **Pointer Compatibility Path**:
   - `csilk_json_parse()` and `csilk_json_object()` allocate 1 single 16-byte root node on heap.
   - For `csilk_json_get()`, allocate 16-byte view dynamically or return pointer safely.
   - Completely removes `tls_view_ring[65536]` and eliminates 2MB/thread TLS consumption.

---

## 4. Verification Strategy

1. **Benchmark (`tests/core/test_json_accessor_bench.c`)**:
   - Accessor latency (cycles/get) for flat, nested, array, and string lookups.
   - Throughput (operations/sec).
   - Memory footprint metrics (0 MB vs 2 MB).
2. **Thread Safety & Sanitizers**:
   - Verify 0 data races under ThreadSanitizer during concurrent cross-thread reads.
   - Verify 0 memory leaks / 0 UAF under AddressSanitizer.
