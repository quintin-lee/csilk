#ifndef CSILK_DB_INTERNAL_H
#define CSILK_DB_INTERNAL_H

/**
 * @file db_internal.h
 * @brief Internal definitions for the unified database layer.
 *
 * Declares the private connection-pool struct shared between the core DB
 * registry (db.c) and the individual driver implementations (sqlite.c,
 * mysql.c, postgres.c, redis.c, mongodb.c).
 *
 * @copyright MIT License
 */

#include <csilk/core/sys_io.h>
#include "csilk/core/sync.h"

#include "csilk/drivers/db.h"

struct csilk_db_pool_s {
    csilk_db_driver_t* driver;
    void*              connection;
    csilk_mutex_t      mutex;
};

#endif
