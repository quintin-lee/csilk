/**
 * @file test_wf_tools_monitor.c
 * @brief Unit tests for the workflow tool-registry growth paths and the
 *        monitor broadcast loop that the high-level test suite leaves
 *        uncovered (realloc expansion, discovery-hook install/clear, and the
 *        _wf_broadcast pruning path over a registered monitor).
 * @copyright MIT License
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/workflow/workflow.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"
#include "modules/workflow/workflow_internal.h" /* _wf_broadcast (CSILK_INTERNAL) */

/* A tool callback that just echoes back a fixed string. */
static char*
echo_tool(const char* args_json, void* user_data)
{
    (void)args_json;
    (void)user_data;
    return strdup("{\"ok\":1}");
}

/* A discovery callback that would resolve extra tools; we only assert it is
 * stored and forwardable, not that it is invoked here. */
static int count_discovery_calls = 0;

static int
discovery_fn(csilk_wf_t*                  wf,
             const csilk_wf_tool_entry_t* entry,
             size_t                       index,
             csilk_wf_tool_entry_t**      out,
             size_t*                      out_count,
             void*                        user_data)
{
    (void)wf;
    (void)entry;
    (void)index;
    (void)out;
    (void)out_count;
    (void)user_data;
    count_discovery_calls++;
    return 0;
}

static void
test_register_tool_grows_capacity(void)
{
    printf("Testing csilk_wf_register_tool realloc growth (>4 tools)...\n");
    csilk_wf_t* wf = csilk_wf_new("tool_growth_wf");
    assert(wf != NULL);

    /* Force several capacity doublings: start at 0, grow to 4, 8, 16. */
    for (int i = 0; i < 12; i++) {
        char name[32];
        snprintf(name, sizeof(name), "tool_%d", i);
        csilk_wf_register_tool(
            wf, name, "desc", "{\"type\":\"object\"}", echo_tool, (void*)(intptr_t)(i + 1));
    }

    /* Guard: NULL wf / NULL name / NULL fn are all no-ops. */
    csilk_wf_register_tool(NULL, "x", "d", NULL, echo_tool, NULL);
    csilk_wf_register_tool(wf, NULL, "d", NULL, echo_tool, NULL);
    csilk_wf_register_tool(wf, "y", "d", NULL, NULL, NULL);

    csilk_wf_free(wf);
    printf("  passed\n");
}

static void
test_set_tool_discovery(void)
{
    printf("Testing csilk_wf_set_tool_discovery install/clear...\n");
    csilk_wf_t* wf = csilk_wf_new("discovery_wf");
    assert(wf != NULL);

    csilk_wf_set_tool_discovery(NULL, discovery_fn, NULL); /* NULL wf no-op */
    csilk_wf_set_tool_discovery(wf, discovery_fn, wf);     /* install */
    csilk_wf_set_tool_discovery(wf, NULL, NULL);           /* clear */

    csilk_wf_free(wf);
    printf("  passed\n");
}

static void
test_monitor_broadcast_pruning(void)
{
    printf("Testing _wf_broadcast over a registered plain (non-WS/SSE) monitor...\n");
    csilk_wf_t* wf = csilk_wf_new("broadcast_wf");
    assert(wf != NULL);

    csilk_ctx_t* mock = csilk_test_ctx_new();
    assert(mock != NULL);

    /* Register a monitor, then broadcast with a node_id + payload so the full
     * JSON construction and the send/prune loop all run. */
    csilk_wf_register_monitor(wf, mock);
    _wf_broadcast(wf, "node_start", "n1", "{\"x\":1}");
    _wf_broadcast(wf, "workflow_end", NULL, NULL);

    /* Unregister a monitor that is not present (no-op path). */
    csilk_ctx_t* other = csilk_test_ctx_new();
    csilk_wf_unregister_monitor(wf, other);
    csilk_test_ctx_free(other);

    /* Unregister the real one. */
    csilk_wf_unregister_monitor(wf, mock);

    csilk_test_ctx_free(mock);
    csilk_wf_free(wf);
    printf("  passed\n");
}

int
main(void)
{
    test_register_tool_grows_capacity();
    test_set_tool_discovery();
    test_monitor_broadcast_pruning();
    printf("test_wf_tools_monitor: ALL PASSED\n");
    return 0;
}
