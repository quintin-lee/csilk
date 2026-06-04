
csilk_wf_ctx_t* ctx = (csilk_wf_ctx_t*)handle->data;
uv_mutex_lock(&ctx->mutex);
ctx->is_terminated = 1;
ctx->is_ttl_expired = 1;
uv_mutex_unlock(&ctx->mutex);
printf("[Workflow] TTL Expired for execution %s\n", ctx->exec_id);
}

static const char*
_wf_run_ext_internal(csilk_wf_t* wf,
		     csilk_data_t* input,
		     void (*callback)(csilk_data_t*),
		     void (*trace_cb)(csilk_data_t*, csilk_wf_trace_t*))
{
	if (!wf || wf->node_count == 0) {
		if (callback) {
			callback(nullptr);
		}
		if (trace_cb) {
			trace_cb(nullptr, nullptr);
		}
		return nullptr;
	}
	csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
	ctx->wf = wf;
	ctx->initial_input = input;
	ctx->callback = callback;
	ctx->trace_callback = trace_cb;
	ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
	ctx->node_approved = calloc(wf->node_count, sizeof(int));
	ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
	ctx->arena = csilk_arena_new(0);
	uv_mutex_init(&ctx->mutex);
	uv_mutex_init(&ctx->arena_mutex);
	uv_mutex_init(&ctx->trace_mutex);
	csilk_generate_uuid(ctx->exec_id);

	_wf_register_active_ctx(wf, ctx);

	if (wf->ttl_sec > 0) {
		uv_timer_init(wf->loop, &ctx->ttl_timer);
		ctx->ttl_timer.data = ctx;
		uv_timer_start(&ctx->ttl_timer, on_workflow_ttl, wf->ttl_sec * 1000, 0);
	}

	_wf_broadcast(wf, "workflow_start", ctx->exec_id, input ? (char*)input->value : nullptr);

	char* m_graph = csilk_wf_to_mermaid(wf);
	_wf_broadcast(wf, "workflow_topology", nullptr, m_graph);
	free(m_graph);
	if (wf->wal_dir) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, ctx->exec_id);
		ctx->wal_path = strdup(path);
		_wf_wal_log_event(ctx, WF_EV_START, nullptr, input);
	}
	if (trace_cb) {
		ctx->trace = calloc(1, sizeof(csilk_wf_trace_t));
		ctx->trace->exec_id = strdup(ctx->exec_id);
		ctx->trace->start_time = uv_hrtime() / 1000;
	}
	int started = 0;
	for (size_t i = 0; i < wf->node_count; i++) {
		if (wf->nodes[i]->is_entry) {
			_wf_execute_node(ctx, wf->nodes[i], input);
			started = 1;
		}
	}
	if (!started) {
		for (size_t i = 0; i < wf->node_count; i++) {
			if (wf->nodes[i]->incoming_count == 0) {
				_wf_execute_node(ctx, wf->nodes[i], input);
				started = 1;
			}
		}
	}
	if (!started) {
		if (callback) {
			callback(nullptr);
		}
		if (trace_cb) {
			trace_cb(nullptr, nullptr);
		}
		_wf_cleanup_ctx(ctx);
		return nullptr;
	}
	return ctx->exec_id;
}

void
csilk_wf_resume(csilk_wf_t* wf, const char* exec_id, void (*callback)(csilk_data_t* result))
{
	if (!wf || !exec_id || !wf->wal_dir) {
		return;
	}
	char wal_path[512];
	snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
	FILE* f = fopen(wal_path, "rb");
	if (!f) {
		return;
	}
	csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
	ctx->wf = wf;
	ctx->callback = callback;
	ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
	ctx->node_approved = calloc(wf->node_count, sizeof(int));
	ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
	ctx->arena = csilk_arena_new(0);
	uv_mutex_init(&ctx->mutex);
	uv_mutex_init(&ctx->arena_mutex);
	uv_mutex_init(&ctx->trace_mutex);
	strncpy(ctx->exec_id, exec_id, sizeof(ctx->exec_id) - 1);
	ctx->exec_id[sizeof(ctx->exec_id) - 1] = '\0';
	ctx->wal_path = strdup(wal_path);
	int *node_started = calloc(wf->node_count, sizeof(int)),
	    *node_finished = calloc(wf->node_count, sizeof(int));
	int wf_ended = 0;
	csilk_wf_wal_header_t header;
	while (fread(&header, sizeof(header), 1, f) == 1) {
		if (header.magic != CSILK_WF_MAGIC) {
			break;
		}
		char* payload = header.payload_len > 0 ? malloc(header.payload_len) : nullptr;
		if (payload && fread(payload, header.payload_len, 1, f) != 1) {
			free(payload);
			break;
		}
		switch (header.type) {
		case WF_EV_NODE_START:
			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, payload) == 0) {
					node_started[i] = 1;
					break;
				}
			}
			break;
		case WF_EV_NODE_FINISH: {
			char* node_id = payload;
			size_t nid_len = strlen(node_id);
			if (nid_len + 1 >= header.payload_len) {
				break;
			}
			char* data_type = node_id + nid_len + 1;
			size_t dtype_len = strlen(data_type);
			if (nid_len + 1 + dtype_len + 1 >= header.payload_len) {
				break;
			}
			char* data_val = data_type + dtype_len + 1;

			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, node_id) == 0) {
					node_finished[i] = 1;
					ctx->node_outputs[i] = csilk_wf_data_new(
					    ctx, data_type, csilk_wf_strdup(ctx, data_val));
					csilk_wf_node_t* n = wf->nodes[i];
					for (size_t j = 0; j < n->edge_count; j++) {
						ctx->node_input_counts[n->edges[j].target->index]++;
					}
					break;
				}
			}
			break;
		}
		case WF_EV_PAUSE: {
			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, payload) == 0) {
					ctx->is_paused = 1;
					break;
				}
			}
			break;
		}
		case WF_EV_END:
			wf_ended = 1;
			break;
		}
		free(payload);
	}
	fclose(f);
	if (wf_ended) {
		if (callback) {
			callback(nullptr);
		}
		_wf_cleanup_ctx(ctx);
	} else if (ctx->is_paused) {
		_wf_cleanup_ctx(ctx);
	} else {
		for (size_t i = 0; i < wf->node_count; i++) {
			csilk_wf_node_t* n = wf->nodes[i];
			int trigger = 0;
			csilk_data_t* trigger_input = ctx->initial_input;
			if (node_started[i] && !node_finished[i]) {
				trigger = 1;
			} else if (!node_started[i]) {
				int threshold = n->incoming_count == 0 ? 1 : n->incoming_count;
				if (ctx->node_input_counts[n->index] >= threshold) {
					trigger = 1;
				}
			}
			if (trigger) {
				for (size_t j = 0; j < wf->node_count; j++) {
					csilk_wf_node_t* p = wf->nodes[j];
					for (size_t k = 0; k < p->edge_count; k++) {
						if (p->edges[k].target == n && node_finished[j]) {
							trigger_input = ctx->node_outputs[j];
							break;
						}
					}
				}
				_wf_execute_node(ctx, n, trigger_input);
			}
		}
	}
	free(node_started);
	free(node_finished);
}

void
csilk_wf_signal_continue(csilk_wf_t* wf,
			 const char* exec_id,
			 csilk_data_t* input,
			 void (*callback)(csilk_data_t* result))
{
	if (!wf || !exec_id || !wf->wal_dir) {
		return;
	}

	char wal_path[512];
	snprintf(wal_path, sizeof(wal_path), "%s/%s.wal", wf->wal_dir, exec_id);
	FILE* f = fopen(wal_path, "rb");
	if (!f) {
		return;
	}

	csilk_wf_ctx_t* ctx = calloc(1, sizeof(csilk_wf_ctx_t));
	ctx->wf = wf;
	ctx->callback = callback;
	ctx->node_input_counts = calloc(wf->node_count, sizeof(int));
	ctx->node_approved = calloc(wf->node_count, sizeof(int));
	ctx->node_outputs = calloc(wf->node_count, sizeof(csilk_data_t*));
	ctx->arena = csilk_arena_new(0);
	uv_mutex_init(&ctx->mutex);
	uv_mutex_init(&ctx->arena_mutex);
	uv_mutex_init(&ctx->trace_mutex);
	strncpy(ctx->exec_id, exec_id, sizeof(ctx->exec_id) - 1);
	ctx->exec_id[sizeof(ctx->exec_id) - 1] = '\0';
	ctx->wal_path = strdup(wal_path);

	char* paused_node_id = nullptr;

	csilk_wf_wal_header_t header;
	while (fread(&header, sizeof(header), 1, f) == 1) {
		if (header.magic != CSILK_WF_MAGIC) {
			break;
		}
		char* payload = header.payload_len > 0 ? malloc(header.payload_len) : nullptr;
		if (payload && fread(payload, header.payload_len, 1, f) != 1) {
			free(payload);
			break;
		}
		switch (header.type) {
		case WF_EV_NODE_FINISH: {
			char* node_id = payload;
			size_t nid_len = strlen(node_id);
			if (nid_len + 1 >= header.payload_len) {
				break;
			}
			char* data_type = node_id + nid_len + 1;
			size_t dtype_len = strlen(data_type);
			if (nid_len + 1 + dtype_len + 1 >= header.payload_len) {
				break;
			}
			char* data_val = data_type + dtype_len + 1;

			for (size_t i = 0; i < wf->node_count; i++) {
				if (strcmp(wf->nodes[i]->id, node_id) == 0) {
					ctx->node_outputs[i] = csilk_wf_data_new(
					    ctx, data_type, csilk_wf_strdup(ctx, data_val));
					csilk_wf_node_t* n = wf->nodes[i];
					for (size_t j = 0; j < n->edge_count; j++) {
						ctx->node_input_counts[n->edges[j].target->index]++;
					}
					break;
				}
			}
			break;
		}
		case WF_EV_PAUSE:
			free(paused_node_id);
			paused_node_id = strdup(payload);
			break;
		case WF_EV_END:
			break;
		}
		free(payload);
	}
	fclose(f);

	if (paused_node_id) {
		csilk_wf_node_t* n = csilk_wf_get_node(wf, paused_node_id);
		if (n) {
			ctx->node_approved[n->index] = 1;
			_wf_execute_node(ctx, n, input);
		}
		free(paused_node_id);
	} else {
		_wf_cleanup_ctx(ctx);
	}
}
