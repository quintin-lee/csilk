#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gin.h"

int main() {
    const char* yaml_content = 
        "port: 9090\n"
        "server:\n"
        "  idle_timeout_ms: 10000\n"
        "  max_body_size: 2048\n"
        "logger:\n"
        "  level: DEBUG\n"
        "  file_path: test_cfg.log\n"
        "cors:\n"
        "  enable: 1\n"
        "  allow_origin: '*'\n";

    FILE* f = fopen("test_config.yaml", "w");
    fputs(yaml_content, f);
    fclose(f);

    gin_config_t cfg;
    printf("Testing YAML config loading...\n");
    assert(gin_load_config("test_config.yaml", &cfg) == 0);

    assert(cfg.port == 9090);
    assert(cfg.server.idle_timeout_ms == 10000);
    assert(cfg.server.max_body_size == 2048);
    assert(cfg.logger.level == GIN_LOG_DEBUG);
    assert(strcmp(cfg.logger.file_path, "test_cfg.log") == 0);
    assert(cfg.cors.enable == 1);
    assert(strcmp(cfg.cors.config.allow_origin, "*") == 0);

    // Default value check
    assert(cfg.server.listen_backlog == 128);

    // Cleanup
    free((void*)cfg.logger.file_path);
    free((void*)cfg.cors.config.allow_origin);
    remove("test_config.yaml");

    printf("test_config: PASS\n");
    return 0;
}
