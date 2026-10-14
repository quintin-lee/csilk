/**
 * @file tool_registry.c
 * @brief Unified AI tool registry with JSON Schema validation and dispatch.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/ai/tool_registry.h"
#include "csilk/core/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char*                 name;
    char*                 description;
    char*                 schema_json_str;
    csilk_json_t*         schema_obj;
    csilk_ai_tool_exec_fn fn;
    void*                 user_data;
} tool_item_t;

struct csilk_ai_tool_registry_s {
    tool_item_t*  items;
    size_t        count;
    size_t        capacity;
    csilk_mutex_t mutex;
};

csilk_ai_tool_registry_t*
csilk_ai_tool_registry_new(void)
{
    csilk_ai_tool_registry_t* reg = calloc(1, sizeof(csilk_ai_tool_registry_t));
    if (!reg) {
        return NULL;
    }
    csilk_mutex_init(&reg->mutex);
    return reg;
}

static void
free_tool_item(tool_item_t* item)
{
    if (!item) {
        return;
    }
    free(item->name);
    free(item->description);
    free(item->schema_json_str);
    if (item->schema_obj) {
        csilk_json_free(item->schema_obj);
    }
}

void
csilk_ai_tool_registry_free(csilk_ai_tool_registry_t* reg)
{
    if (!reg) {
        return;
    }
    csilk_mutex_lock(&reg->mutex);
    for (size_t i = 0; i < reg->count; i++) {
        free_tool_item(&reg->items[i]);
    }
    free(reg->items);
    csilk_mutex_unlock(&reg->mutex);
    csilk_mutex_destroy(&reg->mutex);
    free(reg);
}

int
csilk_ai_tool_registry_register(csilk_ai_tool_registry_t* reg,
                                const char*               name,
                                const char*               description,
                                const char*               parameters_schema_json,
                                csilk_ai_tool_exec_fn     fn,
                                void*                     user_data)
{
    if (!reg || !name || !fn) {
        return -1;
    }

    csilk_mutex_lock(&reg->mutex);

    /* Check for duplicate name */
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->items[i].name, name) == 0) {
            csilk_mutex_unlock(&reg->mutex);
            return -1;
        }
    }

    if (reg->count >= reg->capacity) {
        size_t       new_cap = reg->capacity == 0 ? 8 : reg->capacity * 2;
        tool_item_t* new_items = realloc(reg->items, sizeof(tool_item_t) * new_cap);
        if (!new_items) {
            csilk_mutex_unlock(&reg->mutex);
            return -1;
        }
        reg->items = new_items;
        reg->capacity = new_cap;
    }

    tool_item_t* cur = &reg->items[reg->count];
    cur->name = strdup(name);
    cur->description = description ? strdup(description) : NULL;
    cur->schema_json_str = parameters_schema_json ? strdup(parameters_schema_json) : NULL;
    cur->schema_obj = parameters_schema_json ? csilk_json_parse(parameters_schema_json) : NULL;
    cur->fn = fn;
    cur->user_data = user_data;
    reg->count++;

    csilk_mutex_unlock(&reg->mutex);
    return 0;
}

int
csilk_ai_tool_registry_unregister(csilk_ai_tool_registry_t* reg, const char* name)
{
    if (!reg || !name) {
        return -1;
    }

    csilk_mutex_lock(&reg->mutex);
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->items[i].name, name) == 0) {
            free_tool_item(&reg->items[i]);
            reg->items[i] = reg->items[--reg->count];
            csilk_mutex_unlock(&reg->mutex);
            return 0;
        }
    }
    csilk_mutex_unlock(&reg->mutex);
    return -1;
}

csilk_ai_tool_t*
csilk_ai_tool_registry_export_tools(csilk_ai_tool_registry_t* reg, size_t* out_count)
{
    if (!reg || !out_count) {
        return NULL;
    }

    csilk_mutex_lock(&reg->mutex);
    if (reg->count == 0) {
        *out_count = 0;
        csilk_mutex_unlock(&reg->mutex);
        return NULL;
    }

    csilk_ai_tool_t* tools = calloc(reg->count, sizeof(csilk_ai_tool_t));
    if (!tools) {
        *out_count = 0;
        csilk_mutex_unlock(&reg->mutex);
        return NULL;
    }

    for (size_t i = 0; i < reg->count; i++) {
        tools[i].type = "function";
        tools[i].function.name = strdup(reg->items[i].name);
        tools[i].function.description =
            reg->items[i].description ? strdup(reg->items[i].description) : NULL;
        if (reg->items[i].schema_obj) {
            tools[i].function.parameters_json = csilk_json_copy(reg->items[i].schema_obj);
        } else {
            tools[i].function.parameters_json = csilk_json_object();
        }
    }

    *out_count = reg->count;
    csilk_mutex_unlock(&reg->mutex);
    return tools;
}

void
csilk_ai_tools_free(csilk_ai_tool_t* tools, size_t count)
{
    if (!tools) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free((void*)tools[i].function.name);
        free((void*)tools[i].function.description);
        if (tools[i].function.parameters_json) {
            csilk_json_free((csilk_json_t*)tools[i].function.parameters_json);
        }
    }
    free(tools);
}

char*
csilk_ai_tool_dispatch(csilk_ai_tool_registry_t* reg, const char* name, const char* args_json)
{
    if (!reg || !name) {
        return NULL;
    }

    csilk_mutex_lock(&reg->mutex);
    tool_item_t item = {0};
    bool        found = false;

    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->items[i].name, name) == 0) {
            item = reg->items[i];
            found = true;
            break;
        }
    }
    csilk_mutex_unlock(&reg->mutex);

    if (!found || !item.fn) {
        return strdup("{\"error\":\"tool not found\"}");
    }

    /* JSON Schema validation if schema is present */
    if (item.schema_obj) {
        csilk_json_t* args = csilk_json_parse(args_json ? args_json : "{}");
        if (!args) {
            return strdup("{\"error\":\"invalid JSON arguments\"}");
        }

        csilk_json_t* required = csilk_json_get(item.schema_obj, "required");
        if (required && csilk_json_is_array(required)) {
            size_t n_req = csilk_json_array_size(required);
            for (size_t r = 0; r < n_req; r++) {
                csilk_json_t* req_field = csilk_json_array_get(required, r);
                const char*   fname = csilk_json_string_value(req_field);
                if (fname && !csilk_json_get(args, fname)) {
                    csilk_json_free(args);
                    char err_msg[256];
                    snprintf(err_msg,
                             sizeof(err_msg),
                             "{\"error\":\"missing required parameter: %s\"}",
                             fname);
                    return strdup(err_msg);
                }
            }
        }
        csilk_json_free(args);
    }

    return item.fn(args_json ? args_json : "{}", item.user_data);
}

size_t
csilk_ai_tool_registry_get_count(csilk_ai_tool_registry_t* reg)
{
    if (!reg) {
        return 0;
    }
    csilk_mutex_lock(&reg->mutex);
    size_t count = reg->count;
    csilk_mutex_unlock(&reg->mutex);
    return count;
}
