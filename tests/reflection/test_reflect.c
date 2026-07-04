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
    int32_t   id;       /**< User ID. */
    char      name[32]; /**< User name string. */
    float     score;    /**< User score value. */
    TestPoint pos;      /**< Position (nested TestPoint). */
} TestUser;

typedef struct TestArray_s {
    char  tags[3][16];
    char* dynamic_tags[2];
} TestArray;

typedef struct TestNode_s {
    char*              name;
    struct TestNode_s* next;
} TestNode;

// Extend type map BEFORE including csilk.h/csilk_reflect.h
#undef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP                                                                        \
    , struct TestUser_s : "TestUser",                                                              \
                          struct TestPoint_s : "TestPoint",                                        \
                                               struct TestArray_s : "TestArray",                   \
                                                                    struct TestNode_s : "TestNode"

#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"
#include "csilk/test/test.h"

#define POINT_REFLECT_MAP(X)                                                                       \
    X(TestPoint, x, CSILK_TYPE_INT16, sizeof(int16_t), 0, false, nullptr)                          \
    X(TestPoint, y, CSILK_TYPE_INT16, sizeof(int16_t), 0, false, nullptr)

#define USER_REFLECT_MAP(X)                                                                        \
    X(TestUser, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr)                          \
    X(TestUser, name, CSILK_TYPE_STRING, 32, 0, false, nullptr)                                    \
    X(TestUser, score, CSILK_TYPE_FLOAT, sizeof(float), 0, false, nullptr)                         \
    X(TestUser, pos, CSILK_TYPE_STRUCT, sizeof(TestPoint), 0, false, "TestPoint")

// Automatic registration
CSILK_REGISTER_REFLECT(TestPoint, POINT_REFLECT_MAP)
CSILK_REGISTER_REFLECT(TestUser, USER_REFLECT_MAP)

#define ARRAY_REFLECT_MAP(X)                                                                       \
    X(TestArray, tags, CSILK_TYPE_STRING, 16, 3, false, nullptr)                                   \
    X(TestArray, dynamic_tags, CSILK_TYPE_STRING, sizeof(char*), 2, true, nullptr)

CSILK_REGISTER_REFLECT(TestArray, ARRAY_REFLECT_MAP)

#define NODE_REFLECT_MAP(X)                                                                        \
    X(TestNode, name, CSILK_TYPE_STRING, sizeof(char*), 0, true, nullptr)                          \
    X(TestNode, next, CSILK_TYPE_STRUCT, sizeof(TestNode), 0, true, "TestNode")

CSILK_REGISTER_REFLECT(TestNode, NODE_REFLECT_MAP)

void
test_marshal()
{
    TestUser user = {
        .id = 1, .name = "Alice", .score = 95.5f, .pos = {10, 20}
    };
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
    TestUser    user = {0};

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
    int      ok = csilk_bind(c, TestUser, &user);
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
    char*   json = csilk_marshal(&i32);
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

    TestArray a = {
        .tags = {"tag1", "tag2", "tag3"},
          .dynamic_tags = {"dyn1", "dyn2"}
    };

    char* json = csilk_marshal(&a);
    assert(json != nullptr);
    assert(strstr(json, "\"tags\":[\"tag1\",\"tag2\",\"tag3\"]") != nullptr);
    assert(strstr(json, "\"dynamic_tags\":[\"dyn1\",\"dyn2\"]") != nullptr);
    free(json);

    const char* input = "{\"tags\":[\"A\",\"B\",\"C\"], \"dynamic_tags\":[\"D\",\"E\"]}";
    TestArray   a2 = {0};
    assert(csilk_unmarshal(input, &a2) == 1);
    assert(strcmp(a2.tags[0], "A") == 0);
    assert(strcmp(a2.tags[1], "B") == 0);
    assert(strcmp(a2.tags[2], "C") == 0);
    assert(strcmp(a2.dynamic_tags[0], "D") == 0);
    assert(strcmp(a2.dynamic_tags[1], "E") == 0);

    // Verify csilk_struct_free_reflect on a struct containing an array of string pointers
    csilk_struct_free_reflect("TestArray", &a2);
    assert(a2.dynamic_tags[0] == nullptr);
    assert(a2.dynamic_tags[1] == nullptr);

    printf("test_arrays passed\n");
}

void
test_deep_free()
{
    printf("Testing deep recursive freeing...\n");

    // 1. Test basic types: string
    char* s = nullptr;
    assert(csilk_unmarshal("\"hello dynamic\"", &s) == 1);
    assert(s != nullptr);
    assert(strcmp(s, "hello dynamic") == 0);

    csilk_struct_free_reflect("string", &s);
    assert(s == nullptr);

    // 2. Test deeply nested struct pointer (TestNode)
    const char* json_node = "{\"name\":\"root\", \"next\":{\"name\":\"child\", \"next\":null}}";
    TestNode    root = {0};
    assert(csilk_unmarshal(json_node, &root) == 1);
    assert(root.name != nullptr);
    assert(strcmp(root.name, "root") == 0);
    assert(root.next != nullptr);
    assert(root.next->name != nullptr);
    assert(strcmp(root.next->name, "child") == 0);
    assert(root.next->next == nullptr);

    // Keep a copy of the next pointer to check that the struct memory itself was freed
    TestNode* child_ptr = root.next;

    csilk_struct_free_reflect("TestNode", &root);

    assert(root.name == nullptr);
    assert(root.next == nullptr);
    // We cannot safely dereference child_ptr since it has been freed,
    // but the recursive free has successfully freed its contents and set root.next to NULL.

    printf("test_deep_free passed\n");
}

void
test_cyclic_reflection()
{
    printf("Testing cyclic reflection safety...\n");

    // 1. Test static cycle detection at registration time
    // We dynamically register a cyclic structure. Stderr will print a warning.
    csilk_field_desc_t cyclic_meta[] = {
        {"self",  CSILK_TYPE_STRUCT, 0, sizeof(void*), 0, true,  "CyclicX"},
        {nullptr, 0,                 0, 0,             0, false, nullptr  }
    };
    csilk_reflect_register("CyclicX", cyclic_meta, 1);

    // 2. Test runtime recursion depth limit protection
    // Create a circular list of nodes
    TestNode n1 = {nullptr, nullptr};
    char*    name = malloc(16);
    if (name) {
        strcpy(name, "cyclic_node");
    }
    n1.name = name;
    n1.next = &n1; // Cyclic reference!

    // This call should NOT crash. It will abort when depth exceeds 32.
    csilk_struct_free_reflect("TestNode", &n1);

    // n1.name should be freed and set to nullptr.
    assert(n1.name == nullptr);
    // n1.next was not freed because it points to &n1 (an ancestor), so it was safely nullified to break the loop.
    assert(n1.next == nullptr);

    printf("test_cyclic_reflection passed\n");
}

int
main()
{
    test_marshal();
    test_unmarshal();
    test_context_reflect();
    test_basic_types();
    test_arrays();
    test_deep_free();
    test_cyclic_reflection();
    return 0;
}
