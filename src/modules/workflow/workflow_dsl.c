/**
 * @file workflow_dsl.c
 * @brief Declarative workflow DSL parser: builds workflow graphs from JSON
 *        definitions supplied as in-memory strings or files.
 *
 * @copyright MIT License
 */

#include "csilk/core/json/json.h"
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
    return _csilk_wf_parse_json_doc(json_str, "dsl_workflow", err_buf, err_len);
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

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
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

    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz) {
        free(buf);
        if (err_buf && err_len > 0) {
            snprintf(err_buf, err_len, "Failed to read file: %s", filepath);
        }
        return NULL;
    }

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
