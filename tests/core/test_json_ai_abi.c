/**
 * @file test_json_ai_abi.c
 * @brief Regression guards for the JSON ownership contract and the AI driver
 *        ABI (docs/superpowers/plans/2026-09-01-json-ai-abi-migration.md).
 *
 * Contract under test:
 *  1. csilk_json_t is opaque; consumers only use pointer APIs.
 *  2. Insertion transfers ownership exactly once — the child wrapper is
 *     consumed, the caller must not touch or free it afterwards.
 *  3. Failed insertion retains ownership with the caller: the child stays
 *     valid, mutable, and must be freed by the caller.
 *  4. Borrowed views stay valid until the root is freed (per-root view
 *     arena); freeing a view handle directly is a safe no-op.
 *  5. csilk_json_free(NULL) is safe; the root is freed exactly once.
 *  6. C struct layout of the AI chat ABI matches the Python ctypes mirror
 *     in python/csilk/lib.py (LP64 offsets, documented below).
 *
 * Run under ASAN to prove the ownership paths (leaks / double frees /
 * use-after-free are failures).
 */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json/json.h"
#include "csilk/drivers/ai.h"

/* ====================================================================
 * ABI layout guards (LP64): field order and offsets must stay in sync
 * with CsilkAiMessage / CsilkAiChatRequest in python/csilk/lib.py.
 * ==================================================================== */
static_assert(offsetof(csilk_ai_message_t, role) == 0, "AI ABI: role offset");
static_assert(offsetof(csilk_ai_message_t, content) == 1 * sizeof(void*), "AI ABI: content offset");
static_assert(offsetof(csilk_ai_message_t, tool_calls) == 2 * sizeof(void*),
              "AI ABI: tool_calls offset");
static_assert(offsetof(csilk_ai_message_t, tool_call_count) == 3 * sizeof(void*),
              "AI ABI: tool_call_count offset");
static_assert(offsetof(csilk_ai_message_t, tool_call_id) == 3 * sizeof(void*) + sizeof(size_t),
              "AI ABI: tool_call_id offset");
static_assert(sizeof(csilk_ai_message_t) == 5 * sizeof(void*), "AI ABI: message size");

static_assert(offsetof(csilk_ai_chat_request_t, model) == 0, "AI ABI: model offset");
static_assert(offsetof(csilk_ai_chat_request_t, messages) == 1 * sizeof(void*),
              "AI ABI: messages offset");
static_assert(offsetof(csilk_ai_chat_request_t, message_count) == 2 * sizeof(void*),
              "AI ABI: message_count offset");
static_assert(offsetof(csilk_ai_chat_request_t, temperature) == 3 * sizeof(void*),
              "AI ABI: temperature offset");
static_assert(offsetof(csilk_ai_chat_request_t, top_p) == 3 * sizeof(void*) + 8,
              "AI ABI: top_p offset");
static_assert(offsetof(csilk_ai_chat_request_t, presence_penalty) == 3 * sizeof(void*) + 16,
              "AI ABI: presence_penalty offset");
static_assert(offsetof(csilk_ai_chat_request_t, frequency_penalty) == 3 * sizeof(void*) + 24,
              "AI ABI: frequency_penalty offset");
static_assert(offsetof(csilk_ai_chat_request_t, max_tokens) == 3 * sizeof(void*) + 32,
              "AI ABI: max_tokens offset");
static_assert(offsetof(csilk_ai_chat_request_t, stop) == 4 * sizeof(void*) + 32,
              "AI ABI: stop offset");
static_assert(offsetof(csilk_ai_chat_request_t, stop_count) == 5 * sizeof(void*) + 32,
              "AI ABI: stop_count offset");
static_assert(offsetof(csilk_ai_chat_request_t, user) == 6 * sizeof(void*) + 32,
              "AI ABI: user offset");
static_assert(offsetof(csilk_ai_chat_request_t, stream) == 7 * sizeof(void*) + 32,
              "AI ABI: stream offset");
static_assert(offsetof(csilk_ai_chat_request_t, on_chunk) == 8 * sizeof(void*) + 32,
              "AI ABI: on_chunk offset");
static_assert(offsetof(csilk_ai_chat_request_t, user_data) == 9 * sizeof(void*) + 32,
              "AI ABI: user_data offset");
static_assert(offsetof(csilk_ai_chat_request_t, timeout_ms) == 10 * sizeof(void*) + 32,
              "AI ABI: timeout_ms offset");
static_assert(offsetof(csilk_ai_chat_request_t, tools) == 11 * sizeof(void*) + 32,
              "AI ABI: tools offset");
static_assert(offsetof(csilk_ai_chat_request_t, tool_count) == 12 * sizeof(void*) + 32,
              "AI ABI: tool_count offset");
static_assert(offsetof(csilk_ai_chat_request_t, tool_choice) == 13 * sizeof(void*) + 32,
              "AI ABI: tool_choice offset");
static_assert(offsetof(csilk_ai_chat_request_t, reasoning_effort) == 14 * sizeof(void*) + 32,
              "AI ABI: reasoning_effort offset");
static_assert(sizeof(csilk_ai_chat_request_t) == 15 * sizeof(void*) + 32, "AI ABI: request size");
/* Response: 2 ptr + 1 size_t + 3 int + padding + 2 ptr = 7 pointers. */
static_assert(offsetof(csilk_ai_chat_response_t, prompt_tokens) == 3 * sizeof(void*),
              "AI ABI: prompt_tokens offset");
static_assert(offsetof(csilk_ai_chat_response_t, raw_response) == 5 * sizeof(void*),
              "AI ABI: raw_response offset");
static_assert(offsetof(csilk_ai_chat_response_t, error_message) == 6 * sizeof(void*),
              "AI ABI: error_message offset");
static_assert(sizeof(csilk_ai_chat_response_t) == 7 * sizeof(void*), "AI ABI: response size");

/* ====================================================================
 * 1. Build -> insert -> serialize -> free-root-once cycle.
 *    Children are never touched again after a successful insertion;
 *    ASAN proves the root free reclaimed everything exactly once.
 * ==================================================================== */
static void
test_build_serialize_free_cycle(void)
{
    csilk_json_t* root = csilk_json_object();
    assert(root != nullptr);

    assert(csilk_json_add_string(root, "model", "abi-guard"));
    assert(csilk_json_add_number(root, "temperature", 0.5));
    assert(csilk_json_add_int(root, "max_tokens", 128));
    assert(csilk_json_add_bool(root, "stream", true));
    assert(csilk_json_add_null(root, "user"));

    csilk_json_t* messages = csilk_json_array();
    assert(messages != nullptr);

    csilk_json_t* message = csilk_json_object();
    assert(message != nullptr);
    assert(csilk_json_add_string(message, "role", "user"));
    assert(csilk_json_add_string(message, "content", "hello"));
    /* Ownership of `message` transfers to `messages`. */
    assert(csilk_json_array_append(messages, message));

    /* Ownership of `messages` transfers to `root`. */
    assert(csilk_json_add_array(root, "messages", messages));

    size_t len = 0;
    char*  encoded = csilk_json_serialize(root, &len);
    assert(encoded != nullptr);
    assert(len == strlen(encoded));
    assert(strstr(encoded, "\"model\":\"abi-guard\"") != nullptr);
    assert(strstr(encoded, "\"max_tokens\":128") != nullptr);
    assert(strstr(encoded, "\"stream\":true") != nullptr);
    assert(strstr(encoded, "\"user\":null") != nullptr);
    assert(strstr(encoded, "\"role\":\"user\"") != nullptr);
    free(encoded);

    char* pretty = csilk_json_serialize_pretty(root, nullptr);
    assert(pretty != nullptr);
    assert(strchr(pretty, '\n') != nullptr);
    free(pretty);

    /* Single free of the root reclaims the whole tree. */
    csilk_json_free(root);
}

/* ====================================================================
 * 2. Failed insertion retains ownership with the caller.
 *    Inserting into an immutable (parsed) root must fail without
 *    consuming the child: the child stays usable and caller-owned.
 * ==================================================================== */
static void
test_failed_insertion_retains_ownership(void)
{
    csilk_json_t* immutable_root = csilk_json_parse("{\"a\":1}");
    assert(immutable_root != nullptr);

    csilk_json_t* child = csilk_json_object();
    assert(child != nullptr);

    /* Immutable target cannot take insertion — must fail cleanly. */
    assert(!csilk_json_add_object(immutable_root, "k", child));
    assert(!csilk_json_add_array(immutable_root, "k", child));
    assert(!csilk_json_array_append(immutable_root, child));

    /* NULL keys/items fail without side effects. */
    assert(!csilk_json_add_object(child, nullptr, nullptr));
    assert(!csilk_json_add_string(child, nullptr, "x"));

    /* Child is still owned by the caller and fully usable. */
    assert(csilk_json_add_string(child, "kept", "mine"));
    char* encoded = csilk_json_serialize(child, nullptr);
    assert(encoded != nullptr);
    assert(strstr(encoded, "\"kept\":\"mine\"") != nullptr);
    free(encoded);

    csilk_json_free(child);
    csilk_json_free(immutable_root);
}

/* ====================================================================
 * 3. Borrowed views live until the root is freed; freeing a view
 *    handle directly is a no-op that must not disturb the arena or
 *    invalidate sibling views.
 * ==================================================================== */
static void
test_views_live_until_root_free(void)
{
    csilk_json_t* root = csilk_json_parse(
        "{\"name\":\"csilk\",\"retries\":3,\"nested\":{\"deep\":\"value\"},\"list\":[10,20]}");
    assert(root != nullptr);

    /* Multiple views over the same root must all stay valid. */
    csilk_json_t* name = csilk_json_get(root, "name");
    csilk_json_t* name_again = csilk_json_get(root, "name");
    csilk_json_t* nested = csilk_json_get_object(root, "nested");
    csilk_json_t* list = csilk_json_get_array(root, "list");
    assert(name != nullptr && name_again != nullptr);
    assert(nested != nullptr && list != nullptr);

    assert(csilk_json_is_string(name));
    assert(strcmp(csilk_json_string_value(name), "csilk") == 0);
    assert(strcmp(csilk_json_string_value(name_again), "csilk") == 0);

    csilk_json_t* deep = csilk_json_get(nested, "deep");
    assert(deep != nullptr);
    assert(strcmp(csilk_json_string_value(deep), "value") == 0);

    assert(csilk_json_array_size(list) == 2);
    csilk_json_t* first = csilk_json_array_get(list, 0);
    csilk_json_t* second = csilk_json_array_get(list, 1);
    assert(first != nullptr && second != nullptr);
    assert(csilk_json_number_value(first) == 10.0);
    assert(csilk_json_number_value(second) == 20.0);

    /* Typed accessors on the root still work through views. */
    assert(strcmp(csilk_json_get_string(root, "name"), "csilk") == 0);
    assert(csilk_json_get_int(root, "retries") == 3);
    assert(strcmp(csilk_json_get_string(nested, "deep"), "value") == 0);

    /* Freeing a view handle is a safe no-op; sibling views survive. */
    csilk_json_free(name_again);
    assert(strcmp(csilk_json_string_value(name), "csilk") == 0);

    csilk_json_free(root);
}

/* ====================================================================
 * 4. Free contract: NULL is safe, parse errors produce NULL, and the
 *    round-trip preserves scalar fidelity.
 * ==================================================================== */
static void
test_free_null_and_parse_err(void)
{
    csilk_json_free(nullptr);

    const char*   error = nullptr;
    csilk_json_t* bad = csilk_json_parse_err("{\"a\":", &error);
    assert(bad == nullptr);
    assert(error != nullptr && error[0] != '\0');

    csilk_json_t* root = csilk_json_parse_err("{\"n\":-42,\"s\":\"x\"}", &error);
    assert(root != nullptr);
    assert(error == nullptr);
    assert(csilk_json_get_int(root, "n") == -42);
    assert(strcmp(csilk_json_get_string(root, "s"), "x") == 0);

    /* parse_len's real contract: parse a NON-NUL-terminated buffer —
     * no strlen/truncation hazard (yyjson reads exactly `len` bytes). */
    char buf[13] = "{k:v,more:1}"; /* junk past the doc; buf has no NUL */
    memcpy(buf, "{\"n\":1234567}", 13);
    csilk_json_t* term_free = csilk_json_parse_len(buf, 13);
    assert(term_free != nullptr);
    assert(csilk_json_get_int(term_free, "n") == 1234567);
    csilk_json_free(term_free);

    /* Escaped-NUL round trip: "x\u0000y" parses, serializes, re-parses. */
    csilk_json_t* esc_root = csilk_json_parse("{\"s\":\"x\\u0000y\"}");
    assert(esc_root != nullptr);
    char* esc_out = csilk_json_serialize(esc_root, nullptr);
    assert(esc_out != nullptr);
    assert(strstr(esc_out, "\\u0000") != nullptr);
    csilk_json_t* esc_again = csilk_json_parse(esc_out);
    assert(esc_again != nullptr);
    const char* esc_val = csilk_json_get_string(esc_again, "s");
    assert(esc_val != nullptr);
    assert(esc_val[0] == 'x' && esc_val[1] == '\0' && esc_val[2] == 'y');
    free(esc_out);
    csilk_json_free(esc_again);
    csilk_json_free(esc_root);

    /* Empty-container round trip. */
    csilk_json_t* empty = csilk_json_parse("[]");
    assert(empty != nullptr);
    assert(csilk_json_is_array(empty));
    assert(csilk_json_array_size(empty) == 0);
    csilk_json_free(empty);
}

/* ====================================================================
 * 5. In-place mutation on a mutable string node is reflected by
 *    serialization of the owning root.
 * ==================================================================== */
static void
test_set_string_mutation(void)
{
    /* Mutable handle: in-place set, visible through the owning root. */
    csilk_json_t* root = csilk_json_object();
    assert(root != nullptr);
    assert(csilk_json_add_string(root, "role", "user"));
    csilk_json_t* role = csilk_json_get(root, "role");
    assert(role != nullptr);
    assert(csilk_json_set_string(role, "assistant"));

    char* encoded = csilk_json_serialize(root, nullptr);
    assert(encoded != nullptr);
    assert(strstr(encoded, "\"role\":\"assistant\"") != nullptr);
    free(encoded);

    /* Immutable view: a borrowed view of a parsed doc cannot mutate —
     * it must FAIL explicitly instead of silently discarding the change. */
    csilk_json_t* parsed = csilk_json_parse("{\"role\":\"user\"}");
    assert(parsed != nullptr);
    csilk_json_t* prole = csilk_json_get(parsed, "role");
    assert(prole != nullptr);
    assert(!csilk_json_set_string(prole, "assistant"));
    encoded = csilk_json_serialize(parsed, nullptr);
    assert(encoded != nullptr);
    assert(strstr(encoded, "\"role\":\"user\"") != nullptr);
    free(encoded);

    /* Immutable OWNER root (a parsed string document) converts in place. */
    csilk_json_t* s = csilk_json_parse("\"old\"");
    assert(s != nullptr);
    assert(csilk_json_set_string(s, "new"));
    encoded = csilk_json_serialize(s, nullptr);
    assert(encoded != nullptr);
    assert(strcmp(encoded, "\"new\"") == 0);
    free(encoded);

    /* Non-string nodes reject. */
    assert(!csilk_json_set_string(root, "nope"));
    assert(!csilk_json_set_string(nullptr, "nope"));

    csilk_json_free(role); /* view free is a no-op */
    csilk_json_free(root);
    csilk_json_free(parsed);
    csilk_json_free(s);
}

int
main(void)
{
    test_build_serialize_free_cycle();
    test_failed_insertion_retains_ownership();
    test_views_live_until_root_free();
    test_free_null_and_parse_err();
    test_set_string_mutation();
    printf("test_json_ai_abi: all ownership/ABI guards passed\n");
    return 0;
}
