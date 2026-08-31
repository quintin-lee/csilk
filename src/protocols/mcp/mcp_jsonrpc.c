/**
 * @file mcp_jsonrpc.c
 * @brief JSON-RPC 2.0 message parsing and serialization for MCP.
 *
 * Implements conversion between raw JSON text and the internal
 * csilk_mcp_msg_t frame used by the MCP server and client. Error and
 * response messages can be built directly from an id and payload without a
 * full parse.
 *
 * @copyright MIT License
 */

#include "csilk/core/json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcp_internal.h"

/**
 * @brief Parse a JSON-RPC 2.0 message from raw text into a csilk_mcp_msg_t.
 *
 * Validates the input buffer, extracts jsonrpc, id, method, params, result,
 * and error fields, and copies the relevant nodes. If the jsonrpc field is
 * absent it defaults to "2.0".
 *
 * @param[in]  buf   Raw JSON message text.
 * @param[in]  len   Length of @p buf in bytes.
 * @param[in]  arena Optional arena allocator; when non-NULL, all memory for
 *                    the message is drawn from the arena instead of the heap.
 * @return A newly allocated csilk_mcp_msg_t, or NULL on parse or allocation
 *         failure, or when @p buf is NULL/empty. The caller frees it (unless
 *         allocated from an arena).
 */
csilk_mcp_msg_t*
csilk_mcp_msg_parse(const char* buf, size_t len, csilk_arena_t* arena)
{
    if (!buf || len == 0) {
        return NULL;
    }

    csilk_json_t* root = csilk_json_parse_len(buf, len);
    if (!root) {
        return NULL;
    }

    csilk_mcp_msg_t* msg = NULL;
    if (arena) {
        msg = (csilk_mcp_msg_t*)csilk_arena_alloc(arena, sizeof(csilk_mcp_msg_t));
    } else {
        msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    }
    if (!msg) {
        csilk_json_free(root);
        return NULL;
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

/**
 * @brief Serialize a csilk_mcp_msg_t back into a JSON-RPC 2.0 text string.
 *
 * Builds a JSON object containing jsonrpc, and any of id, method, params,
 * result, and error that are present (jsonrpc defaults to "2.0" when empty).
 *
 * @param[in]  msg   The message frame to serialize (may be NULL).
 * @param[in]  arena Optional arena allocator; when non-NULL the returned
 *                    string is arena-allocated, otherwise heap-allocated with
 *                    malloc and must be freed by the caller.
 * @return A NUL-terminated JSON string, or NULL on allocation failure. Returns
 *         NULL if @p msg is NULL.
 */
char*
csilk_mcp_msg_serialize(const csilk_mcp_msg_t* msg, csilk_arena_t* arena)
{
    if (!msg) {
        return NULL;
    }

    csilk_json_t* root = csilk_json_object();
    if (!root) {
        return NULL;
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

/**
 * @brief Build an error csilk_mcp_msg_t from an id, code, and message.
 *
 * Always allocates from the heap (never an arena). Produces a frame with
 * jsonrpc "2.0", an optional copied id, and an "error" object containing the
 * numeric code and human-readable message.
 *
 * @param[in] id      Request id to echo (may be NULL for notifications).
 * @param[in] code    JSON-RPC error code (e.g., CSILK_MCP_INVALID_PARAMS).
 * @param[in] message Human-readable error message (may be NULL).
 * @return A newly allocated csilk_mcp_msg_t, or NULL on allocation failure.
 *         The caller frees it with free().
 */
csilk_mcp_msg_t*
csilk_mcp_msg_create_error(csilk_json_t* id, int code, const char* message)
{
    csilk_mcp_msg_t* msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    if (!msg) {
        return NULL;
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

/**
 * @brief Build a success-response csilk_mcp_msg_t from an id and result.
 *
 * Always allocates from the heap (never an arena). Produces a frame with
 * jsonrpc "2.0", an optional copied id, and a copied "result" payload.
 *
 * @param[in] id     Request id to echo (may be NULL for notifications).
 * @param[in] result Result payload to copy (may be NULL).
 * @return A newly allocated csilk_mcp_msg_t, or NULL on allocation failure.
 *         The caller frees it with free().
 */
csilk_mcp_msg_t*
csilk_mcp_msg_create_response(csilk_json_t* id, csilk_json_t* result)
{
    csilk_mcp_msg_t* msg = (csilk_mcp_msg_t*)calloc(1, sizeof(csilk_mcp_msg_t));
    if (!msg) {
        return NULL;
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
