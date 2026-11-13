/**
 * @file mcp_server.c
 * @brief MCP server implementation (stdio and HTTP/SSE transports).
 *
 * Implements the Model Context Protocol server: a registry of tools and
 * workflows guarded by a mutex, a JSON-RPC request dispatcher that handles
 * the standard "initialize", "tools/list", and "tools/call" methods, and
 * transport bindings for a blocking stdio loop and an HTTP POST route.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/protocols/mcp.h"
#include "mcp_internal.h"
#include "workflow/workflow_internal.h"

/**
 * @brief Create a new MCP server instance.
 *
 * Allocates and zero-initializes the server, applying default name/version
 * when the supplied values are NULL, and initializes the registry mutex.
 *
 * @param[in] name    Server name (defaults to "csilk-mcp-server" if NULL).
 * @param[in] version Server version (defaults to "1.0.0" if NULL).
 * @return A newly allocated csilk_mcp_server_t, or NULL on allocation failure.
 */
csilk_mcp_server_t*
csilk_mcp_server_new(const char* name, const char* version)
{
    csilk_mcp_server_t* server = (csilk_mcp_server_t*)calloc(1, sizeof(csilk_mcp_server_t));
    if (!server) {
        return NULL;
    }

    snprintf(server->name, sizeof(server->name), "%s", name ? name : "csilk-mcp-server");
    snprintf(server->version, sizeof(server->version), "%s", version ? version : "1.0.0");
    csilk_mutex_init(&server->mutex);

    return server;
}

/**
 * @brief Destroy an MCP server and release all registered tools.
 *
 * Frees the name/description/parameters_json strings of each registered tool
 * entry, the tools and workflows arrays, and the mutex. Safe to call with a
 * NULL pointer.
 *
 * @param[in] server The MCP server to free (may be NULL).
 */
void
csilk_mcp_server_free(csilk_mcp_server_t* server)
{
    if (!server) {
        return;
    }

    csilk_mutex_lock(&server->mutex);
    if (server->tools) {
        for (size_t i = 0; i < server->tool_count; i++) {
            if (server->tools[i]) {
                free(server->tools[i]->name);
                free(server->tools[i]->description);
                free(server->tools[i]->parameters_json);
                free(server->tools[i]);
            }
        }
        free(server->tools);
    }
    if (server->resources) {
        for (size_t i = 0; i < server->resource_count; i++) {
            if (server->resources[i]) {
                free(server->resources[i]->uri);
                free(server->resources[i]->name);
                free(server->resources[i]->mime_type);
                free(server->resources[i]->static_text);
                free(server->resources[i]);
            }
        }
        free(server->resources);
    }
    if (server->prompts) {
        for (size_t i = 0; i < server->prompt_count; i++) {
            if (server->prompts[i]) {
                free(server->prompts[i]->name);
                free(server->prompts[i]->description);
                free(server->prompts[i]->prompt_template);
                free(server->prompts[i]);
            }
        }
        free(server->prompts);
    }
    if (server->workflows) {
        free(server->workflows);
    }
    csilk_mutex_unlock(&server->mutex);
    csilk_mutex_destroy(&server->mutex);
    free(server);
}

/**
 * @brief Register a tool handler with the MCP server.
 *
 * Copies the tool's name, description, and parameters JSON and stores its
 * function pointer and user data. The server's tool array is grown via
 * realloc. Thread-safe (takes the registry mutex).
 *
 * @param[in,out] server The MCP server to register with.
 * @param[in]     tool   The tool entry (name must be non-NULL).
 * @return 0 on success, or -1 on invalid arguments or allocation failure.
 */
int
csilk_mcp_server_register_tool(csilk_mcp_server_t* server, csilk_wf_tool_entry_t* tool)
{
    if (!server || !tool || !tool->name) {
        return -1;
    }

    csilk_mutex_lock(&server->mutex);
    csilk_wf_tool_entry_t** new_tools = (csilk_wf_tool_entry_t**)realloc(
        server->tools, sizeof(csilk_wf_tool_entry_t*) * (server->tool_count + 1));
    if (!new_tools) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }
    server->tools = new_tools;

    csilk_wf_tool_entry_t* entry = (csilk_wf_tool_entry_t*)calloc(1, sizeof(csilk_wf_tool_entry_t));
    if (!entry) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }

    entry->name = strdup(tool->name);
    entry->description = tool->description ? strdup(tool->description) : NULL;
    entry->parameters_json = tool->parameters_json ? strdup(tool->parameters_json) : NULL;
    entry->fn = tool->fn;
    entry->user_data = tool->user_data;

    server->tools = new_tools;
    server->tools[server->tool_count++] = entry;
    csilk_mutex_unlock(&server->mutex);

    return 0;
}

int
csilk_mcp_server_register_resource(csilk_mcp_server_t* server,
                                   const char*         uri,
                                   const char*         name,
                                   const char*         mime_type,
                                   const char*         static_text,
                                   char* (*read_fn)(const char* uri, void* user_data),
                                   void* user_data)
{
    if (!server || !uri || !name) {
        return -1;
    }

    csilk_mutex_lock(&server->mutex);
    csilk_mcp_resource_entry_t** next = realloc(
        server->resources, sizeof(csilk_mcp_resource_entry_t*) * (server->resource_count + 1));
    if (!next) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }
    server->resources = next;

    csilk_mcp_resource_entry_t* r = calloc(1, sizeof(csilk_mcp_resource_entry_t));
    if (!r) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }
    r->uri = strdup(uri);
    r->name = strdup(name);
    r->mime_type = strdup(mime_type ? mime_type : "text/plain");
    r->static_text = static_text ? strdup(static_text) : NULL;
    r->read_fn = read_fn;
    r->user_data = user_data;

    server->resources[server->resource_count++] = r;
    csilk_mutex_unlock(&server->mutex);
    return 0;
}

int
csilk_mcp_server_register_prompt(csilk_mcp_server_t* server,
                                 const char*         name,
                                 const char*         description,
                                 const char*         prompt_template)
{
    if (!server || !name || !prompt_template) {
        return -1;
    }

    csilk_mutex_lock(&server->mutex);
    csilk_mcp_prompt_entry_t** next =
        realloc(server->prompts, sizeof(csilk_mcp_prompt_entry_t*) * (server->prompt_count + 1));
    if (!next) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }
    server->prompts = next;

    csilk_mcp_prompt_entry_t* p = calloc(1, sizeof(csilk_mcp_prompt_entry_t));
    if (!p) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }
    p->name = strdup(name);
    p->description = description ? strdup(description) : NULL;
    p->prompt_template = strdup(prompt_template);

    server->prompts[server->prompt_count++] = p;
    csilk_mutex_unlock(&server->mutex);
    return 0;
}

/**
 * @brief Register a workflow with the MCP server.
 *
 * Appends the workflow pointer to the server's workflow array (grown via
 * realloc). Thread-safe (takes the registry mutex).
 *
 * @param[in,out] server The MCP server to register with.
 * @param[in]     wf     The workflow to register (must be non-NULL).
 * @return 0 on success, or -1 on invalid arguments or allocation failure.
 */
int
csilk_mcp_server_register_workflow(csilk_mcp_server_t* server, csilk_wf_t* wf)
{
    if (!server || !wf) {
        return -1;
    }

    csilk_mutex_lock(&server->mutex);
    csilk_wf_t** new_wfs = (csilk_wf_t**)realloc(
        server->workflows, sizeof(csilk_wf_t*) * (server->workflow_count + 1));
    if (!new_wfs) {
        csilk_mutex_unlock(&server->mutex);
        return -1;
    }

    server->workflows = new_wfs;
    server->workflows[server->workflow_count++] = wf;
    csilk_mutex_unlock(&server->mutex);

    return 0;
}

/** @brief Dispatch a single parsed JSON-RPC request, returning a response/error frame. */
static csilk_mcp_msg_t*
handle_mcp_request(csilk_mcp_server_t* server, const csilk_mcp_msg_t* req)
{
    if (!req || !req->method) {
        return csilk_mcp_msg_create_error(
            req ? req->id : NULL, CSILK_MCP_INVALID_REQUEST, "Invalid Request");
    }

    if (strcmp(req->method, "initialize") == 0) {
        csilk_json_t* res = csilk_json_object();
        csilk_json_add_string(res, "protocolVersion", "2024-11-05");

        csilk_json_t* server_info = csilk_json_object();
        csilk_json_add_string(server_info, "name", server->name);
        csilk_json_add_string(server_info, "version", server->version);
        csilk_json_add_object(res, "serverInfo", server_info);

        csilk_json_t* caps = csilk_json_object();
        csilk_json_t* tools_cap = csilk_json_object();
        csilk_json_add_bool(tools_cap, "listChanged", false);
        csilk_json_add_object(caps, "tools", tools_cap);

        csilk_json_t* res_cap = csilk_json_object();
        csilk_json_add_bool(res_cap, "subscribe", false);
        csilk_json_add_bool(res_cap, "listChanged", false);
        csilk_json_add_object(caps, "resources", res_cap);

        csilk_json_t* prompt_cap = csilk_json_object();
        csilk_json_add_bool(prompt_cap, "listChanged", false);
        csilk_json_add_object(caps, "prompts", prompt_cap);

        csilk_json_add_object(res, "capabilities", caps);

        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "ping") == 0) {
        return csilk_mcp_msg_create_response(req->id, csilk_json_object());
    }

    if (strcmp(req->method, "tools/list") == 0) {
        csilk_json_t* res = csilk_json_object();
        csilk_json_t* tools_arr = csilk_json_array();

        csilk_mutex_lock(&server->mutex);
        for (size_t i = 0; i < server->tool_count; i++) {
            csilk_wf_tool_entry_t* t = server->tools[i];
            csilk_json_t*          t_obj = csilk_json_object();
            csilk_json_add_string(t_obj, "name", t->name);
            csilk_json_add_string(t_obj, "description", t->description ? t->description : "");

            if (t->parameters_json) {
                csilk_json_t* schema = csilk_json_parse(t->parameters_json);
                if (schema) {
                    csilk_json_add_object(t_obj, "inputSchema", schema);
                } else {
                    csilk_json_add_object(t_obj, "inputSchema", csilk_json_object());
                }
            } else {
                csilk_json_add_object(t_obj, "inputSchema", csilk_json_object());
            }
            csilk_json_array_append(tools_arr, t_obj);
        }
        csilk_mutex_unlock(&server->mutex);

        csilk_json_add_object(res, "tools", tools_arr);
        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "tools/call") == 0) {
        if (!req->params) {
            return csilk_mcp_msg_create_error(req->id, CSILK_MCP_INVALID_PARAMS, "Missing params");
        }

        csilk_json_t* j_name = csilk_json_get(req->params, "name");
        csilk_json_t* j_args = csilk_json_get(req->params, "arguments");
        if (!j_name || !csilk_json_is_string(j_name)) {
            return csilk_mcp_msg_create_error(
                req->id, CSILK_MCP_INVALID_PARAMS, "Missing tool name");
        }

        const char*            name = csilk_json_string_value(j_name);
        csilk_wf_tool_entry_t* matched = NULL;

        csilk_mutex_lock(&server->mutex);
        for (size_t i = 0; i < server->tool_count; i++) {
            if (strcmp(server->tools[i]->name, name) == 0) {
                matched = server->tools[i];
                break;
            }
        }
        csilk_mutex_unlock(&server->mutex);

        if (!matched || !matched->fn) {
            return csilk_mcp_msg_create_error(
                req->id, CSILK_MCP_METHOD_NOT_FOUND, "Tool not found");
        }

        char* args_str = j_args ? csilk_json_serialize(j_args, NULL) : strdup("{}");
        char* tool_res = matched->fn(args_str, matched->user_data);
        free(args_str);

        csilk_json_t* res = csilk_json_object();
        csilk_json_t* content_arr = csilk_json_array();
        csilk_json_t* item = csilk_json_object();
        csilk_json_add_string(item, "type", "text");
        csilk_json_add_string(item, "text", tool_res ? tool_res : "");
        csilk_json_array_append(content_arr, item);
        csilk_json_add_object(res, "content", content_arr);
        csilk_json_add_bool(res, "isError", false);

        if (tool_res) {
            free(tool_res);
        }

        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "resources/list") == 0) {
        csilk_json_t* res = csilk_json_object();
        csilk_json_t* arr = csilk_json_array();

        csilk_mutex_lock(&server->mutex);
        for (size_t i = 0; i < server->resource_count; i++) {
            csilk_mcp_resource_entry_t* r = server->resources[i];
            csilk_json_t*               item = csilk_json_object();
            csilk_json_add_string(item, "uri", r->uri);
            csilk_json_add_string(item, "name", r->name);
            csilk_json_add_string(item, "mimeType", r->mime_type ? r->mime_type : "text/plain");
            csilk_json_array_append(arr, item);
        }
        csilk_mutex_unlock(&server->mutex);

        csilk_json_add_object(res, "resources", arr);
        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "resources/read") == 0) {
        if (!req->params) {
            return csilk_mcp_msg_create_error(req->id, CSILK_MCP_INVALID_PARAMS, "Missing params");
        }
        const char* uri = csilk_json_get_string(req->params, "uri");
        if (!uri || !uri[0]) {
            return csilk_mcp_msg_create_error(req->id, CSILK_MCP_INVALID_PARAMS, "Missing uri");
        }

        csilk_mutex_lock(&server->mutex);
        csilk_mcp_resource_entry_t* matched = NULL;
        for (size_t i = 0; i < server->resource_count; i++) {
            if (strcmp(server->resources[i]->uri, uri) == 0) {
                matched = server->resources[i];
                break;
            }
        }

        if (!matched) {
            csilk_mutex_unlock(&server->mutex);
            return csilk_mcp_msg_create_error(
                req->id, CSILK_MCP_INVALID_PARAMS, "Resource not found");
        }

        char* dyn_text = NULL;
        if (matched->read_fn) {
            dyn_text = matched->read_fn(matched->uri, matched->user_data);
        }
        const char* final_text =
            dyn_text ? dyn_text : (matched->static_text ? matched->static_text : "");

        csilk_json_t* res = csilk_json_object();
        csilk_json_t* arr = csilk_json_array();
        csilk_json_t* item = csilk_json_object();
        csilk_json_add_string(item, "uri", matched->uri);
        csilk_json_add_string(
            item, "mimeType", matched->mime_type ? matched->mime_type : "text/plain");
        csilk_json_add_string(item, "text", final_text);
        csilk_json_array_append(arr, item);
        csilk_json_add_object(res, "contents", arr);
        csilk_mutex_unlock(&server->mutex);

        free(dyn_text);
        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "prompts/list") == 0) {
        csilk_json_t* res = csilk_json_object();
        csilk_json_t* arr = csilk_json_array();

        csilk_mutex_lock(&server->mutex);
        for (size_t i = 0; i < server->prompt_count; i++) {
            csilk_mcp_prompt_entry_t* p = server->prompts[i];
            csilk_json_t*             item = csilk_json_object();
            csilk_json_add_string(item, "name", p->name);
            csilk_json_add_string(item, "description", p->description ? p->description : "");
            csilk_json_array_append(arr, item);
        }
        csilk_mutex_unlock(&server->mutex);

        csilk_json_add_object(res, "prompts", arr);
        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "prompts/get") == 0) {
        if (!req->params) {
            return csilk_mcp_msg_create_error(req->id, CSILK_MCP_INVALID_PARAMS, "Missing params");
        }
        const char* name = csilk_json_get_string(req->params, "name");
        if (!name || !name[0]) {
            return csilk_mcp_msg_create_error(
                req->id, CSILK_MCP_INVALID_PARAMS, "Missing prompt name");
        }

        csilk_mutex_lock(&server->mutex);
        csilk_mcp_prompt_entry_t* matched = NULL;
        for (size_t i = 0; i < server->prompt_count; i++) {
            if (strcmp(server->prompts[i]->name, name) == 0) {
                matched = server->prompts[i];
                break;
            }
        }

        if (!matched) {
            csilk_mutex_unlock(&server->mutex);
            return csilk_mcp_msg_create_error(
                req->id, CSILK_MCP_INVALID_PARAMS, "Prompt not found");
        }

        csilk_json_t* res = csilk_json_object();
        csilk_json_add_string(res, "description", matched->description ? matched->description : "");
        csilk_json_t* msgs = csilk_json_array();
        csilk_json_t* m = csilk_json_object();
        csilk_json_add_string(m, "role", "user");
        csilk_json_t* cnt = csilk_json_object();
        csilk_json_add_string(cnt, "type", "text");
        csilk_json_add_string(
            cnt, "text", matched->prompt_template ? matched->prompt_template : "");
        csilk_json_add_object(m, "content", cnt);
        csilk_json_array_append(msgs, m);
        csilk_json_add_object(res, "messages", msgs);
        csilk_mutex_unlock(&server->mutex);

        return csilk_mcp_msg_create_response(req->id, res);
    }

    return csilk_mcp_msg_create_error(
        req->id, CSILK_MCP_METHOD_NOT_FOUND, "Method not implemented");
}

/**
 * @brief Run a blocking JSON-RPC stdio server loop.
 *
 * Reads one line at a time from stdin, parses each as a JSON-RPC request,
 * dispatches it via handle_mcp_request(), and writes the serialized response
 * followed by a newline to stdout (flushed). The loop ends on EOF (stdin
 * closed). Memory for each request/response is freed before the next read.
 *
 * @param[in,out] server The MCP server handling the requests.
 * @return 0 on a clean EOF-driven exit, or -1 if @p server is NULL.
 */
int
csilk_mcp_server_start_stdio(csilk_mcp_server_t* server)
{
    if (!server) {
        return -1;
    }

    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len == 0) {
            continue;
        }

        csilk_mcp_msg_t* req = csilk_mcp_msg_parse(line, len, NULL);
        if (req) {
            csilk_mcp_msg_t* resp = handle_mcp_request(server, req);
            if (resp) {
                char* out = csilk_mcp_msg_serialize(resp, NULL);
                if (out) {
                    fprintf(stdout, "%s\n", out);
                    fflush(stdout);
                    free(out);
                }
                if (resp->id) {
                    csilk_json_free(resp->id);
                }
                if (resp->result) {
                    csilk_json_free(resp->result);
                }
                if (resp->error) {
                    csilk_json_free(resp->error);
                }
                free(resp);
            }
            if (req->id) {
                csilk_json_free(req->id);
            }
            if (req->params) {
                csilk_json_free(req->params);
            }
            if (req->method) {
                free(req->method);
            }
            free(req);
        }
    }

    return 0;
}

/** @brief HTTP route handler: parse the POST body as JSON-RPC, dispatch, and reply. */
static void
mcp_sse_post_handler(csilk_ctx_t* c)
{
    csilk_mcp_server_t* server = (csilk_mcp_server_t*)csilk_get_param(c, "_mcp_server");
    size_t              body_len = 0;
    const char*         body = csilk_get_body(c, &body_len);

    if (!server || !body || body_len == 0) {
        csilk_set_status(c, 400);
        const char* err_str = "{\"error\":\"Invalid request\"}";
        csilk_set_response_body(c, err_str, strlen(err_str), 0);
        return;
    }

    csilk_mcp_msg_t* req = csilk_mcp_msg_parse(body, body_len, NULL);
    if (!req) {
        csilk_set_status(c, 400);
        const char* err_str = "{\"error\":\"Parse error\"}";
        csilk_set_response_body(c, err_str, strlen(err_str), 0);
        return;
    }

    csilk_mcp_msg_t* resp = handle_mcp_request(server, req);
    char*            out = resp ? csilk_mcp_msg_serialize(resp, NULL) : strdup("{}");
    const char*      payload = out ? out : "{}";

    csilk_set_status(c, 200);
    csilk_set_request_header(c, "Content-Type", "application/json");
    csilk_set_response_body(c, payload, strlen(payload), 0);

    if (out) {
        free(out);
    }
    if (resp) {
        if (resp->id) {
            csilk_json_free(resp->id);
        }
        if (resp->result) {
            csilk_json_free(resp->result);
        }
        if (resp->error) {
            csilk_json_free(resp->error);
        }
        free(resp);
    }
    if (req->id) {
        csilk_json_free(req->id);
    }
    if (req->params) {
        csilk_json_free(req->params);
    }
    if (req->method) {
        free(req->method);
    }
    free(req);
}

static void
mcp_sse_get_handler(csilk_ctx_t* c)
{
    const char* prefix = (const char*)csilk_get_param(c, "_mcp_prefix");
    if (!prefix) {
        prefix = "/mcp";
    }

    csilk_set_status(c, 200);
    csilk_set_header(c, "Content-Type", "text/event-stream");
    csilk_set_header(c, "Cache-Control", "no-cache");
    csilk_set_header(c, "Connection", "keep-alive");

    char endpoint_ev[384];
    snprintf(endpoint_ev, sizeof(endpoint_ev), "event: endpoint\ndata: %s/message\n\n", prefix);
    csilk_set_response_body(c, endpoint_ev, strlen(endpoint_ev), 0);
}

/**
 * @brief Bind the MCP JSON-RPC endpoint as an HTTP POST route on an app.
 *
 * Registers a POST handler at "<route_prefix>/message" (default "/mcp") that
 * parses the request body as a JSON-RPC message and replies with the
 * dispatch result. Also registers a GET handler at "<route_prefix>/sse" that
 * emits the standard MCP endpoint event.
 *
 * @param[in,out] server       The MCP server handling dispatched requests.
 * @param[in,out] app          The application to register the route on.
 * @param[in]     route_prefix URL prefix for the endpoint (defaults to
 *                             "/mcp" when NULL).
 * @return 0 on success, or -1 on invalid arguments.
 */
int
csilk_mcp_server_bind_app(csilk_mcp_server_t* server, csilk_app_t* app, const char* route_prefix)
{
    if (!server || !app) {
        return -1;
    }

    const char* pfx = route_prefix ? route_prefix : "/mcp";
    char        msg_path[256];
    char        sse_path[256];
    snprintf(msg_path, sizeof(msg_path), "%s/message", pfx);
    snprintf(sse_path, sizeof(sse_path), "%s/sse", pfx);

    csilk_app_add_route(app, "POST", msg_path, mcp_sse_post_handler);
    csilk_app_add_route(app, "GET", sse_path, mcp_sse_get_handler);
    return 0;
}

/**
 * @brief Register a WASM module as an MCP server tool.
 */
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
