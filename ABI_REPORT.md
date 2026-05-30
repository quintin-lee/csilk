# ABI Stability Assessment Report

> Generated: 2026-05-28 | Assessment of `csilk_ctx_s` opaque type conversion (PLAN.md P2-1)

## Summary

**Verdict: LOW PRIORITY — Current design is adequate for the project stage.**

Making `csilk_ctx_s` fully opaque would require significant refactoring of
internal code, tests, and examples for marginal benefit at this point. It
should be deferred until a v1.0 stable release is planned.

---

## Current State

### Public API (`include/csilk.h:83`)
```c
typedef struct csilk_ctx_s csilk_ctx_t;
```
Only a forward declaration — user code cannot directly instantiate or
dereference the struct without including `context_internal.h`.

### Full Definition (`include/csilk/core/context_internal.h:107-218`)
The struct has 30+ fields across 9 functional groups:
1.  Handler chain state (handler_index, handlers, aborted)
2.  Error recovery (jump_buffer, has_jump_buffer)
3.  Memory management (arena)
4.  Request data (request — method, path, headers, body, query, form)
5.  Response data (response — status, headers, body, sent)
6.  URL path parameters (params[CSILK_MAX_PARAMS], params_count)
7.  Protocol mode flags (is_websocket, is_sse)
8.  Callbacks (on_ws_message, on_close)
9.  Driver pointers, sendfile state, tracing UUID

### Who includes `context_internal.h`?

| Consumer | Count | Notes |
|---|---|---|
| Framework source (`src/`) | ~10 files | Expected — internal implementation |
| Tests (`tests/`) | 30+ files | Direct field access for testing |
| Examples (`examples/`) | 3 files | c->request.method/path/status, c->is_websocket |

---

## Direct Struct Field Access (Non-Framework Code)

### Tests accessing `c->response.*` directly:
- `test_redirect.c` — `c->response.headers.buckets`
- `test_gzip.c` — `c->response.body`, `.body_len`, `.body_is_managed`, `.status`
- `test_logger.c` — `c->response.status`
- `test_session_ext.c` — `c->request.headers.buckets`, `c->arena`

### Examples accessing struct fields directly:
- `advanced_server.c:91` — `c->is_websocket` check in handler
- `example_app.c:381` — `c->request.method`, `c->request.path`, `c->response.status`

---

## Impact Analysis

### If `csilk_ctx_s` were made fully opaque:

**Breakages**:
| File | Field accessed |
|---|---|
| `test_redirect.c` | `c->response.headers.buckets` |
| `test_gzip.c` | `c->response.body`, `.body_len`, `.status` |
| `test_logger.c` | `c->response.status` |
| `test_session_ext.c` | `c->request.headers`, `c->arena` |
| `examples/advanced_server.c` | `c->is_websocket` |
| `examples/example_app.c` | `c->request.method/path`, `c->response.status` |

**New accessor APIs needed**:
- `csilk_ctx_get_method(c)` → returns method string
- `csilk_ctx_get_path(c)` → returns path string
- `csilk_ctx_get_status(c)` → returns status code
- `csilk_ctx_set_status(c, status)` → sets status code
- `csilk_ctx_set_body(c, data, len)` → sets response body
- `csilk_ctx_is_websocket(c)` → returns int
- `csilk_ctx_get_arena(c)` → returns arena pointer
- `csilk_request_get_header(c, key)` → returns header value
- `csilk_response_set_header(c, key, value)` → sets response header

**Other considerations**:
- `csilk_client_t` (defined in `context_internal.h`) is also exposed
- `csilk_request_t`, `csilk_response_t`, `csilk_param_t` types are part
  of the internal layout and would need separate public headers or
  opaque accessor APIs
- SSE/WebSocket callback function pointers stored directly in struct

---

## Recommendation

### Defer opaque conversion until v1.0 preparation

**Rationale**:
1. **Project maturity**: csilk is pre-1.0 (current: v0.3.0). ABI stability
   is not a contractual promise at this stage.
2. **Test dependency**: 30+ test files include `context_internal.h`. Moving
   them to pure API-based testing is a separate project.
3. **Low user impact**: External users receive `csilk_ctx_t*` and interact
   through the well-documented public API in `csilk.h`. Including
   `context_internal.h` is an explicit opt-in.
4. **No installation exposure**: `context_internal.h` is installed alongside
   the public headers but is clearly documented as "Internal — do not
   include." Users who include it anyway assume the risk.

### Mitigation steps (when v1.0 is planned)

1. Move `context_internal.h` to `src/core/` (out of `include/`).
2. Add accessor macros/inline functions in `csilk.h` for commonly-needed
   fields (method, path, status, is_websocket).
3. Update tests to use accessors instead of direct field access.
4. Update examples to remove `context_internal.h` include.
5. Release with a deprecation notice in minor version before the
   breaking change.
