# Server Configuration Isolation & Data Race Audit Spec

## 1. Audit & Problem Statement

### 1.1 Data Race Analysis on `server->config`
When `csilk_server_set_config(server, &config)` is called during runtime:
- `server->config = *config` performs a non-atomic multi-word copy.
- Worker threads concurrently read `client->server->config.max_body_size`, `config.read_timeout_ms`, `config.max_header_size`, etc. on incoming requests.
- **Risk**: Word tearing (partial 64-bit updates), inconsistent multi-field state, and data race flagged by TSAN.

### 1.2 Configuration Classification

| Classification | Fields | Lifecycle | Synchronization |
|---|---|---|---|
| **Startup-Only (Immutable)** | `worker_threads`, `listen_backlog`, `enable_tls`, `tls_cert_file`, `tls_key_file`, `tls_ca_file`, `tls_verify_peer`, `enable_arena_alignment`, `enable_openapi`, `enable_uring_sqpoll`, `tcp_nodelay`, `tcp_keepalive` | Initialized in `csilk_server_run()` / worker init | Read-only during runtime |
| **Runtime-Mutable (Dynamic)** | `idle_timeout_ms`, `read_timeout_ms`, `write_timeout_ms`, `request_timeout_ms`, `max_body_size`, `max_header_size`, `max_url_size`, `max_headers_count`, `max_connections`, `enable_simd`, `h2_push_enable`, `h2_max_push_per_request`, `backpressure_max_queue_depth`, `backpressure_max_latency_us` | Modified via `csilk_server_set_config()` / `csilk_server_set_max_connections()` | `_Atomic` + `memory_order_relaxed` |

---

## 2. Architecture & Design

### 2.1 `csilk_runtime_config_t` Structure
```c
typedef struct csilk_runtime_config_s {
    _Atomic(unsigned int) idle_timeout_ms;
    _Atomic(unsigned int) read_timeout_ms;
    _Atomic(unsigned int) write_timeout_ms;
    _Atomic(unsigned int) request_timeout_ms;
    _Atomic(size_t)       max_body_size;
    _Atomic(size_t)       max_header_size;
    _Atomic(size_t)       max_url_size;
    _Atomic(size_t)       max_headers_count;
    _Atomic(int)          max_connections;
    _Atomic(int)          enable_simd;
    _Atomic(int)          h2_push_enable;
    _Atomic(int)          h2_max_push_per_request;
    _Atomic(size_t)       backpressure_max_queue_depth;
    _Atomic(unsigned int) backpressure_max_latency_us;
} csilk_runtime_config_t;
```

### 2.2 Inline Hot-Path Accessors (`srv_impl.h`)
- `csilk_server_get_max_body_size(server)`
- `csilk_server_get_max_header_size(server)`
- `csilk_server_get_max_url_size(server)`
- `csilk_server_get_max_headers_count(server)`
- `csilk_server_get_idle_timeout_ms(server)`
- `csilk_server_get_read_timeout_ms(server)`
- `csilk_server_get_write_timeout_ms(server)`
- `csilk_server_get_request_timeout_ms(server)`
- `csilk_server_get_enable_simd(server)`
- `csilk_server_get_h2_push_enable(server)`
- `csilk_server_get_h2_max_push(server)`

### 2.3 Atomic Publishing in `csilk_server_set_config()`
Populates `server->runtime_config` atomically using `atomic_store_explicit(..., memory_order_relaxed)`.

---

## 3. Verification & Benchmark
- Unit tests validating runtime config modifications while threads are reading.
- TSAN clean execution.
- 0 performance overhead on request hot paths.
