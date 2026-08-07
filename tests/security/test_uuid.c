#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/test/test.h"

static int
is_valid_uuid_format(const char* uuid)
{
    if (strlen(uuid) != 36) {
        return 0;
    }
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') {
                return 0;
            }
        } else {
            if (!isxdigit((unsigned char)uuid[i])) {
                return 0;
            }
        }
    }
    return 1;
}

static void
test_uuid_format(void)
{
    printf("Testing UUID v4 format...\n");

    char buf[37];
    csilk_generate_uuid(buf);

    assert(is_valid_uuid_format(buf));
    assert(buf[36] == '\0');

    printf("  generated: %s\n", buf);
    printf("  passed\n");
}

static void
test_uuid_version(void)
{
    printf("Testing UUID v4 version byte...\n");

    for (int i = 0; i < 100; i++) {
        char buf[37];
        csilk_generate_uuid(buf);
        assert(buf[14] == '4');
    }

    printf("  passed\n");
}

static void
test_uuid_variant(void)
{
    printf("Testing UUID v4 variant byte...\n");

    for (int i = 0; i < 100; i++) {
        char buf[37];
        csilk_generate_uuid(buf);
        char v = buf[19];
        assert(v == '8' || v == '9' || v == 'a' || v == 'b');
    }

    printf("  passed\n");
}

static void
test_uuid_uniqueness(void)
{
    printf("Testing UUID uniqueness (1000 UUIDs)...\n");

#define UUID_COUNT 1000
    char uuids[UUID_COUNT][37];

    for (int i = 0; i < UUID_COUNT; i++) {
        csilk_generate_uuid(uuids[i]);
        for (int j = 0; j < i; j++) {
            assert(strcmp(uuids[i], uuids[j]) != 0);
        }
    }
#undef UUID_COUNT

    printf("  passed\n");
}

static void
test_uuid_context_dispatch(void)
{
    printf("Testing UUID via context dispatch...\n");

    csilk_ctx_t* c = csilk_test_ctx_new();

    char buf[37];
    _csilk_generate_uuid(c, buf);

    assert(is_valid_uuid_format(buf));
    assert(buf[14] == '4');

    csilk_test_ctx_free(c);
    printf("  passed\n");
}

static void
test_uuid_null_context_dispatch(void)
{
    printf("Testing UUID via NULL context dispatch...\n");

    char buf[37];
    _csilk_generate_uuid(nullptr, buf);

    assert(is_valid_uuid_format(buf));
    assert(buf[14] == '4');

    printf("  passed\n");
}

int
main(void)
{
    test_uuid_format();
    test_uuid_version();
    test_uuid_variant();
    test_uuid_uniqueness();
    test_uuid_context_dispatch();
    test_uuid_null_context_dispatch();

    printf("test_uuid: ALL PASSED\n");
    return 0;
}
