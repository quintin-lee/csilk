# ABI Stability Roadmap — v1.0 Context Opaque Conversion

> Status: **Complete** | Completed: v0.3.0 | Based on: docs/meta/ABI_REPORT.md
>
> **ABI Rule**: All public API functions **MUST** take/return `csilk_ctx_t*` (opaque) — direct struct access **MUST NOT** be exposed in public headers. Accessor function call overhead **SHOULD** be zero when inlined (single pointer dereference, ≤ 1 CPU cycle).

## Current State

`csilk_ctx_s` internal layout is hidden in `src/core/ctx_types.h`. All public
API uses the opaque `csilk_ctx_t*` handle through accessor functions. 

All phases below were completed during the v0.3.0 development cycle:

## Completed Phases

### Phase A: Accessor API Expansion (v0.3.0) ✅

Implemented accessor/mutator API in `include/csilk/context.h`:

```c
const char* csilk_get_method(csilk_ctx_t* c);       // (was csilk_ctx_get_method in plan)
const char* csilk_get_path(csilk_ctx_t* c);          // (was csilk_ctx_get_path in plan)
int         csilk_get_status(csilk_ctx_t* c);        // (was csilk_ctx_get_status in plan)
int         csilk_is_websocket(csilk_ctx_t* c);      // (was csilk_ctx_is_websocket in plan)
csilk_arena_t* csilk_get_arena(csilk_ctx_t* c);      // (was csilk_ctx_get_arena in plan)
const char* csilk_get_header(csilk_ctx_t* c, const char* key);
void        csilk_set_header(csilk_ctx_t* c, const char* key, const char* value);
void        csilk_set_response_body(csilk_ctx_t* c, const char* data, size_t len, int managed);
// ... and 30+ more accessors fully implemented
```

### Phase B-E: Migration Complete (v0.3.0) ✅

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
