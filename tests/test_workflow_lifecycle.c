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
test_wf_basic_lifecycle()
{
	printf("Testing workflow basic lifecycle...\n");
	csilk_wf_t* wf = csilk_wf_new("test_wf");
	assert(wf != nullptr);

	csilk_wf_node_t* n1 = csilk_wf_add(wf, "n1", mock_handler, nullptr);
	assert(n1 != nullptr);
	csilk_wf_node_t* n2 = csilk_wf_add(wf, "n2", mock_handler, nullptr);
	assert(n2 != nullptr);

	csilk_wf_free(wf);
	printf("  passed\n");
}

void
test_wf_null_safety()
{
	printf("Testing workflow null safety...\n");

	csilk_wf_free(nullptr);

	csilk_wf_node_t* n = csilk_wf_add(nullptr, "x", mock_handler, nullptr);
	assert(n == nullptr);

	n = csilk_wf_get_node(nullptr, "x");
	assert(n == nullptr);

	assert(csilk_wf_add(nullptr, nullptr, nullptr, nullptr) == nullptr);

	printf("  passed\n");
}

void
test_wf_get_node()
{
	printf("Testing workflow get_node...\n");
	csilk_wf_t* wf = csilk_wf_new("get_test");
	csilk_wf_add(wf, "a", mock_handler, nullptr);
	csilk_wf_add(wf, "b", mock_handler, nullptr);

	assert(csilk_wf_get_node(wf, "a") != nullptr);
	assert(csilk_wf_get_node(wf, "b") != nullptr);
	assert(csilk_wf_get_node(wf, "missing") == nullptr);
	assert(csilk_wf_get_node(wf, "") == nullptr);

	csilk_wf_free(wf);
	printf("  passed\n");
}

int
main()
{
	test_wf_basic_lifecycle();
	test_wf_null_safety();
	test_wf_get_node();
	printf("All workflow lifecycle tests passed!\n");
	return 0;
}
