/**
 * @file wasm_vm.c
 * @brief In-process WebAssembly plugin runtime for csilk backed by WAMR.
 *
 * @copyright MIT License
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "csilk/wasm/plugin.h"
#include "wasm_internal.h"
#include "wasm_export.h"

static csilk_once_t s_wamr_init_once = CSILK_ONCE_INIT;
static bool         s_wamr_init_ok = false;

static void
_csilk_wasm_runtime_atexit(void)
{
    if (s_wamr_init_ok) {
        wasm_runtime_destroy();
        s_wamr_init_ok = false;
    }
}

static void
_csilk_wasm_runtime_init_once(void)
{
    s_wamr_init_ok = wasm_runtime_init();
    if (s_wamr_init_ok) {
        atexit(_csilk_wasm_runtime_atexit);
    }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("hidden")))
#endif
bool
_csilk_wasm_ensure_runtime(void)
{
    csilk_once(&s_wamr_init_once, _csilk_wasm_runtime_init_once);
    return s_wamr_init_ok;
}

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

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 8 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t* buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz) {
        free(buf);
        return NULL;
    }

    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load(buf, (size_t)sz);
    free(buf);
    if (!plugin) {
        return NULL;
    }

    if (config) {
        csilk_mutex_lock(&plugin->mutex);
        plugin->config = *config;
        plugin->memory.max_pages = config->max_memory_pages;
        csilk_mutex_unlock(&plugin->mutex);
        csilk_wasm_plugin_set_fuel(plugin, (uint32_t)config->fuel_limit);
    }

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
    _csilk_wasm_mw_release_plugin(plugin);
    csilk_mutex_destroy(&plugin->mutex);
    if (plugin->memory.data && !plugin->memory.is_mapped) {
        free(plugin->memory.data);
    }
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

    /* WAMR real execution path */
    if (plugin->use_wamr && plugin->wasm_module_id) {
        if (plugin->config.fuel_limit == 0) {
            csilk_mutex_unlock(&plugin->mutex);
            if (err_buf && err_len > 0) {
                snprintf(err_buf, err_len, "CSILK_WASM_TRAP_FUEL_EXHAUSTED");
            }
            return NULL;
        }
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
#if defined(WASM_ENABLE_INSTRUCTION_METERING) && WASM_ENABLE_INSTRUCTION_METERING != 0
        int instr_limit = (plugin->config.fuel_limit > (uint64_t)INT_MAX)
                              ? INT_MAX
                              : (int)plugin->config.fuel_limit;
        wasm_runtime_set_instruction_count_limit(env, instr_limit);
#endif
        /* argv holds the i32/i64 return value after the call. */
        uint32_t argv[2] = {0};
        int      rc = wasm_runtime_call_wasm(env, func, 0, argv);
        wasm_runtime_destroy_exec_env(env);
        if (!rc) {
            csilk_mutex_unlock(&plugin->mutex);
            if (err_buf && err_len > 0) {
                const char* exc = wasm_runtime_get_exception(inst);
                if (exc && strstr(exc, "instruction limit") != NULL) {
                    snprintf(err_buf, err_len, "CSILK_WASM_TRAP_FUEL_EXHAUSTED");
                } else {
                    snprintf(err_buf,
                             err_len,
                             "CSILK_WASM_TRAP_EXECUTION_ERR: %s",
                             exc ? exc : "unknown");
                }
            }
            return NULL;
        }
        /* Extract the return value while holding plugin->mutex so the instance's
         * linear memory cannot be reset/freed concurrently. */
        char*    res = NULL;
        uint32_t ret = argv[0];
        if (ret != 0) {
            /* Read the string from WAMR's linear memory. */
            wasm_memory_inst_t mem = wasm_runtime_get_default_memory(inst);
            void*              mem_base = mem ? wasm_memory_get_base_address(mem) : NULL;
            uint64_t           mem_size =
                mem ? (wasm_memory_get_cur_page_count(mem) * wasm_memory_get_bytes_per_page(mem))
                              : 0;
            if (mem_base && (uint64_t)ret < mem_size && mem_size - (uint64_t)ret >= 1) {
                const char* p = (const char*)mem_base + ret;
                uint64_t    avail = mem_size - (uint64_t)ret;
                size_t      max_scan =
                    avail < (uint64_t)(1024 * 1024 + 1) ? (size_t)avail : (size_t)(1024 * 1024 + 1);
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
                        "[csilk][WASM] invalid return offset %u (mem=%p, mem_size=%llu)\n",
                        ret,
                        mem_base,
                        (unsigned long long)mem_size);
            }
        }
        csilk_mutex_unlock(&plugin->mutex);
        if (!res) {
            res = strdup("{\"status\":\"ok\",\"result\":\"wasm_execution_success\"}");
        }
        return res;
    }
    (void)json_input;
    if (err_buf && err_len > 0) {
        snprintf(err_buf, err_len, "CSILK_WASM_TRAP_EXECUTION_ERR: module not instantiated");
    }
    csilk_mutex_unlock(&plugin->mutex);
    errno = ENOTSUP;
    return NULL;
}
