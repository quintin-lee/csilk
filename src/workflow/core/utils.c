
/* --- Memory Helpers --- */
void*
csilk_wf_alloc(csilk_wf_ctx_t* ctx, size_t size)
{
	if (!ctx) {
		return nullptr;
	}
	uv_mutex_lock(&ctx->arena_mutex);
	void* ptr = csilk_arena_alloc(ctx->arena, size);
	uv_mutex_unlock(&ctx->arena_mutex);
	return ptr;
}

char*
csilk_wf_strdup(csilk_wf_ctx_t* ctx, const char* s)
{
	if (!s) {
		return nullptr;
	}
	size_t len = strlen(s);
	char* news = csilk_wf_alloc(ctx, len + 1);
	if (news) {
		memcpy(news, s, len + 1);
	}
	return news;
}

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
	char* path_copy = strdup(path);
	char* saveptr;
	char* token = strtok_r(path_copy, ".", &saveptr);

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

/** @brief Internal: resolve template expressions in a prompt string.
 *
 * Template syntax:
 *   {{node_id.value}}          -> raw value output from a node
 *   {{node_id.value.path.to}}  -> JSONPath into a node's JSON output
 *   {{input.value}}            -> the workflow's initial input
 *   {{input.value.path.to}}    -> JSONPath into the initial input
 *
 * Algorithm:
 * 1. Iterate over all workflow nodes, searching for {{node_id.value}}
 *    patterns in the template string.
 * 2. For each match, look up the node's output. If followed by ".path",
 *    parse the node output as JSON and traverse with _csilk_json_get_path().
 *    Otherwise, use the raw output value.
 * 3. Replace the {{...}} placeholder with the resolved value using arena
 *    memory.
 * 4. Repeat for {{input.value}} patterns using the workflow's initial input.
 *
 * @param ctx      Workflow execution context.
 * @param template Template string with {{...}} placeholders.
 * @return Resolved string allocated in ctx->arena.
 * @note Unresolvable patterns (missing node output, bad path) are replaced
 *       with "(null)". */
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
		char* escaped = cJSON_PrintUnformatted(j);
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

static char*

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
	char* path_copy = strdup(path);
	char* saveptr;
	char* token = strtok_r(path_copy, ".", &saveptr);

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
		} else {
			char* json = cJSON_PrintUnformatted(curr);
			result = csilk_wf_strdup(ctx, json);
			free(json);
		}
	}
	free(path_copy);
	return result;
}
_wf_resolve_templates(csilk_wf_ctx_t* ctx, const char* template)
{
	if (!template) {
		return nullptr;
	}
	char* res = csilk_wf_strdup(ctx, template);

	// 1. Handle node references: {{node_id.value}} or
	// {{node_id.value.path.to.field}}
	for (size_t i = 0; i < ctx->wf->node_count; i++) {
		csilk_wf_node_t* n = ctx->wf->nodes[i];
		char base_pattern[256];
		snprintf(base_pattern, sizeof(base_pattern), "{{%s.value", n->id);

		char* pos;
		while ((pos = strstr(res, base_pattern)) != nullptr) {
			char* end = strstr(pos, "}}");
			if (!end) {
				break;
			}

			size_t pat_full_len = end - pos + 2;
			char* replacement = "(null)";
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
					char* path = malloc(path_len + 1);
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
					char* filters = malloc(flen + 1);
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
			char* new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
			size_t prefix_len = pos - res;
			memcpy(new_res, res, prefix_len);
			memcpy(new_res + prefix_len, replacement, rep_len);
			memcpy(new_res + prefix_len + rep_len, end + 2, strlen(end + 2) + 1);
			res = new_res;
		}
	}

	// 2. Handle initial input: {{input.value}}
	const char* in_pattern = "{{input.value";
	char* pos;
	while ((pos = strstr(res, in_pattern)) != nullptr) {
		char* end = strstr(pos, "}}");
		if (!end) {
			break;
		}

		size_t pat_full_len = end - pos + 2;
		char* replacement = "(null)";

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
				char* path = malloc(path_len + 1);
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
				replacement =
				    csilk_wf_strdup(ctx, (char*)ctx->initial_input->value);
			}

			if (filter_start) {
				size_t flen = end - filter_start;
				char* filters = malloc(flen + 1);
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
		char* new_res = csilk_wf_alloc(ctx, res_len - pat_full_len + rep_len + 1);
		size_t prefix_len = pos - res;
		memcpy(new_res, res, prefix_len);
		memcpy(new_res + prefix_len, replacement, rep_len);
		memcpy(new_res + prefix_len + rep_len, end + 2, strlen(end + 2) + 1);
		res = new_res;
	}

	return res;
}

/** @brief Per-tool-call context for parallel tool execution within an
 *  AI node. Each tool call runs on its own libuv thread-pool worker. */
typedef struct {
	csilk_wf_ctx_t* ctx;		   /**< Workflow context (for tool registry lookup). */
	csilk_ai_tool_call_t* tc;	   /**< Tool call arguments from the AI response. */
	char* result;			   /**< Tool output string (allocated by tool fn). */
	uv_mutex_t* mutex;		   /**< Shared mutex for the pending counter. */
	uv_cond_t* cond;		   /**< Shared condition variable for completion. */
	int* pending;			   /**< Shared atomic-like pending count. */
	csilk_wf_tool_entry_t* discovered; /**< Dynamically discovered tools. */
	size_t discovered_count;	   /**< Number of discovered tools. */
} sub_tool_work_t;

/** @brief libuv thread-pool work callback for tool execution.
 *  Looks up the tool by name in the workflow's tool registry and
 *  calls its function with the arguments from the AI tool call. */
static void
sub_worker_cb(uv_work_t* req)
{
	sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
	sw->result = nullptr;
	for (size_t j = 0; j < sw->ctx->wf->tool_count; j++) {
		if (strcmp(sw->ctx->wf->tools[j].name, sw->tc->name) == 0) {
			sw->result = sw->ctx->wf->tools[j].fn(sw->tc->arguments,
							      sw->ctx->wf->tools[j].user_data);
			return;
		}
	}
	for (size_t j = 0; j < sw->discovered_count; j++) {
		if (strcmp(sw->discovered[j].name, sw->tc->name) == 0) {
			sw->result =
			    sw->discovered[j].fn(sw->tc->arguments, sw->discovered[j].user_data);
			return;
		}
	}
}

/** @brief libuv after-work callback for tool execution.
 *  Decrements the shared pending counter and signals the condition
 *  variable to wake the main AI node handler thread. */
static void
after_sub_worker_cb(uv_work_t* req, int status)
{
	(void)status;
	sub_tool_work_t* sw = (sub_tool_work_t*)req->data;
	uv_mutex_lock(sw->mutex);
	(*sw->pending)--;
	uv_cond_signal(sw->cond);
	uv_mutex_unlock(sw->mutex);
}

typedef struct {
	csilk_wf_ctx_t* ctx;
	const char* node_id;
} stream_ctx_t;

static void
on_ai_stream(const char* chunk, void* user_data)
{
	stream_ctx_t* s_ctx = (stream_ctx_t*)user_data;
	_wf_broadcast(s_ctx->ctx->wf, "node_stream", s_ctx->node_id, chunk);
}

/** @brief Built-in handler for AI workflow nodes.
 *
 * Algorithm:
 * 1. Resolve template expressions in the prompt string.
 * 2. Create an AI engine instance (OpenAI driver by default) using
 *    AGENT_API_KEY and AGENT_API_BASE environment variables.
 * 3. Build tool definitions from the workflow's registered tools.
 * 4. Construct a message array (optional system message + user prompt).
 * 5. Enter a request loop (max 10 iterations):
 *    a. Send a chat completion request with the current message array.
 *    b. If the response contains tool calls, execute each tool in
 *       parallel on the libuv thread pool, wait for all to complete
 *       via a condition variable, append tool results as new messages.
 *    c. If the response is a direct text completion, extract the content
 *       and AI metadata (model, token counts), return as csilk_data_t.
 * 6. Clean up all temporary allocations (messages, tools, AI handle).
 *
 * @param ctx       Workflow execution context.
 * @param input     Ignored (AI prompts come from config templates).
 * @param user_data Pointer to csilk_ai_config_t with model, prompt, etc.
 * @return Output data with type "text/plain" and AI metadata, or nullptr on
 *         failure (missing API key, driver init failure, all retries
 *         exhausted). */
static csilk_data_t*
