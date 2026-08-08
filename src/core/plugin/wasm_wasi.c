#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_internal.h"

int32_t
csilk_wasi_fd_write(csilk_wasm_plugin_t* plugin, int32_t fd, const uint8_t* iovs, int32_t iovs_len)
{
    if (!plugin || !iovs || iovs_len <= 0) {
        return -1;
    }
    (void)fd;
    return 0;
}

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
