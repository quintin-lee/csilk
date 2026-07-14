#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

static void
test_full_yaml(void)
{
    const char* yaml = "port: 9090\n"
                       "server:\n"
                       "  idle_timeout_ms: 10000\n"
                       "  read_timeout_ms: 5000\n"
                       "  write_timeout_ms: 5000\n"
                       "  request_timeout_ms: 10000\n"
                       "  max_body_size: 2048\n"
                       "  max_header_size: 8192\n"
                       "  max_url_size: 4096\n"
                       "  max_headers_count: 50\n"
                       "  max_connections: 1000\n"
                       "  listen_backlog: 256\n"
                       "  tcp_nodelay: 0\n"
                       "  tcp_keepalive: 60\n"
                       "  worker_threads: 4\n"
                       "logger:\n"
                       "  level: DEBUG\n"
                       "  file_path: test_full_cfg.log\n"
                       "  max_file_size: 1048576\n"
                       "  use_colors: 0\n"
                       "  json_format: 1\n"
                       "cors:\n"
                       "  enable: 1\n"
                       "  allow_origin: '*'\n"
                       "  allow_methods: GET,POST\n"
                       "  allow_headers: Content-Type\n"
                       "  allow_credentials: 1\n"
                       "  max_age: 3600\n"
                       "rate_limit:\n"
                       "  enable: 1\n"
                       "  requests_per_minute: 50\n"
                       "static_files:\n"
                       "  enable: 1\n"
                       "  root_dir: ./public\n"
                       "  prefix: /static\n"
                       "middleware:\n"
                       "  enable_logger: 1\n"
                       "  enable_recovery: 1\n"
                       "  enable_csrf: 1\n"
                       "  enable_auth: 0\n"
                       "  auth_token: secret123\n";

    FILE* f = fopen("test_full_config.yaml", "w");
    assert(f != nullptr);
    fputs(yaml, f);
    fclose(f);

    csilk_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int ret = csilk_load_config("test_full_config.yaml", &cfg);
    assert(ret == 0);

    assert(cfg.port == 9090);
    assert(cfg.server.idle_timeout_ms == 10000);
    assert(cfg.server.read_timeout_ms == 5000);
    assert(cfg.server.write_timeout_ms == 5000);
    assert(cfg.server.request_timeout_ms == 10000);
    assert(cfg.server.max_body_size == 2048);
    assert(cfg.server.max_header_size == 8192);
    assert(cfg.server.max_url_size == 4096);
    assert(cfg.server.max_headers_count == 50);
    assert(cfg.server.max_connections == 1000);
    assert(cfg.server.listen_backlog == 256);
    assert(cfg.server.tcp_nodelay == 0);
    assert(cfg.server.tcp_keepalive == 60);
    assert(cfg.server.worker_threads == 4);
    assert(cfg.logger.level == CSILK_LOG_DEBUG);
    assert(cfg.logger.file_path != nullptr);
    assert(strcmp(cfg.logger.file_path, "test_full_cfg.log") == 0);
    assert(cfg.logger.max_file_size == 1048576);
    assert(cfg.logger.use_colors == 0);
    assert(cfg.logger.json_format == 1);
    assert(cfg.cors.enable == 1);
    assert(strcmp(cfg.cors.config.allow_origin, "*") == 0);
    assert(strcmp(cfg.cors.config.allow_methods, "GET,POST") == 0);
    assert(strcmp(cfg.cors.config.allow_headers, "Content-Type") == 0);
    assert(cfg.cors.config.allow_credentials == 1);
    assert(cfg.cors.config.max_age == 3600);
    assert(cfg.rate_limit.enable == 1);
    assert(cfg.rate_limit.requests_per_minute == 50);
    assert(cfg.static_files.enable == 1);
    assert(strcmp(cfg.static_files.root_dir, "./public") == 0);
    assert(strcmp(cfg.static_files.prefix, "/static") == 0);
    assert(cfg.middleware.enable_logger == 1);
    assert(cfg.middleware.enable_recovery == 1);
    assert(cfg.middleware.enable_csrf == 1);
    assert(cfg.middleware.enable_auth == 0);
    assert(cfg.middleware.auth_token != nullptr);
    assert(strcmp(cfg.middleware.auth_token, "secret123") == 0);

    csilk_config_free(&cfg);
    remove("test_full_config.yaml");
}

static void
test_level_parsing(void)
{
    const char* yaml = "logger:\n"
                       "  level: WARN\n";

    FILE* f = fopen("test_level_warn.yaml", "w");
    if (!f) {
        return;
    }
    fputs(yaml, f);
    fclose(f);

    csilk_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int ret = csilk_load_config("test_level_warn.yaml", &cfg);
    assert(ret == 0);
    assert(cfg.logger.level == CSILK_LOG_WARN);
    csilk_config_free(&cfg);
    remove("test_level_warn.yaml");
}

static void
test_level_error(void)
{
    const char* yaml = "logger:\n"
                       "  level: ERROR\n";

    FILE* f = fopen("test_level_error.yaml", "w");
    if (!f) {
        return;
    }
    fputs(yaml, f);
    fclose(f);

    csilk_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int ret = csilk_load_config("test_level_error.yaml", &cfg);
    assert(ret == 0);
    assert(cfg.logger.level == CSILK_LOG_ERROR);
    csilk_config_free(&cfg);
    remove("test_level_error.yaml");
}

static void
test_level_fatal(void)
{
    const char* yaml = "logger:\n"
                       "  level: FATAL\n";

    FILE* f = fopen("test_level_fatal.yaml", "w");
    if (!f) {
        return;
    }
    fputs(yaml, f);
    fclose(f);

    csilk_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int ret = csilk_load_config("test_level_fatal.yaml", &cfg);
    assert(ret == 0);
    assert(cfg.logger.level == CSILK_LOG_FATAL);
    csilk_config_free(&cfg);
    remove("test_level_fatal.yaml");
}

static void
test_level_unknown(void)
{
    const char* yaml = "logger:\n"
                       "  level: UNKNOWN\n";

    FILE* f = fopen("test_level_unknown.yaml", "w");
    if (!f) {
        return;
    }
    fputs(yaml, f);
    fclose(f);

    csilk_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    int ret = csilk_load_config("test_level_unknown.yaml", &cfg);
    assert(ret == 0);
    assert(cfg.logger.level == CSILK_LOG_INFO);
    csilk_config_free(&cfg);
    remove("test_level_unknown.yaml");
}

static void
test_null_path(void)
{
    int ret = csilk_load_config(nullptr, nullptr);
    assert(ret == -1);
}

static void
test_missing_file(void)
{
    csilk_config_t cfg;
    int            ret = csilk_load_config("/tmp/nonexistent_config_xyz.yaml", &cfg);
    assert(ret == -1);
}

int
main()
{
    printf("Testing full comprehensive YAML config...\n");
    test_full_yaml();

    printf("Testing log level: WARN...\n");
    test_level_parsing();

    printf("Testing log level: ERROR...\n");
    test_level_error();

    printf("Testing log level: FATAL...\n");
    test_level_fatal();

    printf("Testing log level: UNKNOWN...\n");
    test_level_unknown();

    printf("Testing null path...\n");
    test_null_path();

    printf("Testing missing file...\n");
    test_missing_file();

    printf("test_config_comprehensive: PASS\n");
    return 0;
}
