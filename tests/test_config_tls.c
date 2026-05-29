#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

int
main()
{
	const char* yaml_content = "server:\n"
				   "  enable_tls: 1\n"
				   "  tls_cert_file: cert.pem\n"
				   "  tls_key_file: key.pem\n"
				   "  tls_ca_file: ca.pem\n"
				   "  tls_verify_peer: 1\n";

	FILE* f = fopen("test_config_tls.yaml", "w");
	fputs(yaml_content, f);
	fclose(f);

	csilk_config_t cfg;
	printf("Testing TLS YAML config loading...\n");
	int ret = csilk_load_config("test_config_tls.yaml", &cfg);
	assert(ret == 0);

	assert(cfg.server.enable_tls == 1);
	assert(cfg.server.tls_cert_file != NULL);
	assert(strcmp(cfg.server.tls_cert_file, "cert.pem") == 0);
	assert(cfg.server.tls_key_file != NULL);
	assert(strcmp(cfg.server.tls_key_file, "key.pem") == 0);
	assert(cfg.server.tls_ca_file != NULL);
	assert(strcmp(cfg.server.tls_ca_file, "ca.pem") == 0);
	assert(cfg.server.tls_verify_peer == 1);

	printf("Testing TLS YAML config validation...\n");
	const char* error_msg = NULL;
	ret = csilk_config_validate(&cfg, &error_msg);
	assert(ret == 0);

	csilk_config_free(&cfg);
	assert(cfg.server.tls_cert_file == NULL);
	assert(cfg.server.tls_key_file == NULL);
	assert(cfg.server.tls_ca_file == NULL);

	remove("test_config_tls.yaml");

	printf("Testing TLS validation failure (missing cert)...\n");
	memset(&cfg, 0, sizeof(cfg));
	cfg.port = 8080;
	cfg.server.max_body_size = 1024;
	cfg.server.max_header_size = 1024;
	cfg.server.listen_backlog = 128;
	cfg.server.worker_threads = 1;
	cfg.server.enable_tls = 1;
	ret = csilk_config_validate(&cfg, &error_msg);
	assert(ret == -1);
	assert(strstr(error_msg, "tls_cert_file") != NULL);

	printf("Testing TLS validation failure (missing key)...\n");
	cfg.server.tls_cert_file = "cert.pem";
	ret = csilk_config_validate(&cfg, &error_msg);
	assert(ret == -1);
	assert(strstr(error_msg, "tls_key_file") != NULL);

	printf("Testing TLS validation failure (missing CA for verify_peer)...\n");
	cfg.server.tls_key_file = "key.pem";
	cfg.server.tls_verify_peer = 1;
	ret = csilk_config_validate(&cfg, &error_msg);
	assert(ret == -1);
	assert(strstr(error_msg, "tls_ca_file") != NULL);

	printf("test_config_tls: PASS\n");
	return 0;
}
