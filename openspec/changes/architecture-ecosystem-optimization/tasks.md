## 1. ABI Stability (Opaque Structs)

- [x] 1.1 Move `csilk_ctx_s` and `csilk_server_s` definitions from `include/csilk/types.h` to `src/core/ctx_internal.h` and `src/core/srv_internal.h`.
- [x] 1.2 Update public headers (`types.h`, `context.h`, `server.h`) to only contain `typedef struct ...` forward declarations.
- [x] 1.3 Add and implement missing getter/setter functions in `src/core/context.c` (e.g., `csilk_get_headers`, `csilk_set_status`).
- [x] 1.4 Refactor all `examples/` and `tests/` to use the new getter/setter API instead of direct struct access.

## 2. OpenAPI Generation

- [x] 2.1 Define `csilk_route_metadata_t` struct and `CSILK_REGISTER_ROUTE_DOC` macro in `include/csilk/router.h`.
- [x] 2.2 Implement metadata collection during server initialization in `src/core/server.c`.
- [x] 2.3 Implement OpenAPI JSON building using `cJSON` in a new file `src/core/openapi.c`.
- [x] 2.4 Register the `/openapi.json` route automatically in `csilk_server_run` if enabled via `csilk_server_config_t`.
- [x] 2.5 Add an integration test `tests/test_openapi.c` to verify the JSON output.

## 3. Darwin Specific Optimizations

- [x] 3.1 Modify `src/core/arena.c` to use `#ifdef __APPLE__` and `mach_vm_allocate` instead of `malloc`/`posix_memalign`.
- [x] 3.2 Add `mach/mach_init.h` and `mach/mach_vm.h` includes conditionally in `arena.c`.
- [x] 3.3 Add macOS specific `uv_loop_init` flags or hints in `src/core/server.c` if applicable for kqueue.

## 4. Verification

- [x] 4.1 Ensure full compilation succeeds on Linux (and ideally macOS if available).
- [x] 4.2 Run `ctest` to ensure no functionality is broken by the opaque struct refactoring.
