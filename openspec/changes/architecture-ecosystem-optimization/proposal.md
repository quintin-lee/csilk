## Why

As `csilk` matures into a v1.0 release, maintaining backward compatibility and providing a robust developer experience become paramount. Currently, internal structures (like context and configurations) are exposed, risking ABI breakage across minor updates. Additionally, integrating seamlessly into modern backend ecosystems requires automated API documentation (OpenAPI) and better cross-platform support (specifically native optimizations for macOS/Darwin environments).

## What Changes

- **ABI Stability (Opaque Context)**: Refactoring `csilk_ctx_t` and `csilk_server_t` to be fully opaque in public headers. **BREAKING** (This will require users to use getters/setters rather than directly accessing struct members).
- **OpenAPI / Swagger Generation**: Implementing a metadata layer and a built-in endpoint to auto-generate OpenAPI v3 JSON specs from registered routes and data models.
- **Cross-Platform Darwin Enhancements**: Implementing `kqueue` for event polling (via libuv optimizations if applicable) and `mach_vm` for memory management specific to macOS.

## Capabilities

### New Capabilities
- `abi-stability`: Refactoring public data structures to be opaque pointers, providing getter/setter APIs to ensure ABI backward compatibility.
- `openapi-generator`: Adding an automated OpenAPI v3 JSON generation capability based on route and model annotations.
- `darwin-optimizations`: Adding macOS-specific optimizations for event handling and memory allocation.

### Modified Capabilities
(None - these are new ecosystem features)

## Impact

- **Public API**: `include/csilk/types.h`, `include/csilk/context.h`, and `include/csilk/server.h` will see breaking changes as structs become opaque.
- **Handlers**: Application handlers will need to be updated to use the new setter/getter API (e.g., instead of `c->request.method`, use `csilk_get_method(c)`).
- **Core Engine**: `src/core/context.c` and `src/core/server.c` will contain the concrete struct definitions.
- **New Feature**: A new endpoint `/openapi.json` will be exposed if OpenAPI generation is enabled.
