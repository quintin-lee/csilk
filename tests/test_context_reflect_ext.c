#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"
#include "csilk/test/test.h"

typedef struct {
	int id;
	char name[32];
} reflect_test_t;

#define REFLECT_TEST_MAP(X)                                                                        \
	X(reflect_test_t, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr)                \
	X(reflect_test_t, name, CSILK_TYPE_STRING, 32, 0, false, nullptr)

// Setup type map for _Generic (if used by macros)
#undef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP , reflect_test_t : "reflect_test_t"

CSILK_REGISTER_REFLECT(reflect_test_t, REFLECT_TEST_MAP)

int
main()
{
	printf("Testing context reflection boundary cases...\n");

	csilk_ctx_t* ctx = csilk_test_ctx_new();

	reflect_test_t data = {0};

	// 1. nullptr checks
	assert(csilk_bind_reflect(nullptr, "reflect_test_t", &data) == 0);
	assert(csilk_bind_reflect(ctx, "reflect_test_t", nullptr) == 0);

	// 2. Missing body
	csilk_test_ctx_set_body(ctx, nullptr, 0);
	assert(csilk_bind_reflect(ctx, "reflect_test_t", &data) == 0);

	// 3. Unregistered type
	csilk_test_ctx_set_body(ctx, "{\"id\":1}", 8);
	assert(csilk_bind_reflect(ctx, "non_existent", &data) == 0);

	// 4. nullptr type_name with no handler metadata
	// In the real system, current_handler is set by the router.
	// For testing, we might need a way to mock it if bind_reflect(ctx, nullptr, ...) is used.
	assert(csilk_bind_reflect(ctx, nullptr, &data) == 0);

	// 5. nullptr type_name with handler metadata
	// This requires setting the internal current_handler.
	// Since we are refactoring for opaque ctx, we should avoid direct member access.
	// However, csilk_bind_reflect(ctx, nullptr, data) is an edge case.
	// For now we skip testing the 'nullptr type_name with handler' path here
	// because it's hard to mock without internal access.

	// 6. Malformed JSON
	csilk_test_ctx_set_body(ctx, "{\"id\":", 6);
	assert(csilk_bind_reflect(ctx, "reflect_test_t", &data) == 0);

	// 7. csilk_json_reflect nullptr checks
	csilk_json_reflect(nullptr, 200, "reflect_test_t", &data); // Should not crash
	csilk_json_reflect(ctx, 200, "reflect_test_t", nullptr);   // Should not crash

	// 8. csilk_json_reflect unregistered type
	csilk_json_reflect(ctx, 200, "non_existent", &data);
	// assert(csilk_get_status(ctx) == 0); // Status should not be set if it failed

	// 9. csilk_json_reflect with registered type
	data.id = 42;
	csilk_json_reflect(ctx, 201, "reflect_test_t", &data);
	assert(csilk_get_status(ctx) == 201);
	size_t body_len = 0;
	const char* body = csilk_get_response_body(ctx, &body_len);
	assert(body != nullptr);
	assert(strstr(body, "42") != nullptr);

	csilk_test_ctx_free(ctx);
	printf("test_context_reflect_ext: PASS\n");
	return 0;
}
