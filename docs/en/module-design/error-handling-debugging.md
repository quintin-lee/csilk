# Error Handling & Debugging Guide

> **Version**: 0.5.1 | **Last updated**: 2026-08-22

This guide details csilk's unified error return codes, structured asynchronous logging subsystem, deferred RAII cleanup, and diagnostics tooling.

---

## 1. Error Code Taxonomy

```c
typedef enum csilk_ret_e {
    CSILK_OK = 0,           // Success
    CSILK_ERR = -1,         // Generic internal error
    CSILK_ERR_MEMORY = -2,  // Memory allocation failure
    CSILK_ERR_INVALID = -3, // Invalid argument
    CSILK_ERR_NOT_FOUND = -4, // Target resource not found
    CSILK_ERR_BUSY = -5,    // Resource locked / busy
    CSILK_ERR_TIMEOUT = -6, // Operation timed out
    CSILK_ERR_IO = -7,      // I/O descriptor failure
    CSILK_ERR_TLS = -8,     // TLS / SSL handshake error
    CSILK_ERR_JSON = -9,    // JSON serialization / parse error
    CSILK_ERR_MQ = -10,     // Message queue error
} csilk_ret_t;
```

---

## 2. Asynchronous Logging Subsystem

- **Lock-Free Ring Buffer Queue**: Log emissions format records into a thread-safe MPSC queue with zero lock contention on the I/O event loop.
- **Dedicated Drain Thread**: Dedicated background worker flushes entries to `stdout`, `stderr`, or rotating log files with JSON or text formatting.

---

## 3. RAII Deferred Cleanup (csilk_ctx_defer)

```c
void csilk_ctx_defer(csilk_ctx_t* c, csilk_defer_fn fn, void* arg);
```

Allows handlers and middleware to register LIFO destructors executed automatically when the request arena resets.

---

## 4. Diagnostics & Live Profiling

- `/admin/debug/stats`: Real-time worker connection counts, queue depth, and memory RSS.
- `/admin/debug/flamegraph`: On-demand non-blocking stack sampling and SVG flamegraph generation.
- `/metrics`: Standard Prometheus metrics exporter.
