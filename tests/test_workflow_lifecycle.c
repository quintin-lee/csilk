#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow.h"

csilk_data_t*
mock_handler(csilk_wf_ctx_t* ctx, csilk_data_t* input, void* user_data)
{
	(void)ctx;
	(void)input;
	(void)user_data;
	return NULL;
}

void
test_wf_lifecycle()
{
	printf("Testing workflow lifecycle...\n");
	csilk_wf_t* wf = csilk_wf_new("test_wf");
	assert(wf != NULL);

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", mock_handler, NULL);
	assert(n1 != NULL);

	csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", mock_handler, NULL);
	assert(n2 != NULL);

	csilk_wf_free(wf);
	printf("test_wf_lifecycle: PASS\n");
}

int
main()
{
	test_wf_lifecycle();
	printf("All workflow lifecycle tests passed!\n");
	return 0;
}
