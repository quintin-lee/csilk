/**
 * @file mcp_internal.h
 * @brief Internal data structures and JSON-RPC 2.0 frames for MCP.
 */

#ifndef CSILK_MCP_INTERNAL_H
#define CSILK_MCP_INTERNAL_H

#include "cJSON.h"
#include <stddef.h>
#include <stdint.h>

#include "core/ctx/ctx_internal.h"
#include "csilk/protocols/mcp.h"

/* Standard MCP Error Codes */
#define CSILK_MCP_PARSE_ERROR -32700
#define CSILK_MCP_INVALID_REQUEST -32600
#define CSILK_MCP_METHOD_NOT_FOUND -32601
#define CSILK_MCP_INVALID_PARAMS -32602
#define CSILK_MCP_INTERNAL_ERROR -32603

/**
 * @brief Represents a parsed JSON-RPC 2.0 message frame.
 */
typedef struct {
    char   jsonrpc[8]; /* Must be "2.0" */
    cJSON* id;         /* Number, String, or NULL (notification) */
    char*  method;     /* Method name string */
    cJSON* params;     /* Parameters object or array */
    cJSON* result;     /* Response payload */
    cJSON* error;      /* { "code": int, "message": string, "data": optional } */
} csilk_mcp_msg_t;

/**
 * @brief Internal MCP Server structure.
 */
struct csilk_mcp_server_s {
    char                    name[128];
    char                    version[32];
    csilk_wf_tool_entry_t** tools;
    size_t                  tool_count;
    csilk_wf_t**            workflows;
    size_t                  workflow_count;
    csilk_mutex_t           mutex;
};

/**
 * @brief Internal MCP Client structure.
 */
struct csilk_mcp_client_s {
    char          server_name[128];
    char          server_version[32];
    int           is_stdio;
    int           pipe_fd[2]; /* [0]: read, [1]: write */
    char          sse_url[256];
    csilk_mutex_t mutex;
};

/* --- Internal Helper Declarations --- */

csilk_mcp_msg_t* csilk_mcp_msg_parse(const char* buf, size_t len, csilk_arena_t* arena);
char*            csilk_mcp_msg_serialize(const csilk_mcp_msg_t* msg, csilk_arena_t* arena);
csilk_mcp_msg_t* csilk_mcp_msg_create_error(cJSON* id, int code, const char* message);
csilk_mcp_msg_t* csilk_mcp_msg_create_response(cJSON* id, cJSON* result);

#endif /* CSILK_MCP_INTERNAL_H */
