/**
 * @file wf_tools.c
 * @brief Workflow tool registration and dynamic tool-discovery hooks.
 *
 * @copyright MIT License
 */

#include "workflow_internal.h"
#include "csilk/core/sync.h"
#include "csilk/core/plugin/wasm.h"

/**
 * @brief Workflow node handler that loads and executes a WASM file.
 */
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

/**
 * @brief Register a WASM module as a workflow node.
 */
int
csilk_wf_add_wasm_node(csilk_wf_t* wf, const char* node_id, const char* wasm_filepath)
{
    if (!wf || !node_id || !wasm_filepath) {
        return -1;
    }

    csilk_wf_node_t* node =
        csilk_wf_add(wf, node_id, csilk_wasm_node_handler, strdup(wasm_filepath));
    if (node) {
        csilk_wf_node_set_free(node, free);
    }
    return node != NULL ? 0 : -1;
}

/**
 * @brief Registers a static tool (function) callable by AI workflow nodes.
 *
 * Appends a tool entry to the workflow's tool table, copying the name,
 * description, and parameter JSON schema. The tool's function is invoked by
 * the AI node handler when the model emits a matching tool call.
 *
 * @param wf             The workflow instance.
 * @param name           Unique tool name (must not be NULL).
 * @param description     Human-readable tool description (may be NULL).
 * @param parameters_json JSON schema describing the tool's parameters (may be NULL).
 * @param fn             Tool implementation callback (must not be NULL).
 * @param user_data      Opaque pointer passed to fn when invoked.
 * @note Guarded by wf->monitor_mutex. No-op if wf, name, or fn is NULL.
 */
void
csilk_wf_register_tool(csilk_wf_t*      wf,
                       const char*      name,
                       const char*      description,
                       const char*      parameters_json,
                       csilk_wf_tool_fn fn,
                       void*            user_data)
{
    if (!wf || !name || !fn) {
        return;
    }
    csilk_mutex_lock(&wf->monitor_mutex);
    if (wf->tool_count >= wf->tool_capacity) {
        size_t                 new_cap = wf->tool_capacity == 0 ? 4 : wf->tool_capacity * 2;
        csilk_wf_tool_entry_t* new_tools =
            realloc(wf->tools, sizeof(csilk_wf_tool_entry_t) * new_cap);
        if (new_tools) {
            wf->tools = new_tools;
            wf->tool_capacity = new_cap;
        }
    }
    if (wf->tool_count < wf->tool_capacity) {
        csilk_wf_tool_entry_t* entry = &wf->tools[wf->tool_count++];
        entry->name = strdup(name);
        entry->description = description ? strdup(description) : NULL;
        entry->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
        entry->fn = fn;
        entry->user_data = user_data;
    }
    csilk_mutex_unlock(&wf->monitor_mutex);
}

/**
 * @brief Installs a dynamic tool-discovery callback on the workflow.
 *
 * The discovery function is called at AI node execution time to resolve
 * additional tools (beyond the statically registered ones) based on context.
 *
 * @param wf        The workflow instance.
 * @param discovery Discovery callback (may be NULL to clear).
 * @param user_data Opaque pointer forwarded to the discovery callback.
 * @note Guarded by wf->monitor_mutex. No-op if wf is NULL.
 */
void
csilk_wf_set_tool_discovery(csilk_wf_t* wf, csilk_wf_tool_discovery_fn discovery, void* user_data)
{
    if (!wf) {
        return;
    }
    csilk_mutex_lock(&wf->monitor_mutex);
    wf->tool_discovery = discovery;
    wf->tool_discovery_user_data = user_data;
    csilk_mutex_unlock(&wf->monitor_mutex);
}
