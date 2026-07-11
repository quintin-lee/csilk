#include "workflow_internal.h"
#include "csilk/core/sync.h"

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
        entry->description = description ? strdup(description) : nullptr;
        entry->parameters_json = parameters_json ? strdup(parameters_json) : nullptr;
        entry->fn = fn;
        entry->user_data = user_data;
    }
    csilk_mutex_unlock(&wf->monitor_mutex);
}

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
