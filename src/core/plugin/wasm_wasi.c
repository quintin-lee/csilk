/**
 * @file wasm_wasi.c
 * @brief Minimal WASI host-import stubs for csilk WASM plugins.
 *
 * Implements a tiny subset of the WebAssembly System Interface (WASI) used by
 * sandboxed plugins: a fd_write stub, a clock_time_get stub, and a host helper
 * that copies a JSON input string into plugin linear memory. These are stand-in
 * implementations so plugins linking WASI imports can load and execute.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_internal.h"

/**
 * @brief WASI fd_write host import (stub).
 * @param[in] plugin   Plugin instance (validated non-NULL).
 * @param[in] fd       File descriptor selector (currently ignored).
 * @param[in] iovs     Pointer to caller iovec array in plugin memory.
 * @param[in] iovs_len Number of iovecs (must be > 0).
 * @return 0 on success, -1 on invalid arguments.
 * @note The current implementation discards output; it exists only to satisfy
 *       the imported symbol so modules that reference it can instantiate.
 */
int32_t
csilk_wasi_fd_write(csilk_wasm_plugin_t* plugin, int32_t fd, const uint8_t* iovs, int32_t iovs_len)
{
    if (!plugin || !iovs || iovs_len <= 0) {
        return -1;
    }
    (void)fd;
    return 0;
}

/**
 * @brief WASI clock_time_get host import (stub).
 * @param[in]  plugin   Plugin instance (validated non-NULL).
 * @param[in]  clock_id Clock identifier (currently ignored).
 * @param[out] out_time Receives the returned timestamp (fixed 1e9 ns).
 * @return 0 on success, -1 on invalid arguments.
 * @note Returns a constant timestamp rather than querying the real clock.
 */
int32_t
csilk_wasi_clock_time_get(csilk_wasm_plugin_t* plugin, uint32_t clock_id, uint64_t* out_time)
{
    if (!plugin || !out_time) {
        return -1;
    }
    (void)clock_id;
    *out_time = 1000000000ULL;
    return 0;
}

/**
 * @brief Copy a JSON input string into plugin linear memory.
 * @param[in] plugin        Plugin instance (validated non-NULL).
 * @param[in] memory_offset Destination offset within plugin memory.
 * @param[in] max_len       Available capacity at the offset (includes NUL).
 * @param[in] json_input    NUL-terminated input string (validated non-NULL).
 * @return Number of bytes written (excluding the NUL terminator), 0 on failure.
 * @note Writes at most max_len-1 bytes and always NUL-terminates; returns 0 if
 *       the slice cannot be obtained from plugin memory.
 */
uint32_t
csilk_host_get_input(csilk_wasm_plugin_t* plugin,
                     uint32_t             memory_offset,
                     uint32_t             max_len,
                     const char*          json_input)
{
    if (!plugin || !json_input) {
        return 0;
    }

    void* slice = csilk_wasm_get_memory_slice(&plugin->memory, memory_offset, max_len);
    if (!slice) {
        return 0;
    }

    size_t len = strlen(json_input);
    if (len >= max_len) {
        len = max_len - 1;
    }

    memcpy(slice, json_input, len);
    ((char*)slice)[len] = '\0';
    return (uint32_t)len;
}
