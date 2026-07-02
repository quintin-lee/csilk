#include <assert.h>
#include <stdio.h>
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

    printf("test_config_validate passed\n");
}

int
main()
{
    test_config_validate();
    return 0;
}
