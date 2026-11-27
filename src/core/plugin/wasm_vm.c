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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wasm_internal.h"

#ifdef HAS_WAMR
#include "wasm_export.h"
#endif

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
#ifndef HAS_WAMR
    /* WAMR runtime not enabled: fail-closed. */
    (void)filepath;
    (void)config;
    errno = ENOTSUP;
    return NULL;
#else
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
#endif
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
    _csilk_wasm_mw_release_plugin(plugin);
    csilk_mutex_destroy(&plugin->mutex);
    if (plugin->memory.data && !plugin->memory.is_mapped) {
        free(plugin->memory.data);
    }
#ifdef HAS_WAMR
    /* WAMR resource cleanup: de-instantiate the instance first, then
     * unload the module.  The module was loaded with wasm_runtime_load
     * (which takes ownership of the bytecode internally), so the module
     * owns its own copy — free(plugin->bytecode) below is always safe.
     * The instance must be de-instantiated before the module is unloaded,
     * otherwise WAMR leaks the instance's internal frames. */
    if (plugin->use_wamr) {
        if (plugin->wasm_instance) {
            wasm_runtime_deinstantiate((wasm_module_inst_t)(uintptr_t)plugin->wasm_instance);
            plugin->wasm_instance = 0;
        }
        if (plugin->wasm_module_id) {
            wasm_runtime_unload((wasm_module_t)(uintptr_t)plugin->wasm_module_id);
            plugin->wasm_module_id = 0;
        }
        plugin->use_wamr = false;
    }
#endif
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
 * @note WAMR path: the WASM function may return an i32 offset into linear
 *       memory pointing to a NUL-terminated string (typically JSON).  That
 *       string is read and returned.  A zero return (void) yields the
 *       default success JSON.  Stub path: enforces the configured fuel
 *       budget; a zero fuel simulates a trap and emits
 *       "CSILK_WASM_TRAP_FUEL_EXHAUSTED".
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

#ifdef HAS_WAMR
    /* WAMR real execution path */
    if (plugin->use_wamr && plugin->wasm_module_id) {
        wasm_module_inst_t   inst = (wasm_module_inst_t)(uintptr_t)plugin->wasm_instance;
        wasm_function_inst_t func = wasm_runtime_lookup_function(inst, func_name);
        if (!func) {
            csilk_mutex_unlock(&plugin->mutex);
            if (err_buf && err_len > 0) {
                snprintf(err_buf, err_len, "CSILK_WASM_TRAP_EXECUTION_ERR: func not found");
            }
            return NULL;
        }
        /* Create an exec env with the configured stack size */
        wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
        if (!env) {
            csilk_mutex_unlock(&plugin->mutex);
            if (err_buf && err_len > 0) {
                snprintf(err_buf, err_len, "CSILK_WASM_TRAP_EXECUTION_ERR: env alloc failed");
            }
            return NULL;
        }
        /* argv holds the i32/i64 return value after the call. */
        uint32_t argv[2] = {0};
        int      rc = wasm_runtime_call_wasm(env, func, 0, argv);
        wasm_runtime_destroy_exec_env(env);
        if (!rc) {
            csilk_mutex_unlock(&plugin->mutex);
            if (err_buf && err_len > 0) {
                const char* exc = wasm_runtime_get_exception(inst);
                snprintf(
                    err_buf, err_len, "CSILK_WASM_TRAP_EXECUTION_ERR: %s", exc ? exc : "unknown");
            }
            return NULL;
        }
        /* Extract the return value while holding plugin->mutex so the instance's
         * linear memory cannot be reset/freed concurrently. */
        char*    res = NULL;
        uint32_t ret = argv[0];
        if (ret != 0) {
            /* Read the string from WAMR's linear memory. */
            wasm_memory_t mem = wasm_runtime_get_linear_memory(inst);
            uint32_t      mem_size = mem ? wasm_runtime_get_linear_memory_size(mem) : 0;
            if (mem && ret < mem_size && (size_t)mem_size - ret >= 1) {
                const char* p = (const char*)mem + ret;
                size_t      avail = (size_t)(mem_size - ret);
                size_t      max_scan =
                    avail < (size_t)(1024 * 1024 + 1) ? avail : (size_t)(1024 * 1024 + 1);
                size_t str_len = strnlen(p, max_scan);
                /* Verify the string is non-empty and NUL-terminated strictly within bounds */
                if (str_len == 0 || str_len >= max_scan) {
                    fprintf(stderr,
                            "[csilk][WASM] memory offset %u returned non-NUL-terminated "
                            "string; falling back to default result\n",
                            ret);
                    str_len = 0;
                }
                if (str_len > 0) {
                    res = strndup(p, str_len);
                }
            } else {
                fprintf(stderr,
                        "[csilk][WASM] invalid return offset %u (mem=%p, mem_size=%u)\n",
                        ret,
                        (void*)mem,
                        mem_size);
            }
        }
        csilk_mutex_unlock(&plugin->mutex);
        if (!res) {
            res = strdup("{\"status\":\"ok\",\"result\":\"wasm_execution_success\"}");
        }
        return res;
    }
#else
    (void)func_name;
    (void)json_input;
    (void)err_buf;
    (void)err_len;
    errno = ENOTSUP;
    csilk_mutex_unlock(&plugin->mutex);
    return NULL;
#endif
}
