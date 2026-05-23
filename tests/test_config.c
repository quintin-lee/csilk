#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "csilk.h"

int main() {
    const char* yaml_content = 
        "port: 9090\n"
        "server:\n"
        "  idle_timeout_ms: 10000\n"
        "  max_body_size: 2048\n"
        "  tcp_nodelay: 0\n"
        "  tcp_keepalive: 60\n"
        "logger:\n"
        "  level: DEBUG\n"
        "  file_path: test_cfg.log\n"
        "cors:\n"
        "  enable: 1\n"
        "  allow_origin: '*'\n"
        "rate_limit:\n"
        "  enable: 1\n"
        "  requests_per_minute: 50\n"
        "static_files:\n"
        "  enable: 1\n"
        "  root_dir: ./public\n"
        "  prefix: /static\n";

    FILE* f = fopen("test_config_ext.yaml", "w");
    fputs(yaml_content, f);
    fclose(f);

    csilk_config_t cfg;
    printf("Testing Extended YAML config loading...\n");
    {
        int ret = csilk_load_config("test_config_ext.yaml", &cfg);
        assert(ret == 0);
    }

    assert(cfg.port == 9090);
    assert(cfg.server.idle_timeout_ms == 10000);
    assert(cfg.server.max_body_size == 2048);
    assert(cfg.server.tcp_nodelay == 0);
    assert(cfg.server.tcp_keepalive == 60);
    assert(cfg.logger.level == CSILK_LOG_DEBUG);
    assert(strcmp(cfg.logger.file_path, "test_cfg.log") == 0);
    assert(cfg.cors.enable == 1);
    assert(strcmp(cfg.cors.config.allow_origin, "*") == 0);
    assert(cfg.rate_limit.enable == 1);
    assert(cfg.rate_limit.requests_per_minute == 50);
    assert(cfg.static_files.enable == 1);
    assert(strcmp(cfg.static_files.root_dir, "./public") == 0);
    assert(strcmp(cfg.static_files.prefix, "/static") == 0);

    assert(cfg.server.listen_backlog == 128);

    csilk_config_free(&cfg);
    remove("test_config_ext.yaml");

    printf("test_config_ext: PASS\n");
    return 0;
}
