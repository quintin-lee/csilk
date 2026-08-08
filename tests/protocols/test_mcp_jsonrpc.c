#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/mcp/mcp_internal.h"

static void
test_jsonrpc_parse_request(void)
{
    const char* json = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{}}";
    csilk_mcp_msg_t* msg = csilk_mcp_msg_parse(json, strlen(json), nullptr);

    assert(msg != nullptr);
    assert(strcmp(msg->jsonrpc, "2.0") == 0);
    assert(msg->id != nullptr);
    assert(msg->id->valueint == 1);
    assert(msg->method != nullptr);
    assert(strcmp(msg->method, "tools/list") == 0);

    char* out = csilk_mcp_msg_serialize(msg, nullptr);
    assert(out != nullptr);
    assert(strstr(out, "tools/list") != nullptr);

    free(out);
    cJSON_Delete(msg->id);
    if (msg->params) {
        cJSON_Delete(msg->params);
    }
    free(msg->method);
    free(msg);
    printf("test_jsonrpc_parse_request passed\n");
}

static void
test_jsonrpc_create_error(void)
{
    cJSON*           id = cJSON_CreateNumber(42);
    csilk_mcp_msg_t* msg =
        csilk_mcp_msg_create_error(id, CSILK_MCP_METHOD_NOT_FOUND, "Method not found");

    assert(msg != nullptr);
    assert(msg->error != nullptr);
    cJSON* code = cJSON_GetObjectItem(msg->error, "code");
    assert(code != nullptr && code->valueint == CSILK_MCP_METHOD_NOT_FOUND);

    char* out = csilk_mcp_msg_serialize(msg, nullptr);
    assert(out != nullptr);
    assert(strstr(out, "Method not found") != nullptr);

    free(out);
    cJSON_Delete(id);
    if (msg->id) {
        cJSON_Delete(msg->id);
    }
    if (msg->error) {
        cJSON_Delete(msg->error);
    }
    free(msg);
    printf("test_jsonrpc_create_error passed\n");
}

int
main(void)
{
    test_jsonrpc_parse_request();
    test_jsonrpc_create_error();
    printf("All test_mcp_jsonrpc tests passed successfully!\n");
    return 0;
}
