#include <stdio.h>
#include <assert.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

// Define a test that triggers a panic
void
panic_handler(csilk_ctx_t* c)
{
	csilk_panic(c);
}

void
normal_handler(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "OK");
}

int
main()
{
	csilk_ctx_t* c = csilk_test_ctx_new();
	csilk_handler_t handlers[] = {
	    csilk_recovery_handler, panic_handler, normal_handler, nullptr};
	csilk_test_ctx_set_handlers(c, handlers);

	printf("Testing recovery...\n");
	csilk_next(c);

	int status = csilk_get_status(c);
	if (status == CSILK_STATUS_INTERNAL_SERVER_ERROR) {
		printf("Recovered from panic! Status: %d\n", status);
		csilk_test_ctx_free(c);
		return 0; // Test passed
	} else {
		printf("Failed to recover! Status: %d\n", status);
		csilk_test_ctx_free(c);
		return 1; // Test failed
	}
}
