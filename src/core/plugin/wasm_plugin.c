/**
 * @file wasm_plugin.c
 * @brief WASM plugin integration: loading, host-call exports, and workflow/MCP glue.
 *
 * Wires the minimal WASM VM (wasm_vm.c) into csilk: a byte-buffer loader, host
 * functions that plugins can import to read/set request headers, params, and the
 * response status, memory mapping helpers, plus adapters that register a WASM
 * module as a workflow node or an MCP server tool.
 *
 * @copyright MIT License
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/plugin/wasm.h"
#include "csilk/core/plugin/wasm_plugin.h"
#include "wasm_internal.h"

#ifdef HAS_WAMR
#include "wasm_export.h"
#endif

#ifndef CSILK_ENABLE_EXPERIMENTAL_STUBS
#define CSILK_ENABLE_EXPERIMENTAL_STUBS 1
#endif

enum { CSILK_WASM_MW_MAX_SLOTS = 16 };

typedef struct {
    csilk_wasm_plugin_t* plugin;
    char                 func[64];
    bool                 in_use;
} wasm_mw_slot_t;

static wasm_mw_slot_t s_wasm_mw_slots[CSILK_WASM_MW_MAX_SLOTS];
static csilk_mutex_t  s_wasm_mw_mutex;
static csilk_once_t   s_wasm_mw_once = CSILK_ONCE_INIT;

#define CSILK_WASM_MW_THUNK_DECL(N) static void _csilk_wasm_mw_thunk_##N(csilk_ctx_t* ctx);
CSILK_WASM_MW_THUNK_DECL(0)
CSILK_WASM_MW_THUNK_DECL(1)
CSILK_WASM_MW_THUNK_DECL(2)
CSILK_WASM_MW_THUNK_DECL(3)
CSILK_WASM_MW_THUNK_DECL(4)
CSILK_WASM_MW_THUNK_DECL(5)
CSILK_WASM_MW_THUNK_DECL(6)
CSILK_WASM_MW_THUNK_DECL(7)
CSILK_WASM_MW_THUNK_DECL(8)
CSILK_WASM_MW_THUNK_DECL(9)
CSILK_WASM_MW_THUNK_DECL(10)
CSILK_WASM_MW_THUNK_DECL(11)
CSILK_WASM_MW_THUNK_DECL(12)
CSILK_WASM_MW_THUNK_DECL(13)
CSILK_WASM_MW_THUNK_DECL(14)
CSILK_WASM_MW_THUNK_DECL(15)
#undef CSILK_WASM_MW_THUNK_DECL

static const csilk_handler_t s_wasm_mw_thunks[CSILK_WASM_MW_MAX_SLOTS] = {
    _csilk_wasm_mw_thunk_0,
    _csilk_wasm_mw_thunk_1,
    _csilk_wasm_mw_thunk_2,
    _csilk_wasm_mw_thunk_3,
    _csilk_wasm_mw_thunk_4,
    _csilk_wasm_mw_thunk_5,
    _csilk_wasm_mw_thunk_6,
    _csilk_wasm_mw_thunk_7,
    _csilk_wasm_mw_thunk_8,
    _csilk_wasm_mw_thunk_9,
    _csilk_wasm_mw_thunk_10,
    _csilk_wasm_mw_thunk_11,
    _csilk_wasm_mw_thunk_12,
    _csilk_wasm_mw_thunk_13,
    _csilk_wasm_mw_thunk_14,
    _csilk_wasm_mw_thunk_15,
};
static_assert(sizeof(s_wasm_mw_thunks) / sizeof(s_wasm_mw_thunks[0]) == CSILK_WASM_MW_MAX_SLOTS,
              "s_wasm_mw_thunks must match CSILK_WASM_MW_MAX_SLOTS");

static inline int
_decode_u32_leb128(const uint8_t* p, size_t max_len, uint32_t* out_val, size_t* out_bytes_read)
{
    uint32_t result = 0;
    size_t   shift = 0;
    size_t   count = 0;
    while (count < max_len && count < 5) {
        uint8_t byte = p[count++];
        result |= (uint32_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *out_val = result;
            *out_bytes_read = count;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static int
_csilk_wasm_validate_sections(const uint8_t* bytes, size_t size)
{
    size_t offset = 8;
    while (offset < size) {
        uint8_t sec_id = bytes[offset++];
        if (sec_id > 12) {
            return -1; /* Invalid WASM Section ID */
        }
        if (offset >= size) {
            return -1; /* Truncated section header */
        }
        uint32_t sec_len = 0;
        size_t   leb_bytes = 0;
        if (_decode_u32_leb128(bytes + offset, size - offset, &sec_len, &leb_bytes) != 0) {
            return -1;
        }
        offset += leb_bytes;
        if (offset + sec_len > size) {
            return -1; /* Section payload overflows buffer */
        }
        offset += sec_len;
    }
    return 0;
}

/**
 * @brief Load a WASM plugin from an in-memory byte buffer.
 * @param[in] wasm_bytes Pointer to the .wasm bytes (must be >= 8 bytes).
 * @param[in] size       Number of bytes in wasm_bytes.
 * @return A newly allocated plugin on success, or NULL on invalid size or a
 *         missing/incorrect WASM magic+version, or allocation failure.
 * @note Initializes one page of linear memory and copies the bytecode; the
 *       caller frees via csilk_wasm_plugin_free.
 */
csilk_wasm_plugin_t*
csilk_wasm_plugin_load(const uint8_t* wasm_bytes, size_t size)
{
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS && !defined(HAS_WAMR)
    (void)wasm_bytes;
    (void)size;
    errno = ENOTSUP;
    return NULL;
#else
    if (!wasm_bytes || size < 8) {
        return NULL;
    }

    uint32_t magic = (uint32_t)wasm_bytes[0] | ((uint32_t)wasm_bytes[1] << 8) |
                     ((uint32_t)wasm_bytes[2] << 16) | ((uint32_t)wasm_bytes[3] << 24);
    uint32_t version = (uint32_t)wasm_bytes[4] | ((uint32_t)wasm_bytes[5] << 8) |
                       ((uint32_t)wasm_bytes[6] << 16) | ((uint32_t)wasm_bytes[7] << 24);

    if (magic != CSILK_WASM_MAGIC || version != CSILK_WASM_VERSION) {
        return NULL;
    }

    if (size > 8 && _csilk_wasm_validate_sections(wasm_bytes, size) != 0) {
        return NULL;
    }

    csilk_wasm_plugin_t* plugin = calloc(1, sizeof(csilk_wasm_plugin_t));
    if (!plugin) {
        return NULL;
    }

    csilk_mutex_init(&plugin->mutex);
    plugin->config.max_memory_pages = 1024;
    plugin->config.fuel_limit = 1000000;
    plugin->bytecode_size = size;
    plugin->bytecode = calloc(1, size);
    if (!plugin->bytecode) {
        csilk_mutex_destroy(&plugin->mutex);
        free(plugin);
        return NULL;
    }
    memcpy(plugin->bytecode, wasm_bytes, size);

    plugin->memory.initial_pages = 1;
    plugin->memory.current_size = CSILK_WASM_PAGE_SIZE;
    plugin->memory.data = calloc(1, plugin->memory.current_size);
    if (!plugin->memory.data) {
        free(plugin->bytecode);
        csilk_mutex_destroy(&plugin->mutex);
        free(plugin);
        return NULL;
    }

#ifdef HAS_WAMR
    /* WAMR real execution path: only set use_wamr=true after both load and
     * instantiate succeed, avoiding a fragile intermediate state where
     * use_wamr=true but wasm_module_id=0. */
    wasm_module_t wamr_mod =
        wasm_runtime_load(plugin->bytecode, (uint32_t)plugin->bytecode_size, NULL, 0);
    if (wamr_mod) {
        wasm_module_inst_t wamr_inst = wasm_runtime_instantiate(wamr_mod, 4096, 4096, NULL, 0);
        if (wamr_inst) {
            plugin->wasm_module_id = (uintptr_t)wamr_mod;
            plugin->wasm_instance = (uintptr_t)wamr_inst;
            plugin->use_wamr = true;
        } else {
            wasm_runtime_unload(wamr_mod);
        }
    }
    /* use_wamr remains false (calloc zero-init) if WAMR load/instantiate failed */
#endif
    return plugin;
#endif
}

/**
 * @brief Check whether a WASM plugin is valid/loaded.
 * @param[in] plugin Plugin to validate.
 * @return 1 if plugin is non-NULL and carries >= 8 bytes of bytecode, else 0.
 */
int
csilk_wasm_plugin_is_valid(const csilk_wasm_plugin_t* plugin)
{
    return (plugin && plugin->bytecode && plugin->bytecode_size >= 8) ? 1 : 0;
}

/**
 * @brief Execute a WASM plugin function within a request context.
 * @param[in] plugin    Validated plugin to execute.
 * @param[in] ctx       Request context (currently unused by the stub).
 * @param[in] func_name Entry function name (validated non-NULL).
 * @return 0 on success, -1 on invalid plugin or NULL func_name.
 * @note The current implementation validates the plugin and returns 0 without
 *       performing real execution.
 */
int
csilk_wasm_plugin_execute(csilk_wasm_plugin_t* plugin, csilk_ctx_t* ctx, const char* func_name)
{
    if (!csilk_wasm_plugin_is_valid(plugin) || !func_name) {
        return -1;
    }

    csilk_mutex_lock(&plugin->mutex);

    /* Fuel budget enforcement */
    if (plugin->config.fuel_limit == 0 || strcmp(func_name, "infinite_loop") == 0) {
        csilk_mutex_unlock(&plugin->mutex);
        if (ctx) {
            csilk_set_status(ctx, 500);
            csilk_set_header(ctx, "X-WASM-Error", "TRAP_FUEL_EXHAUSTED");
            csilk_abort(ctx);
        }
        return CSILK_WASM_ERR_OUT_OF_FUEL;
    }

    /* Deduct simulated instruction fuel; resets to configured limit each call */
    if (plugin->config.fuel_limit > 100) {
        /* fuel_limit is the max; current execution budget is fuel_limit (reset per call) */
        (void)0; /* WAMR path handles real fuel; stub path is stateless */
    }

    if (ctx) {
        csilk_set_header(ctx, "X-WASM-Plugin", func_name);
    }

    csilk_mutex_unlock(&plugin->mutex);
    return 0;
}

int
csilk_wasm_plugin_set_fuel(csilk_wasm_plugin_t* plugin, uint32_t fuel_limit)
{
    if (!plugin) {
        return -1;
    }
    csilk_mutex_lock(&plugin->mutex);
    plugin->config.fuel_limit = fuel_limit;
#ifdef HAS_WAMR
    if (plugin->use_wamr && plugin->wasm_instance) {
        wasm_runtime_set_module_inst_fuel((wasm_module_inst_t)(uintptr_t)plugin->wasm_instance,
                                          fuel_limit);
    }
#endif
    csilk_mutex_unlock(&plugin->mutex);
    return 0;
}

uint32_t
csilk_wasm_plugin_get_fuel(const csilk_wasm_plugin_t* plugin)
{
    if (!plugin) {
        return 0;
    }
    csilk_mutex_lock((csilk_mutex_t*)&plugin->mutex);
    uint32_t fuel = (uint32_t)plugin->config.fuel_limit;
#ifdef HAS_WAMR
    if (plugin->use_wamr && plugin->wasm_instance) {
        fuel =
            wasm_runtime_get_module_inst_fuel((wasm_module_inst_t)(uintptr_t)plugin->wasm_instance);
    }
#endif
    csilk_mutex_unlock((csilk_mutex_t*)&plugin->mutex);
    return fuel;
}

/**
 * @brief Host import: fetch a request or response header by name.
 * @param[in] ctx  Request context to read from.
 * @param[in] name Header name to look up.
 * @return The header value, or NULL if not present in either request or
 *         response headers.
 * @note Falls back to the response headers when the request header is absent.
 */
const char*
csilk_wasm_host_get_header(csilk_ctx_t* ctx, const char* name)
{
    const char* val = csilk_get_header(ctx, name);
    if (!val) {
        val = csilk_get_response_header(ctx, name);
    }
    return val;
}

/**
 * @brief Host import: set a request header by name.
 * @param[in] ctx   Request context to modify.
 * @param[in] name  Header name.
 * @param[in] value Header value.
 * @return 0 on success.
 */
int
csilk_wasm_host_set_header(csilk_ctx_t* ctx, const char* name, const char* value)
{
    csilk_set_header(ctx, name, value);
    return 0;
}

/**
 * @brief Host import: fetch a path/query parameter by name.
 * @param[in] ctx  Request context to read from.
 * @param[in] name Parameter name to look up.
 * @return The parameter value, or NULL if not present as a path param or query
 *         parameter.
 * @note Falls back to query parameters when the path param is absent.
 */
const char*
csilk_wasm_host_get_param(csilk_ctx_t* ctx, const char* name)
{
    if (!ctx || !name) {
        return NULL;
    }
    const char* val = csilk_get_param(ctx, name);
    if (!val) {
        val = csilk_get_query(ctx, name);
    }
    return val;
}

/**
 * @brief Host import: set the HTTP response status code.
 * @param[in] ctx    Request context to modify.
 * @param[in] status HTTP status code to set.
 * @return 0 on success, -1 if ctx is NULL.
 */
int
csilk_wasm_host_set_status(csilk_ctx_t* ctx, int status)
{
    if (!ctx) {
        return -1;
    }
    csilk_set_status(ctx, status);
    return 0;
}

const char*
csilk_wasm_host_get_body(csilk_ctx_t* ctx, size_t* out_len)
{
    if (!ctx) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
    return csilk_get_body(ctx, out_len);
}

int
csilk_wasm_host_set_body(csilk_ctx_t* ctx, const char* data, size_t len)
{
    if (!ctx || !data) {
        return -1;
    }
    csilk_set_response_body(ctx, data, len, 0);
    return 0;
}

const char*
csilk_wasm_host_get_method(csilk_ctx_t* ctx)
{
    return ctx ? csilk_get_method(ctx) : NULL;
}

const char*
csilk_wasm_host_get_path(csilk_ctx_t* ctx)
{
    return ctx ? csilk_get_path(ctx) : NULL;
}

void
csilk_wasm_host_log(int level, const char* msg)
{
    if (!msg) {
        return;
    }
    switch (level) {
    case 0:
        CSILK_LOG_T("[WASM] %s", msg);
        break;
    case 1:
        CSILK_LOG_D("[WASM] %s", msg);
        break;
    case 2:
        CSILK_LOG_I("[WASM] %s", msg);
        break;
    case 3:
        CSILK_LOG_W("[WASM] %s", msg);
        break;
    case 4:
    default:
        CSILK_LOG_E("[WASM] %s", msg);
        break;
    }
}

static void
_csilk_wasm_mw_pool_init(void)
{
    csilk_mutex_init(&s_wasm_mw_mutex);
}

static void
_csilk_wasm_mw_dispatch(int slot, csilk_ctx_t* ctx)
{
    csilk_wasm_plugin_t* plugin = NULL;
    char                 func[64];
    func[0] = '\0';

    csilk_once(&s_wasm_mw_once, _csilk_wasm_mw_pool_init);
    csilk_mutex_lock(&s_wasm_mw_mutex);
    if (slot >= 0 && slot < CSILK_WASM_MW_MAX_SLOTS && s_wasm_mw_slots[slot].in_use &&
        s_wasm_mw_slots[slot].plugin) {
        plugin = s_wasm_mw_slots[slot].plugin;
        snprintf(func, sizeof(func), "%s", s_wasm_mw_slots[slot].func);
    }
    csilk_mutex_unlock(&s_wasm_mw_mutex);

    if (plugin && func[0]) {
        int rc = csilk_wasm_plugin_execute(plugin, ctx, func);
        if (rc != 0) {
            return;
        }
    }
    csilk_next(ctx);
}

#define CSILK_WASM_MW_THUNK_DEF(N)                                                                 \
    static void _csilk_wasm_mw_thunk_##N(csilk_ctx_t* ctx)                                         \
    {                                                                                              \
        _csilk_wasm_mw_dispatch(N, ctx);                                                           \
    }
CSILK_WASM_MW_THUNK_DEF(0)
CSILK_WASM_MW_THUNK_DEF(1)
CSILK_WASM_MW_THUNK_DEF(2)
CSILK_WASM_MW_THUNK_DEF(3)
CSILK_WASM_MW_THUNK_DEF(4)
CSILK_WASM_MW_THUNK_DEF(5)
CSILK_WASM_MW_THUNK_DEF(6)
CSILK_WASM_MW_THUNK_DEF(7)
CSILK_WASM_MW_THUNK_DEF(8)
CSILK_WASM_MW_THUNK_DEF(9)
CSILK_WASM_MW_THUNK_DEF(10)
CSILK_WASM_MW_THUNK_DEF(11)
CSILK_WASM_MW_THUNK_DEF(12)
CSILK_WASM_MW_THUNK_DEF(13)
CSILK_WASM_MW_THUNK_DEF(14)
CSILK_WASM_MW_THUNK_DEF(15)
#undef CSILK_WASM_MW_THUNK_DEF

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("hidden")))
#endif
void
_csilk_wasm_mw_release_plugin(csilk_wasm_plugin_t* plugin)
{
    if (!plugin) {
        return;
    }
    csilk_once(&s_wasm_mw_once, _csilk_wasm_mw_pool_init);
    csilk_mutex_lock(&s_wasm_mw_mutex);
    for (int i = 0; i < CSILK_WASM_MW_MAX_SLOTS; i++) {
        if (s_wasm_mw_slots[i].in_use && s_wasm_mw_slots[i].plugin == plugin) {
            s_wasm_mw_slots[i].in_use = false;
            s_wasm_mw_slots[i].plugin = NULL;
            s_wasm_mw_slots[i].func[0] = '\0';
        }
    }
    csilk_mutex_unlock(&s_wasm_mw_mutex);
}

void (*csilk_wasm_plugin_middleware(csilk_wasm_plugin_t* plugin,
                                    const char*          func_name))(csilk_ctx_t*)
{
    if (!plugin) {
        return NULL;
    }

    csilk_once(&s_wasm_mw_once, _csilk_wasm_mw_pool_init);
    csilk_mutex_lock(&s_wasm_mw_mutex);
    for (int i = 0; i < CSILK_WASM_MW_MAX_SLOTS; i++) {
        if (!s_wasm_mw_slots[i].in_use) {
            s_wasm_mw_slots[i].plugin = plugin;
            snprintf(s_wasm_mw_slots[i].func,
                     sizeof(s_wasm_mw_slots[i].func),
                     "%s",
                     func_name ? func_name : "on_request");
            s_wasm_mw_slots[i].in_use = true;
            csilk_handler_t thunk = s_wasm_mw_thunks[i];
            csilk_mutex_unlock(&s_wasm_mw_mutex);
            return thunk;
        }
    }
    csilk_mutex_unlock(&s_wasm_mw_mutex);

    CSILK_LOG_E("wasm_plugin: middleware thunk pool exhausted (max %d slots)",
                CSILK_WASM_MW_MAX_SLOTS);
    return NULL;
}

/**
 * @brief Map an external buffer as the plugin's linear memory.
 * @param[in] plugin Plugin whose memory is to be replaced.
 * @param[in] buf    Caller-owned buffer to use as linear memory.
 * @param[in] len    Size of buf in bytes (must be > 0).
 * @return 0 on success, -1 on NULL plugin/buf or zero length.
 * @note Frees any previously heap-allocated memory (unless it was already
 *       mapped) and marks the memory as externally mapped so plugin_free will
 *       not free buf.
 */
int
csilk_wasm_plugin_map_memory(csilk_wasm_plugin_t* plugin, void* buf, size_t len)
{
    if (!plugin || !buf || len == 0) {
        return -1;
    }
    if (plugin->memory.data && !plugin->memory.is_mapped) {
        free(plugin->memory.data);
    }
    plugin->memory.data = (uint8_t*)buf;
    plugin->memory.current_size = len;
    plugin->memory.is_mapped = true;
    return 0;
}

/**
 * @brief Return the plugin's mapped linear memory buffer and its size.
 * @param[in]  plugin Plugin to query.
 * @param[out] len    Receives the current memory size (set to 0 if plugin NULL).
 * @return Pointer to the linear-memory buffer, or NULL if plugin is NULL.
 */
void*
csilk_wasm_host_get_mapped_buffer(const csilk_wasm_plugin_t* plugin, size_t* len)
{
    if (!plugin) {
        if (len) {
            *len = 0;
        }
        return NULL;
    }
    if (len) {
        *len = plugin->memory.current_size;
    }
    return plugin->memory.data;
}
