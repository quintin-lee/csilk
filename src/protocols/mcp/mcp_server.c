#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/protocols/mcp.h"
#include "mcp_internal.h"
#include "workflow/workflow_internal.h"

csilk_mcp_server_t*
csilk_mcp_server_new(const char* name, const char* version)
{
    csilk_mcp_server_t* server = (csilk_mcp_server_t*)calloc(1, sizeof(csilk_mcp_server_t));
    if (!server) {
        return nullptr;
    }

    snprintf(server->name, sizeof(server->name), "%s", name ? name : "csilk-mcp-server");
    snprintf(server->version, sizeof(server->version), "%s", version ? version : "1.0.0");
    csilk_mutex_init(&server->mutex);

    return server;
}

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
    if (server->workflows) {
        free(server->workflows);
    }
    csilk_mutex_unlock(&server->mutex);
    csilk_mutex_destroy(&server->mutex);
    free(server);
}

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
    entry->description = tool->description ? strdup(tool->description) : nullptr;
    entry->parameters_json = tool->parameters_json ? strdup(tool->parameters_json) : nullptr;
    entry->fn = tool->fn;
    entry->user_data = tool->user_data;

    server->tools = new_tools;
    server->tools[server->tool_count++] = entry;
    csilk_mutex_unlock(&server->mutex);

    return 0;
}

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

static csilk_mcp_msg_t*
handle_mcp_request(csilk_mcp_server_t* server, const csilk_mcp_msg_t* req)
{
    if (!req || !req->method) {
        return csilk_mcp_msg_create_error(
            req ? req->id : nullptr, CSILK_MCP_INVALID_REQUEST, "Invalid Request");
    }

    if (strcmp(req->method, "initialize") == 0) {
        cJSON* res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "protocolVersion", "2024-11-05");

        cJSON* server_info = cJSON_CreateObject();
        cJSON_AddStringToObject(server_info, "name", server->name);
        cJSON_AddStringToObject(server_info, "version", server->version);
        cJSON_AddItemToObject(res, "serverInfo", server_info);

        cJSON* caps = cJSON_CreateObject();
        cJSON* tools_cap = cJSON_CreateObject();
        cJSON_AddBoolToObject(tools_cap, "listChanged", false);
        cJSON_AddItemToObject(caps, "tools", tools_cap);
        cJSON_AddItemToObject(res, "capabilities", caps);

        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "tools/list") == 0) {
        cJSON* res = cJSON_CreateObject();
        cJSON* tools_arr = cJSON_CreateArray();

        csilk_mutex_lock(&server->mutex);
        for (size_t i = 0; i < server->tool_count; i++) {
            csilk_wf_tool_entry_t* t = server->tools[i];
            cJSON*                 t_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(t_obj, "name", t->name);
            cJSON_AddStringToObject(t_obj, "description", t->description ? t->description : "");

            if (t->parameters_json) {
                cJSON* schema = cJSON_Parse(t->parameters_json);
                if (schema) {
                    cJSON_AddItemToObject(t_obj, "inputSchema", schema);
                } else {
                    cJSON_AddItemToObject(t_obj, "inputSchema", cJSON_CreateObject());
                }
            } else {
                cJSON_AddItemToObject(t_obj, "inputSchema", cJSON_CreateObject());
            }
            cJSON_AddItemToArray(tools_arr, t_obj);
        }
        csilk_mutex_unlock(&server->mutex);

        cJSON_AddItemToObject(res, "tools", tools_arr);
        return csilk_mcp_msg_create_response(req->id, res);
    }

    if (strcmp(req->method, "tools/call") == 0) {
        if (!req->params) {
            return csilk_mcp_msg_create_error(req->id, CSILK_MCP_INVALID_PARAMS, "Missing params");
        }

        cJSON* j_name = cJSON_GetObjectItem(req->params, "name");
        cJSON* j_args = cJSON_GetObjectItem(req->params, "arguments");
        if (!j_name || !cJSON_IsString(j_name)) {
            return csilk_mcp_msg_create_error(
                req->id, CSILK_MCP_INVALID_PARAMS, "Missing tool name");
        }

        const char*            name = j_name->valuestring;
        csilk_wf_tool_entry_t* matched = nullptr;

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

        char* args_str = j_args ? cJSON_PrintUnformatted(j_args) : strdup("{}");
        char* tool_res = matched->fn(args_str, matched->user_data);
        free(args_str);

        cJSON* res = cJSON_CreateObject();
        cJSON* content_arr = cJSON_CreateArray();
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", tool_res ? tool_res : "");
        cJSON_AddItemToArray(content_arr, item);
        cJSON_AddItemToObject(res, "content", content_arr);
        cJSON_AddBoolToObject(res, "isError", false);

        if (tool_res) {
            free(tool_res);
        }

        return csilk_mcp_msg_create_response(req->id, res);
    }

    return csilk_mcp_msg_create_error(
        req->id, CSILK_MCP_METHOD_NOT_FOUND, "Method not implemented");
}

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

        csilk_mcp_msg_t* req = csilk_mcp_msg_parse(line, len, nullptr);
        if (req) {
            csilk_mcp_msg_t* resp = handle_mcp_request(server, req);
            if (resp) {
                char* out = csilk_mcp_msg_serialize(resp, nullptr);
                if (out) {
                    fprintf(stdout, "%s\n", out);
                    fflush(stdout);
                    free(out);
                }
                if (resp->id) {
                    cJSON_Delete(resp->id);
                }
                if (resp->result) {
                    cJSON_Delete(resp->result);
                }
                if (resp->error) {
                    cJSON_Delete(resp->error);
                }
                free(resp);
            }
            if (req->id) {
                cJSON_Delete(req->id);
            }
            if (req->params) {
                cJSON_Delete(req->params);
            }
            if (req->method) {
                free(req->method);
            }
            free(req);
        }
    }

    return 0;
}

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

    csilk_mcp_msg_t* req = csilk_mcp_msg_parse(body, body_len, nullptr);
    if (!req) {
        csilk_set_status(c, 400);
        const char* err_str = "{\"error\":\"Parse error\"}";
        csilk_set_response_body(c, err_str, strlen(err_str), 0);
        return;
    }

    csilk_mcp_msg_t* resp = handle_mcp_request(server, req);
    char*            out = resp ? csilk_mcp_msg_serialize(resp, nullptr) : strdup("{}");
    const char*      payload = out ? out : "{}";

    csilk_set_status(c, 200);
    csilk_set_request_header(c, "Content-Type", "application/json");
    csilk_set_response_body(c, payload, strlen(payload), 0);

    if (out) {
        free(out);
    }
    if (resp) {
        if (resp->id) {
            cJSON_Delete(resp->id);
        }
        if (resp->result) {
            cJSON_Delete(resp->result);
        }
        if (resp->error) {
            cJSON_Delete(resp->error);
        }
        free(resp);
    }
    if (req->id) {
        cJSON_Delete(req->id);
    }
    if (req->params) {
        cJSON_Delete(req->params);
    }
    if (req->method) {
        free(req->method);
    }
    free(req);
}

int
csilk_mcp_server_bind_app(csilk_mcp_server_t* server, csilk_app_t* app, const char* route_prefix)
{
    if (!server || !app) {
        return -1;
    }

    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "%s/message", route_prefix ? route_prefix : "/mcp");

    csilk_app_add_route(app, "POST", path_buf, mcp_sse_post_handler);
    return 0;
}
