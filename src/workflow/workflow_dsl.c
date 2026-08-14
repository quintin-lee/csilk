/**
 * @file workflow_dsl.c
 * @brief Declarative workflow DSL parser: builds workflow graphs from JSON
 *        definitions supplied as in-memory strings or files.
 *
 * @copyright MIT License
 */

#include "csilk/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "workflow_internal.h"

/**
 * @brief Builds a workflow from a JSON DSL string with detailed error reporting.
 *
 * Parses the "name", "budget", "persistence", "nodes" (ai_chat / agent_react
 * types with optional config) and "depends_on" wiring sections. On failure an
 * explanatory message is written to err_buf when provided.
 *
 * @param json_str Null-terminated JSON DSL string (must not be empty).
 * @param err_buf  Optional caller buffer receiving a human-readable error (may be NULL).
 * @param err_len  Capacity of err_buf in bytes (ignored when err_buf is NULL).
 * @return A new csilk_wf_t on success, or NULL on parse/construction failure.
 * @note The caller owns the returned workflow and must free it with csilk_wf_free().
 */
csilk_wf_t*
csilk_wf_from_json_ext(const char* json_str, char* err_buf, size_t err_len)
{
    if (!json_str || !*json_str) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Empty JSON string");
        }
        return NULL;
    }

    csilk_json_t* root = csilk_json_parse(json_str);
    if (!root) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "JSON parse error");
        }
        return NULL;
    }

    csilk_json_t* j_name = csilk_json_get(root, "name");
    const char*   wf_name =
        (j_name && csilk_json_is_string(j_name)) ? csilk_json_string_value(j_name) : "dsl_workflow";

    csilk_wf_t* wf = csilk_wf_new(wf_name);
    if (!wf) {
        csilk_json_free(root);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Failed to create workflow instance");
        }
        return NULL;
    }

    csilk_json_t* j_budget = csilk_json_get(root, "budget");
    if (j_budget && csilk_json_is_object(j_budget)) {
        csilk_json_t* j_tokens = csilk_json_get(j_budget, "max_tokens");
        if (j_tokens && csilk_json_is_number(j_tokens)) {
            csilk_wf_set_budget(wf, csilk_json_int_value(j_tokens));
        }
    }

    csilk_json_t* j_persistence = csilk_json_get(root, "persistence");
    if (j_persistence && csilk_json_is_object(j_persistence)) {
        csilk_json_t* j_enabled = csilk_json_get(j_persistence, "enabled");
        csilk_json_t* j_dir = csilk_json_get(j_persistence, "wal_dir");
        if (j_enabled && csilk_json_is_true(j_enabled) && j_dir && csilk_json_is_string(j_dir)) {
            csilk_wf_set_persistence(wf, csilk_json_string_value(j_dir));
        }
    }

    csilk_json_t* j_nodes = csilk_json_get(root, "nodes");
    if (j_nodes && csilk_json_is_array(j_nodes)) {
        int node_count = csilk_json_array_size(j_nodes);
        for (int i = 0; i < node_count; i++) {
            csilk_json_t* item = csilk_json_array_get(j_nodes, i);
            if (!item) {
                continue;
            }

            csilk_json_t* j_id = csilk_json_get(item, "id");
            csilk_json_t* j_type = csilk_json_get(item, "type");
            if (!j_id || !csilk_json_is_string(j_id) || !j_type || !csilk_json_is_string(j_type)) {
                continue;
            }

            const char*   id = csilk_json_string_value(j_id);
            const char*   type = csilk_json_string_value(j_type);
            csilk_json_t* j_cfg = csilk_json_get(item, "config");

            if (strcmp(type, "ai_chat") == 0) {
                csilk_ai_config_t cfg = {0};
                if (j_cfg) {
                    csilk_json_t* j_m = csilk_json_get(j_cfg, "model");
                    csilk_json_t* j_sp = csilk_json_get(j_cfg, "system_prompt");
                    if (j_m && csilk_json_is_string(j_m)) {
                        cfg.model = csilk_json_string_value(j_m);
                    }
                    if (j_sp && csilk_json_is_string(j_sp)) {
                        cfg.system_msg = csilk_json_string_value(j_sp);
                    }
                }
                csilk_wf_add_ai(wf, id, &cfg);
            } else if (strcmp(type, "agent_react") == 0) {
                csilk_agent_react_config_t cfg = {0};
                if (j_cfg) {
                    csilk_json_t* j_m = csilk_json_get(j_cfg, "model");
                    csilk_json_t* j_sp = csilk_json_get(j_cfg, "system_prompt");
                    if (j_m && csilk_json_is_string(j_m)) {
                        cfg.model = csilk_json_string_value(j_m);
                    }
                    if (j_sp && csilk_json_is_string(j_sp)) {
                        cfg.system_prompt = csilk_json_string_value(j_sp);
                    }
                }
                csilk_wf_add_agent_react(wf, id, &cfg);
            }
        }

        /* Wire dependencies */
        for (int i = 0; i < node_count; i++) {
            csilk_json_t* item = csilk_json_array_get(j_nodes, i);
            if (!item) {
                continue;
            }

            csilk_json_t* j_id = csilk_json_get(item, "id");
            csilk_json_t* j_deps = csilk_json_get(item, "depends_on");
            if (!j_id || !csilk_json_is_string(j_id) || !j_deps || !csilk_json_is_array(j_deps)) {
                continue;
            }

            csilk_wf_node_t* target = csilk_wf_get_node(wf, csilk_json_string_value(j_id));
            if (!target) {
                continue;
            }

            int dep_count = csilk_json_array_size(j_deps);
            for (int d = 0; d < dep_count; d++) {
                csilk_json_t* dep_item = csilk_json_array_get(j_deps, d);
                if (dep_item && csilk_json_is_string(dep_item)) {
                    csilk_wf_node_t* src = csilk_wf_get_node(wf, csilk_json_string_value(dep_item));
                    if (src) {
                        csilk_wf_bind(src, target);
                    }
                }
            }
        }
    }

    csilk_json_free(root);
    return wf;
}

/**
 * @brief Loads a workflow DSL from a file on disk.
 *
 * Reads the entire file into memory and delegates to csilk_wf_from_json_ext().
 * On read/open failure an explanatory message is written to err_buf when provided.
 *
 * @param filepath Path to the JSON DSL file (must not be NULL).
 * @param err_buf  Optional caller buffer receiving a human-readable error (may be NULL).
 * @param err_len  Capacity of err_buf in bytes (ignored when err_buf is NULL).
 * @return A new csilk_wf_t on success, or NULL on failure.
 * @note The caller owns the returned workflow.
 */
csilk_wf_t*
csilk_wf_from_file(const char* filepath, char* err_buf, size_t err_len)
{
    if (!filepath) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Null file path");
        }
        return NULL;
    }

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Failed to open file: %s", filepath);
        }
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "File is empty: %s", filepath);
        }
        return NULL;
    }

    char* buf = (char*)calloc(1, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    fread(buf, 1, (size_t)sz, f);
    fclose(f);

    csilk_wf_t* wf = csilk_wf_from_json_ext(buf, err_buf, err_len);
    free(buf);
    return wf;
}

/**
 * @brief Serializes a workflow graph to a JSON string.
 *
 * Emits a minimal representation containing the workflow name, version, and a
 * flat list of node ids (typed "generic"). Edges and configurations are not
 * included in the current export.
 *
 * @param wf The workflow instance (must not be NULL).
 * @return A heap-allocated JSON string, or NULL if wf is NULL. Caller frees.
 */
char*
csilk_wf_to_json(csilk_wf_t* wf)
{
    if (!wf) {
        return NULL;
    }

    csilk_json_t* root = csilk_json_object();
    csilk_json_add_string(root, "name", wf->name ? wf->name : "dsl_workflow");
    csilk_json_add_string(root, "version", "1.0.0");

    csilk_json_t* j_nodes = csilk_json_array();
    for (size_t i = 0; i < wf->node_count; i++) {
        csilk_wf_node_t* n = wf->nodes[i];
        csilk_json_t*    item = csilk_json_object();
        csilk_json_add_string(item, "id", n->id);
        csilk_json_add_string(item, "type", "generic");
        csilk_json_array_append(j_nodes, item);
    }
    csilk_json_add_object(root, "nodes", j_nodes);

    char* formatted = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    return formatted;
}
