#ifndef CSILK_MQ_INTERNAL_H
#define CSILK_MQ_INTERNAL_H

#include <stddef.h>

#include "csilk/mq.h"

int _mq_enqueue(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);
int _mq_append_wal(csilk_mq_t* mq, const char* topic, const void* payload, size_t len);
int _mq_recovery(csilk_mq_t* mq);

#endif
