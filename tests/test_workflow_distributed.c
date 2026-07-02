#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <csilk/core/sys_io.h>

#include "cJSON.h"
#include "csilk/app/workflow.h"
#include "csilk/csilk.h"

static int         g_remote_task_received = 0;
static int         g_done = 0;
static char*       g_final_result = nullptr;
static csilk_mq_t* g_mq = nullptr;

/* --- Mock Remote Worker --- */
static void
mock_worker_handler(csilk_mq_ctx_t* ctx)
{
    size_t      len;
    const char* payload = csilk_mq_get_payload(ctx, &len);
    printf("[MockWorker] Received task: %.*s\n", (int)len, payload);
    g_remote_task_received++;

    cJSON* root = cJSON_Parse(payload);
    if (!root) {
        csilk_mq_next(ctx);
        return;
    }
    const char* exec_id = cJSON_GetObjectItem(root, "exec_id")->valuestring;
    const char* node_id = cJSON_GetObjectItem(root, "node_id")->valuestring;

    // Simulate some work, then publish result back to the workflow engine
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "exec_id", exec_id);
    cJSON_AddStringToObject(res, "node_id", node_id);
    cJSON_AddStringToObject(res, "output", "remote_worker_result");
    char* res_json = cJSON_PrintUnformatted(res);

    printf("[MockWorker] Publishing result back to MQ for node '%s'...\n", node_id);
    csilk_mq_publish(g_mq, "csilk.wf.results", res_json, strlen(res_json));

    free(res_json);
    cJSON_Delete(res);
    cJSON_Delete(root);
    csilk_mq_next(ctx);
}

void
on_distributed_done(csilk_data_t* result)
{
    printf("[Test] Workflow finished callback received\n");
    if (result && result->value) {
        g_final_result = strdup((char*)result->value);
    }
    g_done = 1;
}

/* Local node handler */
csilk_data_t*
local_step_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    (void)user_data;
    printf("[LocalNode] Running...\n");
    return csilk_wf_data_new(ctx, "text", csilk_wf_strdup(ctx, "local_start"));
}

void
test_workflow_distributed_mq()
{
    printf("Testing distributed node execution via MQ (Simulated)...\n");
    g_remote_task_received = 0;
    g_done = 0;
    mkdir("test_distributed_wals", 0755);

    csilk_server_t* server = csilk_server_new(nullptr);
    g_mq = csilk_server_get_mq(server);

    csilk_wf_t* wf = csilk_wf_new("dist_wf");
    csilk_wf_set_persistence(wf, "test_distributed_wals");
    csilk_wf_enable_distributed(wf, g_mq);

    // Node 1: Local
    // Node 2: Remote (Triggered after local)
    csilk_wf_node_t* n1 = csilk_wf_add(wf, "local", local_step_handler, nullptr);
    csilk_wf_node_t* n2 = csilk_wf_add(wf, "remote", nullptr, nullptr);
    csilk_wf_node_set_remote(n2, 1);

    csilk_wf_bind(n1, n2);
    csilk_wf_node_set_entry(n1, 1);

    // Register mock worker BEFORE running
    csilk_mq_subscribe(g_mq, "csilk.wf.tasks", mock_worker_handler);

    csilk_wf_run(wf, nullptr, on_distributed_done);

    // Run the loop.
    for (int i = 0; i < 200 && !g_done; i++) {
        csilk_io_run(csilk_io_default_loop(), CSILK_IO_RUN_NOWAIT);
        usleep(5000);
    }

    assert(g_remote_task_received == 1);
    assert(g_done == 1);
    assert(g_final_result != nullptr);
    assert(strstr(g_final_result, "remote_worker_result") != nullptr);

    csilk_wf_free(wf);
    csilk_server_free(server);
    free(g_final_result);
    printf("test_workflow_distributed_mq: PASS\n");
}

int
main()
{
    test_workflow_distributed_mq();
    return 0;
}
