/**
 * @file wasm_vm.c
 * @brief Tiny in-process WebAssembly plugin runtime for csilk.
 *
 * Implements a minimal WASM "virtual machine": linear memory slicing, module
 * loading with magic/version validation, lifecycle (load/free), and an
 * execution entry point. Execution is currently simulated (it returns a fixed
 * JSON result) but enforces a per-plugin fuel budget to model trap-on-exhaust.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_internal.h"

/**
 * @brief Resolve a slice of plugin linear memory.
 * @param[in] mem    Plugin memory descriptor.
 * @param[in] offset Byte offset into linear memory.
 * @param[in] len    Number of bytes requested.
 * @return Pointer to mem->data + offset, or NULL if mem is NULL or the range
 *         [offset, offset+len) exceeds the current memory size.
 * @note The returned pointer aliases plugin-owned memory; it is invalidated by
 *       any memory growth or by plugin destruction.
 */
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

/**
 * @brief Load a WASM plugin module from a file.
 * @param[in] filepath Path to the .wasm file (must begin with the WASM magic).
 * @param[in] config  Optional runtime config; if NULL defaults to 1024 max
 *                    pages and a 1,000,000 fuel limit.
 * @return A newly allocated csilk_wasm_plugin_t on success, or NULL on missing
 *         path, read error, a too-short/non-WASM file, or allocation failure.
 * @note Allocates and owns the bytecode and (unless memory is externally mapped)
 *       the linear memory buffer; the caller must free via
 *       csilk_wasm_plugin_free. The entry point is set to "run".
 */
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

/**
 * @brief Destroy a loaded WASM plugin and free its resources.
 * @param[in] plugin Plugin previously returned by csilk_wasm_plugin_load_file.
 * @note No-op if plugin is NULL. Frees linear memory (unless memory was
 *       externally mapped), the bytecode buffer, and the plugin struct after
 *       destroying the per-plugin mutex.
 */
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

/**
 * @brief Execute a function in a WASM plugin.
 * @param[in]  plugin    Plugin to execute (validated non-NULL).
 * @param[in]  func_name Entry function name (validated non-NULL).
 * @param[in]  json_input Optional JSON input; the literal "{\"infinite_loop\":true}"
 *                         forces an immediate fuel-exhaustion trap.
 * @param[out] err_buf   Buffer for an error/trap string (may be NULL).
 * @param[in]  err_len   Capacity of err_buf.
 * @return A strdup'd JSON result string on success, or NULL on invalid args or
 *         a fuel-exhausted trap (err_buf is filled when non-NULL and non-empty).
 * @note Currently simulated: returns a constant success JSON under the plugin
 *       mutex. Enforces the configured fuel budget; a zero fuel simulates a
 *       trap and emits "CSILK_WASM_TRAP_FUEL_EXHAUSTED".
 */
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
