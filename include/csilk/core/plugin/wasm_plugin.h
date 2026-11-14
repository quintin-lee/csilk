/**
 * @file wasm_plugin.h
 * @brief WebAssembly (WASM) Dynamic Sandbox Plugin Engine for csilk.
 *
 * @warning EXPERIMENTAL / SIMULATED RUNTIME: Validates the 8-byte WASM header
 *          and maps linear memory, but csilk_wasm_plugin_execute() does not run
 *          a real WebAssembly bytecode interpreter/JIT yet. Disable simulation
 *          mode at configure time with -DCSILK_ENABLE_EXPERIMENTAL_STUBS=OFF.
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

/* --- WASM Host C-ABI Context API --- */

/**
 * @brief WASM Host API: Get request header value by name.
 * @param ctx Request context.
 * @param name Header key name.
 * @return Header string value or NULL if not present.
 */
const char* csilk_wasm_host_get_header(csilk_ctx_t* ctx, const char* name);

/**
 * @brief WASM Host API: Set or modify response header.
 * @param ctx Request context.
 * @param name Header key name.
 * @param value Header value string.
 * @return 0 on success, -1 on failure.
 */
int csilk_wasm_host_set_header(csilk_ctx_t* ctx, const char* name, const char* value);

/**
 * @brief WASM Host API: Get request URL query or path parameter.
 * @param ctx Request context.
 * @param name Parameter name.
 * @return Parameter value string or NULL if not found.
 */
const char* csilk_wasm_host_get_param(csilk_ctx_t* ctx, const char* name);

/**
 * @brief WASM Host API: Set response status code from WASM plugin.
 * @param ctx Request context.
 * @param status HTTP status code (e.g., 200, 403, 500).
 * @return 0 on success, -1 on failure.
 */
int csilk_wasm_host_set_status(csilk_ctx_t* ctx, int status);

/**
 * @brief WASM Host API: Get request body buffer and length.
 * @param ctx Request context.
 * @param[out] out_len Receives body length in bytes.
 * @return Pointer to body bytes or NULL if body is empty.
 */
const char* csilk_wasm_host_get_body(csilk_ctx_t* ctx, size_t* out_len);

/**
 * @brief WASM Host API: Set or replace response body data.
 * @param ctx Request context.
 * @param data Response payload bytes.
 * @param len Size of response payload in bytes.
 * @return 0 on success, -1 on failure.
 */
int csilk_wasm_host_set_body(csilk_ctx_t* ctx, const char* data, size_t len);

/**
 * @brief WASM Host API: Get HTTP request method (GET, POST, etc.).
 * @param ctx Request context.
 * @return HTTP method string or NULL.
 */
const char* csilk_wasm_host_get_method(csilk_ctx_t* ctx);

/**
 * @brief WASM Host API: Get HTTP request path.
 * @param ctx Request context.
 * @return HTTP path string or NULL.
 */
const char* csilk_wasm_host_get_path(csilk_ctx_t* ctx);

/**
 * @brief WASM Host API: Emit a log message via the Host logger.
 * @param level Log severity level (0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR).
 * @param msg NUL-terminated message string.
 */
void csilk_wasm_host_log(int level, const char* msg);

/**
 * @brief Creates a csilk middleware handler wrapping a WASM plugin exported function.
 * @param plugin Loaded WASM plugin.
 * @param func_name Name of exported function to invoke on each request.
 * @return Middleware function pointer matching csilk_handler_t.
 */
void (*csilk_wasm_plugin_middleware(csilk_wasm_plugin_t* plugin,
                                    const char*          func_name))(csilk_ctx_t*);

/**
 * @brief Map shared linear memory buffer between Host and WASM plugin sandbox.
 *
 * @param plugin WASM plugin instance handle.
 * @param buf Pointer to host memory buffer.
 * @param len Size of memory buffer in bytes.
 * @return 0 on success, -1 on invalid plugin or buffer.
 */
int csilk_wasm_plugin_map_memory(csilk_wasm_plugin_t* plugin, void* buf, size_t len);

/**
 * @brief WASM Host API: Retrieve shared linear memory mapping view.
 *
 * @param plugin WASM plugin instance handle.
 * @param[out] len Receives length of mapped linear memory.
 * @return Pointer to mapped linear memory buffer, or NULL if not mapped.
 */
void* csilk_wasm_host_get_mapped_buffer(const csilk_wasm_plugin_t* plugin, size_t* len);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CORE_WASM_PLUGIN_H */
