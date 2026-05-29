# HTTP/2 Integration Pre-study

## 1. Overview
HTTP/2 (RFC 7540) introduces binary framing, multiplexing, and header compression (HPACK). Integrating HTTP/2 into `csilk` requires significant changes to the current connection model.

## 2. Key Challenges

### 2.1 Binary Framing & Multiplexing
- **Current Model**: One TCP connection = One HTTP/1.1 request/response cycle (or sequential keep-alive).
- **HTTP/2 Model**: One TCP connection = Multiple concurrent bidirectional streams.
- **Impact**: `csilk_client_t` must be decoupled from `csilk_ctx_t`. A single client will own multiple contexts, one per stream.

### 2.2 Protocol Negotiation (ALPN)
- HTTP/2 over TLS requires ALPN.
- **Impact**: `src/core/server.c` needs to configure ALPN via OpenSSL and detect the protocol after handshake.

### 2.3 Frame Parsing
- `llhttp` does not support HTTP/2.
- **Recommendation**: Integrate `nghttp2` library. It provides a state-of-the-art H2 frame parser and generator.

### 2.4 Header Compression (HPACK)
- HPACK is complex to implement from scratch.
- **Recommendation**: Use `nghttp2`'s built-in HPACK support.

## 3. Proposed Architecture

### 3.1 Connection Dispatcher
A new layer between `on_read` and the request handler:
- If ALPN = `h2`, route data to H2 frame parser.
- If ALPN = `http/1.1` or no ALPN, route to `llhttp`.

### 3.2 Stream Mapping
- Each H2 stream ID will be mapped to a `csilk_ctx_t`.
- `csilk_ctx_t` needs to support writing frames back to the shared TCP socket with the correct stream ID.

### 3.3 Flow Control & Priority
- Implementation of H2 window updates and stream priorities.

## 4. Integration Cost Assessment
- **Complexity**: High. Requires refactoring the client/context relationship.
- **Dependencies**: New dependency on `nghttp2`.
- **Performance**: Significant improvement for high-latency connections and multiplexed assets.
- **Estimate**: 3-4 man-weeks for a stable implementation.

## 5. Conclusion
HTTP/2 is highly desirable for modern web performance. The recommended path is integrating `nghttp2` and refactoring `csilk_server_t` to handle multiple contexts per client.
