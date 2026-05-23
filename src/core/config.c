/**
 * @file config.c
 * @brief YAML configuration loader and validator implementation.
 * @copyright MIT License
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

/** @brief Load and parse configuration from a YAML file. */
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
    config->server.worker_threads = 1;
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
                        else if (strcmp(current_key, "max_header_size") == 0) config->server.max_header_size = (size_t)atoll(val);
                        else if (strcmp(current_key, "listen_backlog") == 0) config->server.listen_backlog = atoi(val);
                        else if (strcmp(current_key, "tcp_nodelay") == 0) config->server.tcp_nodelay = atoi(val);
                        else if (strcmp(current_key, "tcp_keepalive") == 0) config->server.tcp_keepalive = atoi(val);
                        else if (strcmp(current_key, "worker_threads") == 0) config->server.worker_threads = atoi(val);
                    } else if (strcmp(current_section, "logger") == 0) {
                        if (strcmp(current_key, "level") == 0) config->logger.level = string_to_log_level(val);
                        else if (strcmp(current_key, "file_path") == 0) {
                            config->logger.file_path = strdup(val);
                            if (!config->logger.file_path) error = 1;
                        }
                        else if (strcmp(current_key, "max_file_size") == 0) config->logger.max_file_size = (size_t)atoll(val);
                        else if (strcmp(current_key, "use_colors") == 0) config->logger.use_colors = atoi(val);
                        else if (strcmp(current_key, "json_format") == 0) config->logger.json_format = atoi(val);
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
                    } else if (strcmp(current_section, "middleware") == 0) {
                        if (strcmp(current_key, "enable_logger") == 0) config->middleware.enable_logger = atoi(val);
                        else if (strcmp(current_key, "enable_recovery") == 0) config->middleware.enable_recovery = atoi(val);
                        else if (strcmp(current_key, "enable_csrf") == 0) config->middleware.enable_csrf = atoi(val);
                        else if (strcmp(current_key, "enable_auth") == 0) config->middleware.enable_auth = atoi(val);
                        else if (strcmp(current_key, "auth_token") == 0) {
                            if (config->middleware.auth_token) free(config->middleware.auth_token);
                            config->middleware.auth_token = strdup(val);
                            if (!config->middleware.auth_token) error = 1;
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

/** @brief Free all dynamically allocated strings in the configuration. */
void csilk_config_free(csilk_config_t* config) {
    if (!config) return;

    if (config->logger.file_path) {
        free((void*)config->logger.file_path);
        config->logger.file_path = NULL;
    }

    if (config->cors.config.allow_origin) {
        free((void*)config->cors.config.allow_origin);
        config->cors.config.allow_origin = NULL;
    }
    if (config->cors.config.allow_methods) {
        free((void*)config->cors.config.allow_methods);
        config->cors.config.allow_methods = NULL;
    }
    if (config->cors.config.allow_headers) {
        free((void*)config->cors.config.allow_headers);
        config->cors.config.allow_headers = NULL;
    }

    if (config->static_files.root_dir) {
        free(config->static_files.root_dir);
        config->static_files.root_dir = NULL;
    }
    if (config->static_files.prefix) {
        free(config->static_files.prefix);
        config->static_files.prefix = NULL;
    }

    if (config->middleware.auth_token) {
        free(config->middleware.auth_token);
        config->middleware.auth_token = NULL;
    }
}

/** @brief Validate configuration values for semantic correctness. */
int csilk_config_validate(const csilk_config_t* config, const char** error_msg) {
    if (!config) {
        if (error_msg) *error_msg = "Null config";
        return -1;
    }
    if (config->port < 1 || config->port > 65535) {
        if (error_msg) *error_msg = "Port must be between 1 and 65535";
        return -1;
    }
    if (config->server.idle_timeout_ms < 0) {
        if (error_msg) *error_msg = "idle_timeout_ms must be >= 0";
        return -1;
    }
    if (config->server.max_body_size == 0) {
        if (error_msg) *error_msg = "max_body_size must be > 0";
        return -1;
    }
    if (config->server.max_header_size == 0) {
        if (error_msg) *error_msg = "max_header_size must be > 0";
        return -1;
    }
    if (config->server.listen_backlog < 1) {
        if (error_msg) *error_msg = "listen_backlog must be >= 1";
        return -1;
    }
    if (config->server.worker_threads < 1) {
        if (error_msg) *error_msg = "worker_threads must be >= 1";
        return -1;
    }
    if (config->rate_limit.enable && config->rate_limit.requests_per_minute < 1) {
        if (error_msg) *error_msg = "requests_per_minute must be > 0 when rate_limit is enabled";
        return -1;
    }
    if (config->static_files.enable && (!config->static_files.root_dir || strlen(config->static_files.root_dir) == 0)) {
        if (error_msg) *error_msg = "root_dir must be set when static_files is enabled";
        return -1;
    }
    if (config->middleware.enable_auth && (!config->middleware.auth_token || strlen(config->middleware.auth_token) == 0)) {
        if (error_msg) *error_msg = "auth_token must be set when auth middleware is enabled";
        return -1;
    }
    return 0;
}
