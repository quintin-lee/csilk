/**
 * @file wasm_plugin.h
 * @brief WebAssembly (WASM) Dynamic Sandbox Plugin Engine for csilk.
 */

#ifndef CSILK_CORE_WASM_PLUGIN_H
#define CSILK_CORE_WASM_PLUGIN_H

#include "csilk/csilk.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle for WebAssembly Plugin instance. */
typedef struct csilk_wasm_plugin_s csilk_wasm_plugin_t;

/**
 * @brief Load and validate a WebAssembly bytecode module into a plugin instance.
 *
 * Validates the 4-byte WASM magic header (\0asm) and version number, preparing
 * the module for sandboxed execution during request processing.
 *
 * @param wasm_bytes Pointer to raw WASM bytecode memory buffer.
 * @param size Length of WASM bytecode in bytes.
 * @return Heap-allocated csilk_wasm_plugin_t handle, or NULL on invalid WASM bytecode or allocation failure.
 */
csilk_wasm_plugin_t* csilk_wasm_plugin_load(const uint8_t* wasm_bytes, size_t size);

/**
 * @brief Execute an exported function within a WebAssembly plugin in the context of an HTTP request.
 *
 * @param plugin Loaded WASM plugin handle.
 * @param ctx Request context passed into the WASM execution environment.
 * @param func_name Name of the exported WASM function to invoke (e.g., "on_request", "filter_headers").
 * @return 0 on successful execution, non-zero on runtime error or exported symbol not found.
 */
int csilk_wasm_plugin_execute(csilk_wasm_plugin_t* plugin, csilk_ctx_t* ctx, const char* func_name);

/**
 * @brief Check if a WASM plugin module is valid and initialized.
 *
 * @param plugin Plugin handle to inspect.
 * @return 1 if valid, 0 otherwise.
 */
int csilk_wasm_plugin_is_valid(const csilk_wasm_plugin_t* plugin);

/**
 * @brief Free resources associated with a loaded WebAssembly plugin instance.
 *
 * @param plugin Plugin handle to release.
 */
void csilk_wasm_plugin_free(csilk_wasm_plugin_t* plugin);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CORE_WASM_PLUGIN_H */
