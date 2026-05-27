/**
 * @file workflow_loader.c
 * @brief Declarative YAML/JSON loader for AI Workflows.
 */

#include "csilk/app/workflow.h"
#include "cJSON.h"
#include <yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Global Handler Registry --- */

typedef struct handler_entry_s {
    char* name;
    csilk_wf_handler_t handler;
    struct handler_entry_s* next;
} handler_entry_t;

static handler_entry_t* g_handlers = NULL;

void csilk_wf_register_handler(const char* name, csilk_wf_handler_t handler) {
    if (!name || !handler) return;
    handler_entry_t* entry = calloc(1, sizeof(handler_entry_t));
    entry->name = strdup(name);
    entry->handler = handler;
    entry->next = g_handlers;
    g_handlers = entry;
}

static csilk_wf_handler_t find_handler(const char* name) {
    handler_entry_t* curr = g_handlers;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            return curr->handler;
        }
        curr = curr->next;
    }
    return NULL;
}

/* --- JSON Parser --- */

csilk_wf_t* csilk_wf_from_json(const char* json_str) {
    if (!json_str) return NULL;
    
    cJSON* root = cJSON_Parse(json_str);
    if (!root) return NULL;
    
    cJSON* name_item = cJSON_GetObjectItem(root, "name");
    const char* wf_name = (cJSON_IsString(name_item)) ? name_item->valuestring : "DeclarativeWF";
    
    csilk_wf_t* wf = csilk_wf_new(wf_name);
    if (!wf) {
        cJSON_Delete(root);
        return NULL;
    }
    
    cJSON* steps = cJSON_GetObjectItem(root, "steps");
    if (cJSON_IsArray(steps)) {
        int count = cJSON_GetArraySize(steps);
        for (int i = 0; i < count; i++) {
            cJSON* step = cJSON_GetArrayItem(steps, i);
            cJSON* id_item = cJSON_GetObjectItem(step, "id");
            cJSON* type_item = cJSON_GetObjectItem(step, "type");
            
            if (!cJSON_IsString(id_item)) continue;
            const char* id = id_item->valuestring;
            const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "handler";
            
            csilk_wf_node_t* node = NULL;
            if (strcmp(type, "ai") == 0) {
                cJSON* config = cJSON_GetObjectItem(step, "config");
                csilk_ai_config_t aic = {0};
                if (cJSON_IsObject(config)) {
                    cJSON* model = cJSON_GetObjectItem(config, "model");
                    cJSON* prompt = cJSON_GetObjectItem(config, "prompt");
                    cJSON* sys = cJSON_GetObjectItem(config, "system_msg");
                    if (cJSON_IsString(model)) aic.model = model->valuestring;
                    if (cJSON_IsString(prompt)) aic.prompt = prompt->valuestring;
                    if (cJSON_IsString(sys)) aic.system_msg = sys->valuestring;
                }
                node = csilk_wf_add_ai(wf, id, &aic);
            } else {
                cJSON* handler_item = cJSON_GetObjectItem(step, "handler");
                if (cJSON_IsString(handler_item)) {
                    csilk_wf_handler_t h = find_handler(handler_item->valuestring);
                    if (h) {
                        node = csilk_wf_add(wf, id, h, NULL);
                    } else {
                        printf("[Workflow] Warning: Handler '%s' not registered for step '%s'.\n", handler_item->valuestring, id);
                    }
                }
            }
            
            if (node) {
                cJSON* entry_item = cJSON_GetObjectItem(step, "entry");
                if (cJSON_IsTrue(entry_item)) {
                    csilk_wf_node_set_entry(node, 1);
                }
                
                cJSON* join_item = cJSON_GetObjectItem(step, "join");
                if (cJSON_IsString(join_item) && strcmp(join_item->valuestring, "or") == 0) {
                    csilk_wf_node_set_join(node, CSILK_WF_JOIN_OR);
                }
            }
        }
    }
    
    // Pass 2: Connections
    cJSON* conns = cJSON_GetObjectItem(root, "connections");
    if (cJSON_IsArray(conns)) {
        int count = cJSON_GetArraySize(conns);
        for (int i = 0; i < count; i++) {
            cJSON* conn = cJSON_GetArrayItem(conns, i);
            cJSON* from_item = cJSON_GetObjectItem(conn, "from");
            cJSON* to_item = cJSON_GetObjectItem(conn, "to");
            if (!cJSON_IsString(from_item) || !cJSON_IsString(to_item)) continue;
            
            csilk_wf_node_t* n_from = csilk_wf_get_node(wf, from_item->valuestring);
            csilk_wf_node_t* n_to = csilk_wf_get_node(wf, to_item->valuestring);
            
            if (n_from && n_to) {
                cJSON* cond_item = cJSON_GetObjectItem(conn, "condition");
                cJSON* loop_item = cJSON_GetObjectItem(conn, "loop");
                const char* cond = cJSON_IsString(cond_item) ? cond_item->valuestring : NULL;
                
                if (cJSON_IsTrue(loop_item)) {
                    csilk_wf_on_loop(n_from, cond, n_to);
                } else if (cond) {
                    csilk_wf_on(n_from, cond, n_to);
                } else {
                    csilk_wf_bind(n_from, n_to);
                }
            }
        }
    }
    
    // Pass 3: Error Targets
    if (cJSON_IsArray(steps)) {
        int count = cJSON_GetArraySize(steps);
        for (int i = 0; i < count; i++) {
            cJSON* step = cJSON_GetArrayItem(steps, i);
            cJSON* id_item = cJSON_GetObjectItem(step, "id");
            cJSON* err_item = cJSON_GetObjectItem(step, "on_error");
            if (cJSON_IsString(id_item) && cJSON_IsString(err_item)) {
                csilk_wf_node_t* n = csilk_wf_get_node(wf, id_item->valuestring);
                csilk_wf_node_t* err_target = csilk_wf_get_node(wf, err_item->valuestring);
                if (n && err_target) {
                    csilk_wf_on_error(n, err_target);
                }
            }
        }
    }
    
    cJSON_Delete(root);
    return wf;
}

/* --- YAML Loader --- */

static cJSON* parse_yaml_file(const char* path) {
    FILE* fh = fopen(path, "rb");
    if (!fh) return NULL;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(fh);
        return NULL;
    }
    yaml_parser_set_input_file(&parser, fh);

    cJSON* root = NULL;
    cJSON* stack[64];
    int stack_ptr = 0;
    
    // To handle mapping keys:
    char* current_key = NULL;

    yaml_event_t event;
    int done = 0;
    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) break;

        switch (event.type) {
            case YAML_MAPPING_START_EVENT: {
                cJSON* obj = cJSON_CreateObject();
                if (!root) { root = obj; stack[stack_ptr++] = obj; }
                else {
                    cJSON* parent = stack[stack_ptr - 1];
                    if (cJSON_IsArray(parent)) cJSON_AddItemToArray(parent, obj);
                    else if (cJSON_IsObject(parent) && current_key) {
                        cJSON_AddItemToObject(parent, current_key, obj);
                        free(current_key); current_key = NULL;
                    }
                    stack[stack_ptr++] = obj;
                }
                break;
            }
            case YAML_SEQUENCE_START_EVENT: {
                cJSON* arr = cJSON_CreateArray();
                if (!root) { root = arr; stack[stack_ptr++] = arr; }
                else {
                    cJSON* parent = stack[stack_ptr - 1];
                    if (cJSON_IsArray(parent)) cJSON_AddItemToArray(parent, arr);
                    else if (cJSON_IsObject(parent) && current_key) {
                        cJSON_AddItemToObject(parent, current_key, arr);
                        free(current_key); current_key = NULL;
                    }
                    stack[stack_ptr++] = arr;
                }
                break;
            }
            case YAML_SCALAR_EVENT: {
                cJSON* parent = stack_ptr > 0 ? stack[stack_ptr - 1] : NULL;
                if (parent) {
                    if (cJSON_IsObject(parent)) {
                        if (!current_key) {
                            current_key = strdup((char*)event.data.scalar.value);
                        } else {
                            const char* val = (char*)event.data.scalar.value;
                            cJSON* scalar = NULL;
                            if (strcmp(val, "true") == 0) scalar = cJSON_CreateBool(1);
                            else if (strcmp(val, "false") == 0) scalar = cJSON_CreateBool(0);
                            else scalar = cJSON_CreateString(val);
                            cJSON_AddItemToObject(parent, current_key, scalar);
                            free(current_key); current_key = NULL;
                        }
                    } else if (cJSON_IsArray(parent)) {
                        const char* val = (char*)event.data.scalar.value;
                        cJSON* scalar = NULL;
                        if (strcmp(val, "true") == 0) scalar = cJSON_CreateBool(1);
                        else if (strcmp(val, "false") == 0) scalar = cJSON_CreateBool(0);
                        else scalar = cJSON_CreateString(val);
                        cJSON_AddItemToArray(parent, scalar);
                    }
                }
                break;
            }
            case YAML_MAPPING_END_EVENT:
            case YAML_SEQUENCE_END_EVENT:
                if (stack_ptr > 0) stack_ptr--;
                break;
            case YAML_STREAM_END_EVENT:
                done = 1;
                break;
            default:
                break;
        }
        yaml_event_delete(&event);
    }
    yaml_parser_delete(&parser);
    fclose(fh);
    if (current_key) free(current_key);
    return root;
}

csilk_wf_t* csilk_wf_load_yaml(const char* path) {
    cJSON* root = parse_yaml_file(path);
    if (!root) return NULL;
    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    csilk_wf_t* wf = csilk_wf_from_json(json_str);
    free(json_str);
    return wf;
}

