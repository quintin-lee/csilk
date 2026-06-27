# Protocols Module

## 1. Overview

The Protocols module extends the HTTP/1.1 server with real-time, bidirectional, and interactive capabilities. It provides **WebSocket** (RFC 6455), **Server-Sent Events** (SSE, RFC 8895), **Swagger/OpenAPI UI**, and a **WebSocket Room** system for multi-client broadcast.

**Files**: `src/protocols/websocket.c`, `src/protocols/swagger.c`, `src/protocols/ws_room.c`, `src/middleware/sse.c`

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    HTTP Request Pipeline                      │
│                                                              │
│  ┌─────────────────────┐    ┌───────────────────────────┐   │
│  │ WebSocket            │    │ SSE                       │   │
│  │ ┌──────────────────┐ │    │ ┌────────────────────────┐│   │
│  │ │ csilk_ws_handshake│ │    │ │ csilk_sse_init()       ││   │
│  │ │ (101 Upgrade)     │ │    │ │ (200 + text/event-     ││   │
│  │ │ → is_websocket=1  │ │    │ │  stream headers)       ││   │
│  │ └────────┬─────────┘ │    │ │ → is_sse=1             ││   │
│  │          │           │    │ └───────────┬────────────┘│   │
│  │  ┌───────▼────────┐  │    │             │             │   │
│  │  │ Frame I/O       │  │    │  ┌──────────▼──────────┐ │   │
│  │  │ csilk_ws_send() │  │    │  │ csilk_sse_send()    │ │   │
│  │  │ csilk_ws_parse  │  │    │  │ csilk_sse_close()   │ │   │
│  │  │  _frame()       │  │    │  └─────────────────────┘ │   │
│  │  │ csilk_ws_close()│  │    │                           │   │
│  │  └───────┬────────┘  │    └───────────────────────────┘   │
│  │          │           │                                     │
│  │  ┌───────▼────────┐  │    ┌───────────────────────────┐   │
│  │  │ WS Room         │  │    │ Swagger UI                │   │
│  │  │ csilk_ws_join   │  │    │ csilk_serve_swagger_ui()  │   │
│  │  │  _room()        │  │    │ (embedded HTML + JS)      │   │
│  │  │ csilk_ws_leave  │  │    │ loads /openapi.json       │   │
│  │  │  _room()        │  │    └───────────────────────────┘   │
│  │  │ csilk_ws_broad  │  │                                     │
│  │  │  cast_room()    │  │                                     │
│  │  │ (backed by MQ)  │  │                                     │
│  │  └─────────────────┘  │                                     │
│  └───────────────────────┘                                     │
└─────────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **Protocols hijack the HTTP socket** — Both WebSocket and SSE bypass the normal HTTP response pipeline after the initial handshake. They write directly to the underlying libuv TCP/TLS stream, taking ownership of the connection for the remainder of its lifetime.

2. **WebSocket Room via MQ** — The room broadcasting system leverages the internal Message Queue (MQ) as a pub/sub event bus. Each room maps to an MQ topic (`ws.room.<name>`), decoupling broadcasters from delivery.

3. **Direct socket writes** — All SSE and WebSocket frame writes use `uv_write()` directly on the client stream, bypassing csilk's normal response writer. This avoids buffering delays and enables real-time throughput.

---

## 3. WebSocket (RFC 6455)

### 3.1 Handshake

```
Client → Server:  GET /ws HTTP/1.1
                   Upgrade: websocket
                   Connection: Upgrade
                   Sec-WebSocket-Key: <base64-random-16-bytes>
                   Sec-WebSocket-Version: 13

Server → Client:  HTTP/1.1 101 Switching Protocols
                   Upgrade: websocket
                   Connection: Upgrade
                   Sec-WebSocket-Accept: <base64(sha1(key + GUID))>
```

The handshake is performed by `csilk_ws_handshake()`:

```
1. Read Sec-WebSocket-Key header → 400 if missing
2. Compute SHA-1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
3. Base64-encode the 20-byte digest → Sec-WebSocket-Accept
4. Set response headers (Upgrade, Connection, Accept)
5. Set status to 101 (Switching Protocols)
6. Mark ctx->is_websocket = 1
```

After the handshake, the function returns and the handler is expected to enter a read loop driven by libuv's `on_read` callback.

### 3.2 Frame I/O

**Sending** (`csilk_ws_send`):

```
csilk_ws_send(ctx, data, len, opcode):
  1. Determine mask (0 for server→client frames; client→server is masked)
  2. Build frame header:
     Byte 0: FIN | opcode (e.g., 0x81 = text frame, 0x82 = binary)
     Byte 1: MASK | payload_len (7, 16, or 64 bits depending on length)
     Bytes 2+: Extended length (if applicable)
     Bytes N+: Masking key (if applicable)
     Bytes K+: Payload
  3. uv_write() on the client stream (async)
  4. Write completion callback frees the request + buffer
```

**Receiving** (`csilk_ws_parse_frame`):

```
Called from on_read() when ctx->is_websocket is set:
  1. Parse frame header (FIN, opcode, MASK, length)
  2. If masked: unmask payload with XOR key
  3. Dispatch by opcode:
     - 0x1 (text)   → invoke user on_message callback
     - 0x8 (close)  → send close frame back, close connection
     - 0x9 (ping)   → send pong frame
     - 0xA (pong)   → no-op
```

**Close** (`csilk_ws_close`):

Sends a close frame (opcode 0x8) with optional status code and reason. Per RFC 6455, the close handshake is bidirectional — the initiator sends close, the peer must respond with its own close frame, after which the TCP connection may be closed.

### 3.3 API

| Function | Role |
|---|---|
| `csilk_ws_handshake(ctx)` | Perform HTTP→WS upgrade (101) |
| `csilk_ws_send(ctx, data, len, opcode)` | Send a WebSocket data frame (text/binary) |
| `csilk_ws_parse_frame(ctx, data, len)` | Parse and dispatch an incoming frame |
| `csilk_ws_close(ctx, status, reason)` | Initiate close handshake |
| `csilk_ctx_set_websocket(ctx, 1)` | Mark context as WebSocket mode |

### 3.4 Callback Registration

The user registers an on_message callback via the context API:

```c
void on_ws_message(csilk_ctx_t* ctx, const uint8_t* data, size_t len, int opcode) {
    // Handle received message
    csilk_ws_send(ctx, data, len, 0x1); // echo back as text
}

void ws_handler(csilk_ctx_t* ctx) {
    csilk_ws_handshake(ctx);
    ctx->on_ws_message = on_ws_message;
}
```

The callback is invoked from `csilk_ws_parse_frame()` on the event loop thread whenever a complete text or binary frame is received.

---

## 4. WebSocket Rooms

**File**: `src/protocols/ws_room.c`

### Architecture

```
┌─────────────────────┐     MQ topic "ws.room.chat"     ┌─────────────────────┐
│  Publisher          │ ──────────────────────────────→  │  Room "chat"         │
│  (any handler)      │                                   │  ┌────────────────┐ │
│                     │                                   │  │ Client A (WS)  │ │
│                     │                                   │  │ Client B (WS)  │ │
│                     │                                   │  │ Client C (WS)  │ │
│                     │                                   │  └────────────────┘ │
│                     │                                   │  MQ subscriber:    │
│                     │                                   │  on_room_message() │
│                     │                                   │  → ws_send all     │
└─────────────────────┘                                   └─────────────────────┘
```

### API

| Function | Role |
|---|---|
| `csilk_ws_join_room(ctx, room_name)` | Add client to room, create room + MQ subscriber if new |
| `csilk_ws_leave_room(ctx, room_name)` | Remove client from room (O(1) swap-to-end) |
| `csilk_ws_broadcast_room(ctx, room_name, message)` | Publish to room via MQ topic `ws.room.<name>` |

### Room Manager

```c
ws_room_manager_t:
  ├── rooms[] → ws_room_t:
  │               ├── room_name (strdup'd)
  │               ├── clients[] (csilk_ctx_t* pointers)
  │               ├── clients_count / clients_capacity
  └── mutex (uv_mutex_t, serializes all room ops)
```

- Rooms are created lazily on first `csilk_ws_join_room()` call.
- Each room creates an MQ subscriber on topic `ws.room.<name>` that broadcasts to all clients in the room.
- Client removal uses swap-to-end for O(1) deletion.
- Thread-safe via `g_room_manager.mutex`.

### Broadcast Flow

```
csilk_ws_broadcast_room(ctx, "chat", "hello"):
  1. csilk_mq_publish(mq, "ws.room.chat", "hello", 5)
  2. MQ dispatch → on_room_message(mq_ctx):
     a. Extract room name from topic (skip "ws.room." prefix)
     b. Lock room manager mutex
     c. Find room by name
     d. For each client: csilk_ws_send(client, payload, len, 0x1)
     e. Unlock room manager mutex
```

---

## 5. Server-Sent Events (SSE, RFC 8895)

**File**: `src/middleware/sse.c`

### Connection Flow

```
csilk_sse_init(ctx):
  1. Set headers: Content-Type: text/event-stream
                   Cache-Control: no-cache
                   Connection: keep-alive
                   X-Accel-Buffering: no
  2. Mark ctx->is_sse = 1
  3. Write raw HTTP 200 OK response headers + above headers directly
     to client socket via uv_write()
```

The SSE handler does NOT call `csilk_next()` — it hijacks the socket for the connection lifetime.

### Event Format

```
event: update\n
data: {"key":"value"}\n
\n
```

Per RFC 8895 §2, each event consists of optional `event:` and `data:` fields terminated by a blank line.

### API

| Function | Role |
|---|---|
| `csilk_sse_init(ctx)` | Initialize SSE connection, write headers |
| `csilk_sse_send(ctx, event, data)` | Send an SSE event (optional event type) |
| `csilk_sse_close(ctx)` | Close the SSE connection via uv_close() |

### Graceful Shutdown

During server shutdown, `close_active_clients()` in `server.c` detects SSE connections via `ctx->is_sse` and sends a close event before closing:

```c
if (client->ctx.is_sse) {
    csilk_sse_send(&client->ctx, "close", "Server stopping");
    csilk_sse_close(&client->ctx);
}
```

---

## 6. Swagger / OpenAPI UI

**File**: `src/protocols/swagger.c`

### Implementation

`csilk_serve_swagger_ui()` serves an embedded HTML page containing the Swagger UI distribution (compiled into the binary as a string constant). The page loads `/openapi.json` at runtime from the same server to render interactive API documentation.

```
csilk_serve_swagger_ui(ctx):
  1. Set Content-Type: text/html; charset=utf-8
  2. csilk_string(ctx, 200, swagger_ui_html)
```

### OpenAPI Spec Generation

The OpenAPI specification (`/openapi.json`) is generated dynamically from route metadata. Routes registered with `csilk_app_add_route_extended_perm()` or Python's `app.route()` with `summary`/`description`/`input_type`/`output_type` parameters automatically populate the OpenAPI spec. The JSON is served by a built-in handler registered alongside the Swagger UI.

### Registration

In the App layer, Swagger UI and OpenAPI are registered automatically:

```c
// In app.c, during app creation:
csilk_app_add_route(app, "GET", "/swagger", swagger_handler, 1);
csilk_app_add_route(app, "GET", "/openapi.json", openapi_handler, 1);
```

---

## 7. read() Integration

WebSocket frame parsing is integrated into the core connection read path:

```
on_read(stream, nread, buf):
  if ctx->is_websocket:
      csilk_ws_parse_frame(&client->ctx, base, nread)
  else:
      llhttp_execute(...)    ← normal HTTP parsing
```

When `ctx->is_websocket` is set (after handshake), incoming TCP data is routed directly to the WebSocket frame parser instead of the HTTP parser. This switch happens at the libuv read callback level, with no intermediate buffering.

---

## 8. Concurrency

- **WebSocket I/O** — All frame writes and reads happen on the event loop thread. No concurrent access to the client stream.
- **Rooms** — Protected by `g_room_manager.mutex`. Room operations (join/leave) and broadcasts serialize through the same lock. The actual broadcast work (ws_send) is done under the lock.
- **SSE** — Single-threaded on the event loop. Each `sse_send()` allocates, writes, and frees on the same thread.
- **Swagger** — Stateless handler serving static HTML. No concurrency concerns.

---

## 9. Related Files

| File | Role |
|---|---|
| `src/protocols/websocket.c` | WebSocket handshake, frame send/parse/close |
| `src/protocols/ws_room.c` | Room management with MQ-backed broadcast |
| `src/protocols/swagger.c` | Embedded Swagger UI HTML serving |
| `src/middleware/sse.c` | SSE init/send/close over raw libuv stream |
| `src/core/connection.c` | on_read() routing to WS parser when is_websocket |
| `src/core/server.c` | Graceful shutdown of WS/SSE connections |
| `src/core/context.c` | ctx->is_websocket / is_sse flags and setters |
| `include/csilk/csilk.h` | Public WS/SSE API declarations |
| `python/csilk/context.py` | Python SSE bindings (sse_init, sse_send, sse_close) |

---

## 10. Design Rationale

**Why raw socket writes instead of the normal response pipeline for SSE/WS?**  
SSE and WebSocket connections are long-lived and require streaming writes without buffering. The normal csilk response pipeline buffers the full response body before flushing, which is incompatible with real-time protocols. Direct `uv_write()` calls bypass this buffering and give the protocol code full control over frame timing.

**Why MQ for WebSocket rooms instead of direct client lists?**  
Using the MQ decouples the broadcaster from delivery. Any component (HTTP handler, AI workflow, scheduled job) can broadcast to a room by publishing an MQ message — it doesn't need access to the room's client list or the WebSocket infrastructure. The MQ also enables room communication across worker threads through a single dispatch point on the main loop.

**Why embedded Swagger UI HTML?**  
Serving Swagger UI from an embedded string eliminates a filesystem dependency and simplifies deployment — the server binary is self-contained with no static assets to distribute alongside it.
