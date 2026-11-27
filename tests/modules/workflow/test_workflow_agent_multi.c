#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"
#include "csilk/core/sys_io.h"

static int g_worker_executed = 0;

static csilk_data_t*
sample_worker_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
    (void)input;
    (void)user_data;
    g_worker_executed = 1;
    return csilk_wf_data_new(ctx, "text/plain", csilk_wf_strdup(ctx, "worker_done"));
}

static void
test_multi_agent_worker()
{
    printf("Testing Multi-Agent Worker node & MQ task dispatching...\n");
    csilk_wf_t* wf = csilk_wf_new("multi_agent_wf");
    assert(wf != NULL);

    csilk_mq_t* mq = (csilk_mq_t*)0x123; // mock MQ handle for unit testing check
    csilk_wf_enable_distributed(wf, mq);

    csilk_wf_node_t* w_node =
        csilk_wf_add_agent_worker(wf, "worker_1", "agent.task.code", sample_worker_handler, NULL);
    assert(w_node != NULL);

    csilk_wf_free(wf);
    printf("test_multi_agent_worker: PASS\n");
}

static void
test_agent_memory_store_api()
{
    printf("Testing Agent Long-Term Memory API...\n");
    csilk_ai_t*        ai = csilk_ai_new("openai", "key", "http://localhost");
    csilk_vector_db_t* db = csilk_vector_db_new("qdrant", "http://localhost:6333", NULL);

    csilk_agent_memory_t* mem =
        csilk_agent_memory_new(ai, "text-embedding-ada-002", db, "agent_memory");
    assert(mem != NULL);

    csilk_agent_memory_free(mem);
    csilk_vector_db_free(db);
    csilk_ai_free(ai);
    printf("test_agent_memory_store_api: PASS\n");
}

int
main()
{
    test_multi_agent_worker();
    test_agent_memory_store_api();
    printf("All Multi-Agent & Memory tests passed successfully!\n");
    return 0;
}
