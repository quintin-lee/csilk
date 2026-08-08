#include "cJSON.h"
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

    cJSON* root = cJSON_ParseWithLength(buf, len);
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
        cJSON_Delete(root);
        return nullptr;
    }
    memset(msg, 0, sizeof(csilk_mcp_msg_t));

    cJSON* j_ver = cJSON_GetObjectItem(root, "jsonrpc");
    if (j_ver && cJSON_IsString(j_ver)) {
        snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "%s", j_ver->valuestring);
    } else {
        snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "2.0");
    }

    cJSON* j_id = cJSON_GetObjectItem(root, "id");
    if (j_id) {
        msg->id = cJSON_Duplicate(j_id, 1);
    }

    cJSON* j_method = cJSON_GetObjectItem(root, "method");
    if (j_method && cJSON_IsString(j_method)) {
        if (arena) {
            msg->method = csilk_arena_strdup(arena, j_method->valuestring);
        } else {
            msg->method = strdup(j_method->valuestring);
        }
    }

    cJSON* j_params = cJSON_GetObjectItem(root, "params");
    if (j_params) {
        msg->params = cJSON_Duplicate(j_params, 1);
    }

    cJSON* j_result = cJSON_GetObjectItem(root, "result");
    if (j_result) {
        msg->result = cJSON_Duplicate(j_result, 1);
    }

    cJSON* j_error = cJSON_GetObjectItem(root, "error");
    if (j_error) {
        msg->error = cJSON_Duplicate(j_error, 1);
    }

    cJSON_Delete(root);
    return msg;
}

char*
csilk_mcp_msg_serialize(const csilk_mcp_msg_t* msg, csilk_arena_t* arena)
{
    if (!msg) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }

    cJSON_AddStringToObject(root, "jsonrpc", msg->jsonrpc[0] ? msg->jsonrpc : "2.0");

    if (msg->id) {
        cJSON_AddItemToObject(root, "id", cJSON_Duplicate(msg->id, 1));
    }
    if (msg->method) {
        cJSON_AddStringToObject(root, "method", msg->method);
    }
    if (msg->params) {
        cJSON_AddItemToObject(root, "params", cJSON_Duplicate(msg->params, 1));
    }
    if (msg->result) {
        cJSON_AddItemToObject(root, "result", cJSON_Duplicate(msg->result, 1));
    }
    if (msg->error) {
        cJSON_AddItemToObject(root, "error", cJSON_Duplicate(msg->error, 1));
    }

    char* formatted = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (arena && formatted) {
        char* arena_out = csilk_arena_strdup(arena, formatted);
        free(formatted);
        return arena_out;
    }

    return formatted;
}

csilk_mcp_msg_t*
csilk_mcp_msg_create_error(cJSON* id, int code, const char* message)
{
    csilk_mcp_msg_t* msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    if (!msg) {
        return nullptr;
    }

    snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "2.0");
    if (id) {
        msg->id = cJSON_Duplicate(id, 1);
    }

    cJSON* err_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(err_obj, "code", code);
    cJSON_AddStringToObject(err_obj, "message", message ? message : "Unknown error");
    msg->error = err_obj;

    return msg;
}

csilk_mcp_msg_t*
csilk_mcp_msg_create_response(cJSON* id, cJSON* result)
{
    csilk_mcp_msg_t* msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    if (!msg) {
        return nullptr;
    }

    snprintf(msg->jsonrpc, sizeof(msg->jsonrpc), "2.0");
    if (id) {
        msg->id = cJSON_Duplicate(id, 1);
    }
    if (result) {
        msg->result = cJSON_Duplicate(result, 1);
    }

    return msg;
}
