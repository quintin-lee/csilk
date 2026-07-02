/**
 * @file example_tls.c
 * @brief Standalone HTTPS/TLS server example using csilk.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

static void
hello_handler(csilk_ctx_t* c)
{
    csilk_string(c, 200, "Hello over HTTPS! Connection is secured via TLS.");
}

int
main(void)
{
    /* 1. Initialize router and register route */
    csilk_router_t* router = csilk_router_new();
    csilk_group_t*  root = csilk_group_new(router, "");

    csilk_group_use(root, csilk_logger_handler);
    csilk_group_use(root, csilk_recovery_handler);

    csilk_GET(root, "/", hello_handler);

    /* 2. Configure TLS settings */
    csilk_server_config_t config = {0};
    config.enable_tls = 1;
    config.tls_cert_file = "tests/test_cert.pem";
    config.tls_key_file = "tests/test_key.pem";

    /* 3. Create server */
    csilk_server_t* server = csilk_server_new(router);
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        csilk_group_free(root);
        csilk_router_free(router);
        return 1;
    }

    /* Apply the TLS configuration */
    csilk_server_set_config(server, &config);

    printf("\n🔒 Starting Secure HTTPS Server on https://localhost:8443/\n");
    printf("   - Verify with curl: 'curl -k https://localhost:8443/'\n");
    printf("   - Certificate file: %s\n", config.tls_cert_file);
    printf("   - Key file: %s\n", config.tls_key_file);

    /* 4. Run server on port 8443 */
    csilk_server_run(server, 8443);

    /* Cleanup */
    csilk_server_free(server);
    csilk_group_free(root);
    csilk_router_free(router);

    return 0;
}
