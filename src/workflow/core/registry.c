

CSILK_INTERNAL void
_wf_register_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
	uv_mutex_lock(&wf->ctx_mutex);
	if (wf->active_context_count >= wf->active_context_capacity) {
		size_t new_cap =
		    wf->active_context_capacity == 0 ? 8 : wf->active_context_capacity * 2;
		csilk_wf_ctx_t** new_ctxs =
		    realloc(wf->active_contexts, sizeof(csilk_wf_ctx_t*) * new_cap);
		if (new_ctxs) {
			wf->active_contexts = new_ctxs;
			wf->active_context_capacity = new_cap;
		}
	}
	if (wf->active_context_count < wf->active_context_capacity) {
		wf->active_contexts[wf->active_context_count++] = ctx;
	}
	uv_mutex_unlock(&wf->ctx_mutex);
}

CSILK_INTERNAL void
_wf_unregister_active_ctx(csilk_wf_t* wf, csilk_wf_ctx_t* ctx)
{
	uv_mutex_lock(&wf->ctx_mutex);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		if (wf->active_contexts[i] == ctx) {
			wf->active_contexts[i] = wf->active_contexts[--wf->active_context_count];
			break;
		}
	}
	uv_mutex_unlock(&wf->ctx_mutex);
}

CSILK_INTERNAL csilk_wf_ctx_t*
_wf_find_active_ctx(csilk_wf_t* wf, const char* exec_id)
{
	csilk_wf_ctx_t* found = nullptr;
	uv_mutex_lock(&wf->ctx_mutex);
	for (size_t i = 0; i < wf->active_context_count; i++) {
		if (strcmp(wf->active_contexts[i]->exec_id, exec_id) == 0) {
			found = wf->active_contexts[i];
			break;
		}
	}
	uv_mutex_unlock(&wf->ctx_mutex);
	return found;
}

CSILK_INTERNAL void
_wf_broadcast(csilk_wf_t* wf, const char* event, const char* node_id, const char* payload)
{
	if (!wf || wf->monitor_count == 0) {
		return;
	}
	cJSON* root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "event", event);
	if (node_id) {
		cJSON_AddStringToObject(root, "node_id", node_id);
	}
	if (payload) {
		cJSON_AddStringToObject(root, "payload", payload);
	}
	cJSON_AddNumberToObject(root, "timestamp", (double)time(nullptr));
	char* json = cJSON_PrintUnformatted(root);
	uv_mutex_lock(&wf->monitor_mutex);
	for (size_t i = 0; i < wf->monitor_count; i++) {
		csilk_ws_send(wf->monitors[i], (uint8_t*)json, strlen(json), 0x1);
	}
	uv_mutex_unlock(&wf->monitor_mutex);
	free(json);
	cJSON_Delete(root);
}
