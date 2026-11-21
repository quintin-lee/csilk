#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/test/test.h"

void test_waf_long_and_non_nul_body(void);

int handler_called = 0;

void
dummy_handler(csilk_ctx_t* c)
{
    handler_called = 1;
    csilk_string(c, 200, "OK");
}

void
waf_test_handler(csilk_ctx_t* c)
{
    csilk_waf_middleware(c);
    if (csilk_is_aborted(c)) {
        return;
    }
    dummy_handler(c);
}

void
test_waf_clean_request()
{
    printf("Testing WAF with clean request...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/clean");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 1);
    assert(csilk_get_status(c) == 200);

    csilk_test_ctx_free(c);
    printf("WAF clean request passed!\n");
}

void
test_waf_sqli_query()
{
    printf("Testing WAF with SQLi in query...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/sqli");
    csilk_parse_query(c, "q=UNION SELECT");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 0);        // Handler should not be called
    assert(csilk_get_status(c) == 403); // Forbidden

    csilk_test_ctx_free(c);
    printf("WAF SQLi query passed!\n");
}

void
test_waf_xss_query()
{
    printf("Testing WAF with XSS in query...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/xss");
    csilk_parse_query(c, "q=<SCRIPT>alert(1)</SCRIPT>");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 0);
    assert(csilk_get_status(c) == 403);

    csilk_test_ctx_free(c);
    printf("WAF XSS query passed!\n");
}

void
test_waf_path_traversal()
{
    printf("Testing WAF with Path Traversal...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/files/../../etc/passwd");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 0);
    assert(csilk_get_status(c) == 403);

    csilk_test_ctx_free(c);
    printf("WAF Path Traversal passed!\n");
}

void
test_waf_sqli_multiple_queries()
{
    printf("Testing WAF with multiple query params, one malicious...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/search");
    csilk_parse_query(c, "safe=test&q=UNION SELECT users&limit=10");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 0);
    assert(csilk_get_status(c) == 403);

    csilk_test_ctx_free(c);
    printf("WAF SQLi multiple queries passed!\n");
}

void
test_waf_traversal_deep_nesting()
{
    printf("Testing WAF with deeply nested traversal...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/a/b/c/d/../../../../etc/passwd");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 0);
    assert(csilk_get_status(c) == 403);

    csilk_test_ctx_free(c);
    printf("WAF deep traversal passed!\n");
}

void
test_waf_traversal_in_query()
{
    printf("Testing WAF with path traversal in query...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "GET", "/download");
    csilk_parse_query(c, "file=../../../etc/shadow");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 0);
    assert(csilk_get_status(c) == 403);

    csilk_test_ctx_free(c);
    printf("WAF traversal in query passed!\n");
}

void
test_waf_clean_post()
{
    printf("Testing WAF with clean POST body...\n");
    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "POST", "/submit");
    csilk_parse_query(c, "name=hello");

    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 1);
    assert(csilk_get_status(c) == 200);

    csilk_test_ctx_free(c);
    printf("WAF clean POST passed!\n");
}

void
test_waf_null_safety()
{
    printf("Testing WAF null safety...\n");
    csilk_waf_middleware(nullptr);

    csilk_ctx_t* c = csilk_test_ctx_new();
    handler_called = 0;
    waf_test_handler(c);

    assert(handler_called == 1);
    csilk_test_ctx_free(c);
    printf("WAF null safety passed!\n");
}

int
main()
{
    test_waf_clean_request();
    test_waf_sqli_query();
    test_waf_xss_query();
    test_waf_path_traversal();
    test_waf_sqli_multiple_queries();
    test_waf_traversal_deep_nesting();
    test_waf_traversal_in_query();
    test_waf_clean_post();
    test_waf_null_safety();
    test_waf_long_and_non_nul_body();
    printf("All WAF tests passed!\n");
    return 0;
}

void
test_waf_long_and_non_nul_body(void)
{
    printf("Testing WAF with >256B payload and non-NUL heap body...\n");

    const char* sqli = "UNION SELECT * FROM users";
    size_t      sqli_len = strlen(sqli);
    size_t      len = 300 + sqli_len + 128;
    char*       mal_buf = malloc(len);
    assert(mal_buf != nullptr);
    memset(mal_buf, 'a', len);
    memcpy(mal_buf + 300, sqli, sqli_len);

    csilk_ctx_t* c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "POST", "/submit");
    csilk_test_ctx_set_body(c, mal_buf, len);

    handler_called = 0;
    waf_test_handler(c);
    assert(handler_called == 0);
    assert(csilk_get_status(c) == 403);

    csilk_test_ctx_free(c);
    free(mal_buf);

    /* Clean non-NUL-terminated 512-byte heap body must pass without OOB reads. */
    size_t clean_len = 512;
    char*  clean_buf = malloc(clean_len);
    assert(clean_buf != nullptr);
    memset(clean_buf, 'x', clean_len);

    c = csilk_test_ctx_new();
    csilk_test_ctx_set_request(c, "POST", "/submit");
    csilk_test_ctx_set_body(c, clean_buf, clean_len);

    handler_called = 0;
    waf_test_handler(c);
    assert(handler_called == 1);
    assert(csilk_get_status(c) == 200);

    csilk_test_ctx_free(c);
    free(clean_buf);
    printf("WAF long/non-NUL body passed!\n");
}
