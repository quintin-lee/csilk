/**
 * @file config.c
 * @brief YAML configuration loader implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>
#include "csilk.h"

/** @brief Convert a string to a log level enum value.
 * @param s Log level string (case-insensitive).
 * @return Corresponding csilk_log_level_t value, defaults to CSILK_LOG_INFO. */
static csilk_log_level_t string_to_log_level(const char* s) {
    if (strcasecmp(s, "TRACE") == 0) return CSILK_LOG_TRACE;
    if (strcasecmp(s, "DEBUG") == 0) return CSILK_LOG_DEBUG;
    if (strcasecmp(s, "INFO") == 0) return CSILK_LOG_INFO;
    if (strcasecmp(s, "WARN") == 0) return CSILK_LOG_WARN;
    if (strcasecmp(s, "ERROR") == 0) return CSILK_LOG_ERROR;
    if (strcasecmp(s, "FATAL") == 0) return CSILK_LOG_FATAL;
    return CSILK_LOG_INFO;
}

int csilk_load_config(const char* yaml_path, csilk_config_t* config) {
    if (!yaml_path || !config) return -1;

    FILE* fh = fopen(yaml_path, "rb");
    if (!fh) return -1;

    yaml_parser_t parser;
    yaml_event_t event;

    if (!yaml_parser_initialize(&parser)) {
        fclose(fh);
        return -1;
    }

    yaml_parser_set_input_file(&parser, fh);

    char* current_key = NULL;
    char* current_section = NULL;

    // Default values
    memset(config, 0, sizeof(csilk_config_t));
    config->port = 8080;
    config->server.idle_timeout_ms = 5000;
    config->server.max_body_size = 1024 * 1024;
    config->server.listen_backlog = 128;
    config->server.tcp_nodelay = 1;
    config->logger.level = CSILK_LOG_INFO;
    config->logger.use_colors = -1;

    int done = 0;
    int error = 0;
    while (!done && !error) {
        if (!yaml_parser_parse(&parser, &event)) break;

        switch (event.type) {
            case YAML_SCALAR_EVENT:
                if (!current_key) {
                    current_key = strdup((char*)event.data.scalar.value);
                    if (!current_key) error = 1;
                } else {
                    char* val = (char*)event.data.scalar.value;
                    
                    if (current_section == NULL) {
                        if (strcmp(current_key, "port") == 0) config->port = atoi(val);
                    } else if (strcmp(current_section, "server") == 0) {
                        if (strcmp(current_key, "idle_timeout_ms") == 0) config->server.idle_timeout_ms = atoi(val);
                        else if (strcmp(current_key, "max_body_size") == 0) config->server.max_body_size = (size_t)atoll(val);
                        else if (strcmp(current_key, "listen_backlog") == 0) config->server.listen_backlog = atoi(val);
                        else if (strcmp(current_key, "tcp_nodelay") == 0) config->server.tcp_nodelay = atoi(val);
                        else if (strcmp(current_key, "tcp_keepalive") == 0) config->server.tcp_keepalive = atoi(val);
                    } else if (strcmp(current_section, "logger") == 0) {
                        if (strcmp(current_key, "level") == 0) config->logger.level = string_to_log_level(val);
                        else if (strcmp(current_key, "file_path") == 0) {
                            config->logger.file_path = strdup(val);
                            if (!config->logger.file_path) error = 1;
                        }
                        else if (strcmp(current_key, "max_file_size") == 0) config->logger.max_file_size = (size_t)atoll(val);
                        else if (strcmp(current_key, "use_colors") == 0) config->logger.use_colors = atoi(val);
                    } else if (strcmp(current_section, "cors") == 0) {
                        if (strcmp(current_key, "enable") == 0) config->cors.enable = atoi(val);
                        else if (strcmp(current_key, "allow_origin") == 0) {
                            config->cors.config.allow_origin = strdup(val);
                            if (!config->cors.config.allow_origin) error = 1;
                        }
                        else if (strcmp(current_key, "allow_methods") == 0) {
                            config->cors.config.allow_methods = strdup(val);
                            if (!config->cors.config.allow_methods) error = 1;
                        }
                        else if (strcmp(current_key, "allow_headers") == 0) {
                            config->cors.config.allow_headers = strdup(val);
                            if (!config->cors.config.allow_headers) error = 1;
                        }
                        else if (strcmp(current_key, "allow_credentials") == 0) config->cors.config.allow_credentials = atoi(val);
                        else if (strcmp(current_key, "max_age") == 0) config->cors.config.max_age = atoi(val);
                    } else if (strcmp(current_section, "rate_limit") == 0) {
                        if (strcmp(current_key, "enable") == 0) config->rate_limit.enable = atoi(val);
                        else if (strcmp(current_key, "requests_per_minute") == 0) config->rate_limit.requests_per_minute = atoi(val);
                    } else if (strcmp(current_section, "static_files") == 0) {
                        if (strcmp(current_key, "enable") == 0) config->static_files.enable = atoi(val);
                        else if (strcmp(current_key, "root_dir") == 0) {
                            config->static_files.root_dir = strdup(val);
                            if (!config->static_files.root_dir) error = 1;
                        }
                        else if (strcmp(current_key, "prefix") == 0) {
                            config->static_files.prefix = strdup(val);
                            if (!config->static_files.prefix) error = 1;
                        }
                    }
                    
                    free(current_key);
                    current_key = NULL;
                }
                break;
            case YAML_MAPPING_START_EVENT:
                if (current_key) {
                    if (current_section) free(current_section);
                    current_section = current_key;
                    current_key = NULL;
                }
                break;
            case YAML_MAPPING_END_EVENT:
                if (current_section) {
                    free(current_section);
                    current_section = NULL;
                }
                break;
            case YAML_STREAM_END_EVENT:
                done = 1;
                break;
            default:
                break;
        }
        yaml_event_delete(&event);
    }

    if (current_key) free(current_key);
    if (current_section) free(current_section);

    yaml_parser_delete(&parser);
    fclose(fh);
    return error ? -1 : 0;
}
