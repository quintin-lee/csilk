#ifdef HAS_REDIS
/**
 * @file redis_internal.h
 * @brief Internal declarations shared between redis.c and redis_storage.c.
 * @copyright MIT License
 */

#ifndef REDIS_INTERNAL_H
#define REDIS_INTERNAL_H

#include <hiredis/hiredis.h>

/** @brief Per-connection data for the Redis driver. */
typedef struct {
    redisContext* c; /**< hiredis connection context. */
} redis_conn_t;

#endif               /* REDIS_INTERNAL_H */
#endif               /* HAS_REDIS */
