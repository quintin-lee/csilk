#include <assert.h>
#include <stdio.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

void
test_server_init()
{
	printf("Testing server initialization...\n");
	csilk_router_t* router = csilk_router_new();
	csilk_server_t* server = csilk_server_new(router);
	assert(server != NULL);

	csilk_server_free(server);
	csilk_router_free(router);
	printf("Server initialization test passed.\n");
}

int
main()
{
	test_server_init();
	printf("All server tests passed!\n");
	return 0;
}
