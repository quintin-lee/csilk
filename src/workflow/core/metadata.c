
void
csilk_wf_register_tool(csilk_wf_t* wf,
		       const char* name,
		       const char* description,
		       const char* parameters_json,
		       csilk_wf_tool_fn fn,
		       void* user_data)
{
	if (!wf || !name || !fn) {
		return;
	}
	uv_mutex_lock(&wf->monitor_mutex);
	if (wf->tool_count >= wf->tool_capacity) {
		size_t new_cap = wf->tool_capacity == 0 ? 4 : wf->tool_capacity * 2;
		csilk_wf_tool_entry_t* new_tools =
		    realloc(wf->tools, sizeof(csilk_wf_tool_entry_t) * new_cap);
		if (new_tools) {
			wf->tools = new_tools;
			wf->tool_capacity = new_cap;
		}
	}
	if (wf->tool_count < wf->tool_capacity) {
		csilk_wf_tool_entry_t* entry = &wf->tools[wf->tool_count++];
		entry->name = strdup(name);
		entry->description = description ? strdup(description) : nullptr;
		entry->parameters_json = parameters_json ? strdup(parameters_json) : nullptr;
		entry->fn = fn;
		entry->user_data = user_data;
	}
	uv_mutex_unlock(&wf->monitor_mutex);
}

/**
 * @brief Set a dynamic tool discovery callback for the workflow.
 * @see csilk_wf_tool_discovery_fn in workflow.h for callback contract.
 */
void
csilk_wf_set_tool_discovery(csilk_wf_t* wf, csilk_wf_tool_discovery_fn discovery, void* user_data)
{
	if (!wf) {
		return;
	}
	uv_mutex_lock(&wf->monitor_mutex);
	wf->tool_discovery = discovery;
	wf->tool_discovery_user_data = user_data;
	uv_mutex_unlock(&wf->monitor_mutex);
}

/** @brief Look up a workflow node by its string ID.
 *  @param wf The workflow instance.
 *  @param id Node identifier (set in csilk_wf_add()).
 *  @return Node pointer, or nullptr if not found.
 *  @note Linear search of the node array — O(n). */
csilk_wf_node_t*
csilk_wf_get_node(csilk_wf_t* wf, const char* id)
{
	if (!wf || !id) {
		return nullptr;
	}
	for (size_t i = 0; i < wf->node_count; i++) {
		if (strcmp(wf->nodes[i]->id, id) == 0) {
			return wf->nodes[i];
		}
	}
	return nullptr;
}

/* --- Visualization --- */

char*
csilk_wf_to_mermaid(csilk_wf_t* wf)
{
	if (!wf) {
		return nullptr;
	}
	size_t buf_size = 8192;
	char* buf = malloc(buf_size);
	size_t buf_used = 0;
	buf_used += (size_t)snprintf(buf, buf_size, "graph TD\n");
	for (size_t i = 0; i < wf->node_count; i++) {
		csilk_wf_node_t* n = wf->nodes[i];
		char line[512];
		snprintf(line, sizeof(line), "  \"%s\"[\"%s\"]\n", n->id, n->id);
		if (buf_used + strlen(line) < buf_size) {
			buf_used +=
			    (size_t)snprintf(buf + buf_used, buf_size - buf_used, "%s", line);
		}
		for (size_t j = 0; j < n->edge_count; j++) {
			csilk_wf_edge_t* e = &n->edges[j];
			if (e->condition) {
				snprintf(line,
					 sizeof(line),
					 "  \"%s\" -- \"%s\" --> \"%s\"\n",
					 n->id,
					 e->condition,
					 e->target->id);
			} else {
				snprintf(line,
					 sizeof(line),
					 "  \"%s\" --> \"%s\"\n",
					 n->id,
					 e->target->id);
			}
			if (buf_used + strlen(line) < buf_size) {
				buf_used += (size_t)snprintf(
				    buf + buf_used, buf_size - buf_used, "%s", line);
			}
		}
		if (n->error_target) {
			snprintf(line,
				 sizeof(line),
				 "  \"%s\" -. \"error\" .-> \"%s\"\n",
				 n->id,
				 n->error_target->id);
			if (buf_used + strlen(line) < buf_size) {
				buf_used += (size_t)snprintf(
				    buf + buf_used, buf_size - buf_used, "%s", line);
			}
		}
	}
	return buf;
}

/* --- Tracing Implementation --- */

char*
csilk_wf_trace_to_json(const csilk_wf_trace_t* trace)
{
	if (!trace) {
		return nullptr;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "exec_id", trace->exec_id);
	cJSON_AddNumberToObject(root, "start_time", (double)trace->start_time);
	cJSON_AddNumberToObject(root, "end_time", (double)trace->end_time);
	cJSON_AddNumberToObject(root, "duration_us", (double)(trace->end_time - trace->start_time));
	cJSON* nodes = cJSON_CreateArray();
	for (size_t i = 0; i < trace->node_count; i++) {
		csilk_wf_trace_node_t* n = trace->nodes[i];
		cJSON* nj = cJSON_CreateObject();
		cJSON_AddStringToObject(nj, "node_id", n->node_id);
		cJSON_AddNumberToObject(nj, "start_time", (double)n->start_time);
		cJSON_AddNumberToObject(nj, "end_time", (double)n->end_time);
		cJSON_AddNumberToObject(nj, "duration_us", (double)(n->end_time - n->start_time));
		if (n->input_dump) {
			cJSON_AddStringToObject(nj, "input", n->input_dump);
		}
		if (n->output_dump) {
			cJSON_AddStringToObject(nj, "output", n->output_dump);
		}
		if (n->model) {
			cJSON_AddStringToObject(nj, "model", n->model);
		}
		if (n->prompt_tokens > 0) {
			cJSON_AddNumberToObject(nj, "prompt_tokens", n->prompt_tokens);
		}
		if (n->completion_tokens > 0) {
			cJSON_AddNumberToObject(nj, "completion_tokens", n->completion_tokens);
		}
		if (n->error) {
			cJSON_AddStringToObject(nj, "error", n->error);
		}
		cJSON_AddItemToArray(nodes, nj);
	}
	cJSON_AddItemToObject(root, "nodes", nodes);
	char* out = cJSON_Print(root);
	cJSON_Delete(root);
	return out;
}

void
csilk_wf_trace_free(csilk_wf_trace_t* trace)
{
	if (!trace) {
		return;
	}
	free(trace->exec_id);
	for (size_t i = 0; i < trace->node_count; i++) {
		csilk_wf_trace_node_t* n = trace->nodes[i];
		free(n->node_id);
		free(n->input_dump);
		free(n->output_dump);
		free(n->model);
		free(n->error);
		free(n);
	}
	free(trace->nodes);
	free(trace);
}

/* --- Scheduler Implementation --- */
