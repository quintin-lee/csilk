# HTTP/2 Integration — Implementation Status

> **Status**: Phase 1 (Session scaffolding) and Phase 2 (Request dispatch and response) complete.  
> **Version**: v0.3.0+ | **Last updated**: 2026-05-30

## 1. Overview
HTTP/2 (RFC 7540) introduces binary framing, multiplexing, and header compression (HPACK). The csilk framework has integrated `nghttp2` for frame parsing and generation.

## 2. Implementation Status

### Phase 1 — Session Scaffolding ✅
- **ALPN negotiation**: `src/core/server.c` configures OpenSSL ALPN to offer `h2` and `http/1.1`. After TLS handshake, `alpn_select_cb` detects the negotiated protocol and sets `client->protocol`.
- **nghttp2 session**: `csilk_h2_init_session()` creates an nghttp2 session in server mode, registers callbacks (`on_header`, `on_frame_recv`, `on_data_chunk_recv`, `on_stream_close`, `send_callback`), and configures standard HTTP/2 settings (max concurrent streams, initial window size).
- **Data routing**: After ALPN negotiation, `process_tls_read()` routes decrypted data to `csilk_h2_process_data()` for HTTP/2 connections, or to `llhttp` for HTTP/1.1.
- **`csilk_h2.h` public API**: Exposes `csilk_h2_init_session`, `csilk_h2_process_data`, `csilk_h2_get_or_create_stream`, `csilk_h2_free_streams`.

### Phase 2 — Request Dispatch and Response ✅
- **Unified dispatch**: Extracted `_csilk_dispatch_request()` from the HTTP/1.1 `on_message_complete` handler. Both protocols now share the same routing, middleware chain, and hook-triggering logic.
- **Header parsing** (`on_header_callback`): Parses `:method`, `:path` pseudo-headers and regular headers into arena-backed `csilk_request_t` fields (method, path, query_params, headers).
- **Frame completion** (`on_frame_recv_callback`): When `NGHTTP2_FLAG_END_STREAM` is set on HEADERS or DATA frames, the request is dispatched via `_csilk_dispatch_request()`.
- **Body accumulation** (`on_data_chunk_recv_callback`): Incoming DATA frame payloads are concatenated into `c->request.body` (heap-reallocated, up to `max_body_size`).
- **Stream cleanup** (`on_stream_close_callback`): Removes the stream context from the linked list, calls `csilk_ctx_cleanup()`, frees the arena, and releases the context struct.
- **Response sending** (`csilk_h2_send_response`): Builds an nghttp2 HEADERS frame with `:status` pseudo-header and response headers, then sends body DATA frames via a `nghttp2_data_provider` callback (`body_read_callback`).

### Remaining Work (Future Phases)
- **Flow control**: Window update management for stream/connection-level flow control.
- **Stream priority**: Dependency tree scheduling.
- **Server push**: `PUSH_PROMISE` frame support.
- **HPACK dynamic table**: nghttp2 handles this internally, but tuning table sizes may improve compression.
- **Connection prefaces and SETTINGS**: Currently using nghttp2 defaults — may need customisation.

## 3. Architecture

### 3.1 Connection Dispatcher
A new layer between `on_read` and the request handler:
- If ALPN = `h2`, route data to `csilk_h2_process_data()` (nghttp2 frame parser).
- If ALPN = `http/1.1` or no ALPN, route to `llhttp`.

### 3.2 Stream Mapping
- Each H2 stream ID is mapped to a `csilk_ctx_t` via `csilk_h2_get_or_create_stream()`.
- Stream contexts are stored in a singly-linked list on `csilk_client_t::h2_streams`.
- Multiple streams can be active concurrently on one TCP connection.

## 4. Dependencies
- **nghttp2** (v1.52+): Frame parsing, HPACK, session management.
- **OpenSSL**: TLS 1.3 with ALPN extension.

## 5. Performance Notes
- nghttp2 handles HPACK internally with a dynamic table; no manual header compression needed.
- Stream contexts use the same arena allocator model as HTTP/1.1 contexts for zero-fragmentation memory management.
- The unified `_csilk_dispatch_request` ensures both protocols share the same optimised radix-tree router and middleware chain.
