#ifndef CSILK_DB_INTERNAL_H
#define CSILK_DB_INTERNAL_H

#include <uv.h>
#include "csilk/core/sync.h"

#include "csilk/drivers/db.h"

struct csilk_db_pool_s {
	csilk_db_driver_t* driver;
	void* connection;
	csilk_mutex_t mutex;
};

#endif
