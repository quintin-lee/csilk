#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/app/workflow_dsl.h"
#include "csilk/core/wasm.h"
#include "csilk/core/wasm_plugin.h"
#include "csilk/protocols/mcp.h"
#include "wasm_internal.h"

csilk_wasm_plugin_t*
csilk_wasm_plugin_load(const uint8_t* wasm_bytes, size_t size)
{
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

    csilk_wasm_plugin_t* plugin = calloc(1, sizeof(csilk_wasm_plugin_t));
    if (!plugin) {
        return NULL;
    }

    csilk_mutex_init(&plugin->mutex);
    plugin->bytecode_size = size;
    plugin->bytecode = calloc(1, size);
    if (plugin->bytecode) {
        memcpy(plugin->bytecode, wasm_bytes, size);
    }
    plugin->memory.initial_pages = 1;
    plugin->memory.current_size = CSILK_WASM_PAGE_SIZE;
    plugin->memory.data = calloc(1, plugin->memory.current_size);
    return plugin;
}

int
csilk_wasm_plugin_is_valid(const csilk_wasm_plugin_t* plugin)
{
    return (plugin && plugin->bytecode && plugin->bytecode_size >= 8) ? 1 : 0;
}

int
csilk_wasm_plugin_execute(csilk_wasm_plugin_t* plugin, csilk_ctx_t* ctx, const char* func_name)
{
    if (!csilk_wasm_plugin_is_valid(plugin) || !func_name) {
        return -1;
    }
    (void)ctx;
    return 0;
}

const char*
csilk_wasm_host_get_header(csilk_ctx_t* ctx, const char* name)
{
    return csilk_get_header(ctx, name);
}

int
csilk_wasm_host_set_header(csilk_ctx_t* ctx, const char* name, const char* value)
{
    csilk_set_header(ctx, name, value);
    return 0;
}

const char*
csilk_wasm_host_get_param(csilk_ctx_t* ctx, const char* name)
{
    return csilk_get_param(ctx, name);
}

int
csilk_wasm_host_set_status(csilk_ctx_t* ctx, int status)
{
    (void)ctx;
    (void)status;
    return 0;
}

int
csilk_wasm_plugin_map_memory(csilk_wasm_plugin_t* plugin, void* buf, size_t len)
{
    if (!plugin || !buf || len == 0) {
        return -1;
    }
    return 0;
}

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

static csilk_data_t*
csilk_wasm_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    if (!ctx || !user_data) {
        return NULL;
    }
    (void)input;
    const char*          wasm_filepath = (const char*)user_data;
    csilk_wasm_plugin_t* plugin = csilk_wasm_plugin_load_file(wasm_filepath, NULL);
    if (!plugin) {
        return NULL;
    }

    char          err_buf[128];
    char*         output = csilk_wasm_plugin_exec(plugin, "run", "{}", err_buf, sizeof(err_buf));
    csilk_data_t* res_data = NULL;
    if (output) {
        res_data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
        if (res_data) {
            res_data->type = csilk_wf_strdup(ctx, "application/json");
            res_data->value = csilk_wf_strdup(ctx, output);
            res_data->free_fn = NULL;
            res_data->meta = NULL;
        }
        free(output);
    }

    csilk_wasm_plugin_free(plugin);
    return res_data;
}

int
csilk_wf_add_wasm_node(csilk_wf_t* wf, const char* node_id, const char* wasm_filepath)
{
    if (!wf || !node_id || !wasm_filepath) {
        return -1;
    }

    csilk_wf_node_t* node =
        csilk_wf_add(wf, node_id, csilk_wasm_node_handler, strdup(wasm_filepath));
    return node != NULL ? 0 : -1;
}

int
csilk_mcp_server_register_wasm_tool(csilk_mcp_server_t* server,
                                    const char*         wasm_filepath,
                                    const char*         tool_name,
                                    const char*         description)
{
    if (!server || !wasm_filepath || !tool_name) {
        return -1;
    }

    csilk_wf_tool_entry_t tool;
    memset(&tool, 0, sizeof(tool));
    tool.name = (char*)tool_name;
    tool.description = (char*)description;
    tool.parameters_json = "{\"type\":\"object\"}";

    return csilk_mcp_server_register_tool(server, &tool);
}
