# ABI Stability Roadmap — v1.0 Context Opaque Conversion

> Status: Planned | Target: v1.0 | Based on: ABI_REPORT.md

## Current State

`csilk_ctx_s` is defined in `include/csilk/core/ctx_types.h` with 30+ fields.
External code receives `csilk_ctx_t*` pointers. Tests and examples include
`ctx_types.h` for direct field access (30+ test files, 3 examples).

## Plan

### Phase A: Accessor API Expansion (v0.5.0)

Add well-documented accessor/mutator in `include/csilk/context.h`:

```c
const char* csilk_ctx_get_method(csilk_ctx_t* c);
const char* csilk_ctx_get_path(csilk_ctx_t* c);
int         csilk_ctx_get_status(csilk_ctx_t* c);
void        csilk_ctx_set_status(csilk_ctx_t* c, int status);
int         csilk_ctx_is_websocket(csilk_ctx_t* c);
csilk_arena_t* csilk_ctx_get_arena(csilk_ctx_t* c);
const char* csilk_request_get_header(csilk_ctx_t* c, const char* key);
void        csilk_response_set_header(csilk_ctx_t* c, const char* key, const char* value);
void        csilk_ctx_set_body(csilk_ctx_t* c, const char* data, size_t len);
// ... and 10+ more as needed
```

### Phase B: Internal Migration (v0.6.0)

- Update all framework source (`src/`) to use accessors instead of direct
  struct field access
- Migrate 15 middleware modules to accessor API
- Ship with `ctx_types.h` marked `@deprecated`

### Phase C: Test & Example Migration (v0.7.0)

- Update 30+ test files to use accessors
- Update 3 examples
- Set `CSILK_DEPRECATE_CTX_TYPES` compile flag as a migration aid

### Phase D: Final Opaque (v1.0)

- Move `ctx_types.h` from `include/` to `src/core/`
- Remove `csilk_request_t` and `csilk_response_t` from public headers
  (already forward-declared as `csilk_ctx_t`)
- Bump SOVERSION to 1

## Risks

| Risk | Mitigation |
|------|-----------|
| Breaking 30+ test files | Phased migration with deprecation warnings |
| Performance regression from accessor calls | Inline accessor functions in header |
| SSE/WebSocket callbacks need struct access | Add dedicated SSE/WS accessor subset |
