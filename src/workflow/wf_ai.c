/**
 * @file wf_ai.c
 * @brief AI workflow nodes: memory helpers, template engine, AI chat handler,
 *        vector search handler, tool registration, and tool discovery.
 */

#include "workflow_internal.h"

#include <ctype.h>
#include "csilk/core/sync.h"

/* --- Memory Helpers --- */

/**
 * @brief Allocates memory from the workflow arena in a thread-safe manner.
 *
 * This function uses the workflow context's internal arena allocator. The memory
 * is tied to the execution context and will be automatically freed when the
 * context is destroyed at the end of the workflow execution.
 *
 * @param ctx  The workflow execution context.
 * @param size The number of bytes to allocate.
 * @return A pointer to the allocated memory, or nullptr if ctx is null.
 */
void*
csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size)
{
    if (!ctx) {
        return nullptr;
    }
    csilk_mutex_lock(&ctx->arena_mutex);
    void* ptr = csilk_arena_alloc(ctx->arena, size);
    csilk_mutex_unlock(&ctx->arena_mutex);
    return ptr;
}

/**
 * @brief Duplicates a string using the workflow's arena allocator.
 *
 * Allocates memory from the context's arena and copies the source string.
 * The duplicated string remains valid for the lifetime of the workflow context.
 *
 * @param ctx The workflow execution context.
 * @param s   The source string to duplicate.
 * @return A pointer to the duplicated NUL-terminated string, or nullptr if s is null.
 */
char*
csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s)
{
    if (!s) {
        return nullptr;
    }
    size_t len = strlen(s);
    char*  news = csilk_wf_alloc(ctx, len + 1);
    if (news) {
        memcpy(news, s, len + 1);
    }
    return news;
}

/**
 * @brief Allocates and initializes a new csilk_data_t container.
 *
 * The container structure and its type string are allocated within the workflow's
 * arena. The container can be passed between workflow nodes as input/output.
 *
 * @param ctx   The workflow execution context.
 * @param type  The MIME-like content type (e.g., "text/plain"). Copied via csilk_wf_strdup.
 * @param value The raw data payload pointer.
 * @return A pointer to the newly created data container, or nullptr on failure.
 */
csilk_data_t*
csilk_wf_data_new(csilk_wf_ctx_t* ctx, const char* type, void* value)
{
    csilk_data_t* data = csilk_wf_alloc(ctx, sizeof(csilk_data_t));
    if (data) {
        data->type = csilk_wf_strdup(ctx, type);
        data->value = value;
        data->free_fn = nullptr;
        data->meta = nullptr;
    }
    return data;
}

/** @brief Internal: traverse a cJSON tree following a dot-separated path.
 *
 * Algorithm:
 * 1. Split the path on "." using strtok_r.
 * 2. For each token, if the current cJSON node is an array, index into
 *    it using atoi(); otherwise, use cJSON_GetObjectItemCaseSensitive().
 * 3. If the final value is a string/number/bool, return it as a string
 *    allocated from the workflow arena. For objects/arrays, return a
 *    stringified JSON representation.
 *
 * @param ctx  Workflow context (for arena allocation).
 * @param root Root cJSON node to start traversal from.
 * @param path Dot-separated path (e.g., "user.address.city").
 * @return A string allocated in ctx->arena, or nullptr if the path does not
 *         exist. The returned string is valid for the workflow's lifetime. */
static char*
_csilk_json_get_path(csilk_wf_ctx_t* ctx, cJSON* root, const char* path)
{
    if (!root || !path) {
        return nullptr;
    }

    cJSON* curr = root;
    char*  path_copy = strdup(path);
    char*  saveptr;
    char*  token = strtok_r(path_copy, ".", &saveptr);

    while (token && curr) {
        if (cJSON_IsArray(curr)) {
            curr = cJSON_GetArrayItem(curr, atoi(token));
        } else {
            curr = cJSON_GetObjectItemCaseSensitive(curr, token);
        }
        token = strtok_r(nullptr, ".", &saveptr);
    }

    char* result = nullptr;
    if (curr) {
        if (cJSON_IsString(curr)) {
            result = csilk_wf_strdup(ctx, curr->valuestring);
        } else if (cJSON_IsNumber(curr)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", curr->valuedouble);
            result = csilk_wf_strdup(ctx, buf);
        } else if (cJSON_IsBool(curr)) {
            result = csilk_wf_strdup(ctx, curr->valueint ? "true" : "false");
        } else if (cJSON_IsNull(curr)) {
            result = csilk_wf_strdup(ctx, "null");
        } else {
            // For objects/arrays, return as stringified JSON
            char* tmp = cJSON_PrintUnformatted(curr);
            result = csilk_wf_strdup(ctx, tmp);
            free(tmp);
        }
    }

    free(path_copy);
    return result;
}

/* --- Template Engine & AI Node --- */

/**
 * @brief Applies a text processing filter to a string value.
 *
 * Supported filters:
 * - "upper": Converts the string to uppercase in-place.
 * - "lower": Converts the string to lowercase in-place.
 * - "trim": Trims leading and trailing whitespace.
 * - "summarize:N": Truncates the string to N characters.
 * - "json_escape": Escapes the string for safe inclusion in a JSON string.
 *
 * @param ctx    The workflow execution context.
 * @param filter The name/specification of the filter.
 * @param val    The string to filter (may be modified in-place or replaced).
 * @return The filtered string (may be a new pointer allocated in the arena).
 */
static char*
apply_filter(csilk_wf_ctx_t* ctx, const char* filter, char* val)
{
    if (!filter || !val) {
        return val;
    }

    if (strcmp(filter, "upper") == 0) {
        for (int i = 0; val[i]; i++) {
            val[i] = toupper(val[i]);
        }
    } else if (strcmp(filter, "lower") == 0) {
        for (int i = 0; val[i]; i++) {
            val[i] = tolower(val[i]);
        }
    } else if (strcmp(filter, "trim") == 0) {
        char* start = val;
        while (*start && isspace(*start)) {
            start++;
        }
        char* end = val + strlen(val) - 1;
        while (end > start && isspace(*end)) {
            end--;
        }
        *(end + 1) = '\0';
        return start;
    } else if (strncmp(filter, "summarize:", 10) == 0) {
        size_t len = (size_t)atoi(filter + 10);
        if (strlen(val) > len) {
            val[len] = '\0';
        }
    } else if (strcmp(filter, "json_escape") == 0) {
        cJSON* j = cJSON_CreateString(val);
        char*  escaped = cJSON_PrintUnformatted(j);
        // Remove surrounding quotes
        size_t elen = strlen(escaped);
        if (elen >= 2) {
            escaped[elen - 1] = '\0';
            char* inner = csilk_wf_strdup(ctx, escaped + 1);
            free(escaped);
            cJSON_Delete(j);
            return inner;
        }
        free(escaped);
        cJSON_Delete(j);
    }
    return val;
}

/**
 * @brief Parses and resolves template variables in a template string.
 *
 * Variables can reference the initial workflow input or the output of previous nodes.
 * Syntax formats:
 * - `{{input.value}}` for initial input value.
 * - `{{input.value.path.to.field}}` for JSONPath traversal of input.
 * - `{{node_id.value}}` for predecessor node outputs.
 * - `{{node_id.value.path.to.field}}` for JSONPath traversal of node output.
 * - Pipeling filters: `{{input.value | upper | trim}}`.
 *
 * @param ctx      The workflow execution context.
 * @param template The template string containing variables.
 * @return The fully resolved string, allocated in the workflow's arena.
 */
static char*
resolve_templates(csilk_wf_ctx_t* ctx, const char* template)
{
    if (!template) {
        return nullptr;
    }
    char* res = csilk_wf_strdup(ctx, template);

    // 1. Handle node references: {{node_id.value}} or
    // {{node_id.value.path.to.field}}
    for (size_t i = 0; i < ctx->wf->node_count; i++) {
        csilk_wf_node_t* n = ctx->wf->nodes[i];
        char             base_pattern[256];
        snprintf(base_pattern, sizeof(base_pattern), "{{%s.value", n->id);

        char* pos;
        while ((pos = strstr(res, base_pattern)) != nullptr) {
            char* end = strstr(pos, "}}");
            if (!end) {
                break;
            }

            size_t        pat_full_len = end - pos + 2;
            char*         replacement = "(null)";
            csilk_data_t* out = ctx->node_outputs[n->index];

            if (out && out->value) {
                char* path_start = pos + strlen(base_pattern);
                // We need to find if there is a | before }}
                char* pipe = strchr(path_start, '|');
                if (pipe && pipe > end) {
                    pipe = nullptr;
                }

                char* filter_start = pipe ? pipe + 1 : nullptr;
                char* actual_end = pipe ? pipe : end;

                if (*path_start == '.') {
                    // JSONPath!
                    path_start++; // Skip the dot
                    size_t path_len = actual_end - path_start;
                    char*  path = malloc(path_len + 1);
                    memcpy(path, path_start, path_len);
                    path[path_len] = '\0';

                    cJSON* json = cJSON_Parse((char*)out->value);
                    if (json) {
                        char* val = _csilk_json_get_path(ctx, json, path);
                        if (val) {
                            replacement = val;
                        }
                        cJSON_Delete(json);
                    }
                    free(path);
                } else {
                    // Pure value
                    replacement = csilk_wf_strdup(ctx, (char*)out->value);
                }

                // Apply Filters
                if (filter_start) {
                    size_t flen = end - filter_start;
                    char*  filters = malloc(flen + 1);
                    memcpy(filters, filter_start, flen);
                    filters[flen] = '\0';

                    char* saveptr;
                    char* f = strtok_r(filters, "|", &saveptr);
                    while (f) {
                        // Trim filter name
                        while (*f == ' ') {
                            f++;
                        }
                        char* fe = f + strlen(f) - 1;
                        while (fe > f && *fe == ' ') {
                            *fe = '\0';
                            fe--;
                        }

                        replacement = apply_filter(ctx, f, replacement);
                        f = strtok_r(nullptr, "|", &saveptr);
                    }
                    free(filters);
                }
            }

            size_t rep_len = strlen(replacement);
            size_t res_len = strlen(res);
            char*  new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
            size_t prefix_len = pos - res;
            memcpy(new_res, res, prefix_len);
            memcpy(new_res + prefix_len, replacement, rep_len);
            memcpy(new_res + prefix_len + rep_len, end + 2, strlen(end + 2) + 1);
            res = new_res;
        }
    }

    // 2. Handle initial input: {{input.value}}
    const char* in_pattern = "{{input.value";
    char*       pos;
    while ((pos = strstr(res, in_pattern)) != nullptr) {
        char* end = strstr(pos, "}}");
        if (!end) {
            break;
        }

        size_t pat_full_len = end - pos + 2;
        char*  replacement = "(null)";

        if (ctx->initial_input && ctx->initial_input->value) {
            char* path_start = pos + strlen(in_pattern);
            char* pipe = strchr(path_start, '|');
            if (pipe && pipe > end) {
                pipe = nullptr;
            }
            char* filter_start = pipe ? pipe + 1 : nullptr;
            char* actual_end = pipe ? pipe : end;

            if (*path_start == '.') {
                path_start++;
                size_t path_len = actual_end - path_start;
                char*  path = malloc(path_len + 1);
                memcpy(path, path_start, path_len);
                path[path_len] = '\0';

                cJSON* json = cJSON_Parse((char*)ctx->initial_input->value);
                if (json) {
                    char* val = _csilk_json_get_path(ctx, json, path);
                    if (val) {
                        replacement = val;
                    }
                    cJSON_Delete(json);
                }
                free(path);
            } else {
                replacement = csilk_wf_strdup(ctx, (char*)ctx->initial_input->value);
            }

            if (filter_start) {
                size_t flen = end - filter_start;
                char*  filters = malloc(flen + 1);
                memcpy(filters, filter_start, flen);
                filters[flen] = '\0';
                char* saveptr;
                char* f = strtok_r(filters, "|", &saveptr);
                while (f) {
                    while (*f == ' ') {
                        f++;
                    }
                    char* fe = f + strlen(f) - 1;
                    while (fe > f && *fe == ' ') {
                        *fe = '\0';
                        fe--;
                    }
                    replacement = apply_filter(ctx, f, replacement);
                    f = strtok_r(nullptr, "|", &saveptr);
                }
                free(filters);
            }
        }

        size_t rep_len = strlen(replacement);
        size_t res_len = strlen(res);
        char*  new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
        size_t prefix_len = pos - res;
        memcpy(new_res, res, prefix_len);
        memcpy(new_res + prefix_len, replacement, rep_len);
        memcpy(new_res + prefix_len + rep_len, end + 2, strlen(end + 2) + 1);
        res = new_res;
    }

    return res;
}

typedef struct {
    csilk_wf_ctx_t*        ctx;              /**< Workflow context (for tool registry lookup). */
    csilk_ai_tool_call_t*  tc;               /**< Tool call arguments from the AI response. */
    char*                  result;           /**< Tool output string (allocated by tool fn). */
    csilk_mutex_t*         mutex;            /**< Shared mutex for the pending counter. */
    csilk_cond_t*          cond;             /**< Shared condition variable for completion. */
    int*                   pending;          /**< Shared atomic-like pending count. */
    csilk_wf_tool_entry_t* discovered;       /**< Dynamically discovered tools. */
    size_t                 discovered_count; /**< Number of discovered tools. */
} sub_tool_work_t;

/** @brief Thread-pool work callback for tool execution. */
static void
sub_worker_cb(csilk_io_work_t* req)
{
    sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
    sw->result = nullptr;
    for (size_t j = 0; j < sw->ctx->wf->tool_count; j++) {
        if (strcmp(sw->ctx->wf->tools[j].name, sw->tc->name) == 0) {
            sw->result =
                sw->ctx->wf->tools[j].fn(sw->tc->arguments, sw->ctx->wf->tools[j].user_data);
            return;
        }
    }
    for (size_t j = 0; j < sw->discovered_count; j++) {
        if (strcmp(sw->discovered[j].name, sw->tc->name) == 0) {
            sw->result = sw->discovered[j].fn(sw->tc->arguments, sw->discovered[j].user_data);
            return;
        }
    }
}
/** @brief After-work callback for tool execution. */
static void
after_sub_worker_cb(csilk_io_work_t* req, int status)
{
    (void)status;
    sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
    csilk_mutex_lock(sw->mutex);
    (*sw->pending)--;
    csilk_cond_signal(sw->cond);
    csilk_mutex_unlock(sw->mutex);
}

typedef struct {
    csilk_wf_ctx_t* ctx;
    const char*     node_id;
} stream_ctx_t;

/**
 * @brief Callback invoked when a new text chunk is streamed from the AI model.
 *
 * Broadcasts the chunk to registered WebSocket monitors using the "node_stream" event.
 *
 * @param chunk     The newly received text segment.
 * @param user_data Pointer to the stream_ctx_t structure containing the context and node ID.
 */
static void
on_ai_stream(const char* chunk, void* user_data)
{
    stream_ctx_t* s_ctx = (stream_ctx_t*)user_data;
    _wf_broadcast(s_ctx->ctx->wf, "node_stream", s_ctx->node_id, chunk);
}

/**
 * @brief Destructor function for csilk_ai_config_t.
 *
 * Frees the dynamically allocated members (model, system_msg, prompt) and the config itself.
 * Assigned as node->user_data_free for AI nodes.
 *
 * @param ptr Pointer to the csilk_ai_config_t structure to free.
 */
static void
ai_config_free(void* ptr)
{
    csilk_ai_config_t* c = (csilk_ai_config_t*)ptr;
    free((void*)c->model);
    free((void*)c->system_msg);
    free((void*)c->prompt);
    free(c);
}

/**
 * @brief Destructor function for csilk_vector_search_config_t.
 *
 * Frees the dynamically allocated members (embedding_model, collection, input_template) and the config itself.
 * Assigned as node->user_data_free for Vector Search nodes.
 *
 * @param ptr Pointer to the csilk_vector_search_config_t structure to free.
 */
static void
vector_search_config_free(void* ptr)
{
    csilk_vector_search_config_t* c = (csilk_vector_search_config_t*)ptr;
    free((void*)c->embedding_model);
    free((void*)c->collection);
    free((void*)c->input_template);
    free(c);
}

/**
 * @brief Built-in handler for AI Chat nodes.
 *
 * Initializes the AI engine, resolves prompt templates, sets up tool lists (including dynamic discovery),
 * enforces message history limits, sends requests to the LLM, coordinates parallel tool execution,
 * and records token usage statistics.
 *
 * @param ctx       The workflow execution context.
 * @param input     Input data (unused, as prompt templates are resolved from context).
 * @param user_data Pointer to the csilk_ai_config_t configuration.
 * @return The final text/plain output data container from the AI, or nullptr on failure.
 */
static csilk_data_t*
ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    csilk_ai_config_t* config = (csilk_ai_config_t*)user_data;
    char*              prompt = resolve_templates(ctx, config->prompt);
    const char*        api_key = getenv("AGENT_API_KEY");
    if (!api_key) {
        return nullptr;
    }
    csilk_ai_t* ai = csilk_ai_new("openai", api_key, getenv("AGENT_API_BASE"));
    if (!ai) {
        return nullptr;
    }
    csilk_ai_tool_t*       tools = nullptr;
    csilk_wf_tool_entry_t* discovered = nullptr;
    size_t                 discovered_count = 0;

    /* Call dynamic discovery callback if set */
    if (ctx->wf->tool_discovery) {
        ctx->wf->tool_discovery(ctx->wf,
                                ctx->wf->tools,
                                ctx->wf->tool_count,
                                &discovered,
                                &discovered_count,
                                ctx->wf->tool_discovery_user_data);
    }

    size_t total_tools = ctx->wf->tool_count + discovered_count;
    if (total_tools > 0) {
        tools = calloc(total_tools, sizeof(csilk_ai_tool_t));
        /* Static tools first (take precedence on name collision) */
        for (size_t i = 0; i < ctx->wf->tool_count; i++) {
            tools[i].type = "function";
            tools[i].function.name = ctx->wf->tools[i].name;
            tools[i].function.description = ctx->wf->tools[i].description;
            if (ctx->wf->tools[i].parameters_json) {
                tools[i].function.parameters_json = cJSON_Parse(ctx->wf->tools[i].parameters_json);
            }
        }
        /* Discovered tools appended after static ones */
        for (size_t i = 0; i < discovered_count; i++) {
            size_t idx = ctx->wf->tool_count + i;
            tools[idx].type = "function";
            tools[idx].function.name = discovered[i].name;
            tools[idx].function.description = discovered[i].description;
            if (discovered[i].parameters_json) {
                tools[idx].function.parameters_json = cJSON_Parse(discovered[i].parameters_json);
            }
        }
    }

    stream_ctx_t s_ctx = {ctx, "unknown"};
    for (size_t i = 0; i < ctx->wf->node_count; i++) {
        if (ctx->wf->nodes[i]->handler == ai_node_handler &&
            ctx->wf->nodes[i]->user_data == user_data) {
            s_ctx.node_id = ctx->wf->nodes[i]->id;
            break;
        }
    }

    size_t              msg_capacity = 32;
    csilk_ai_message_t* msgs = calloc(msg_capacity, sizeof(csilk_ai_message_t));
    size_t              msg_count = 0;
    if (config->system_msg) {
        msgs[msg_count].role = "system";
        msgs[msg_count].content = strdup(config->system_msg);
        msg_count++;
    }
    msgs[msg_count].role = "user";
    msgs[msg_count].content = strdup(prompt);
    msg_count++;
    csilk_data_t* out = nullptr;
    int           iterations = 0;
    while (iterations < 10) {
        iterations++;

        // Enforce context window limit
        if (config->max_history_messages > 0 && msg_count > (size_t)config->max_history_messages) {
            size_t keep = (size_t)config->max_history_messages;
            size_t discard_start = 0;
            size_t move_to = 0;

            // If msgs[0] is system, always keep it
            if (strcmp(msgs[0].role, "system") == 0) {
                discard_start = 1;
                move_to = 1;
                keep--;
            }

            size_t discard_count = msg_count - (size_t)config->max_history_messages;
            // Free discarded messages
            for (size_t i = discard_start; i < discard_start + discard_count; i++) {
                free((void*)msgs[i].content);
            }

            // Shift remaining messages
            memmove(&msgs[move_to],
                    &msgs[discard_start + discard_count],
                    sizeof(csilk_ai_message_t) * keep);
            msg_count = (size_t)config->max_history_messages;
        }

        csilk_ai_chat_request_t req = {
            .model = config->model ? config->model : "gpt-3.5-turbo",
            .messages = msgs,
            .message_count = msg_count,
            .temperature = config->temperature > 0 ? config->temperature : 0.7,
            .max_tokens = config->max_tokens > 0 ? config->max_tokens : 1024,
            .tools = tools,
            .tool_count = ctx->wf->tool_count,
            .on_chunk = config->stream ? on_ai_stream : nullptr,
            .user_data = config->stream ? &s_ctx : nullptr};
        csilk_ai_chat_response_t res;
        if (csilk_ai_chat(ai, &req, &res) != 0) {
            break;
        }
        if (res.tool_call_count > 0) {
            if (msg_count + res.tool_call_count + 1 >= msg_capacity) {
                msg_capacity += res.tool_call_count + 16;
                msgs = realloc(msgs, sizeof(csilk_ai_message_t) * msg_capacity);
            }
            msgs[msg_count].role = "assistant";
            msgs[msg_count].content = res.content ? strdup(res.content) : strdup("");
            msg_count++;

            // Parallel Execution of Tool Calls
            csilk_mutex_t m;
            csilk_cond_t  c;
            int           pending = (int)res.tool_call_count;
            csilk_mutex_init(&m);
            csilk_cond_init(&c);
            sub_tool_work_t* sws = calloc(res.tool_call_count, sizeof(sub_tool_work_t));
            csilk_io_work_t* reqs = calloc(res.tool_call_count, sizeof(csilk_io_work_t));

            for (size_t i = 0; i < res.tool_call_count; i++) {
                sws[i].ctx = ctx;
                sws[i].tc = &res.tool_calls[i];
                sws[i].mutex = &m;
                sws[i].cond = &c;
                sws[i].pending = &pending;
                sws[i].discovered = discovered;
                sws[i].discovered_count = discovered_count;
                reqs[i].data = &sws[i];
                csilk_io_queue_work(ctx->wf->loop, &reqs[i], sub_worker_cb, after_sub_worker_cb);
            }

            csilk_mutex_lock(&m);
            while (pending > 0) {
                csilk_cond_wait(&c, &m);
            }
            csilk_mutex_unlock(&m);

            for (size_t i = 0; i < res.tool_call_count; i++) {
                msgs[msg_count].role = "tool";
                msgs[msg_count].content = sws[i].result ? sws[i].result : strdup("{}");
                msg_count++;
            }

            free(sws);
            free(reqs);
            csilk_mutex_destroy(&m);
            csilk_cond_destroy(&c);
            csilk_ai_chat_response_free(&res);
            continue;
        }
        out = csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, res.content));
        csilk_ai_meta_t* meta = csilk_wf_alloc(ctx, sizeof(csilk_ai_meta_t));
        meta->model = csilk_wf_strdup(ctx, req.model);
        meta->prompt_tokens = res.prompt_tokens;
        meta->completion_tokens = res.completion_tokens;
        out->meta = meta;
        csilk_ai_chat_response_free(&res);
        break;
    }
    for (size_t i = 0; i < msg_count; i++) {
        free((void*)msgs[i].content);
    }
    free(msgs);
    if (tools) {
        for (size_t i = 0; i < ctx->wf->tool_count; i++) {
            cJSON_Delete(tools[i].function.parameters_json);
        }
        for (size_t i = 0; i < discovered_count; i++) {
            size_t idx = ctx->wf->tool_count + i;
            cJSON_Delete(tools[idx].function.parameters_json);
        }
        free(tools);
    }
    if (discovered) {
        for (size_t i = 0; i < discovered_count; i++) {
            free(discovered[i].name);
            free(discovered[i].description);
            free(discovered[i].parameters_json);
        }
        free(discovered);
    }
    csilk_ai_free(ai);
    return out;
}

/**
 * @brief Internal built-in handler for Vector Search nodes.
 *
 * Generates an embedding for the input text (or text resolved from a template)
 * using the configured AI engine, searches the Vector DB, and serializes the
 * match IDs, scores, and payloads into a JSON array returned to the workflow.
 *
 * @param ctx       The workflow execution context.
 * @param input     Input data container (fallback text source if template is null).
 * @param user_data Pointer to the csilk_vector_search_config_t configuration.
 * @return An application/json output data container with results, or nullptr on failure.
 */
static csilk_data_t*
vector_search_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    csilk_vector_search_config_t* config = (csilk_vector_search_config_t*)user_data;
    if (!config || !config->ai || !config->db || !config->collection) {
        return nullptr;
    }

    /* 1. Get input text (either from template or input value) */
    const char* text = nullptr;
    char*       resolved = nullptr;
    if (config->input_template) {
        resolved = resolve_templates(ctx, config->input_template);
        text = resolved;
    } else if (input && input->value && strcmp(input->type, "text/plain") == 0) {
        text = (const char*)input->value;
    }

    if (!text) {
        return nullptr;
    }

    /* 2. Generate embeddings */
    csilk_ai_embeddings_response_t eres = {0};
    if (csilk_ai_embeddings(config->ai, config->embedding_model, &text, 1, &eres) != 0) {
        csilk_ai_embeddings_response_free(&eres);
        return nullptr;
    }

    if (eres.count == 0 || eres.dimension == 0) {
        csilk_ai_embeddings_response_free(&eres);
        return nullptr;
    }

    /* 3. Search Vector DB */
    csilk_vector_search_response_t vres = {0};
    if (csilk_vector_db_search(config->db,
                               config->collection,
                               eres.values,
                               eres.dimension,
                               config->limit > 0 ? config->limit : 5,
                               &vres) != 0) {
        csilk_ai_embeddings_response_free(&eres);
        csilk_vector_search_response_free(&vres);
        return nullptr;
    }

    /* 4. Convert results to JSON */
    cJSON* root = cJSON_CreateArray();
    for (size_t i = 0; i < vres.count; i++) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", vres.results[i].id);
        cJSON_AddNumberToObject(item, "score", (double)vres.results[i].score);
        if (vres.results[i].payload) {
            cJSON_AddItemToObject(item, "payload", cJSON_Duplicate(vres.results[i].payload, 1));
        }
        cJSON_AddItemToArray(root, item);
    }

    char* json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    /* 5. Cleanup and return */
    csilk_ai_embeddings_response_free(&eres);
    csilk_vector_search_response_free(&vres);

    csilk_data_t* out = csilk_wf_data_new(ctx, "application/json", json_str);
    if (out) {
        out->free_fn = free;
    } else {
        free(json_str);
    }
    return out;
}

csilk_wf_node_t*
csilk_wf_add_ai(csilk_wf_t* wf, const char* id, const csilk_ai_config_t* config)
{
    csilk_ai_config_t* copy = malloc(sizeof(csilk_ai_config_t));
    memcpy(copy, config, sizeof(csilk_ai_config_t));
    copy->model = config->model ? strdup(config->model) : nullptr;
    copy->system_msg = config->system_msg ? strdup(config->system_msg) : nullptr;
    copy->prompt = config->prompt ? strdup(config->prompt) : nullptr;
    csilk_wf_node_t* node = csilk_wf_add(wf, id, ai_node_handler, copy);
    if (node) {
        node->user_data_free = ai_config_free;
    }
    return node;
}

/**
 * @brief Adds a built-in Vector Search node to the workflow.
 *
 * Creates a duplicate of the Vector Search configuration structure and registers a node
 * with the built-in vector_search_node_handler. Sets the destructor to clean up the config.
 *
 * @param wf     The workflow definition instance.
 * @param id     A unique identifier for the new node.
 * @param config The vector search configuration settings (copied).
 * @return A pointer to the newly created node, or nullptr on failure.
 */
csilk_wf_node_t*
csilk_wf_add_vector_search(csilk_wf_t*                         wf,
                           const char*                         id,
                           const csilk_vector_search_config_t* config)
{
    csilk_vector_search_config_t* copy = malloc(sizeof(csilk_vector_search_config_t));
    memcpy(copy, config, sizeof(csilk_vector_search_config_t));
    copy->embedding_model = config->embedding_model ? strdup(config->embedding_model) : nullptr;
    copy->collection = config->collection ? strdup(config->collection) : nullptr;
    copy->input_template = config->input_template ? strdup(config->input_template) : nullptr;
    /* AI and DB handles are shared, not copied or freed by the node */

    csilk_wf_node_t* node = csilk_wf_add(wf, id, vector_search_node_handler, copy);
    if (node) {
        node->user_data_free = vector_search_config_free;
    }
    return node;
}
