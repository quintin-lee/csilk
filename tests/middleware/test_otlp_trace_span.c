#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/middleware/otlp_trace.h"

static void
test_otlp_trace_span_lifecycle(void)
{
    csilk_otlp_span_t* span = csilk_otlp_tracer_start_span("WorkflowNode:step1", nullptr);
    assert(span != nullptr);

    csilk_otlp_tracer_end_span(span, 1);
    printf("test_otlp_trace_span_lifecycle passed\n");
}

int
main(void)
{
    test_otlp_trace_span_lifecycle();
    printf("All test_otlp_trace_span tests passed successfully!\n");
    return 0;
}
