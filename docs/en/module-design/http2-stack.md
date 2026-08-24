# HTTP/2 Protocol Stack Deep Dive

> **Version**: 0.5.2 | **Last updated**: 2026-08-24

csilk integrates the high-performance `nghttp2` library to provide native HTTP/2 capabilities, including connection multiplexing, binary framing, HPACK header compression, and flow control. This document details session management, stream hash-mapping, and lifecycle validation.

---

## 1. Core HTTP/2 Features

| Feature | Description |
|---------|-------------|
| **Multiplexing** | Concurrent request/response interleaving across a single TCP connection |
| **Binary Framing** | All protocol messages formatted as structured binary frames |
| **Header Compression** | HPACK table compression for request and response headers |
| **Server Push** | Proactive server push to preload client resources |
| **Stream Priorities** | Priority trees and weight allocation for stream scheduling |
| **Flow Control** | Dual-layer (connection-level and stream-level) backpressure |

---

## 2. Architecture Overview

```mermaid
%%{init: {'theme': 'base', 'themeVariables': {'background': '#2E3440','primaryColor':'#81A1C1','primaryBorderColor':'#4C566A','primaryTextColor': '#ECEFF4','secondaryColor': '#3B4252','secondaryBorderColor': '#434C5E','secondaryTextColor': '#D8DEE9','lineColor': '#81A1C1','textColor': '#ECEFF4','mainBkg': '#3B4252','nodeBorder': '#4C566A','clusterBkg': '#2E3440','clusterBorder': '#4C566A','titleColor': '#ECEFF4','edgeLabelBackground': '#3B4252','nodeTextColor': '#ECEFF4'}, 'flowchart': {'htmlLabels': true, 'curve': 'basis'}}}%%
graph TB
    subgraph connection["fa:fa-link TCP Connection"]
        TLS["fa:fa-lock TLS 1.3<br/>ALPN: h2"]
        FRAMES["fa:fa-layer-group Frame Layer<br/>HTTP/2 Frames"]
    end
    
    subgraph session["fa:fa-circle-h HTTP/2 Session"]
        NGHTTP2["fa:fa-cogs nghttp2_session<br/>State Engine"]
        STREAMS["fa:fa-stream HTTP/2 Streams<br/>0-2^31-1"]
        SETTINGS["fa:fa-sliders Settings<br/>WINDOW_SIZE, MAX_CONCURRENT"]
    end
    
    subgraph flow_control["fa:fa-tachometer-alt Flow Control"]
        CONNECTION["fa:fa-arrows-alt Connection Window<br/>65535 default"]
        STREAM["fa:fa-arrows-alt Stream Window<br/>per-stream"]
    end
    
    TLS --> FRAMES
    FRAMES --> NGHTTP2
    NGHTTP2 --> STREAMS
    STREAMS --> CONNECTION & SETTINGS
    
    CONNECTION --> flow_control
    STREAMS --> flow_control
```

---

## 3. Session & Stream Mapping

### 3.1 Stream Map Structure (csilk_h2_stream_map_t)

```c
#define CSILK_H2_INLINE_BUCKETS 16
#define CSILK_H2_STREAM_POOL_MAX 64

/**
 * @brief Adaptive HTTP/2 stream hash map and context recycling pool.
 */
typedef struct csilk_h2_stream_map_s {
    csilk_ctx_t** buckets;  /**< Active bucket array (points to inline or heap buffer) */
    uint32_t      capacity; /**< Bucket capacity (power of two) */
    uint32_t      mask;     /**< Fast bitwise AND mask (capacity - 1) */
    uint32_t      count;    /**< Active concurrent streams count */
    csilk_ctx_t*  inline_buckets[CSILK_H2_INLINE_BUCKETS]; /**< Fast-path 16 inline slots */
    csilk_ctx_t*  free_list;  /**< Linked list of reset, cached contexts */
    uint32_t      pool_count; /**< Cached context count in connection pool */
    uint32_t      pool_max;   /**< Pool capacity limit (default 64) */
} csilk_h2_stream_map_t;
```

HTTP/2 stream contexts (`csilk_ctx_t`) are managed directly inside the client's `h2_stream_map`. Each stream possesses an isolated request arena that resets and returns to `free_list` upon stream closure, eliminating per-stream `malloc`/`free` overhead.

---

## 4. Flow Control & Backpressure

```c
#define H2_DEFAULT_WINDOW_SIZE 65535
#define H2_MAX_WINDOW_SIZE     0x7FFFFFFF  // 2^31 - 1
```

- **Window Sizing**: Connection and stream windows are dynamically replenished with `WINDOW_UPDATE` frames.
- **Write Backpressure**: DATA frames exceeding remaining stream or connection credits pause output and register drain watchers with libuv / io_uring.

---

## 5. Formal Verification & Stress Testing

`tests/protocols/test_h2_stream_lifecycle.c` formalizes stream safety:
- **RST_STREAM Mid-flight Destruction**: Proves that reset streams immediately invalidate in-flight callbacks with 0 UAF.
- **GOAWAY Graceful Draining**: Proves pending streams drain completely while new stream attempts are rejected.
- **10,000 Stream Recycling**: Proves high-throughput stream allocation, fast-path lookup, and `free_list` recycling under 100% zero-leak conditions.
- **Cascading Connection Teardown**: Confirms `csilk_h2_free_streams()` safely reclaims all child streams and context arenas when TCP connections close.

---

## 6. Source Files

| File | Purpose |
|------|---------|
| `src/core/http/h2.c` | HTTP/2 entry point, nghttp2 callbacks, frame routing |
| `src/core/http/h2.h` | `csilk_h2_stream_map_t` and stream manipulation APIs |
| `tests/protocols/test_h2_stream_lifecycle.c` | RST_STREAM / GOAWAY / 10k stream recycling formal audit |
| `docs/design/http2.md` | Architecture design specification |
