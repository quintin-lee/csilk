/**
 * @file wf_ai_nodes.c
 * @brief Template engine, AI chat handler, vector search handler, and
 *        node registration for AI and vector search workflow nodes.
 *
 * @copyright MIT License
 */

#include "workflow_internal.h"
#include "wf_ai_internal.h"

#include <ctype.h>
#include "csilk/core/sync.h"

/* --- Template Engine --- */

/* Applies a single string filter (upper/lower/trim/summarize:/json_escape)
 * to val in place, returning the (possibly transformed) string. */
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
        csilk_json_t* j = csilk_json_string_new(val);
        char*         escaped = csilk_json_serialize(j, NULL);
        size_t        elen = strlen(escaped);
        if (elen >= 2) {
            escaped[elen - 1] = '\0';
            char* inner = csilk_wf_strdup(ctx, escaped + 1);
            free(escaped);
            csilk_json_free(j);
            return inner;
        }
        free(escaped);
        csilk_json_free(j);
    }
    return val;
}

/* Resolves {{node.value.path|filter}} and {{input.value...}} template
 * expressions against node outputs and the initial input. */
static char*
resolve_templates(csilk_wf_ctx_t* ctx, const char* template)
{
    if (!template) {
        return NULL;
    }
    char* res = csilk_wf_strdup(ctx, template);

    for (size_t i = 0; i < ctx->wf->node_count; i++) {
        csilk_wf_node_t* n = ctx->wf->nodes[i];
        char             base_pattern[256];
        snprintf(base_pattern, sizeof(base_pattern), "{{%s.value", n->id);

        char* pos;
        while ((pos = strstr(res, base_pattern)) != NULL) {
            char* end = strstr(pos, "}}");
            if (!end) {
                break;
            }

            size_t        pat_full_len = end - pos + 2;
            char*         replacement = "(null)";
            csilk_data_t* out = ctx->node_outputs[n->index];

            if (out && out->value) {
                char* path_start = pos + strlen(base_pattern);
                char* pipe = strchr(path_start, '|');
                if (pipe && pipe > end) {
                    pipe = NULL;
                }

                char* filter_start = pipe ? pipe + 1 : NULL;
                char* actual_end = pipe ? pipe : end;

                if (*path_start == '.') {
                    path_start++;
                    size_t path_len = actual_end - path_start;
                    char*  path = malloc(path_len + 1);
                    memcpy(path, path_start, path_len);
                    path[path_len] = '\0';

                    csilk_json_t* json = csilk_json_parse((char*)out->value);
                    if (json) {
                        char* val = _csilk_json_get_path(ctx, json, path);
                        if (val) {
                            replacement = val;
                        }
                        csilk_json_free(json);
                    }
                    free(path);
                } else {
                    replacement = csilk_wf_strdup(ctx, (char*)out->value);
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
                        f = strtok_r(NULL, "|", &saveptr);
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

    const char* in_pattern = "{{input.value";
    char*       pos;
    while ((pos = strstr(res, in_pattern)) != NULL) {
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
                pipe = NULL;
            }
            char* filter_start = pipe ? pipe + 1 : NULL;
            char* actual_end = pipe ? pipe : end;

            if (*path_start == '.') {
                path_start++;
                size_t path_len = actual_end - path_start;
                char*  path = malloc(path_len + 1);
                memcpy(path, path_start, path_len);
                path[path_len] = '\0';

                csilk_json_t* json = csilk_json_parse((char*)ctx->initial_input->value);
                if (json) {
                    char* val = _csilk_json_get_path(ctx, json, path);
                    if (val) {
                        replacement = val;
                    }
                    csilk_json_free(json);
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
                    f = strtok_r(NULL, "|", &saveptr);
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

/* --- AI Node Handler --- */

typedef struct {
    csilk_wf_ctx_t*        ctx;
    csilk_ai_tool_call_t*  tc;
    char*                  result;
    csilk_mutex_t*         mutex;
    csilk_cond_t*          cond;
    int*                   pending;
    csilk_wf_tool_entry_t* discovered;
    size_t                 discovered_count;
} sub_tool_work_t;

/* Thread-pool callback: executes one tool call (registered or discovered). */
static void
sub_worker_cb(csilk_io_work_t* req)
{
    sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
    sw->result = NULL;
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

/* Completion callback: decrements the pending tool-call counter and signals
 * the waiting main thread. */
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

/* Streaming callback: rebroadcasts streamed chunks to monitors as "node_stream". */
static void
on_ai_stream(const char* chunk, void* user_data)
{
    stream_ctx_t* s_ctx = (stream_ctx_t*)user_data;
    _wf_broadcast(s_ctx->ctx->wf, "node_stream", s_ctx->node_id, chunk);
}

/* Frees a copied csilk_ai_config_t (model/system_msg/prompt strings). */
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
 * @brief Core AI chat node handler (shared by AI and agent nodes).
 *
 * Resolves the prompt template, builds the message history (applying
 * max_history_messages trimming), and loops up to 10 iterations calling the
 * OpenAI-compatible chat API. Tool calls are dispatched to the thread pool
 * (registered tools plus any discovered via tool_discovery) and their results
 * fed back. On a final text response it returns a csilk_data_t tagged
 * "text/plain" carrying AI token-usage metadata.
 *
 * @param ctx       Workflow execution context.
 * @param input     Incoming node input (currently unused; prompt uses templates).
 * @param user_data Pointer to a csilk_ai_config_t (the node's copied config).
 * @return New csilk_data_t with the assistant's reply, or NULL on missing API
 *         key / client creation failure.
 * @note Requires the AGENT_API_KEY environment variable. Streams chunks to
 *       monitors when config->stream is set.
 */
csilk_data_t*
ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    csilk_ai_config_t* config = (csilk_ai_config_t*)user_data;
    char*              prompt = resolve_templates(ctx, config->prompt);
    const char*        api_key = getenv("AGENT_API_KEY");
    if (!api_key) {
        return NULL;
    }
    csilk_ai_t* ai = csilk_ai_new("openai", api_key, getenv("AGENT_API_BASE"));
    if (!ai) {
        return NULL;
    }
    csilk_ai_tool_t*       tools = NULL;
    csilk_wf_tool_entry_t* discovered = NULL;
    size_t                 discovered_count = 0;

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
        for (size_t i = 0; i < ctx->wf->tool_count; i++) {
            tools[i].type = "function";
            tools[i].function.name = ctx->wf->tools[i].name;
            tools[i].function.description = ctx->wf->tools[i].description;
            if (ctx->wf->tools[i].parameters_json) {
                tools[i].function.parameters_json =
                    csilk_json_parse(ctx->wf->tools[i].parameters_json);
            }
        }
        for (size_t i = 0; i < discovered_count; i++) {
            size_t idx = ctx->wf->tool_count + i;
            tools[idx].type = "function";
            tools[idx].function.name = discovered[i].name;
            tools[idx].function.description = discovered[i].description;
            if (discovered[i].parameters_json) {
                tools[idx].function.parameters_json =
                    csilk_json_parse(discovered[i].parameters_json);
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
    csilk_data_t* out = NULL;
    int           iterations = 0;
    while (iterations < 10) {
        iterations++;

        if (config->max_history_messages > 0 && msg_count > (size_t)config->max_history_messages) {
            size_t keep = (size_t)config->max_history_messages;
            size_t discard_start = 0;
            size_t move_to = 0;

            if (strcmp(msgs[0].role, "system") == 0) {
                discard_start = 1;
                move_to = 1;
                keep--;
            }

            size_t discard_count = msg_count - (size_t)config->max_history_messages;
            for (size_t i = discard_start; i < discard_start + discard_count; i++) {
                free((void*)msgs[i].content);
                if (msgs[i].tool_calls) {
                    for (size_t j = 0; j < msgs[i].tool_call_count; j++) {
                        free(msgs[i].tool_calls[j].id);
                        free(msgs[i].tool_calls[j].name);
                        free(msgs[i].tool_calls[j].arguments);
                    }
                    free((void*)msgs[i].tool_calls);
                }
                free((void*)msgs[i].tool_call_id);
            }

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
            .on_chunk = config->stream ? on_ai_stream : NULL,
            .user_data = config->stream ? &s_ctx : NULL};
        csilk_ai_chat_response_t res;
        if (csilk_ai_chat(ai, &req, &res) != 0) {
            break;
        }
        if (res.tool_call_count > 0) {
            if (msg_count + res.tool_call_count + 1 >= msg_capacity) {
                size_t old_capacity = msg_capacity;
                msg_capacity += res.tool_call_count + 16;
                csilk_ai_message_t* new_msgs =
                    realloc(msgs, sizeof(csilk_ai_message_t) * msg_capacity);
                if (new_msgs) {
                    memset(new_msgs + old_capacity,
                           0,
                           sizeof(csilk_ai_message_t) * (msg_capacity - old_capacity));
                    msgs = new_msgs;
                }
            }
            memset(&msgs[msg_count], 0, sizeof(csilk_ai_message_t));
            msgs[msg_count].role = "assistant";
            msgs[msg_count].content = res.content ? strdup(res.content) : strdup("");
            if (res.tool_call_count > 0) {
                msgs[msg_count].tool_call_count = res.tool_call_count;
                msgs[msg_count].tool_calls =
                    calloc(res.tool_call_count, sizeof(csilk_ai_tool_call_t));
                for (size_t j = 0; j < res.tool_call_count; j++) {
                    msgs[msg_count].tool_calls[j].id =
                        strdup(res.tool_calls[j].id ? res.tool_calls[j].id : "");
                    msgs[msg_count].tool_calls[j].name =
                        strdup(res.tool_calls[j].name ? res.tool_calls[j].name : "");
                    msgs[msg_count].tool_calls[j].arguments =
                        strdup(res.tool_calls[j].arguments ? res.tool_calls[j].arguments : "{}");
                }
            }
            msgs[msg_count].tool_call_id = NULL;
            msg_count++;

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
                memset(&msgs[msg_count], 0, sizeof(csilk_ai_message_t));
                msgs[msg_count].role = "tool";
                msgs[msg_count].content = strdup(sws[i].result ?: "{}");
                msgs[msg_count].tool_call_id =
                    strdup(res.tool_calls[i].id ? res.tool_calls[i].id : "");
                msgs[msg_count].tool_calls = NULL;
                msgs[msg_count].tool_call_count = 0;
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
        if (msgs[i].tool_calls) {
            for (size_t j = 0; j < msgs[i].tool_call_count; j++) {
                free(msgs[i].tool_calls[j].id);
                free(msgs[i].tool_calls[j].name);
                free(msgs[i].tool_calls[j].arguments);
            }
            free(msgs[i].tool_calls);
        }
        free((void*)msgs[i].tool_call_id);
    }
    free(msgs);
    if (tools) {
        for (size_t i = 0; i < ctx->wf->tool_count; i++) {
            csilk_json_free(tools[i].function.parameters_json);
        }
        for (size_t i = 0; i < discovered_count; i++) {
            size_t idx = ctx->wf->tool_count + i;
            csilk_json_free(tools[idx].function.parameters_json);
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

/* --- Vector Search Node Handler --- */

/* Frees a copied csilk_vector_search_config_t (embedding_model/collection/
 * input_template strings). */
static void
vector_search_config_free(void* ptr)
{
    csilk_vector_search_config_t* c = (csilk_vector_search_config_t*)ptr;
    free((void*)c->embedding_model);
    free((void*)c->collection);
    free((void*)c->input_template);
    free(c);
}

/* Vector search node handler: embeds the input, queries the vector DB, and
 * returns the top-k matches as a JSON array. */
static csilk_data_t*
vector_search_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    csilk_vector_search_config_t* config = (csilk_vector_search_config_t*)user_data;
    if (!config || !config->ai || !config->db || !config->collection) {
        return NULL;
    }

    const char* text = NULL;
    char*       resolved = NULL;
    if (config->input_template) {
        resolved = resolve_templates(ctx, config->input_template);
        text = resolved;
    } else if (input && input->value && strcmp(input->type, "text/plain") == 0) {
        text = (const char*)input->value;
    }

    if (!text) {
        return NULL;
    }

    csilk_ai_embeddings_response_t eres = {0};
    if (csilk_ai_embeddings(config->ai, config->embedding_model, &text, 1, &eres) != 0) {
        csilk_ai_embeddings_response_free(&eres);
        return NULL;
    }

    if (eres.count == 0 || eres.dimension == 0) {
        csilk_ai_embeddings_response_free(&eres);
        return NULL;
    }

    csilk_vector_search_response_t vres = {0};
    if (csilk_vector_db_search(config->db,
                               config->collection,
                               eres.values,
                               eres.dimension,
                               config->limit > 0 ? config->limit : 5,
                               &vres) != 0) {
        csilk_ai_embeddings_response_free(&eres);
        csilk_vector_search_response_free(&vres);
        return NULL;
    }

    csilk_json_t* root = csilk_json_array();
    for (size_t i = 0; i < vres.count; i++) {
        csilk_json_t* item = csilk_json_object();
        csilk_json_add_string(item, "id", vres.results[i].id);
        csilk_json_add_number(item, "score", (double)vres.results[i].score);
        if (vres.results[i].payload) {
            csilk_json_add_object(item, "payload", csilk_json_copy(vres.results[i].payload));
        }
        csilk_json_array_append(root, item);
    }

    char* json_str = csilk_json_serialize(root, NULL);
    csilk_json_free(root);

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

/* --- Node Registration --- */

/**
 * @brief Adds a plain AI chat node to the workflow.
 *
 * Copies the supplied csilk_ai_config_t (duplicating its string fields) and
 * registers ai_node_handler as the node callback. The copy is released via the
 * node's user_data_free hook on csilk_wf_free().
 *
 * @param wf     The workflow instance (must not be NULL).
 * @param id     Unique node identifier (must not be NULL).
 * @param config AI chat configuration (must not be NULL).
 * @return The new node pointer, or NULL on invalid args / allocation failure.
 */
csilk_wf_node_t*
csilk_wf_add_ai(csilk_wf_t* wf, const char* id, const csilk_ai_config_t* config)
{
    csilk_ai_config_t* copy = malloc(sizeof(csilk_ai_config_t));
    memcpy(copy, config, sizeof(csilk_ai_config_t));
    copy->model = config->model ? strdup(config->model) : NULL;
    copy->system_msg = config->system_msg ? strdup(config->system_msg) : NULL;
    copy->prompt = config->prompt ? strdup(config->prompt) : NULL;
    csilk_wf_node_t* node = csilk_wf_add(wf, id, ai_node_handler, copy);
    if (node) {
        node->user_data_free = ai_config_free;
    }
    return node;
}

/**
 * @brief Adds a vector-search node to the workflow.
 *
 * Copies the configuration (duplicating its string fields) and registers a node
 * whose handler embeds the resolved input and queries the configured vector
 * database, returning the top-k results as a JSON array ("application/json").
 *
 * @param wf     The workflow instance (must not be NULL).
 * @param id     Unique node identifier (must not be NULL).
 * @param config Vector search configuration (must not be NULL).
 * @return The new node pointer, or NULL on invalid args / allocation failure.
 */
csilk_wf_node_t*
csilk_wf_add_vector_search(csilk_wf_t*                         wf,
                           const char*                         id,
                           const csilk_vector_search_config_t* config)
{
    csilk_vector_search_config_t* copy = malloc(sizeof(csilk_vector_search_config_t));
    memcpy(copy, config, sizeof(csilk_vector_search_config_t));
    copy->embedding_model = config->embedding_model ? strdup(config->embedding_model) : NULL;
    copy->collection = config->collection ? strdup(config->collection) : NULL;
    copy->input_template = config->input_template ? strdup(config->input_template) : NULL;

    csilk_wf_node_t* node = csilk_wf_add(wf, id, vector_search_node_handler, copy);
    if (node) {
        node->user_data_free = vector_search_config_free;
    }
    return node;
}
