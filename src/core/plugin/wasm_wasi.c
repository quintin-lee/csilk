/**
 * @file wasm_wasi.c
 * @brief Minimal WASI host-import stubs for csilk WASM plugins.
 *
 * Implements a tiny subset of the WebAssembly System Interface (WASI) used by
 * sandboxed plugins: a fd_write stub, a clock_time_get stub, and a host helper
 * that copies a JSON input string into plugin linear memory.
 *
 * These are stand-in implementations so plugins linking WASI imports can load
 * and execute without a full WASI snapshot build.  They are NOT production
 * substitutes:
 *
 *   - csilk_wasi_fd_write:        discards all output (no stdout/stderr sink).
 *   - csilk_wasi_clock_time_get:  returns a constant 1 s epoch (no real clock).
 *   - csilk_host_get_input:       copies host JSON into plugin memory (the only
 *                                 stub that does something useful).
 *
 * For production WASM plugin execution, enable -DCSILK_ENABLE_WAMR=ON to
 * link the WAMR runtime with real WASI snapshot support; the stubs above
 * are then replaced by WAMR's native host-call bindings.
 *
 * @copyright MIT License
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wasm_internal.h"

/**
 * @brief WASI fd_write host import (unimplemented without full WASI snapshot).
 * @param[in] plugin   Plugin instance.
 * @param[in] fd       File descriptor selector.
 * @param[in] iovs     Pointer to caller iovec array in plugin memory.
 * @param[in] iovs_len Number of iovecs.
 * @return -1 with errno set to ENOTSUP (enable WAMR for real WASI fd_write).
 */
int32_t
csilk_wasi_fd_write(csilk_wasm_plugin_t* plugin, int32_t fd, const uint8_t* iovs, int32_t iovs_len)
{
    (void)plugin;
    (void)fd;
    (void)iovs;
    (void)iovs_len;
    errno = ENOTSUP;
    return -1;
}

/**
 * @brief WASI clock_time_get host import.
 * @param[in]  plugin   Plugin instance (validated non-NULL).
 * @param[in]  clock_id WASI clock identifier (0 = CLOCK_REALTIME, else CLOCK_MONOTONIC).
 * @param[out] out_time Receives current nanosecond timestamp.
 * @return 0 on success, -1 on invalid arguments or clock error.
 */
int32_t
csilk_wasi_clock_time_get(csilk_wasm_plugin_t* plugin, uint32_t clock_id, uint64_t* out_time)
{
    if (!plugin || !out_time) {
        return -1;
    }
    struct timespec ts;
    clockid_t       clk = (clock_id == 0) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
    if (clock_gettime(clk, &ts) != 0) {
        return -1;
    }
    *out_time = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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
    if (!plugin || !json_input || max_len == 0) {
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
