#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

void
test_config_validate()
{
    csilk_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    const char* err = nullptr;

    // Test invalid port
    cfg.port = 0;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "Port must be between 1 and 65535") == 0);

    // Test valid port, invalid max_body_size
    cfg.port = 8080;
    cfg.server.max_body_size = 0;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "max_body_size must be > 0") == 0);

    // Test valid body, invalid max_header_size
    cfg.server.max_body_size = 1024;
    cfg.server.max_header_size = 0;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "max_header_size must be > 0") == 0);

    // Test valid header, invalid listen_backlog
    cfg.server.max_header_size = 1024;
    cfg.server.listen_backlog = 0;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "listen_backlog must be >= 1") == 0);

    // Test valid, invalid worker_threads
    cfg.server.listen_backlog = 10;
    cfg.server.worker_threads = 0;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "worker_threads must be >= 1") == 0);

    // Test valid config
    cfg.server.worker_threads = 1;
    assert(csilk_config_validate(&cfg, &err) == 0);

    // Test negative timeout checks (unsigned wraparound)
    cfg.server.idle_timeout_ms = (unsigned int)-1;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "idle_timeout_ms must be >= 0") == 0);
    cfg.server.idle_timeout_ms = 0;

    cfg.server.read_timeout_ms = (unsigned int)-1;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "read_timeout_ms must be >= 0") == 0);
    cfg.server.read_timeout_ms = 0;

    cfg.server.write_timeout_ms = (unsigned int)-1;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "write_timeout_ms must be >= 0") == 0);
    cfg.server.write_timeout_ms = 0;

    cfg.server.request_timeout_ms = (unsigned int)-1;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "request_timeout_ms must be >= 0") == 0);
    cfg.server.request_timeout_ms = 0;

    // Test ai.base_url scheme / traversal validation
    char bad_scheme[] = "file:///etc/passwd";
    cfg.ai.base_url = bad_scheme;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "ai.base_url must be a valid HTTP/HTTPS URL") == 0);

    char bad_trav[] = "https://api.openai.com/../v1";
    cfg.ai.base_url = bad_trav;
    assert(csilk_config_validate(&cfg, &err) == -1);
    assert(strcmp(err, "ai.base_url must be a valid HTTP/HTTPS URL") == 0);

    char ok_local[] = "http://127.0.0.1:11434";
    cfg.ai.base_url = ok_local;
    assert(csilk_config_validate(&cfg, &err) == 0);

    char ok_https[] = "https://api.openai.com/v1";
    cfg.ai.base_url = ok_https;
    assert(csilk_config_validate(&cfg, &err) == 0);
    cfg.ai.base_url = nullptr;

    printf("test_config_validate passed\n");
}

static void
test_yaml_hardening(void)
{
    csilk_config_t cfg;
    const char*    err = nullptr;

    // Duplicate string keys should not leak; overflow timeout resets to default 30000
    const char* dup_yaml = "port: 8081\n"
                           "server:\n"
                           "  read_timeout_ms: 999999999999999\n"
                           "  write_timeout_ms: 4500\n"
                           "logger:\n"
                           "  file_path: first.log\n"
                           "  file_path: second.log\n"
                           "middleware:\n"
                           "  enable_auth: 1\n"
                           "  auth_token: old_tok\n"
                           "  auth_token: new_tok\n"
                           "ai:\n"
                           "  base_url: http://invalid\n"
                           "  base_url: https://api.openai.com/v1\n";
    assert(csilk_load_config_memory((const uint8_t*)dup_yaml, strlen(dup_yaml), &cfg) == 0);
    assert(cfg.server.read_timeout_ms == 30000);
    assert(cfg.server.write_timeout_ms == 4500);
    assert(strcmp(cfg.logger.file_path, "second.log") == 0);
    assert(strcmp(cfg.middleware.auth_token, "new_tok") == 0);
    assert(strcmp(cfg.ai.base_url, "https://api.openai.com/v1") == 0);
    assert(csilk_config_validate(&cfg, &err) == 0);
    csilk_config_free(&cfg);

    // Excessively deep YAML (> 32 levels) must fail cleanly without leaks
    char deep_yaml[1024];
    int  pos =
        snprintf(deep_yaml, sizeof(deep_yaml), "ai:\n  base_url: https://api.openai.com/v1\n");
    for (int i = 0; i < 40; i++) {
        pos += snprintf(deep_yaml + pos, sizeof(deep_yaml) - (size_t)pos, "k%d: {", i);
    }
    for (int i = 0; i < 40; i++) {
        pos += snprintf(deep_yaml + pos, sizeof(deep_yaml) - (size_t)pos, "}");
    }
    assert(csilk_load_config_memory((const uint8_t*)deep_yaml, (size_t)pos, &cfg) == -1);

    printf("test_yaml_hardening passed\n");
}

int
main()
{
    test_config_validate();
    test_yaml_hardening();
    return 0;
}
