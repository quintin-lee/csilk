
ai_node_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)input;
	csilk_ai_config_t* config = (csilk_ai_config_t*)user_data;
	char* prompt = _wf_resolve_templates(ctx, config->prompt);
	const char* api_key = getenv("AGENT_API_KEY");
	if (!api_key) {
		return nullptr;
	}
	csilk_ai_t* ai = csilk_ai_new("openai", api_key, getenv("AGENT_API_BASE"));
	if (!ai) {
		return nullptr;
	}
	csilk_ai_tool_t* tools = nullptr;
	csilk_wf_tool_entry_t* discovered = nullptr;
	size_t discovered_count = 0;

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
				tools[i].function.parameters_json =
				    cJSON_Parse(ctx->wf->tools[i].parameters_json);
			}
		}
		/* Discovered tools appended after static ones */
		for (size_t i = 0; i < discovered_count; i++) {
			size_t idx = ctx->wf->tool_count + i;
			tools[idx].type = "function";
			tools[idx].function.name = discovered[i].name;
			tools[idx].function.description = discovered[i].description;
			if (discovered[i].parameters_json) {
				tools[idx].function.parameters_json =
				    cJSON_Parse(discovered[i].parameters_json);
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

	size_t msg_capacity = 32;
	csilk_ai_message_t* msgs = calloc(msg_capacity, sizeof(csilk_ai_message_t));
	size_t msg_count = 0;
	if (config->system_msg) {
		msgs[msg_count].role = "system";
		msgs[msg_count].content = strdup(config->system_msg);
		msg_count++;
	}
	msgs[msg_count].role = "user";
	msgs[msg_count].content = strdup(prompt);
	msg_count++;
	csilk_data_t* out = nullptr;
	int iterations = 0;
	while (iterations < 10) {
		iterations++;

		// Enforce context window limit
		if (config->max_history_messages > 0 &&
		    msg_count > (size_t)config->max_history_messages) {
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
			uv_mutex_t m;
			uv_cond_t c;
			int pending = (int)res.tool_call_count;
			uv_mutex_init(&m);
			uv_cond_init(&c);
			sub_tool_work_t* sws = calloc(res.tool_call_count, sizeof(sub_tool_work_t));
			uv_work_t* reqs = calloc(res.tool_call_count, sizeof(uv_work_t));

			for (size_t i = 0; i < res.tool_call_count; i++) {
				sws[i].ctx = ctx;
				sws[i].tc = &res.tool_calls[i];
				sws[i].mutex = &m;
				sws[i].cond = &c;
				sws[i].pending = &pending;
				sws[i].discovered = discovered;
				sws[i].discovered_count = discovered_count;
				reqs[i].data = &sws[i];
				uv_queue_work(
				    ctx->wf->loop, &reqs[i], sub_worker_cb, after_sub_worker_cb);
			}

			uv_mutex_lock(&m);
			while (pending > 0) {
				uv_cond_wait(&c, &m);
			}
			uv_mutex_unlock(&m);

			for (size_t i = 0; i < res.tool_call_count; i++) {
				msgs[msg_count].role = "tool";
				msgs[msg_count].content =
				    sws[i].result ? sws[i].result : strdup("{}");
				msg_count++;
			}

			free(sws);
			free(reqs);
			uv_mutex_destroy(&m);
			uv_cond_destroy(&c);
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
		free(tools);
	}
	csilk_ai_free(ai);
	return out;
}

/**
 * @brief Internal built-in handler for Vector Search nodes.
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
	char* resolved = nullptr;
	if (config->input_template) {
		resolved = _wf_resolve_templates(ctx, config->input_template);
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
			cJSON_AddItemToObject(
			    item, "payload", cJSON_Duplicate(vres.results[i].payload, 1));
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
		node->user_data_free = _wf_ai_config_free;
	}
	return node;
}

csilk_wf_node_t*
csilk_wf_add_vector_search(csilk_wf_t* wf,
			   const char* id,
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
		node->user_data_free = _wf_vector_search_config_free;
	}
	return node;
}
