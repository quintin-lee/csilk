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

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "csilk/drivers/ai.h"
#include "csilk/drivers/ai/tool_registry.h"
#include "csilk/protocols/mcp.h"
#include "modules/common/curl_common.h"
#include "mcp_internal.h"

/* --- Transport helpers (stdio subprocess & HTTP POST/SSE) --- */

static void
mcp_msg_free(csilk_mcp_msg_t* m)
{
    if (!m) {
        return;
    }
    if (m->id) {
        csilk_json_free(m->id);
    }
    if (m->params) {
        csilk_json_free(m->params);
    }
    if (m->result) {
        csilk_json_free(m->result);
    }
    if (m->error) {
        csilk_json_free(m->error);
    }
    free(m->method);
    free(m);
}

static int
mcp_client_spawn_stdio(csilk_mcp_client_t* client)
{
    if (!client || !client->command) {
        return -1;
    }
    if (client->child_pid > 0 && client->pipe_fd[0] >= 0 && client->pipe_fd[1] >= 0) {
        return 0;
    }

    int in_pipe[2] = {-1, -1};  /* parent writes [1], child reads [0] */
    int out_pipe[2] = {-1, -1}; /* child writes [1], parent reads [0] */
    if (pipe(in_pipe) != 0) {
        return -1;
    }
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (client->argv && client->argv[0]) {
            execvp(client->command, client->argv);
        } else {
            char* fallback_argv[2] = {client->command, NULL};
            execvp(client->command, fallback_argv);
        }
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    client->pipe_fd[0] = out_pipe[0];
    client->pipe_fd[1] = in_pipe[1];
    client->child_pid = (int)pid;
    return 0;
}

static int
mcp_stdio_roundtrip(csilk_mcp_client_t* client, const char* req_json, char** out_line)
{
    if (mcp_client_spawn_stdio(client) != 0) {
        return -1;
    }

    size_t rlen = strlen(req_json);
    char*  line = malloc(rlen + 2);
    if (!line) {
        return -1;
    }
    memcpy(line, req_json, rlen);
    line[rlen] = '\n';
    line[rlen + 1] = '\0';

    size_t sent = 0;
    while (sent < rlen + 1) {
        ssize_t w = write(client->pipe_fd[1], line + sent, (rlen + 1) - sent);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(line);
            return -1;
        }
        sent += (size_t)w;
    }
    free(line);

    size_t cap = 4096;
    size_t len = 0;
    char*  buf = malloc(cap);
    if (!buf) {
        return -1;
    }

    for (;;) {
        struct pollfd pfd = {.fd = client->pipe_fd[0], .events = POLLIN};
        int           prc = poll(&pfd, 1, 5000);
        if (prc <= 0) {
            if (prc < 0 && errno == EINTR) {
                continue;
            }
            free(buf);
            return -1;
        }
        char    ch = 0;
        ssize_t n = read(client->pipe_fd[0], &ch, 1);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (len + 2 >= cap) {
            if (cap > (size_t)(16 * 1024 * 1024)) {
                free(buf);
                return -1;
            }
            cap *= 2;
            char* nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -1;
            }
            buf = nb;
        }
        buf[len++] = ch;
    }
    buf[len] = '\0';
    if (len == 0) {
        free(buf);
        return -1;
    }
    *out_line = buf;
    return 0;
}

static int
mcp_http_roundtrip(csilk_mcp_client_t* client, const char* req_json, char** out_body)
{
    if (!client->sse_url[0]) {
        return -1;
    }

    /* Support both direct message endpoints (/mcp/message) and SSE base URLs
     * (/mcp/sse -> /mcp/message). */
    char   post_url[320];
    size_t ulen = strlen(client->sse_url);
    if (ulen >= 4 && strcmp(client->sse_url + ulen - 4, "/sse") == 0) {
        snprintf(post_url, sizeof(post_url), "%.*s/message", (int)(ulen - 4), client->sse_url);
    } else {
        snprintf(post_url, sizeof(post_url), "%s", client->sse_url);
    }

    csilk_curl_buf_t   chunk = {0};
    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    long               http_code = 0;
    int                rc = csilk_curl_request_json(
        "POST", post_url, hdrs, req_json, 3000L, 10000L, &chunk, &http_code, NULL, 0);
    curl_slist_free_all(hdrs);

    if (rc != 0 || http_code < 200 || http_code >= 300 || chunk.len == 0) {
        free(chunk.data);
        return -1;
    }

    *out_body = chunk.data;
    return 0;
}

static int
mcp_ws_roundtrip(csilk_mcp_client_t* client, const char* req_json, char** out_body)
{
    const char* url = client->ws_url[0] ? client->ws_url : client->sse_url;
    if (!url || !url[0] || !req_json || !out_body) {
        return -1;
    }

#ifdef CURLWS_TEXT
    char target_url[320];
    if (strncmp(url, "ws://", 5) == 0) {
        snprintf(target_url, sizeof(target_url), "http://%s", url + 5);
    } else if (strncmp(url, "wss://", 6) == 0) {
        snprintf(target_url, sizeof(target_url), "https://%s", url + 6);
    } else {
        snprintf(target_url, sizeof(target_url), "%s", url);
    }

    if (!csilk_curl_url_allowed(target_url)) {
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, target_url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
    csilk_curl_apply_safe_defaults(curl, 3000L, 5000L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        size_t sent = 0;
        if (curl_ws_send(curl, req_json, strlen(req_json), &sent, 0, CURLWS_TEXT) == CURLE_OK) {
            enum { MCP_WS_MAX_RESP = 16 * 1024 * 1024 };
            size_t cap = 4096;
            size_t total = 0;
            char*  resp = malloc(cap);
            if (resp) {
                int ok = 1;
                for (;;) {
                    if (cap - total <= 1) {
                        if (cap >= MCP_WS_MAX_RESP) {
                            ok = 0;
                            break;
                        }
                        size_t next_cap = cap * 2;
                        if (next_cap > MCP_WS_MAX_RESP) {
                            next_cap = MCP_WS_MAX_RESP;
                        }
                        char* nb = realloc(resp, next_cap);
                        if (!nb) {
                            ok = 0;
                            break;
                        }
                        resp = nb;
                        cap = next_cap;
                    }
                    size_t                      nread = 0;
                    const struct curl_ws_frame* meta = NULL;
                    CURLcode rc = curl_ws_recv(curl, resp + total, cap - total - 1, &nread, &meta);
                    if (rc == CURLE_AGAIN) {
                        continue;
                    }
                    if (rc != CURLE_OK) {
                        ok = 0;
                        break;
                    }
                    total += nread;
                    if (!meta || (meta->flags & CURLWS_CLOSE)) {
                        break;
                    }
                    if (!(meta->flags & CURLWS_CONT) && meta->bytesleft == 0) {
                        break;
                    }
                }
                if (ok && total > 0) {
                    resp[total] = '\0';
                    *out_body = resp;
                    curl_easy_cleanup(curl);
                    return 0;
                }
                free(resp);
            }
        }
    }
    curl_easy_cleanup(curl);
#endif

    /* If direct WebSocket frame transfer is unsupported or fails, fall back to HTTP */
    return mcp_http_roundtrip(client, req_json, out_body);
}

static csilk_mcp_msg_t*
mcp_client_rpc(csilk_mcp_client_t* client, const char* method, csilk_json_t* params_owned)
{
    if (!client || !method) {
        if (params_owned) {
            csilk_json_free(params_owned);
        }
        return NULL;
    }

    csilk_json_t* req_obj = csilk_json_object();
    if (!req_obj) {
        if (params_owned) {
            csilk_json_free(params_owned);
        }
        return NULL;
    }
    csilk_json_add_string(req_obj, "jsonrpc", "2.0");
    csilk_json_add_int(req_obj, "id", ++client->next_id);
    csilk_json_add_string(req_obj, "method", method);
    if (params_owned) {
        csilk_json_add_object(req_obj, "params", params_owned);
    }

    char* req_str = csilk_json_serialize(req_obj, NULL);
    csilk_json_free(req_obj);
    if (!req_str) {
        return NULL;
    }

    char* resp_str = NULL;
    int   rc = client->is_stdio ? mcp_stdio_roundtrip(client, req_str, &resp_str)
                                : (client->is_ws ? mcp_ws_roundtrip(client, req_str, &resp_str)
                                                 : mcp_http_roundtrip(client, req_str, &resp_str));
    free(req_str);
    if (rc != 0 || !resp_str) {
        free(resp_str);
        return NULL;
    }

    csilk_mcp_msg_t* resp = csilk_mcp_msg_parse(resp_str, strlen(resp_str), NULL);
    free(resp_str);
    return resp;
}

static int
mcp_client_ensure_initialized(csilk_mcp_client_t* client)
{
    if (client->initialized) {
        return 0;
    }
    csilk_json_t*    params = csilk_json_object();
    csilk_mcp_msg_t* resp = mcp_client_rpc(client, "initialize", params);
    if (!resp || resp->error || !resp->result) {
        mcp_msg_free(resp);
        return -1;
    }
    client->initialized = 1;
    mcp_msg_free(resp);
    return 0;
}

/**
 * @brief Connect to an MCP server over stdio.
 *
 * The child process is spawned lazily on the first RPC call so callers such as
 * the stdio pool can construct handles for commands without spawning a process
 * until an RPC is issued.
 */
csilk_mcp_client_t*
csilk_mcp_client_connect_stdio(const char* command, char* const argv[])
{
    if (!command) {
        return NULL;
    }

    csilk_mcp_client_t* client = (csilk_mcp_client_t*)calloc(1, sizeof(csilk_mcp_client_t));
    if (!client) {
        return NULL;
    }

    snprintf(client->server_name, sizeof(client->server_name), "%s", command);
    snprintf(client->server_version, sizeof(client->server_version), "1.0.0");
    client->command = strdup(command);
    if (!client->command) {
        free(client);
        return NULL;
    }
    if (argv) {
        size_t n = 0;
        while (argv[n]) {
            n++;
        }
        client->argv = calloc(n + 1, sizeof(char*));
        if (!client->argv) {
            free(client->command);
            free(client);
            return NULL;
        }
        for (size_t i = 0; i < n; i++) {
            client->argv[i] = strdup(argv[i]);
            if (!client->argv[i]) {
                for (size_t j = 0; j < i; j++) {
                    free(client->argv[j]);
                }
                free(client->argv);
                free(client->command);
                free(client);
                return NULL;
            }
        }
    }
    client->is_stdio = 1;
    client->pipe_fd[0] = -1;
    client->pipe_fd[1] = -1;
    csilk_mutex_init(&client->mutex);

    return client;
}

/**
 * @brief Connect to a remote MCP server over a Server-Sent Events (SSE)
 * endpoint.
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
    client->pipe_fd[0] = -1;
    client->pipe_fd[1] = -1;
    csilk_mutex_init(&client->mutex);

    return client;
}

/**
 * @brief Connect to a remote MCP server over a WebSocket endpoint.
 */
csilk_mcp_client_t*
csilk_mcp_client_connect_ws(const char* ws_url)
{
    if (!ws_url) {
        return NULL;
    }

    csilk_mcp_client_t* client = (csilk_mcp_client_t*)calloc(1, sizeof(csilk_mcp_client_t));
    if (!client) {
        return NULL;
    }

    snprintf(client->ws_url, sizeof(client->ws_url), "%s", ws_url);
    snprintf(client->sse_url, sizeof(client->sse_url), "%s", ws_url);
    client->is_stdio = 0;
    client->is_ws = 1;
    client->pipe_fd[0] = -1;
    client->pipe_fd[1] = -1;
    csilk_mutex_init(&client->mutex);

    return client;
}

/**
 * @brief Destroy an MCP client and release its transport & imported proxy state.
 */
void
csilk_mcp_client_free(csilk_mcp_client_t* client)
{
    if (!client) {
        return;
    }

    if (client->pipe_fd[1] >= 0) {
        close(client->pipe_fd[1]);
        client->pipe_fd[1] = -1;
    }
    if (client->pipe_fd[0] >= 0) {
        close(client->pipe_fd[0]);
        client->pipe_fd[0] = -1;
    }
    if (client->child_pid > 0) {
        int st = 0;
        if (waitpid(client->child_pid, &st, WNOHANG) != client->child_pid) {
            kill(client->child_pid, SIGTERM);
            usleep(10000);
            if (waitpid(client->child_pid, &st, WNOHANG) != client->child_pid) {
                kill(client->child_pid, SIGKILL);
                /* Blocking waitpid: loop on EINTR so a signal here cannot leave
                 * the child un-reaped (zombie). */
                int w = waitpid(client->child_pid, &st, 0);
                while (w < 0 && errno == EINTR) {
                    w = waitpid(client->child_pid, &st, 0);
                }
            }
        }
        client->child_pid = 0;
    }

    free(client->command);
    if (client->argv) {
        for (size_t i = 0; client->argv[i]; i++) {
            free(client->argv[i]);
        }
        free(client->argv);
    }
    for (size_t i = 0; i < client->imported_count; i++) {
        free(client->imported_names[i]);
    }
    free(client->imported_names);
    csilk_mutex_destroy(&client->mutex);
    free(client);
}

/* Proxy context passed into csilk_wf_register_tool / csilk_ai_tool_registry_register. */
typedef struct {
    csilk_mcp_client_t* client;
    char*               tool_name;
} mcp_tool_proxy_t;

static char*
mcp_proxy_tool_exec(const char* args_json, void* user_data)
{
    mcp_tool_proxy_t* proxy = (mcp_tool_proxy_t*)user_data;
    if (!proxy || !proxy->client || !proxy->tool_name) {
        return NULL;
    }
    return csilk_mcp_client_call_tool(proxy->client, proxy->tool_name, args_json);
}

char*
csilk_mcp_client_call_tool(csilk_mcp_client_t* client, const char* tool_name, const char* args_json)
{
    if (!client || !tool_name) {
        return NULL;
    }

    csilk_mutex_lock(&client->mutex);
    if (mcp_client_ensure_initialized(client) != 0) {
        csilk_mutex_unlock(&client->mutex);
        return NULL;
    }

    csilk_json_t* params = csilk_json_object();
    csilk_json_add_string(params, "name", tool_name);
    csilk_json_t* parsed_args = args_json ? csilk_json_parse(args_json) : NULL;
    if (parsed_args && csilk_json_is_object(parsed_args)) {
        csilk_json_add_object(params, "arguments", parsed_args);
    } else {
        if (parsed_args) {
            csilk_json_free(parsed_args);
        }
        csilk_json_add_object(params, "arguments", csilk_json_object());
    }

    csilk_mcp_msg_t* resp = mcp_client_rpc(client, "tools/call", params);
    csilk_mutex_unlock(&client->mutex);

    if (!resp || resp->error || !resp->result) {
        mcp_msg_free(resp);
        return NULL;
    }

    /* Extract first text block from MCP result.content[0].text when present,
     * otherwise serialize the entire result payload. */
    char*         out = NULL;
    csilk_json_t* content = csilk_json_get_array(resp->result, "content");
    if (content && csilk_json_array_size(content) > 0) {
        csilk_json_t* first = csilk_json_array_get(content, 0);
        const char*   txt = first ? csilk_json_get_string(first, "text") : NULL;
        if (txt) {
            out = strdup(txt);
        }
    }
    if (!out) {
        out = csilk_json_serialize(resp->result, NULL);
    }
    mcp_msg_free(resp);
    return out;
}

static int
mcp_client_fetch_tools(csilk_mcp_client_t* client, csilk_mcp_msg_t** out_resp)
{
    csilk_mutex_lock(&client->mutex);
    if (mcp_client_ensure_initialized(client) != 0) {
        csilk_mutex_unlock(&client->mutex);
        return -1;
    }
    csilk_mcp_msg_t* resp = mcp_client_rpc(client, "tools/list", csilk_json_object());
    csilk_mutex_unlock(&client->mutex);
    if (!resp || resp->error || !resp->result) {
        mcp_msg_free(resp);
        return -1;
    }
    *out_resp = resp;
    return 0;
}

static int
mcp_track_proxy(csilk_mcp_client_t* client, void* proxy)
{
    csilk_mutex_lock(&client->mutex);
    char** next = realloc(client->imported_names, (client->imported_count + 1) * sizeof(char*));
    if (!next) {
        csilk_mutex_unlock(&client->mutex);
        return -1;
    }
    client->imported_names = next;
    client->imported_names[client->imported_count++] = (char*)proxy;
    csilk_mutex_unlock(&client->mutex);
    return 0;
}

static void
mcp_untrack_proxy(csilk_mcp_client_t* client, void* proxy)
{
    csilk_mutex_lock(&client->mutex);
    for (size_t i = client->imported_count; i > 0; i--) {
        if (client->imported_names[i - 1] == (char*)proxy) {
            client->imported_names[i - 1] = client->imported_names[--client->imported_count];
            break;
        }
    }
    csilk_mutex_unlock(&client->mutex);
}

int
csilk_mcp_client_import_to_registry(csilk_mcp_client_t* client, csilk_ai_tool_registry_t* reg)
{
    if (!client || !reg) {
        return -1;
    }

    csilk_mcp_msg_t* resp = NULL;
    if (mcp_client_fetch_tools(client, &resp) != 0) {
        return -1;
    }

    csilk_json_t* tools = csilk_json_get_array(resp->result, "tools");
    if (!tools) {
        mcp_msg_free(resp);
        return 0;
    }

    int    imported = 0;
    size_t count = csilk_json_array_size(tools);
    for (size_t i = 0; i < count; i++) {
        csilk_json_t* item = csilk_json_array_get(tools, i);
        if (!item) {
            continue;
        }
        const char*   name = csilk_json_get_string(item, "name");
        const char*   desc = csilk_json_get_string(item, "description");
        csilk_json_t* schema = csilk_json_get_object(item, "inputSchema");
        if (!name || !name[0]) {
            continue;
        }

        char*             schema_str = schema ? csilk_json_serialize(schema, NULL) : strdup("{}");
        size_t            nlen = strlen(name) + 1;
        mcp_tool_proxy_t* proxy = malloc(sizeof(mcp_tool_proxy_t) + nlen);
        if (!proxy) {
            free(schema_str);
            continue;
        }
        proxy->client = client;
        proxy->tool_name = (char*)(proxy + 1);
        memcpy(proxy->tool_name, name, nlen);

        if (mcp_track_proxy(client, proxy) != 0) {
            free(proxy);
            free(schema_str);
            continue;
        }

        if (csilk_ai_tool_registry_register(reg,
                                            name,
                                            desc ? desc : "",
                                            schema_str ? schema_str : "{}",
                                            mcp_proxy_tool_exec,
                                            proxy) == 0) {
            imported++;
        } else {
            mcp_untrack_proxy(client, proxy);
            free(proxy);
        }
        free(schema_str);
    }

    mcp_msg_free(resp);
    return imported;
}

int
csilk_mcp_client_import_tools(csilk_mcp_client_t* client, csilk_wf_t* wf)
{
    if (!client || !wf) {
        return -1;
    }

    csilk_mcp_msg_t* resp = NULL;
    if (mcp_client_fetch_tools(client, &resp) != 0) {
        return -1;
    }

    csilk_json_t* tools = csilk_json_get_array(resp->result, "tools");
    if (!tools) {
        mcp_msg_free(resp);
        return 0;
    }

    int    imported = 0;
    size_t count = csilk_json_array_size(tools);
    for (size_t i = 0; i < count; i++) {
        csilk_json_t* item = csilk_json_array_get(tools, i);
        if (!item) {
            continue;
        }
        const char*   name = csilk_json_get_string(item, "name");
        const char*   desc = csilk_json_get_string(item, "description");
        csilk_json_t* schema = csilk_json_get_object(item, "inputSchema");
        if (!name || !name[0]) {
            continue;
        }

        size_t            nlen = strlen(name) + 1;
        mcp_tool_proxy_t* proxy = malloc(sizeof(mcp_tool_proxy_t) + nlen);
        if (!proxy) {
            continue;
        }
        proxy->client = client;
        proxy->tool_name = (char*)(proxy + 1);
        memcpy(proxy->tool_name, name, nlen);

        csilk_mutex_lock(&client->mutex);
        char** next = realloc(client->imported_names, (client->imported_count + 1) * sizeof(char*));
        if (!next) {
            csilk_mutex_unlock(&client->mutex);
            free(proxy);
            continue;
        }
        client->imported_names = next;
        client->imported_names[client->imported_count++] = (char*)proxy;
        csilk_mutex_unlock(&client->mutex);

        char* schema_str = schema ? csilk_json_serialize(schema, NULL) : strdup("{}");
        csilk_wf_register_tool(
            wf, name, desc ? desc : "", schema_str ? schema_str : "{}", mcp_proxy_tool_exec, proxy);
        free(schema_str);
        imported++;
    }

    mcp_msg_free(resp);
    return imported;
}

int
csilk_mcp_client_inject_context(csilk_mcp_client_t* client, csilk_ai_context_t* ctx)
{
    if (!client || !ctx) {
        return -1;
    }
    if (mcp_client_ensure_initialized(client) != 0) {
        return -1;
    }

    int injected_count = 0;

    /* 1. Fetch prompts */
    csilk_json_t*    prompt_params = csilk_json_object();
    csilk_mcp_msg_t* p_resp = mcp_client_rpc(client, "prompts/list", prompt_params);
    if (p_resp && p_resp->result) {
        csilk_json_t* prompts = csilk_json_get_array(p_resp->result, "prompts");
        if (prompts) {
            size_t count = csilk_json_array_size(prompts);
            for (size_t i = 0; i < count; i++) {
                csilk_json_t* p = csilk_json_array_get(prompts, i);
                if (!p) {
                    continue;
                }
                const char* pname = csilk_json_get_string(p, "name");
                const char* pdesc = csilk_json_get_string(p, "description");
                if (pname) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "[MCP PROMPT: %s]\n%s", pname, pdesc ? pdesc : "");
                    csilk_ai_context_add(ctx, "system", buf);
                    injected_count++;
                }
            }
        }
    }
    if (p_resp) {
        mcp_msg_free(p_resp);
    }

    /* 2. Fetch resources */
    csilk_json_t*    res_params = csilk_json_object();
    csilk_mcp_msg_t* r_resp = mcp_client_rpc(client, "resources/list", res_params);
    if (r_resp && r_resp->result) {
        csilk_json_t* resources = csilk_json_get_array(r_resp->result, "resources");
        if (resources) {
            size_t count = csilk_json_array_size(resources);
            for (size_t i = 0; i < count; i++) {
                csilk_json_t* r = csilk_json_array_get(resources, i);
                if (!r) {
                    continue;
                }
                const char* uri = csilk_json_get_string(r, "uri");
                const char* rname = csilk_json_get_string(r, "name");
                if (uri) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "[MCP RESOURCE: %s (%s)]", rname ? rname : uri, uri);
                    csilk_ai_context_add(ctx, "system", buf);
                    injected_count++;
                }
            }
        }
    }
    if (r_resp) {
        mcp_msg_free(r_resp);
    }

    return injected_count;
}
