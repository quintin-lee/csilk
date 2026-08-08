#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "workflow_internal.h"

csilk_wf_t*
csilk_wf_from_json_ext(const char* json_str, char* err_buf, size_t err_len)
{
    if (!json_str || !*json_str) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Empty JSON string");
        }
        return nullptr;
    }

    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "JSON parse error near '%s'", cJSON_GetErrorPtr());
        }
        return nullptr;
    }

    cJSON*      j_name = cJSON_GetObjectItem(root, "name");
    const char* wf_name = (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : "dsl_workflow";

    csilk_wf_t* wf = csilk_wf_new(wf_name);
    if (!wf) {
        cJSON_Delete(root);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Failed to create workflow instance");
        }
        return nullptr;
    }

    cJSON* j_budget = cJSON_GetObjectItem(root, "budget");
    if (j_budget && cJSON_IsObject(j_budget)) {
        cJSON* j_tokens = cJSON_GetObjectItem(j_budget, "max_tokens");
        if (j_tokens && cJSON_IsNumber(j_tokens)) {
            csilk_wf_set_budget(wf, j_tokens->valueint);
        }
    }

    cJSON* j_persistence = cJSON_GetObjectItem(root, "persistence");
    if (j_persistence && cJSON_IsObject(j_persistence)) {
        cJSON* j_enabled = cJSON_GetObjectItem(j_persistence, "enabled");
        cJSON* j_dir = cJSON_GetObjectItem(j_persistence, "wal_dir");
        if (j_enabled && cJSON_IsTrue(j_enabled) && j_dir && cJSON_IsString(j_dir)) {
            csilk_wf_set_persistence(wf, j_dir->valuestring);
        }
    }

    cJSON* j_nodes = cJSON_GetObjectItem(root, "nodes");
    if (j_nodes && cJSON_IsArray(j_nodes)) {
        int node_count = cJSON_GetArraySize(j_nodes);
        for (int i = 0; i < node_count; i++) {
            cJSON* item = cJSON_GetArrayItem(j_nodes, i);
            if (!item) {
                continue;
            }

            cJSON* j_id = cJSON_GetObjectItem(item, "id");
            cJSON* j_type = cJSON_GetObjectItem(item, "type");
            if (!j_id || !cJSON_IsString(j_id) || !j_type || !cJSON_IsString(j_type)) {
                continue;
            }

            const char* id = j_id->valuestring;
            const char* type = j_type->valuestring;
            cJSON*      j_cfg = cJSON_GetObjectItem(item, "config");

            if (strcmp(type, "ai_chat") == 0) {
                csilk_ai_config_t cfg = {0};
                if (j_cfg) {
                    cJSON* j_m = cJSON_GetObjectItem(j_cfg, "model");
                    cJSON* j_sp = cJSON_GetObjectItem(j_cfg, "system_prompt");
                    if (j_m && cJSON_IsString(j_m)) {
                        cfg.model = j_m->valuestring;
                    }
                    if (j_sp && cJSON_IsString(j_sp)) {
                        cfg.system_msg = j_sp->valuestring;
                    }
                }
                csilk_wf_add_ai(wf, id, &cfg);
            } else if (strcmp(type, "agent_react") == 0) {
                csilk_agent_react_config_t cfg = {0};
                if (j_cfg) {
                    cJSON* j_m = cJSON_GetObjectItem(j_cfg, "model");
                    cJSON* j_sp = cJSON_GetObjectItem(j_cfg, "system_prompt");
                    if (j_m && cJSON_IsString(j_m)) {
                        cfg.model = j_m->valuestring;
                    }
                    if (j_sp && cJSON_IsString(j_sp)) {
                        cfg.system_prompt = j_sp->valuestring;
                    }
                }
                csilk_wf_add_agent_react(wf, id, &cfg);
            }
        }

        /* Wire dependencies */
        for (int i = 0; i < node_count; i++) {
            cJSON* item = cJSON_GetArrayItem(j_nodes, i);
            if (!item) {
                continue;
            }

            cJSON* j_id = cJSON_GetObjectItem(item, "id");
            cJSON* j_deps = cJSON_GetObjectItem(item, "depends_on");
            if (!j_id || !cJSON_IsString(j_id) || !j_deps || !cJSON_IsArray(j_deps)) {
                continue;
            }

            csilk_wf_node_t* target = csilk_wf_get_node(wf, j_id->valuestring);
            if (!target) {
                continue;
            }

            int dep_count = cJSON_GetArraySize(j_deps);
            for (int d = 0; d < dep_count; d++) {
                cJSON* dep_item = cJSON_GetArrayItem(j_deps, d);
                if (dep_item && cJSON_IsString(dep_item)) {
                    csilk_wf_node_t* src = csilk_wf_get_node(wf, dep_item->valuestring);
                    if (src) {
                        csilk_wf_bind(src, target);
                    }
                }
            }
        }
    }

    cJSON_Delete(root);
    return wf;
}

csilk_wf_t*
csilk_wf_from_file(const char* filepath, char* err_buf, size_t err_len)
{
    if (!filepath) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Null file path");
        }
        return nullptr;
    }

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Failed to open file: %s", filepath);
        }
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "File is empty: %s", filepath);
        }
        return nullptr;
    }

    char* buf = (char*)calloc(1, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }

    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    csilk_wf_t* wf = csilk_wf_from_json_ext(buf, err_buf, err_len);
    free(buf);
    return wf;
}

char*
csilk_wf_to_json(csilk_wf_t* wf)
{
    if (!wf) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", wf->name ? wf->name : "dsl_workflow");
    cJSON_AddStringToObject(root, "version", "1.0.0");

    cJSON* j_nodes = cJSON_CreateArray();
    for (size_t i = 0; i < wf->node_count; i++) {
        csilk_wf_node_t* n = wf->nodes[i];
        cJSON*           item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", n->id);
        cJSON_AddStringToObject(item, "type", "generic");
        cJSON_AddItemToArray(j_nodes, item);
    }
    cJSON_AddItemToObject(root, "nodes", j_nodes);

    char* formatted = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return formatted;
}
