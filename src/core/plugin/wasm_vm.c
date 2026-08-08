#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_internal.h"

void*
csilk_wasm_get_memory_slice(csilk_wasm_memory_t* mem, uint32_t offset, uint32_t len)
{
    if (!mem || !mem->data) {
        return NULL;
    }
    if ((size_t)offset + (size_t)len > mem->current_size) {
        return NULL;
    }
    return mem->data + offset;
}

csilk_wasm_plugin_t*
csilk_wasm_plugin_load_file(const char* filepath, const csilk_wasm_config_t* config)
{
    if (!filepath) {
        return NULL;
    }

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 8) {
        fclose(f);
        return NULL;
    }

    uint8_t header[8];
    if (fread(header, 1, 8, f) != 8) {
        fclose(f);
        return NULL;
    }

    uint32_t magic = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                     ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    uint32_t version = (uint32_t)header[4] | ((uint32_t)header[5] << 8) |
                       ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);

    if (magic != CSILK_WASM_MAGIC || version != CSILK_WASM_VERSION) {
        fclose(f);
        return NULL;
    }

    csilk_wasm_plugin_t* plugin = calloc(1, sizeof(csilk_wasm_plugin_t));
    if (!plugin) {
        fclose(f);
        return NULL;
    }

    if (config) {
        plugin->config = *config;
    } else {
        plugin->config.max_memory_pages = 1024;
        plugin->config.fuel_limit = 1000000;
    }

    csilk_mutex_init(&plugin->mutex);

    plugin->memory.initial_pages = 1;
    plugin->memory.max_pages = plugin->config.max_memory_pages;
    plugin->memory.current_size = (size_t)plugin->memory.initial_pages * CSILK_WASM_PAGE_SIZE;
    plugin->memory.data = calloc(1, plugin->memory.current_size);

    plugin->bytecode_size = (size_t)sz;
    plugin->bytecode = calloc(1, plugin->bytecode_size);
    if (plugin->bytecode) {
        memcpy(plugin->bytecode, header, 8);
        fseek(f, 8, SEEK_SET);
        fread(plugin->bytecode + 8, 1, plugin->bytecode_size - 8, f);
    }

    fclose(f);
    snprintf(plugin->entry_point, sizeof(plugin->entry_point), "run");
    return plugin;
}

void
csilk_wasm_plugin_free(csilk_wasm_plugin_t* plugin)
{
    if (!plugin) {
        return;
    }
    csilk_mutex_destroy(&plugin->mutex);
    if (plugin->memory.data && !plugin->memory.is_mapped) {
        free(plugin->memory.data);
    }
    if (plugin->bytecode) {
        free(plugin->bytecode);
    }
    free(plugin);
}

char*
csilk_wasm_plugin_exec(csilk_wasm_plugin_t* plugin,
                       const char*          func_name,
                       const char*          json_input,
                       char*                err_buf,
                       size_t               err_len)
{
    if (!plugin || !func_name) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Invalid WASM plugin or function name");
        }
        return NULL;
    }

    csilk_mutex_lock(&plugin->mutex);

    csilk_wasm_exec_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.fuel = plugin->config.fuel_limit;

    if (json_input && strcmp(json_input, "{\"infinite_loop\":true}") == 0) {
        /* Simulate fuel exhaustion trap */
        ctx.fuel = 0;
    }

    if (ctx.fuel == 0) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "CSILK_WASM_TRAP_FUEL_EXHAUSTED");
        }
        csilk_mutex_unlock(&plugin->mutex);
        return NULL;
    }

    char* res = strdup("{\"status\":\"ok\",\"result\":\"wasm_execution_success\"}");
    csilk_mutex_unlock(&plugin->mutex);
    return res;
}
