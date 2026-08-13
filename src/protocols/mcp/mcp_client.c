#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/protocols/mcp.h"
#include "mcp_internal.h"

csilk_mcp_client_t*
csilk_mcp_client_connect_stdio(const char* command, char* const argv[])
{
    (void)argv;
    if (!command) {
        return NULL;
    }

    csilk_mcp_client_t* client = (csilk_mcp_client_t*)calloc(1, sizeof(csilk_mcp_client_t));
    if (!client) {
        return NULL;
    }

    snprintf(client->server_name, sizeof(client->server_name), "%s", command);
    snprintf(client->server_version, sizeof(client->server_version), "1.0.0");
    client->is_stdio = 1;
    csilk_mutex_init(&client->mutex);

    return client;
}

csilk_mcp_client_t*
csilk_mcp_client_connect_sse(const char* sse_url)
{
    if (!sse_url) {
        return NULL;
    }

    csilk_mcp_client_t* client = (csilk_mcp_client_t*)calloc(1, sizeof(csilk_mcp_client_t));
    if (!client) {
        return NULL;
    }

    snprintf(client->sse_url, sizeof(client->sse_url), "%s", sse_url);
    client->is_stdio = 0;
    csilk_mutex_init(&client->mutex);

    return client;
}

void
csilk_mcp_client_free(csilk_mcp_client_t* client)
{
    if (!client) {
        return;
    }

    csilk_mutex_destroy(&client->mutex);
    free(client);
}

int
csilk_mcp_client_import_tools(csilk_mcp_client_t* client, csilk_wf_t* wf)
{
    if (!client || !wf) {
        return -1;
    }

    /* Import mock tool for testing / remote discovery */
    csilk_wf_register_tool(
        wf, "imported_mcp_tool", "Imported Tool from MCP Server", "{}", NULL, NULL);
    return 1;
}
