/**
 * @file wasm_sdk.h
 * @brief WebAssembly Guest Plugin SDK for csilk.
 *
 * Provides the guest-side ABI definitions, host-import bindings, and convenience
 * macros for developing WebAssembly plugins targeting csilk.
 *
 * @copyright MIT License
 */

#ifndef CSILK_PLUGIN_WASM_SDK_H
#define CSILK_PLUGIN_WASM_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSILK_WASM_ERR_OK 0
#define CSILK_WASM_ERR_ABORT (-1)
#define CSILK_WASM_ERR_OUT_OF_FUEL (-2)

#if defined(__wasm__) || defined(__wasm32__) || defined(__wasm64__)
#define CSILK_WASM_EXPORT __attribute__((visibility("default")))
#define CSILK_WASM_IMPORT(module, name) __attribute__((import_module(module), import_name(name)))
#else
#define CSILK_WASM_EXPORT
#define CSILK_WASM_IMPORT(module, name)
#endif

/*
 * Host-imported function declarations.
 */
#if defined(__wasm__) || defined(__wasm32__) || defined(__wasm64__)
CSILK_WASM_IMPORT("env", "host_get_header")
int host_get_header(const char* name, char* out_buf, uint32_t cap);

CSILK_WASM_IMPORT("env", "host_set_header")
int host_set_header(const char* name, const char* value);

CSILK_WASM_IMPORT("env", "host_get_body")
int host_get_body(char* out_buf, uint32_t cap);

CSILK_WASM_IMPORT("env", "host_set_status")
int host_set_status(int32_t code);
#endif

/**
 * @brief Standard Guest entrypoint contract.
 *
 * Each plugin must implement this function to process requests.
 * @return 0 on success, non-zero (or CSILK_WASM_ERR_ABORT) to abort request chain.
 */
CSILK_WASM_EXPORT int on_request(void);

/**
 * @brief Set the HTTP response status code and mark chain as aborted.
 * @param status HTTP status code (e.g., 400, 401, 403, 500).
 * @return CSILK_WASM_ERR_ABORT.
 */
static inline int
csilk_wasm_abort(int status)
{
#if defined(__wasm__) || defined(__wasm32__) || defined(__wasm64__)
    host_set_status(status);
#else
    (void)status;
#endif
    return CSILK_WASM_ERR_ABORT;
}

#ifdef __cplusplus
}
#endif

#endif /* CSILK_PLUGIN_WASM_SDK_H */
