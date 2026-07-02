#ifndef CSILK_SCAFFOLD_H
#define CSILK_SCAFFOLD_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[64];
    char output_dir[256];
    int port;

    // Template
    int template_type; // 0: Starter, 1: API, 2: AI, 3: Admin

    // Features (Security)
    bool has_waf;
    bool has_jwt;
    bool has_cors;
    bool has_csrf;
    bool has_session;

    // Features (Performance)
    bool has_recovery;
    bool has_logger;
    bool has_gzip;
    bool has_ratelimit;
    bool has_request_id;

    // Protocols
    bool has_ws;
    bool has_sse;

    // Advanced
    bool has_admin;
    bool has_prometheus;
    bool has_swagger;
    bool has_workflow;
    bool has_vector_db;

    // Drivers
    bool has_sqlite;
    bool has_mysql;
    bool has_postgres;
    bool has_mongodb;
    bool has_redis;
    bool has_qdrant;
    bool has_milvus;

    // Verification
    char local_csilk_path[512];
} project_config_t;

#endif
