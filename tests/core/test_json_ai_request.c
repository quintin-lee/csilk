#include <assert.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include <stdlib.h>

int
main(void)
{
    csilk_json_t* root = csilk_json_object();
    assert(root != nullptr);
    assert(csilk_json_add_string(root, "model", "test-model"));

    csilk_json_t* messages = csilk_json_array();
    assert(messages != nullptr);
    csilk_json_t* message = csilk_json_object();
    assert(message != nullptr);
    assert(csilk_json_add_string(message, "role", "user"));
    assert(csilk_json_add_string(message, "content", "hello"));
    assert(csilk_json_array_append(messages, message));
    assert(csilk_json_add_object(root, "messages", messages));

    char* encoded = csilk_json_serialize(root, nullptr);
    assert(encoded != nullptr);
    assert(strstr(encoded, "test-model") != nullptr);
    assert(strstr(encoded, "hello") != nullptr);

    free(encoded);
    csilk_json_free(root);
    return 0;
}
