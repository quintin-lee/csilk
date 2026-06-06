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
	return nullptr;
}

void
test_wf_lifecycle()
{
	printf("Testing workflow lifecycle...\n");
	csilk_wf_t* wf = csilk_wf_new("test_wf");
	assert(wf != nullptr);

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", mock_handler, nullptr);
	assert(n1 != nullptr);

	csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", mock_handler, nullptr);
	assert(n2 != nullptr);

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
