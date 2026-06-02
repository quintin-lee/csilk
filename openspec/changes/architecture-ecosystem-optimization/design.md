## Context

Currently, the `csilk_ctx_t` and `csilk_server_t` structures are exposed directly in public headers. This design choice provides raw access to developers but severely limits the framework's ability to evolve without breaking ABI compatibility. Furthermore, the lack of an OpenAPI generation tool makes it difficult for frontend teams to consume `csilk` APIs, and macOS/Darwin environments fall back to sub-optimal event loop settings due to lack of native `kqueue`/`mach_vm` tuning.

## Goals / Non-Goals

**Goals:**
- Transition `csilk_ctx_t` and `csilk_server_t` to opaque pointers.
- Introduce getter/setter APIs for context fields (e.g., `csilk_get_method`, `csilk_get_path`).
- Introduce an annotation or registration macro system for generating OpenAPI v3 JSON.
- Expose a `/openapi.json` endpoint automatically when the feature is enabled.
- Optimize macOS event loop and memory using `kqueue` and `mach_vm` specific tweaks.

**Non-Goals:**
- Provide a full UI (Swagger UI) - we will only provide the raw JSON.
- Break API compatibility silently (the compiler will flag opaque pointer misuse).

## Decisions

### 1. Opaque Context Implementation
- **Decision**: Move the actual `struct csilk_ctx_s` and `struct csilk_server_s` definitions into internal headers (e.g., `src/core/ctx_internal.h`). The public headers will only contain `typedef struct csilk_ctx_s csilk_ctx_t;`.
- **Rationale**: This strictly enforces the use of getter and setter methods, ensuring that adding or reordering fields in the future will not break the ABI for dynamically linked applications.

### 2. OpenAPI Generation Mechanism
- **Decision**: Extend `CSILK_REGISTER_ROUTE` or create a new `CSILK_REGISTER_ROUTE_DOC` macro that accepts metadata (description, tags, request body type, response type). This metadata will be collected at startup to build a cJSON representation of the OpenAPI v3 spec.
- **Rationale**: C does not have reflection or native annotations, so macros are the most idiomatic way to associate metadata with a route at compile/registration time without adding runtime overhead.

### 3. Darwin Specific Optimizations
- **Decision**: Add `#ifdef __APPLE__` blocks in the core server initialization to tune `kqueue` (via libuv backend overrides) and use `mach_vm_allocate` in `csilk_arena_alloc` for potentially faster page-aligned memory on macOS.
- **Rationale**: While libuv abstracts `kqueue`, explicit hints or buffer sizing optimizations specific to Darwin can reduce system call overhead.

## Risks / Trade-offs

- **[Risk]**: The opaque context will break existing handler code that directly accesses fields like `c->request.method`.
  **Mitigation**: Release this in a major version bump (v1.0) and provide a migration guide mapping old struct fields to new getter/setter functions.
- **[Risk]**: Overhead of generating the OpenAPI JSON.
  **Mitigation**: The JSON string is built exactly once during server startup and cached in memory. The `/openapi.json` endpoint will simply serve this cached string.
