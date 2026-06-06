#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Define test structures with explicit tags for _Generic
/** @brief Test structure for 2D point reflection tests. */
typedef struct TestPoint_s {
	int16_t x; /**< X coordinate. */
	int16_t y; /**< Y coordinate. */
} TestPoint;

/** @brief Test structure for user data reflection tests. */
typedef struct TestUser_s {
	int32_t id;    /**< User ID. */
	char name[32]; /**< User name string. */
	float score;   /**< User score value. */
	TestPoint pos; /**< Position (nested TestPoint). */
} TestUser;

typedef struct TestArray_s {
	char tags[3][16];
	char* dynamic_tags[2];
} TestArray;

// Extend type map BEFORE including csilk.h/csilk_reflect.h
#undef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP                                                                        \
	, struct TestUser_s : "TestUser",                                                          \
			      struct TestPoint_s : "TestPoint",                                    \
						   struct TestArray_s : "TestArray"

#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"
#include "csilk/test/test.h"

#define POINT_REFLECT_MAP(X)                                                                       \
	X(TestPoint, x, CSILK_TYPE_INT16, sizeof(int16_t), 0, false, nullptr)                      \
	X(TestPoint, y, CSILK_TYPE_INT16, sizeof(int16_t), 0, false, nullptr)

#define USER_REFLECT_MAP(X)                                                                        \
	X(TestUser, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr)                      \
	X(TestUser, name, CSILK_TYPE_STRING, 32, 0, false, nullptr)                                \
	X(TestUser, score, CSILK_TYPE_FLOAT, sizeof(float), 0, false, nullptr)                     \
	X(TestUser, pos, CSILK_TYPE_STRUCT, sizeof(TestPoint), 0, false, "TestPoint")

// Automatic registration
CSILK_REGISTER_REFLECT(TestPoint, POINT_REFLECT_MAP)
CSILK_REGISTER_REFLECT(TestUser, USER_REFLECT_MAP)

#define ARRAY_REFLECT_MAP(X)                                                                       \
	X(TestArray, tags, CSILK_TYPE_STRING, 16, 3, false, nullptr)                               \
	X(TestArray, dynamic_tags, CSILK_TYPE_STRING, sizeof(char*), 2, true, nullptr)

CSILK_REGISTER_REFLECT(TestArray, ARRAY_REFLECT_MAP)

void
test_marshal()
{
	TestUser user = {.id = 1, .name = "Alice", .score = 95.5f, .pos = {10, 20}};
	// Use automatic type dispatch!
	char* json = csilk_marshal(&user);

	assert(json != nullptr);
	assert(strstr(json, "\"id\":1") != nullptr);
	assert(strstr(json, "\"name\":\"Alice\"") != nullptr);
	assert(strstr(json, "\"pos\":{\"x\":10,\"y\":20}") != nullptr);

	free(json);
	printf("test_marshal passed\n");
}

void
test_unmarshal()
{
	const char* json = "{\"id\":2, \"name\":\"Bob\", \"score\":88.0, "
			   "\"pos\":{\"x\":30,\"y\":40}}";
	TestUser user = {0};

	// Use automatic type dispatch!
	int ok = csilk_unmarshal(json, &user);
	assert(ok == 1);
	assert(user.id == 2);
	assert(user.pos.x == 30);

	printf("test_unmarshal passed\n");
}

void
test_context_reflect()
{
	csilk_ctx_t* c = csilk_test_ctx_new();

	// Test binding with macro (uses type string conversion)
	const char* body_str = "{\"id\":3, \"name\":\"Charlie\", \"score\":77.5}";
	csilk_test_ctx_set_body(c, body_str, strlen(body_str));
	TestUser user = {0};
	int ok = csilk_bind(c, TestUser, &user);
	assert(ok == 1);
	assert(user.id == 3);

	// Test response with macro
	csilk_json_t(c, CSILK_STATUS_OK, TestUser, &user);
	assert(csilk_get_status(c) == CSILK_STATUS_OK);
	assert(csilk_get_response_body(c, nullptr) != nullptr);

	csilk_test_ctx_free(c);
	printf("test_context_reflect passed\n");
}

void
test_basic_types()
{
	printf("Testing basic types reflection...\n");

	// int32
	int32_t i32 = 123;
	char* json = csilk_marshal(&i32);
	assert(json != nullptr);
	assert(strcmp(json, "123") == 0);
	free(json);

	i32 = 0;
	assert(csilk_unmarshal("456", &i32) == 1);
	assert(i32 == 456);

	// bool
	bool b = true;
	json = csilk_marshal(&b);
	assert(strcmp(json, "true") == 0);
	free(json);

	b = false;
	assert(csilk_unmarshal("true", &b) == 1);
	assert(b == true);

	// string
	char* s = "hello world";
	json = csilk_marshal(&s);
	assert(strcmp(json, "\"hello world\"") == 0);
	free(json);

	char* s2 = nullptr;
	assert(csilk_unmarshal("\"new string\"", &s2) == 1);
	assert(strcmp(s2, "new string") == 0);
	free(s2);

	printf("test_basic_types passed\n");
}

void
test_arrays()
{
	printf("Testing array reflection...\n");

	TestArray a = {.tags = {"tag1", "tag2", "tag3"}, .dynamic_tags = {"dyn1", "dyn2"}};

	char* json = csilk_marshal(&a);
	assert(json != nullptr);
	assert(strstr(json, "\"tags\":[\"tag1\",\"tag2\",\"tag3\"]") != nullptr);
	assert(strstr(json, "\"dynamic_tags\":[\"dyn1\",\"dyn2\"]") != nullptr);
	free(json);

	const char* input = "{\"tags\":[\"A\",\"B\",\"C\"], \"dynamic_tags\":[\"D\",\"E\"]}";
	TestArray a2 = {0};
	assert(csilk_unmarshal(input, &a2) == 1);
	assert(strcmp(a2.tags[0], "A") == 0);
	assert(strcmp(a2.tags[1], "B") == 0);
	assert(strcmp(a2.tags[2], "C") == 0);
	assert(strcmp(a2.dynamic_tags[0], "D") == 0);
	assert(strcmp(a2.dynamic_tags[1], "E") == 0);

	free(a2.dynamic_tags[0]);
	free(a2.dynamic_tags[1]);
	printf("test_arrays passed\n");
}

int
main()
{
	test_marshal();
	test_unmarshal();
	test_context_reflect();
	test_basic_types();
	test_arrays();
	return 0;
}
