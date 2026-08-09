#include "csilk/core/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcp_internal.h"

csilk_mcp_msg_t*
csilk_mcp_msg_parse(const char* buf, size_t len, csilk_arena_t* arena)
{
    if (!buf || len == 0) {
        return nullptr;
    }

    csilk_json_t* root = csilk_json_parse_len(buf, len);
    if (!root) {
        return nullptr;
    }

    csilk_mcp_msg_t* msg = nullptr;
    if (arena) {
        msg = (csilk_mcp_msg_t*)csilk_arena_alloc(arena, sizeof(csilk_mcp_msg_t));
    } else {
        msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    }
    if (!msg) {
        csilk_json_free(root);
        return nullptr;
    }
    memset(msg, 0, sizeof(csilk_mcp_msg_t));

    csilk_json_t* j_ver = csilk_json_get(root, "jsonrpc");
    if (j_ver && csilk_json_is_string(j_ver)) {
        snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "%s", csilk_json_string_value(j_ver));
    } else {
        snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "2.0");
    }

    csilk_json_t* j_id = csilk_json_get(root, "id");
    if (j_id) {
        msg->id = csilk_json_copy(j_id);
    }

    csilk_json_t* j_method = csilk_json_get(root, "method");
    if (j_method && csilk_json_is_string(j_method)) {
        if (arena) {
            msg->method = csilk_arena_strdup(arena, csilk_json_string_value(j_method));
        } else {
            msg->method = strdup(csilk_json_string_value(j_method));
        }
    }

    csilk_json_t* j_params = csilk_json_get(root, "params");
    if (j_params) {
        msg->params = csilk_json_copy(j_params);
    }

    csilk_json_t* j_result = csilk_json_get(root, "result");
    if (j_result) {
        msg->result = csilk_json_copy(j_result);
    }

    csilk_json_t* j_error = csilk_json_get(root, "error");
    if (j_error) {
        msg->error = csilk_json_copy(j_error);
    }

    csilk_json_free(root);
    return msg;
}

char*
csilk_mcp_msg_serialize(const csilk_mcp_msg_t* msg, csilk_arena_t* arena)
{
    if (!msg) {
        return nullptr;
    }

    csilk_json_t* root = csilk_json_object();
    if (!root) {
        return nullptr;
    }

    csilk_json_add_string(root, "jsonrpc", msg->jsonrpc[0] ? msg->jsonrpc : "2.0");

    if (msg->id) {
        csilk_json_add_object(root, "id", csilk_json_copy(msg->id));
    }
    if (msg->method) {
        csilk_json_add_string(root, "method", msg->method);
    }
    if (msg->params) {
        csilk_json_add_object(root, "params", csilk_json_copy(msg->params));
    }
    if (msg->result) {
        csilk_json_add_object(root, "result", csilk_json_copy(msg->result));
    }
    if (msg->error) {
        csilk_json_add_object(root, "error", csilk_json_copy(msg->error));
    }

    char* formatted = csilk_json_serialize(root, NULL);
    csilk_json_free(root);

    if (arena && formatted) {
        char* arena_out = csilk_arena_strdup(arena, formatted);
        free(formatted);
        return arena_out;
    }

    return formatted;
}

csilk_mcp_msg_t*
csilk_mcp_msg_create_error(csilk_json_t* id, int code, const char* message)
{
    csilk_mcp_msg_t* msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    if (!msg) {
        return nullptr;
    }

    snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "2.0");
    if (id) {
        msg->id = csilk_json_copy(id);
    }

    csilk_json_t* err_obj = csilk_json_object();
    csilk_json_add_int(err_obj, "code", code);
    csilk_json_add_string(err_obj, "message", message ? message : "Unknown error");
    msg->error = err_obj;

    return msg;
}

csilk_mcp_msg_t*
csilk_mcp_msg_create_response(csilk_json_t* id, csilk_json_t* result)
{
    csilk_mcp_msg_t* msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    if (!msg) {
        return nullptr;
    }

    snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "2.0");
    if (id) {
        msg->id = csilk_json_copy(id);
    }
    if (result) {
        msg->result = csilk_json_copy(result);
    }

    return msg;
}
