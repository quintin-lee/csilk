
/* --- Registry for Distributed Workflows --- */
static csilk_wf_t* g_distributed_wfs[32];
static size_t g_distributed_wf_count = 0;

static void
on_remote_result(csilk_mq_ctx_t* m_ctx)
{
	size_t len;
	const char* payload = csilk_mq_get_payload(m_ctx, &len);
	if (!payload) {
		csilk_mq_next(m_ctx);
		return;
	}
	printf("[Workflow] Received remote result: %s\n", payload);

	cJSON* root = cJSON_Parse(payload);
	if (!root) {
		csilk_mq_next(m_ctx);
		return;
	}

	cJSON* j_exec_id = cJSON_GetObjectItem(root, "exec_id");
	cJSON* j_node_id = cJSON_GetObjectItem(root, "node_id");
	cJSON* j_output = cJSON_GetObjectItem(root, "output");

	if (j_exec_id && cJSON_IsString(j_exec_id) && j_output && cJSON_IsString(j_output)) {
		const char* exec_id = j_exec_id->valuestring;
		const char* node_id = j_node_id ? j_node_id->valuestring : nullptr;
		const char* output_str = j_output->valuestring;

		for (size_t i = 0; i < g_distributed_wf_count; i++) {
			csilk_wf_t* wf = g_distributed_wfs[i];

			// 1. Check for Active Context (Hot Resume)
			csilk_wf_ctx_t* active = _wf_find_active_ctx(wf, exec_id);
			if (active) {
				printf("[Workflow] Found active execution %s, "
				       "resuming hot...\n",
				       exec_id);
				csilk_wf_node_t* n =
				    node_id ? csilk_wf_get_node(wf, node_id) : nullptr;
				// If node_id wasn't provided, we might have to search the context for a
				// paused node

				if (n) {
					uv_mutex_lock(&active->mutex);
					active->node_approved[n->index] = 1;
					active
					    ->nodes_active--; // Decrement the count we added during offload
					uv_mutex_unlock(&active->mutex);

					csilk_data_t* out_data =
					    csilk_wf_data_new(active,
							      "application/json",
							      csilk_wf_strdup(active, output_str));
					_wf_execute_node(active, n, out_data);
					break;
				}
			}

			// 2. Fallback to Cold Resume (WAL)
			char path[512];
			snprintf(path, sizeof(path), "%s/%s.wal", wf->wal_dir, exec_id);
			if (access(path, F_OK) == 0) {
				printf("[Workflow] Found WAL for %s, signaling "
				       "continue (cold)...\n",
				       exec_id);
				csilk_data_t out_data = {
				    "application/json", (void*)output_str, nullptr, nullptr};
				csilk_wf_signal_continue(wf, exec_id, &out_data, nullptr);
				break;
			}
		}
	}

	cJSON_Delete(root);
	csilk_mq_next(m_ctx);
}

void
csilk_wf_enable_distributed(csilk_wf_t* wf, csilk_mq_t* mq)
{
	if (!wf || !mq || !wf->wal_dir) {
		return;
	}
	wf->mq = mq;
	if (g_distributed_wf_count < 32) {
		g_distributed_wfs[g_distributed_wf_count++] = wf;
	}
	csilk_mq_subscribe(mq, "csilk.wf.results", on_remote_result);
}
