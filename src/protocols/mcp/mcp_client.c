/**
 * @file mcp_client.c
 * @brief MCP client implementation (stdio and SSE transports).
 *
 * Provides constructors for connecting to a remote MCP server over a child
 * process's stdio pipes or over a Server-Sent Events (SSE) HTTP endpoint,
 * plus tool-import helpers used by the workflow engine.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/protocols/mcp.h"
#include "mcp_internal.h"

/**
 * @brief Connect to an MCP server by launching it as a child process over
 * stdio.
 *
 * Records the command name as the server identity and marks the client as a
 * stdio transport. The underlying pipes are not actually spawned here; this
 * call only initializes the client handle.
 *
 * @param[in]  command The command used to launch the MCP server.
 * @param[in]  argv    Unused argument vector (reserved for future use).
 * @return A newly allocated csilk_mcp_client_t, or NULL on allocation or
 *         argument failure.
 */
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

/**
 * @brief Connect to a remote MCP server over a Server-Sent Events (SSE)
 * endpoint.
 *
 * Stores the SSE URL on the client and marks it as a non-stdio (network)
 * transport.
 *
 * @param[in] sse_url The SSE endpoint URL of the MCP server.
 * @return A newly allocated csilk_mcp_client_t, or NULL on allocation or
 *         argument failure.
 */
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

/**
 * @brief Destroy an MCP client and release its resources.
 *
 * Destroys the internal mutex and frees the client handle. Safe to call with
 * a NULL pointer.
 *
 * @param[in] client The MCP client to free (may be NULL).
 */
void
csilk_mcp_client_free(csilk_mcp_client_t* client)
{
    if (!client) {
        return;
    }

    csilk_mutex_destroy(&client->mutex);
    free(client);
}

/**
 * @brief Register a placeholder tool imported from the MCP server into a
 * workflow.
 *
 * Currently registers a mock tool named "imported_mcp_tool" so that the
 * workflow engine can reference remote tools discovered via MCP. Real remote
 * invocation is not yet wired up.
 *
 * @param[in,out] client The MCP client (validated but not otherwise used).
 * @param[in,out] wf     The workflow to register the imported tool into.
 * @return 1 on success, or -1 if @p client or @p wf is NULL.
 */
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
