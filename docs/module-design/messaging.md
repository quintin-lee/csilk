# Messaging / Event Bus Module

## 1. Overview

The Messaging module provides an **in-process publish/subscribe event bus** (Message Queue) for internal communication within the csilk server. It enables decoupled communication between components — routes, middleware, AI workflows, WebSocket handlers, and custom modules — through topic-based message exchange.

**Files**: `src/messaging/`, `include/csilk/mq.h`

---

## 2. Architecture

```
┌────────────────────────────────────────────────────────┐
│                    Publisher (any thread)                │
│              csilk_mq_publish(mq, topic, data, len)      │
└────────────────────────┬───────────────────────────────┘
                         │
                         ▼
┌────────────────────────────────────────────────────────┐
│                    MQ Instance (csilk_mq_t)              │
│                                                          │
│  ┌──────────────┐    ┌──────────────────────────────┐   │
│  │  WAL (disk)  │◄───│  Publish Path:               │   │
│  │  ── append   │    │  1. _mq_append_wal() (opt.)  │   │
│  │  ── fsync    │    │  2. _mq_enqueue() → queue LL │   │
│  └──────────────┘    │  3. uv_async_send()          │   │
│                       └──────────┬───────────────────┘   │
│                                  │ async signal           │
│                       ┌──────────▼───────────────────┐   │
│                       │  Dispatch (main loop):        │   │
│                       │  on_mq_async() →              │   │
│                       │  1. Dequeue all messages      │   │
│                       │  2. For each msg:             │   │
│                       │     a. Build handler chain    │   │
│                       │     b. Run chain (middleware  │   │
│                       │        → subscribers)         │   │
│                       │     c. Free message           │   │
│                       └──────────────────────────────┘   │
│                                                          │
│  ┌──────────────┐    ┌──────────────────────────────┐   │
│  │ Monitors     │    │  Topic Registry:             │   │
│  │ (WebSocket)  │◄───│  topics → linked list        │   │
│  │ mq_published │    │  topic → handlers[]          │   │
│  │ mq_delivered │    │  global_middlewares[]         │   │
│  └──────────────┘    └──────────────────────────────┘   │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │  Background Offload (libuv thread pool):          │   │
│  │  csilk_mq_offload() → uv_queue_work()             │   │
│  └──────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **In-process only** — Messages never leave the process boundary. This avoids serialization overhead, network latency, and the operational complexity of an external message broker (Kafka, RabbitMQ, Redis Streams). For cross-process messaging, use Redis or a dedicated broker as a database driver.

2. **Thread-safe publishing via `uv_async_t`** — Publishers from any worker thread enqueue messages under a mutex and signal the main loop via `uv_async_send()`. All dispatch (handler chain execution) happens on the main event loop thread, eliminating concurrency in subscriber code.

3. **Middleware-first dispatch** — Each topic has a handler chain built from global middleware → topic middleware → subscribers. Middleware can short-circuit the chain (`csilk_mq_abort()`), pass control (`csilk_mq_next()`), or offload work to the thread pool.

4. **Optional WAL persistence** — When enabled, every publish is durably written to disk before the message is enqueued. On restart, the WAL is replayed to recover undelivered messages.

---

## 3. Core Data Structures

### 3.1 `csilk_mq_msg_t` — A single enqueued message

```c
struct csilk_mq_msg_s {
    char* topic;        // Heap-allocated topic string copy
    void* payload;      // Heap-allocated payload copy
    size_t len;          // Payload byte length
    csilk_mq_msg_t* next; // Linked-list pointer (nullptr for tail)
};
```

Messages are deep-copied on publish — the caller can reuse/free the original buffer immediately.

### 3.2 `csilk_mq_topic_t` — A registered topic node

```c
struct csilk_mq_topic_s {
    char* name;               // Topic name (e.g., "user.created")
    csilk_mq_handler_t* handlers; // Dynamically-grown handler array
    size_t handler_count;      // Current handlers
    size_t handler_capacity;   // Allocated capacity
    csilk_mq_topic_t* next;   // Linked-list pointer
};
```

Topics are created lazily on first `subscribe()` or `use()` call. Topic names support `fnmatch(3)` glob patterns.

### 3.3 `csilk_mq_ctx_t` — Per-message execution context

```c
struct csilk_mq_ctx_s {
    csilk_mq_t* mq;             // Owning MQ instance
    csilk_mq_msg_t* msg;        // The message being processed
    csilk_mq_handler_t* handlers; // Resolved handler chain
    size_t handler_count;        // Total handlers in chain
    int handler_index;           // Current position in chain
    int aborted;                 // Non-zero if abort() was called
};
```

Created fresh for each message on dispatch. Valid only during the handler invocation.

### 3.4 `csilk_mq_t` — The MQ instance

```c
struct csilk_mq_s {
    uv_async_t async_handle;       // Main-loop signal bridge
    uv_mutex_t queue_mutex;        // Guards message linked list
    csilk_mq_msg_t* queue_head;    // Pending message queue (FIFO)
    csilk_mq_msg_t* queue_tail;

    csilk_mq_topic_t* topics;      // Registered topic list

    csilk_mq_handler_t* global_middlewares; // Intercept every topic
    size_t global_mw_count;
    size_t global_mw_capacity;

    uv_file wal_fd;                // WAL file descriptor (-1 if disabled)
    char* wal_path;                // WAL file path
    uv_mutex_t wal_mutex;          // Guards WAL writes

    uint64_t published_total;      // Statistics
    uint64_t delivered_total;
    uint64_t failed_total;
    uint32_t queue_depth;

    csilk_ctx_t** monitors;        // WebSocket monitor connections
    size_t monitor_count;
    size_t monitor_capacity;
    uv_mutex_t monitor_mutex;
};
```

---

## 4. API Surface

### Publishing

| Function | Role |
|---|---|
| `csilk_mq_publish(mq, topic, payload, len)` | Deep-copy payload → optionally WAL-append → enqueue → signal main loop |

### Subscribing

| Function | Role |
|---|---|
| `csilk_mq_subscribe(mq, topic, handler)` | Register a subscriber for a topic |
| `csilk_mq_use(mq, topic, handler)` | Register middleware (nullptr topic = global) |

Both store the handler in the topic's handler array. The distinction is semantic — subscribers and middleware are mixed into the same chain (global first, then topic handlers in registration order).

### Chain Control (inside handler)

| Function | Role |
|---|---|
| `csilk_mq_next(ctx)` | Advance to next handler in the chain |
| `csilk_mq_abort(ctx)` | Short-circuit the entire chain |
| `csilk_mq_get_topic(ctx)` | Get the current message's topic string |
| `csilk_mq_get_payload(ctx, &len)` | Get the current message's payload pointer |

### Background Offload

| Function | Role |
|---|---|
| `csilk_mq_offload(ctx, worker)` | Deep-copy message → dispatch to thread pool → continue chain |

### Persistence

| Function | Role |
|---|---|
| `csilk_mq_set_persistence(mq, wal_path)` | Enable WAL — open file, recover past messages |
| `_mq_append_wal(mq, topic, payload, len)` | Internal: append frame with XOR checksum |
| `_mq_recovery(mq)` | Internal: read WAL frames, validate checksums, re-enqueue |

### Monitoring & Stats

| Function | Role |
|---|---|
| `csilk_mq_get_stats(mq, stats*)` | Read published/delivered/failed/queue_depth/topic_count |
| `csilk_mq_stats_to_json(stats*)` | Convert stats to a JSON string |
| `csilk_mq_register_monitor(mq, ctx)` | Register a WebSocket connection to receive real-time MQ events |
| `csilk_server_get_mq(server)` | Get or lazily create the server's MQ instance |

---

## 5. Dispatch Algorithm

```
on_mq_async(handle):
  1. Lock queue_mutex
  2. Detach entire queue (head → nullptr, tail → nullptr, save count)
  3. Unlock queue_mutex
  4. For each message (draining the linked list):

     a. Build handler chain:
        - Allocate chain[] = malloc((global_mw_count + matched_topic_handlers) * sizeof(handler_t))
        - Copy global_middlewares into chain[]
        - For each topic t in mq->topics:
            if fnmatch(t->name, msg->topic) == 0:
                memcpy chain[] with t->handlers[]

     b. Create context on stack:
        csilk_mq_ctx_t ctx = {mq, msg, chain, total_handlers, -1 /* index */, 0 /* aborted */}

     c. Start chain:
        csilk_mq_next(&ctx)
        // handler_index becomes 0 → calls chain[0](&ctx)
        // Each handler optionally calls csilk_mq_next(&ctx) to advance

     d. Free chain allocation
     e. Free message (topic, payload, msg struct)
```

### Topic Matching

Topic names support `fnmatch(3)` glob patterns. This means:

- `"user.*"` matches `"user.created"`, `"user.deleted"`, etc.
- `"*"` matches all topics
- `"events.*.completed"` matches `"events.order.completed"`

### Handler Chain Execution Order

```
For message on topic "events.user.created":

  1. Global middleware #0   → csilk_mq_next() advances to #1
  2. Global middleware #1   → csilk_mq_next() advances to #2
  ...
  N. Topic "events.*" middleware #0    (fnmatch match)
  M. Topic "events.user.*" middleware   (more specific match)
  O. Topic "events.user.created" subscriber #0
  P. Topic "events.user.created" subscriber #1
```

Any handler may call `csilk_mq_abort()` to stop the chain or `csilk_mq_offload()` to delegate processing to the thread pool.

---

## 6. WAL Persistence

### Frame Format

```
+0: topic_len   (uint32_t, 4 bytes, LE)
+4: topic       (topic_len bytes, NOT null-terminated on disk)
+N: payload_len (uint32_t, 4 bytes)
+M: payload     (payload_len bytes)
+K: checksum    (uint32_t, 4 bytes, XOR of topic + payload bytes)
```

**Overhead**: 12 bytes per message (2× uint32 length fields + 1× uint32 checksum).

### Write Path

```
csilk_mq_publish()
  ├── _mq_append_wal()     ← WAL write (under wal_mutex)
  │     ├── Compute XOR checksum over topic + payload bytes
  │     ├── uv_fs_write()  ← scatter/gather of 5 buffers
  │     └── uv_fs_fsync()  ← durability barrier
  └── _mq_enqueue()        ← in-memory queue
```

The WAL write is **synchronous** on the calling thread — it blocks until the fsync completes. This trades throughput for durability. For high-throughput scenarios, batching writes is an optimization opportunity.

### Recovery Path

```
csilk_mq_set_persistence()
  ├── Open/Create WAL file (O_CREAT | O_RDWR | O_APPEND)
  └── _mq_recovery()
        └── Sequential positional reads from offset 0:
              for each frame:
                1. Read topic_len (4 bytes)
                2. Read topic     (topic_len bytes)
                3. Read payload_len (4 bytes)
                4. Read payload     (payload_len bytes)
                5. Read stored_checksum (4 bytes)
                6. Compute XOR checksum over topic + payload
                7. If match → _mq_enqueue() (re-deliver)
                8. If mismatch → stop (corruption boundary)
```

On checksum mismatch, recovery **stops** rather than attempting to resync — without a frame-length prefix in the header, the next frame boundary cannot be determined. The WAL is not truncated after recovery; new appends continue at the current file offset.

### Integrity

The XOR checksum detects single-bit flips and many corruption patterns but is **not cryptographic**. It protects against accidental corruption (disk errors, partial writes), not malicious tampering.

---

## 7. Background Offload

```
csilk_mq_offload(ctx, worker_function):
  1. Allocate csilk_mq_work_ctx_t
  2. strdup topic, malloc+memcpy payload (deep copies)
  3. uv_queue_work(loop, &wctx->req, worker_cb, worker_after_cb)
     │                              │                │
     │  worker_cb()                 │                │
     │  └─ Runs on THREAD POOL      │                │
     │  └─ Calls user worker()      │                │
     │     with deep copies         │                │
     │                              │                │
     │              worker_after_cb()                │
     │              └─ Runs on MAIN LOOP             │
     │              └─ Frees deep copies + wctx      │
     │                                               │
     └── csilk_mq_next(ctx)  ← chain continues
                                immediately on main
                                loop thread
```

This enables fire-and-forget background processing without blocking the handler chain. The worker function receives exclusive ownership of the deep-copied topic and payload, and must NOT call back into the MQ or context APIs.

---

## 8. Monitoring

WebSocket clients can register as MQ monitors via `csilk_mq_register_monitor(mq, ctx)`. Monitors receive real-time JSON events:

| Event | Trigger | Fields |
|---|---|---|
| `mq_published` | Message enqueued | topic, payload_len, timestamp |
| `mq_delivered` | Message being dispatched | topic, payload_len, timestamp |

Events are broadcast as WebSocket text frames to all registered monitors under `monitor_mutex`. This powers the Admin Dashboard's real-time MQ monitoring page.

---

## 9. Concurrency Model

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
                    │  (linked list)   │
                    └────────┬─────────┘
                             │ uv_async_send()
                             ▼
                    ┌──────────────────┐
                    │  Main Loop       │
                    │  on_mq_async()   │
                    │  └─ dispatch()   │
                    └──────────────────┘
```

- **Publish**: Thread-safe via `queue_mutex`. Blocks only during linked-list append and optional WAL write.
- **Dispatch**: Single-threaded on the main event loop. No locks held during handler execution.
- **WAL**: Serialized by `wal_mutex`. Held for the duration of each `write + fsync`.
- **Stats**: Protected by `queue_mutex`. Updated atomically under the lock.
- **Monitors**: Protected by `monitor_mutex`. Broadcast occurs under the lock.

---

## 10. Lifecycle

```
Server startup:
  csilk_server_get_mq(server)
    └── _csilk_mq_new(loop)
          ├── calloc csilk_mq_t
          ├── uv_mutex_init(queue_mutex, monitor_mutex, wal_mutex)
          ├── uv_async_init(loop, async_handle, on_mq_async)
          └── wal_fd = -1

Runtime:
  csilk_mq_subscribe/use       → lazily create topics, register handlers
  csilk_mq_publish             → WAL-append + enqueue + async signal
  csilk_mq_set_persistence     → open WAL file + recovery replay
  csilk_mq_register_monitor    → add WebSocket client

Server shutdown:
  _csilk_mq_free(mq)
    └── uv_close(async_handle, on_mq_close)
          ├── uv_mutex_destroy(queue_mutex, wal_mutex)
          ├── Close WAL file
          ├── Drain message queue (free all messages)
          ├── Free all topics + handler arrays
          ├── Free global middlewares
          └── free(mq)
```

---

## 11. Usage Examples

### C — Basic pub/sub

```c
csilk_mq_t* mq = csilk_server_get_mq(server);

// Subscriber
void on_user_created(csilk_mq_ctx_t* ctx) {
    const char* topic = csilk_mq_get_topic(ctx);
    size_t len;
    const void* payload = csilk_mq_get_payload(ctx, &len);
    printf("Received on %s: %.*s\n", topic, (int)len, (char*)payload);
}
csilk_mq_subscribe(mq, "user.created", on_user_created);

// Publisher (from any thread)
csilk_mq_publish(mq, "user.created", "hello", 6);
```

### C — Middleware chain with abort

```c
void validator(csilk_mq_ctx_t* ctx) {
    size_t len;
    const char* data = (const char*)csilk_mq_get_payload(ctx, &len);
    if (len == 0) {
        csilk_mq_abort(ctx);  // reject empty messages
        return;
    }
    csilk_mq_next(ctx);
}

void logger(csilk_mq_ctx_t* ctx) {
    printf("Processing: %s\n", csilk_mq_get_topic(ctx));
    csilk_mq_next(ctx);
}

csilk_mq_use(mq, "orders.*", validator);   // global-like via pattern
csilk_mq_use(mq, "orders.*", logger);
csilk_mq_subscribe(mq, "orders.new", handler);
```

### C — Background offload

```c
void heavy_work(const char* topic, const void* payload, size_t len) {
    // Runs on thread pool
    process_data(payload, len);
}

void on_message(csilk_mq_ctx_t* ctx) {
    csilk_mq_offload(ctx, heavy_work);
    // Chain continues immediately; heavy_work runs in background
}
```

### C — WAL persistence

```c
csilk_mq_t* mq = csilk_server_get_mq(server);
csilk_mq_set_persistence(mq, "/var/lib/csilk/mq.wal");
// Past messages recovered from WAL, future messages durably stored
```

### Python (ctypes bindings)

```python
from csilk import App

app = App()
mq = app.mq

@mq.subscribe("events.user.created")
def handle_created(ctx):
    print(f"Got: {ctx.topic}")
    ctx.next()

@mq.use("*")  # global middleware
def log_all(ctx):
    print(f"Before: {ctx.topic}")
    ctx.next()

mq.publish("events.user.created", b"hello")
```

---

## 12. Related Files

| File | Role |
|---|---|
| `src/messaging/mq_core.c` | MQ creation, destruction, stats, monitor registration |
| `src/messaging/mq_dispatch.c` | Message enqueue, publish, async dispatch, chain execution |
| `src/messaging/mq_pubsub.c` | Subscribe, use (middleware), topic lazy-creation |
| `src/messaging/mq_context.c` | Per-message context: next, abort, get_topic, get_payload |
| `src/messaging/mq_offload.c` | Background thread-pool offload |
| `src/messaging/mq_wal.c` | WAL append, recovery, set_persistence |
| `src/messaging/mq_internal.h` | Internal struct definitions and cross-file helpers |
| `include/csilk/mq.h` | Public API for pub/sub, offload, persistence, monitoring |
| `src/messaging/internal/` | Additional internal helpers |
| `python/csilk/app.py` | Python MqContext and MQ binding classes |

---

## 13. Design Rationale

**Why linked-list for topics instead of a hash table?**  
The topic registry is small (typically dozens, not thousands of topics). Linked-list iteration is cache-friendly for the dispatch hot path (which scans all topics for fnmatch matching), and the implementation is trivially simple with no resizing or collision complexity.

**Why fnmatch instead of exact string matching?**  
Glob patterns enable hierarchical topic namespaces (e.g., `"events.*"`, `"orders.*.completed"`), which is essential for middleware that applies to broad categories of messages. The performance cost of `fnmatch` is negligible in the in-process context.

**Why WAL instead of an in-memory-only design?**  
The WAL is optional. In-memory mode has zero persistence overhead. The WAL is there for scenarios where message delivery must survive a process restart — it writes durably before enqueueing, so a crash between enqueue and dispatch does not lose the message.

**Why deep-copy on publish?**  
The caller owns the payload buffer and may free it immediately after `csilk_mq_publish()` returns. Deep-copying ensures the MQ owns its data and no use-after-free bugs can occur, at the cost of one extra allocation per publish.
