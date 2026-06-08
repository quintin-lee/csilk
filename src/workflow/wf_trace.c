/**
 * @file wf_trace.c
 * @brief Workflow visualization (Mermaid DAG export) and execution tracing.
 */

#include "workflow_internal.h"

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
