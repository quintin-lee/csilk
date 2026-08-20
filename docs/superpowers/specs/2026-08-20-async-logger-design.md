# Async Lock-Free Structured Logger Design

## 1. Objective

Refactor `src/core/config/logger.c` from a synchronous global mutex file-writer model to a lock-free asynchronous logging pipeline with dedicated worker-thread decoupling.

### Key Requirements
1. **Near-Zero Disabled Log Overhead**: Branching check at macro invocation skips argument evaluation and formatting when the log level is disabled.
2. **Request ID Preservation**: Thread-local `tl_request_id` captured into text and JSON formats without lock contention.
3. **Structured JSON & Human-Readable Text Modes**: Full parity for JSON reflection/cJSON output and ANSI colored text output.
4. **Asynchronous File Rotation**: Background logger thread safely renames and reopens log files when `max_file_size` is exceeded without stalling worker I/O.
5. **Crash-Safe & Flush on Shutdown**: `csilk_log_flush()` and `csilk_log_close()` drain all remaining entries and sync to disk; exit handlers flush pending buffers.
6. **Configurable Queue Overflow Strategies**:
   - `CSILK_LOG_OVERFLOW_DROP` (0): Drop log and increment atomic drop counter.
   - `CSILK_LOG_OVERFLOW_BLOCK` (1): Yield/wait until space is available in the queue.
   - `CSILK_LOG_OVERFLOW_FALLBACK` (2): Directly write to stderr synchronously on overflow.
7. **Comprehensive Benchmarks**:
   - 0 logs/request (disabled level latency)
   - 1 log/request
   - 10 logs/request
   - 100 logs/request
   - Comparative benchmark against the baseline global mutex model.

---

## 2. Architecture & Data Structures

### 2.1 Pipeline Flow

```
[ Worker Thread 1 ] ──┐
[ Worker Thread 2 ] ──┼──> [ Lock-Free Node Pool ] ──> [ TLS Formatting Buffer ]
[ Worker Thread N ] ──┘                 │                           │
                                        ▼                           ▼
                           [ Lock-Free MPSC Queue ] <───────────────┘
                                        │
                                        ▼ (Wakeup / Batch Dequeue)
                           [ Dedicated Logger Thread ]
                                        │
                               ┌────────┴────────┐
                        (Size Check)       (Write & Flush)
                               │                 │
                               ▼                 ▼
                       [ File Rotation ]    [ Disk FILE* ]
```

### 2.2 Configuration & Overflow Strategy
In `include/csilk/core/types.h`:

```c
typedef enum {
    CSILK_LOG_OVERFLOW_DROP = 0,     /**< Drop new messages when queue is full. */
    CSILK_LOG_OVERFLOW_BLOCK = 1,    /**< Block/yield until space is available. */
    CSILK_LOG_OVERFLOW_FALLBACK = 2  /**< Write synchronously to stderr on overflow. */
} csilk_log_overflow_t;

typedef struct {
    csilk_log_level_t    level;             /**< Minimum level to emit. */
    const char*          file_path;         /**< Path to log file or NULL for stdout/stderr. */
    size_t               max_file_size;     /**< Max file size in bytes before rotation (0 = disabled). */
    int                  use_colors;        /**< ANSI color codes (1=on, 0=off, -1=auto). */
    int                  json_format;       /**< Emit structured JSON records. */
    csilk_log_overflow_t overflow_strategy; /**< Behavior when queue is full. */
    size_t               queue_capacity;    /**< Preallocated node pool capacity (default: 8192). */
} csilk_log_config_t;
```

### 2.3 Preallocated Log Node Slab
In `src/core/config/logger.c`:

```c
#define CSILK_LOG_NODE_BUF_SIZE 2048
#define CSILK_LOG_DEFAULT_QUEUE_CAPACITY 8192

typedef struct csilk_log_node_s {
    csilk_lfq_node_t         lfq_node;  /**< Intrusive node for MPSC queue (must be first). */
    _Atomic(struct csilk_log_node_s*) next_free; /**< Intrusive node for lock-free node pool. */
    size_t                   len;       /**< Formatted message length. */
    char                     buf[CSILK_LOG_NODE_BUF_SIZE]; /**< Formatted payload. */
} csilk_log_node_t;
```

### 2.4 Fast-Path Level Filter
In `include/csilk/core/server.h`:

```c
extern _Atomic(csilk_log_level_t) g_csilk_log_level;
extern _Atomic(int)               g_csilk_log_initialized;

#define CSILK_LOG_IS_ENABLED(lv) \
    (atomic_load_explicit(&g_csilk_log_initialized, memory_order_relaxed) && \
     (lv) >= atomic_load_explicit(&g_csilk_log_level, memory_order_relaxed))

#define CSILK_LOG_T(...) do { if (CSILK_LOG_IS_ENABLED(CSILK_LOG_TRACE)) _csilk_log_internal(CSILK_LOG_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define CSILK_LOG_D(...) do { if (CSILK_LOG_IS_ENABLED(CSILK_LOG_DEBUG)) _csilk_log_internal(CSILK_LOG_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define CSILK_LOG_I(...) do { if (CSILK_LOG_IS_ENABLED(CSILK_LOG_INFO))  _csilk_log_internal(CSILK_LOG_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define CSILK_LOG_W(...) do { if (CSILK_LOG_IS_ENABLED(CSILK_LOG_WARN))  _csilk_log_internal(CSILK_LOG_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define CSILK_LOG_E(...) do { if (CSILK_LOG_IS_ENABLED(CSILK_LOG_ERROR)) _csilk_log_internal(CSILK_LOG_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
#define CSILK_LOG_F(...) do { if (CSILK_LOG_IS_ENABLED(CSILK_LOG_FATAL)) _csilk_log_internal(CSILK_LOG_FATAL, __FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)
```

---

## 3. Lifecycle & Detailed Behavior

### 3.1 Producer Node Acquisition & Formatting
1. Check `CSILK_LOG_IS_ENABLED(lv)`. If false, return immediately ($< 1$ ns).
2. Pop node from lock-free free-node stack:
   - If free stack is empty:
     - `CSILK_LOG_OVERFLOW_DROP`: `atomic_fetch_add_explicit(&g_logger.dropped_count, 1, memory_order_relaxed); return;`
     - `CSILK_LOG_OVERFLOW_BLOCK`: spin / yield / `usleep(10)` in loop until node is available or stopping.
     - `CSILK_LOG_OVERFLOW_FALLBACK`: format to stack buffer and write directly to `stderr`.
3. Format message (Text or JSON) into `node->buf`, storing total length in `node->len`.
4. Enqueue into `g_logger.queue` via `csilk_lfq_enqueue(&g_logger.queue, &node->lfq_node)`.
5. Signal background logger thread (using `csilk_cond_signal` or atomic notification flag).

### 3.2 Consumer Background Thread Execution
1. Background thread `logger_worker_thread`:
   - Dequeue batch of up to $N$ nodes in loop.
   - For each dequeued node:
     - Check file rotation: if `max_file_size > 0 && current_size + node->len >= max_file_size`, perform `rotate_log_files()`.
     - Write: `fwrite(node->buf, 1, node->len, fp)`.
     - Update `current_size += node->len`.
     - Return node to lock-free free-node stack.
   - `fflush(fp)` periodically (after batch or on timeout).
   - If queue is empty, `csilk_cond_timedwait` (e.g. 5-10ms timeout).

### 3.3 Shutdown & Flush
- `csilk_log_flush()`: Signals thread and waits until queue is empty.
- `csilk_log_close()`:
  1. Sets `atomic_store(&g_logger.stopping, 1)`.
  2. Wakes up logger thread.
  3. Joins logger thread (`csilk_thread_join`).
  4. Flushes and closes `fp`.
  5. Frees node pool buffer.

---

## 4. Verification & Testing

1. **Benchmark Suite**: `tests/core/test_logger_async_bench.c`
   - Multi-thread stress test (1, 4, 8, 16 threads).
   - 0 logs/request disabled level latency comparison.
   - 1, 10, 100 logs/request throughput & latency comparison vs synchronous mutex baseline.
   - Overflow strategy verification (Drop / Block / Fallback).
   - Rotation and flush integrity verification.
2. Full test suite with ASAN & TSAN clean pass.
