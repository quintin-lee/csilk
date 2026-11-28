/**
 * @file workflow_loader.c
 * @brief Declarative YAML/JSON loader for AI Workflows.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "csilk/core/json/json.h"
#include "csilk/workflow/workflow.h"
#include "csilk/csilk.h"
#include "workflow_internal.h"

/* --- Global Handler Registry --- */

typedef struct handler_entry_s {
    char*                   name;
    csilk_wf_handler_t      handler;
    struct handler_entry_s* next;
} handler_entry_t;

static handler_entry_t* g_handlers = NULL;

/**
 * @brief Registers a named custom handler for use by the declarative loader.
 *
 * Adds the handler to a process-global singly-linked registry. DSL steps of
 * type "handler" reference handlers by this name; handlers must be registered
 * before csilk_wf_from_json() / csilk_wf_load_yaml() are called.
 *
 * @param name    Unique handler name (must not be NULL).
 * @param handler Node execution callback (must not be NULL).
 * @note Registry is global and not thread-safe; register handlers at startup.
 */
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
 * @return Handler function pointer, or NULL if not registered. */
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
    return NULL;
}

/* --- Unified JSON Parser --- */

static void
wf_set_err(char* err_buf, size_t err_len, const char* msg)
{
    if (err_buf && err_len > 0 && msg) {
        snprintf(err_buf, err_len, "%s", msg);
    }
}

static void
wf_parse_top_options(csilk_wf_t* wf, csilk_json_t* root)
{
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
}

static const char*
json_get_str_or(csilk_json_t* obj, const char* key1, const char* key2)
{
    if (!obj || !csilk_json_is_object(obj)) {
        return NULL;
    }
    csilk_json_t* item = key1 ? csilk_json_get(obj, key1) : NULL;
    if (item && csilk_json_is_string(item)) {
        return csilk_json_string_value(item);
    }
    if (key2) {
        item = csilk_json_get(obj, key2);
        if (item && csilk_json_is_string(item)) {
            return csilk_json_string_value(item);
        }
    }
    return NULL;
}

static void
wf_parse_node_item(csilk_wf_t* wf, csilk_json_t* item, bool default_handler_type)
{
    if (!item || !csilk_json_is_object(item)) {
        return;
    }
    csilk_json_t* id_item = csilk_json_get(item, "id");
    if (!id_item || !csilk_json_is_string(id_item)) {
        CSILK_LOG_W("WorkflowLoader: step skipped - missing 'id' field");
        return;
    }

    const char*   id = csilk_json_string_value(id_item);
    csilk_json_t* type_item = csilk_json_get(item, "type");
    const char*   type = NULL;
    if (type_item && csilk_json_is_string(type_item)) {
        type = csilk_json_string_value(type_item);
    } else if (default_handler_type) {
        type = "handler";
    } else {
        return;
    }

    csilk_json_t*    cfg_obj = csilk_json_get(item, "config");
    csilk_wf_node_t* node = NULL;

    if (strcmp(type, "ai") == 0 || strcmp(type, "ai_chat") == 0) {
        csilk_ai_config_t aic = {0};
        aic.model = json_get_str_or(cfg_obj, "model", NULL);
        aic.prompt = json_get_str_or(cfg_obj, "prompt", NULL);
        aic.system_msg = json_get_str_or(cfg_obj, "system_msg", "system_prompt");
        node = csilk_wf_add_ai(wf, id, &aic);
    } else if (strcmp(type, "agent_react") == 0) {
        csilk_agent_react_config_t rcfg = {0};
        rcfg.model = json_get_str_or(cfg_obj, "model", NULL);
        rcfg.system_prompt = json_get_str_or(cfg_obj, "system_prompt", "system_msg");
        node = csilk_wf_add_agent_react(wf, id, &rcfg);
    } else {
        csilk_json_t* handler_item = csilk_json_get(item, "handler");
        if (handler_item && csilk_json_is_string(handler_item)) {
            csilk_wf_handler_t h = find_handler(csilk_json_string_value(handler_item));
            if (h) {
                node = csilk_wf_add(wf, id, h, NULL);
            } else {
                CSILK_LOG_W("WorkflowLoader: handler '%s' not registered for step '%s'",
                            csilk_json_string_value(handler_item),
                            id);
            }
        }
    }

    if (node) {
        CSILK_LOG_D("WorkflowLoader: loaded step '%s' (type: '%s')", id, type);
        csilk_json_t* entry_item = csilk_json_get(item, "entry");
        if (csilk_json_is_true(entry_item)) {
            csilk_wf_node_set_entry(node, 1);
            CSILK_LOG_D("WorkflowLoader: step '%s' marked as entry point", id);
        }

        csilk_json_t* join_item = csilk_json_get(item, "join");
        if (csilk_json_is_string(join_item) &&
            strcmp(csilk_json_string_value(join_item), "or") == 0) {
            csilk_wf_node_set_join(node, CSILK_WF_JOIN_OR);
            CSILK_LOG_D("WorkflowLoader: step '%s' join policy set to OR", id);
        }
    } else {
        CSILK_LOG_W("WorkflowLoader: failed to add step '%s'", id);
    }
}

static void
wf_wire_node_relations(csilk_wf_t* wf, csilk_json_t* arr)
{
    if (!arr || !csilk_json_is_array(arr)) {
        return;
    }
    int count = csilk_json_array_size(arr);
    for (int i = 0; i < count; i++) {
        csilk_json_t* item = csilk_json_array_get(arr, i);
        csilk_json_t* id_item = item ? csilk_json_get(item, "id") : NULL;
        if (!id_item || !csilk_json_is_string(id_item)) {
            continue;
        }
        const char*      node_id = csilk_json_string_value(id_item);
        csilk_wf_node_t* target = csilk_wf_get_node(wf, node_id);
        if (!target) {
            continue;
        }

        csilk_json_t* j_deps = csilk_json_get(item, "depends_on");
        if (j_deps && csilk_json_is_array(j_deps)) {
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

        csilk_json_t* err_item = csilk_json_get(item, "on_error");
        if (err_item && csilk_json_is_string(err_item)) {
            csilk_wf_node_t* err_target = csilk_wf_get_node(wf, csilk_json_string_value(err_item));
            if (err_target) {
                csilk_wf_on_error(target, err_target);
                CSILK_LOG_D("WorkflowLoader: registered error route '%s' -> '%s'",
                            node_id,
                            csilk_json_string_value(err_item));
            } else {
                CSILK_LOG_W("WorkflowLoader: failed to set error target - "
                            "step '%s' or error step '%s' not found",
                            node_id,
                            csilk_json_string_value(err_item));
            }
        }
    }
}

static void
wf_wire_connections(csilk_wf_t* wf, csilk_json_t* conns)
{
    if (!conns || !csilk_json_is_array(conns)) {
        return;
    }
    int count = csilk_json_array_size(conns);
    for (int i = 0; i < count; i++) {
        csilk_json_t* conn = csilk_json_array_get(conns, i);
        csilk_json_t* from_item = csilk_json_get(conn, "from");
        csilk_json_t* to_item = csilk_json_get(conn, "to");
        if (!csilk_json_is_string(from_item) || !csilk_json_is_string(to_item)) {
            CSILK_LOG_W(
                "WorkflowLoader: connection skipped - missing or invalid 'from'/'to' fields");
            continue;
        }

        csilk_wf_node_t* n_from = csilk_wf_get_node(wf, csilk_json_string_value(from_item));
        csilk_wf_node_t* n_to = csilk_wf_get_node(wf, csilk_json_string_value(to_item));
        if (n_from && n_to) {
            csilk_json_t* cond_item = csilk_json_get(conn, "condition");
            csilk_json_t* loop_item = csilk_json_get(conn, "loop");
            const char*   cond =
                csilk_json_is_string(cond_item) ? csilk_json_string_value(cond_item) : NULL;

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

CSILK_INTERNAL csilk_wf_t*
_csilk_wf_parse_json_doc(const char* json_str,
                         const char* default_name,
                         char*       err_buf,
                         size_t      err_len)
{
    if (!json_str || !*json_str) {
        wf_set_err(err_buf, err_len, "Empty JSON string");
        return NULL;
    }

    CSILK_LOG_T("WorkflowLoader: parsing workflow from JSON string");

    csilk_json_t* root = csilk_json_parse(json_str);
    if (!root) {
        CSILK_LOG_E("WorkflowLoader: failed to parse JSON string");
        wf_set_err(err_buf, err_len, "JSON parse error");
        return NULL;
    }

    csilk_json_t* name_item = csilk_json_get(root, "name");
    const char*   wf_name = (csilk_json_is_string(name_item))
                                ? csilk_json_string_value(name_item)
                                : (default_name ? default_name : "DeclarativeWF");

    csilk_wf_t* wf = csilk_wf_new(wf_name);
    if (!wf) {
        CSILK_LOG_E("WorkflowLoader: failed to create workflow instance '%s'", wf_name);
        csilk_json_free(root);
        wf_set_err(err_buf, err_len, "Failed to create workflow instance");
        return NULL;
    }

    wf_parse_top_options(wf, root);

    csilk_json_t* steps = csilk_json_get(root, "steps");
    if (steps && csilk_json_is_array(steps)) {
        int count = csilk_json_array_size(steps);
        for (int i = 0; i < count; i++) {
            wf_parse_node_item(wf, csilk_json_array_get(steps, i), true);
        }
    }

    csilk_json_t* nodes = csilk_json_get(root, "nodes");
    if (nodes && csilk_json_is_array(nodes)) {
        int count = csilk_json_array_size(nodes);
        for (int i = 0; i < count; i++) {
            wf_parse_node_item(wf, csilk_json_array_get(nodes, i), false);
        }
    }

    wf_wire_connections(wf, csilk_json_get(root, "connections"));
    wf_wire_node_relations(wf, steps);
    wf_wire_node_relations(wf, nodes);

    CSILK_LOG_I("WorkflowLoader: workflow '%s' successfully loaded from JSON", wf_name);
    csilk_json_free(root);
    return wf;
}

/** @brief Build a complete workflow from a JSON string.
 *
 * Delegates to the shared workflow JSON parser supporting both "steps"+"connections"
 * and "nodes"+"depends_on" schemas.
 *
 * @param json_str Null-terminated JSON string.
 * @return A new csilk_wf_t, or NULL on parse failure or empty workflow. */
csilk_wf_t*
csilk_wf_from_json(const char* json_str)
{
    return _csilk_wf_parse_json_doc(json_str, "DeclarativeWF", NULL, 0);
}

/* --- YAML Loader --- */

static csilk_json_t*
yaml_make_scalar(const char* val)
{
    if (strcmp(val, "true") == 0) {
        return csilk_json_bool(1);
    }
    if (strcmp(val, "false") == 0) {
        return csilk_json_bool(0);
    }
    return csilk_json_string_new(val);
}

static bool
yaml_attach_container(csilk_json_t** root,
                      csilk_json_t** stack,
                      int*           stack_ptr,
                      int            max_depth,
                      char**         current_key,
                      bool           is_array,
                      const char*    path)
{
    if (*stack_ptr >= max_depth) {
        CSILK_LOG_E("WorkflowLoader: YAML nesting too deep in file '%s'", path);
        return false;
    }

    csilk_json_t* node = is_array ? csilk_json_array() : csilk_json_object();
    if (!node) {
        return false;
    }

    if (!*root) {
        *root = node;
        stack[(*stack_ptr)++] = node;
        return true;
    }

    csilk_json_t* parent = *stack_ptr > 0 ? stack[*stack_ptr - 1] : NULL;
    /* A nested container is absorbed into the parent's yyjson doc
     * (the standalone handle is freed by the add/append call). Push the live
     * in-doc view back onto the stack, not the freed handle. */
    if (parent && csilk_json_is_array(parent)) {
        csilk_json_array_append(parent, node);
        node = csilk_json_array_get(parent, csilk_json_array_size(parent) - 1);
    } else if (parent && csilk_json_is_object(parent) && *current_key) {
        char* key = *current_key;
        csilk_json_add_object(parent, key, node);
        node = is_array ? csilk_json_get_array(parent, key) : csilk_json_get_object(parent, key);
        free(key);
        *current_key = NULL;
    } else {
        csilk_json_free(node);
        node = NULL;
    }
    if (node) {
        stack[(*stack_ptr)++] = node;
    }
    return true;
}

/** @brief Internal: parse a YAML file into a csilk_json_t tree using libyaml. */
static csilk_json_t*
parse_yaml_file(const char* path)
{
    FILE* fh = fopen(path, "rb");
    if (!fh) {
        CSILK_LOG_E("WorkflowLoader: failed to open YAML file '%s'", path);
        return NULL;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        CSILK_LOG_E("WorkflowLoader: failed to initialize YAML parser");
        fclose(fh);
        return NULL;
    }
    yaml_parser_set_input_file(&parser, fh);

    csilk_json_t* root = NULL;
    enum { WF_YAML_MAX_DEPTH = 64 };
    csilk_json_t* stack[WF_YAML_MAX_DEPTH] = {NULL};
    int           stack_ptr = 0;
    char*         current_key = NULL;

    yaml_event_t event;
    int          done = 0;
    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            CSILK_LOG_E("WorkflowLoader: YAML parse error in file '%s'", path);
            break;
        }

        switch (event.type) {
        case YAML_MAPPING_START_EVENT:
            if (!yaml_attach_container(
                    &root, stack, &stack_ptr, WF_YAML_MAX_DEPTH, &current_key, false, path)) {
                done = 1;
            }
            break;
        case YAML_SEQUENCE_START_EVENT:
            if (!yaml_attach_container(
                    &root, stack, &stack_ptr, WF_YAML_MAX_DEPTH, &current_key, true, path)) {
                done = 1;
            }
            break;
        case YAML_SCALAR_EVENT: {
            csilk_json_t* parent = stack_ptr > 0 ? stack[stack_ptr - 1] : NULL;
            const char*   val = (const char*)event.data.scalar.value;
            if (parent && csilk_json_is_object(parent)) {
                if (!current_key) {
                    current_key = strdup(val);
                } else {
                    csilk_json_add_object(parent, current_key, yaml_make_scalar(val));
                    free(current_key);
                    current_key = NULL;
                }
            } else if (parent && csilk_json_is_array(parent)) {
                csilk_json_array_append(parent, yaml_make_scalar(val));
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
    free(current_key);
    return root;
}

/** @brief Load a workflow from a YAML file on disk.
 *
 * Parses the YAML into csilk_json via parse_yaml_file(), then delegates
 * to csilk_wf_from_json() for workflow construction. This two-step
 * approach avoids duplicated parsing logic.
 *
 * @param path Path to a .yaml or .yml file.
 * @return A new csilk_wf_t, or NULL if the file cannot be read or
 *         the YAML is invalid.
 * @note The caller owns the returned workflow. */
csilk_wf_t*
csilk_wf_load_yaml(const char* path)
{
    CSILK_LOG_I("WorkflowLoader: loading declarative workflow from YAML file '%s'", path);
    csilk_json_t* root = parse_yaml_file(path);
    if (!root) {
        CSILK_LOG_E("WorkflowLoader: failed to parse YAML structure from file '%s'", path);
        return NULL;
    }
    char* json_str = csilk_json_serialize(root, NULL);
    csilk_json_free(root);
    csilk_wf_t* wf = csilk_wf_from_json(json_str);
    free(json_str);
    return wf;
}
