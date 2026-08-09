/**
 * @file workflow_loader.c
 * @brief Declarative YAML/JSON loader for AI Workflows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "csilk/core/json.h"
#include "csilk/app/workflow.h"
#include "csilk/csilk.h"

/* --- Global Handler Registry --- */

typedef struct handler_entry_s {
    char*                   name;
    csilk_wf_handler_t      handler;
    struct handler_entry_s* next;
} handler_entry_t;

static handler_entry_t* g_handlers = nullptr;

void
csilk_wf_register_handler(const char* name, csilk_wf_handler_t handler)
{
    if (!name || !handler) {
        return;
    }
    handler_entry_t* entry = calloc(1, sizeof(handler_entry_t));
    if (!entry) {
        CSILK_LOG_E("WorkflowLoader: failed to allocate memory for handler entry");
        return;
    }
    entry->name = strdup(name);
    entry->handler = handler;
    entry->next = g_handlers;
    g_handlers = entry;
    CSILK_LOG_D("WorkflowLoader: registered custom handler '%s'", name);
}

/** @brief Internal: look up a handler by name in the singly-linked list
 * registry.
 * @param name Handler name (case-sensitive).
 * @return Handler function pointer, or nullptr if not registered. */
static csilk_wf_handler_t
find_handler(const char* name)
{
    handler_entry_t* curr = g_handlers;
    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            return curr->handler;
        }
        curr = curr->next;
    }
    return nullptr;
}

/* --- JSON Parser --- */

/** @brief Build a complete workflow from a JSON string.
 *
 * Parses the JSON into a three-pass construction:
 * Pass 1: Create all nodes from the "steps" array. Each step has an
 *   "id", optional "type" ("ai" or "handler"), and optional "config"
 *   for AI nodes. AI nodes use csilk_wf_add_ai() with prompt/model config;
 *   handler nodes use csilk_wf_add() with a registered callback.
 * Pass 2: Create connections from the "connections" array. Each
 *   connection links a source ("from") to a target ("to"), optionally
 *   with a "condition" for conditional edges or "loop: true" for loops.
 * Pass 3: Set error targets from each step's "on_error" field.
 *
 * @param json_str Null-terminated JSON string.
 * @return A new csilk_wf_t, or nullptr on parse failure or empty workflow.
 * @note The caller owns the returned workflow and must free it with
 *       csilk_wf_free(). Handler functions must be registered via
 *       csilk_wf_register_handler() before calling this function. */
csilk_wf_t*
csilk_wf_from_json(const char* json_str)
{
    if (!json_str) {
        return nullptr;
    }

    CSILK_LOG_T("WorkflowLoader: parsing workflow from JSON string");

    csilk_json_t* root = csilk_json_parse(json_str);
    if (!root) {
        CSILK_LOG_E("WorkflowLoader: failed to parse JSON string");
        return nullptr;
    }

    csilk_json_t* name_item = csilk_json_get(root, "name");
    const char*   wf_name =
        (csilk_json_is_string(name_item)) ? csilk_json_string_value(name_item) : "DeclarativeWF";

    csilk_wf_t* wf = csilk_wf_new(wf_name);
    if (!wf) {
        CSILK_LOG_E("WorkflowLoader: failed to create workflow instance '%s'", wf_name);
        csilk_json_free(root);
        return nullptr;
    }

    csilk_json_t* steps = csilk_json_get(root, "steps");
    if (csilk_json_is_array(steps)) {
        int count = csilk_json_array_size(steps);
        for (int i = 0; i < count; i++) {
            csilk_json_t* step = csilk_json_array_get(steps, i);
            csilk_json_t* id_item = csilk_json_get(step, "id");
            csilk_json_t* type_item = csilk_json_get(step, "type");

            if (!csilk_json_is_string(id_item)) {
                CSILK_LOG_W("WorkflowLoader: step skipped - missing 'id' field");
                continue;
            }
            const char* id = csilk_json_string_value(id_item);
            const char* type =
                csilk_json_is_string(type_item) ? csilk_json_string_value(type_item) : "handler";

            csilk_wf_node_t* node = nullptr;
            if (strcmp(type, "ai") == 0) {
                csilk_json_t*     config = csilk_json_get(step, "config");
                csilk_ai_config_t aic = {0};
                if (csilk_json_is_object(config)) {
                    csilk_json_t* model = csilk_json_get(config, "model");
                    csilk_json_t* prompt = csilk_json_get(config, "prompt");
                    csilk_json_t* sys = csilk_json_get(config, "system_msg");
                    if (csilk_json_is_string(model)) {
                        aic.model = csilk_json_string_value(model);
                    }
                    if (csilk_json_is_string(prompt)) {
                        aic.prompt = csilk_json_string_value(prompt);
                    }
                    if (csilk_json_is_string(sys)) {
                        aic.system_msg = csilk_json_string_value(sys);
                    }
                }
                node = csilk_wf_add_ai(wf, id, &aic);
            } else {
                csilk_json_t* handler_item = csilk_json_get(step, "handler");
                if (csilk_json_is_string(handler_item)) {
                    csilk_wf_handler_t h = find_handler(csilk_json_string_value(handler_item));
                    if (h) {
                        node = csilk_wf_add(wf, id, h, nullptr);
                    } else {
                        CSILK_LOG_W("WorkflowLoader: handler '%s' not "
                                    "registered for step '%s'",
                                    csilk_json_string_value(handler_item),
                                    id);
                    }
                }
            }

            if (node) {
                CSILK_LOG_D("WorkflowLoader: loaded step '%s' (type: '%s')", id, type);

                csilk_json_t* entry_item = csilk_json_get(step, "entry");
                if (csilk_json_is_true(entry_item)) {
                    csilk_wf_node_set_entry(node, 1);
                    CSILK_LOG_D("WorkflowLoader: step '%s' marked as entry point", id);
                }

                csilk_json_t* join_item = csilk_json_get(step, "join");
                if (csilk_json_is_string(join_item) &&
                    strcmp(csilk_json_string_value(join_item), "or") == 0) {
                    csilk_wf_node_set_join(node, CSILK_WF_JOIN_OR);
                    CSILK_LOG_D("WorkflowLoader: step '%s' join policy set to OR", id);
                }
            } else {
                CSILK_LOG_W("WorkflowLoader: failed to add step '%s'", id);
            }
        }
    }

    // Pass 2: Connections
    csilk_json_t* conns = csilk_json_get(root, "connections");
    if (csilk_json_is_array(conns)) {
        int count = csilk_json_array_size(conns);
        for (int i = 0; i < count; i++) {
            csilk_json_t* conn = csilk_json_array_get(conns, i);
            csilk_json_t* from_item = csilk_json_get(conn, "from");
            csilk_json_t* to_item = csilk_json_get(conn, "to");
            if (!csilk_json_is_string(from_item) || !csilk_json_is_string(to_item)) {
                CSILK_LOG_W("WorkflowLoader: connection skipped - missing or "
                            "invalid 'from'/'to' fields");
                continue;
            }

            csilk_wf_node_t* n_from = csilk_wf_get_node(wf, csilk_json_string_value(from_item));
            csilk_wf_node_t* n_to = csilk_wf_get_node(wf, csilk_json_string_value(to_item));

            if (n_from && n_to) {
                csilk_json_t* cond_item = csilk_json_get(conn, "condition");
                csilk_json_t* loop_item = csilk_json_get(conn, "loop");
                const char*   cond =
                    csilk_json_is_string(cond_item) ? csilk_json_string_value(cond_item) : nullptr;

                if (csilk_json_is_true(loop_item)) {
                    csilk_wf_on_loop(n_from, cond, n_to);
                } else if (cond) {
                    csilk_wf_on(n_from, cond, n_to);
                } else {
                    csilk_wf_bind(n_from, n_to);
                }
                CSILK_LOG_D("WorkflowLoader: loaded connection '%s' -> '%s' "
                            "(condition: '%s', loop: %d)",
                            csilk_json_string_value(from_item),
                            csilk_json_string_value(to_item),
                            cond ? cond : "none",
                            csilk_json_is_true(loop_item));
            } else {
                CSILK_LOG_W("WorkflowLoader: connection skipped - failed to find "
                            "nodes for connection '%s' -> '%s'",
                            csilk_json_string_value(from_item),
                            csilk_json_string_value(to_item));
            }
        }
    }

    // Pass 3: Error Targets
    if (csilk_json_is_array(steps)) {
        int count = csilk_json_array_size(steps);
        for (int i = 0; i < count; i++) {
            csilk_json_t* step = csilk_json_array_get(steps, i);
            csilk_json_t* id_item = csilk_json_get(step, "id");
            csilk_json_t* err_item = csilk_json_get(step, "on_error");
            if (csilk_json_is_string(id_item) && csilk_json_is_string(err_item)) {
                csilk_wf_node_t* n = csilk_wf_get_node(wf, csilk_json_string_value(id_item));
                csilk_wf_node_t* err_target =
                    csilk_wf_get_node(wf, csilk_json_string_value(err_item));
                if (n && err_target) {
                    csilk_wf_on_error(n, err_target);
                    CSILK_LOG_D("WorkflowLoader: registered error route '%s' -> '%s'",
                                csilk_json_string_value(id_item),
                                csilk_json_string_value(err_item));
                } else {
                    CSILK_LOG_W("WorkflowLoader: failed to set error target - "
                                "step '%s' or error step '%s' not found",
                                csilk_json_string_value(id_item),
                                csilk_json_string_value(err_item));
                }
            }
        }
    }

    CSILK_LOG_I("WorkflowLoader: workflow '%s' successfully loaded from JSON", wf_name);

    csilk_json_free(root);
    return wf;
}

/* --- YAML Loader --- */

/** @brief Internal: parse a YAML file into a cJSON tree using libyaml.
 *
 * Algorithm (iterative, no recursion):
 * 1. Open the file and initialize a libyaml parser.
 * 2. Maintain a stack of cJSON containers (objects and arrays).
 * 3. For each YAML event:
 *    - MAPPING_START: push a new cJSON object onto the stack.
 *    - SEQUENCE_START: push a new cJSON array onto the stack.
 *    - SCALAR: if parent is an object, the first scalar is a key;
 *      the second scalar is the value paired with the key. If parent
 *      is an array, add the scalar directly.
 *    - MAPPING_END / SEQUENCE_END: pop the container stack.
 * 4. Return the root cJSON node.
 *
 * @param path Filesystem path to the YAML file.
 * @return Root cJSON node (object or array), or nullptr if the file
 *         cannot be opened or parsed.
 * @note The caller must free the returned cJSON with csilk_json_free().
 *       YAML boolean values ("true"/"false") are converted to cJSON
 *       boolean. All other scalars are treated as strings. */
static csilk_json_t*
parse_yaml_file(const char* path)
{
    FILE* fh = fopen(path, "rb");
    if (!fh) {
        CSILK_LOG_E("WorkflowLoader: failed to open YAML file '%s'", path);
        return nullptr;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        CSILK_LOG_E("WorkflowLoader: failed to initialize YAML parser");
        fclose(fh);
        return nullptr;
    }
    yaml_parser_set_input_file(&parser, fh);

    csilk_json_t* root = nullptr;
    enum { WF_YAML_MAX_DEPTH = 64 };
    csilk_json_t* stack[WF_YAML_MAX_DEPTH] = {nullptr};
    int           stack_ptr = 0;

    // To handle mapping keys:
    char* current_key = nullptr;

    yaml_event_t event;
    int          done = 0;
    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            CSILK_LOG_E("WorkflowLoader: YAML parse error in file '%s'", path);
            break;
        }

        switch (event.type) {
        case YAML_MAPPING_START_EVENT: {
            if (stack_ptr >= WF_YAML_MAX_DEPTH) {
                CSILK_LOG_E("WorkflowLoader: YAML nesting too deep in file '%s'", path);
                done = 1;
                break;
            }
            csilk_json_t* obj = csilk_json_object();
            if (!root) {
                root = obj;
            } else {
                csilk_json_t* parent = stack_ptr > 0 ? stack[stack_ptr - 1] : nullptr;
                if (parent && csilk_json_is_array(parent)) {
                    csilk_json_array_append(parent, obj);
                } else if (parent && csilk_json_is_object(parent) && current_key) {
                    csilk_json_add_object(parent, current_key, obj);
                    free(current_key);
                    current_key = nullptr;
                }
            }
            stack[stack_ptr++] = obj;
            break;
        }
        case YAML_SEQUENCE_START_EVENT: {
            if (stack_ptr >= WF_YAML_MAX_DEPTH) {
                CSILK_LOG_E("WorkflowLoader: YAML nesting too deep in file '%s'", path);
                done = 1;
                break;
            }
            csilk_json_t* arr = csilk_json_array();
            if (!root) {
                root = arr;
            } else {
                csilk_json_t* parent = stack_ptr > 0 ? stack[stack_ptr - 1] : nullptr;
                if (parent && csilk_json_is_array(parent)) {
                    csilk_json_array_append(parent, arr);
                } else if (parent && csilk_json_is_object(parent) && current_key) {
                    csilk_json_add_object(parent, current_key, arr);
                    free(current_key);
                    current_key = nullptr;
                }
            }
            stack[stack_ptr++] = arr;
            break;
        }
        case YAML_SCALAR_EVENT: {
            csilk_json_t* parent = stack_ptr > 0 ? stack[stack_ptr - 1] : nullptr;
            if (parent) {
                if (csilk_json_is_object(parent)) {
                    if (!current_key) {
                        current_key = strdup((char*)event.data.scalar.value);
                    } else {
                        const char*   val = (char*)event.data.scalar.value;
                        csilk_json_t* scalar = nullptr;
                        if (strcmp(val, "true") == 0) {
                            scalar = csilk_json_bool(1);
                        } else if (strcmp(val, "false") == 0) {
                            scalar = csilk_json_bool(0);
                        } else {
                            scalar = csilk_json_string_new(val);
                        }
                        csilk_json_add_object(parent, current_key, scalar);
                        free(current_key);
                        current_key = nullptr;
                    }
                } else if (csilk_json_is_array(parent)) {
                    const char*   val = (char*)event.data.scalar.value;
                    csilk_json_t* scalar = nullptr;
                    if (strcmp(val, "true") == 0) {
                        scalar = csilk_json_bool(1);
                    } else if (strcmp(val, "false") == 0) {
                        scalar = csilk_json_bool(0);
                    } else {
                        scalar = csilk_json_string_new(val);
                    }
                    csilk_json_array_append(parent, scalar);
                }
            }
            break;
        }
        case YAML_MAPPING_END_EVENT:
        case YAML_SEQUENCE_END_EVENT:
            if (stack_ptr > 0) {
                stack_ptr--;
            }
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
    if (current_key) {
        free(current_key);
    }
    return root;
}

/** @brief Load a workflow from a YAML file on disk.
 *
 * Parses the YAML into cJSON via parse_yaml_file(), then delegates
 * to csilk_wf_from_json() for workflow construction. This two-step
 * approach avoids duplicated parsing logic.
 *
 * @param path Path to a .yaml or .yml file.
 * @return A new csilk_wf_t, or nullptr if the file cannot be read or
 *         the YAML is invalid.
 * @note The caller owns the returned workflow. */
csilk_wf_t*
csilk_wf_load_yaml(const char* path)
{
    CSILK_LOG_I("WorkflowLoader: loading declarative workflow from YAML file '%s'", path);
    csilk_json_t* root = parse_yaml_file(path);
    if (!root) {
        CSILK_LOG_E("WorkflowLoader: failed to parse YAML structure from file '%s'", path);
        return nullptr;
    }
    char* json_str = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    csilk_wf_t* wf = csilk_wf_from_json(json_str);
    free(json_str);
    return wf;
}
