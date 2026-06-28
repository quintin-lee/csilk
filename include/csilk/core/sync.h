#ifndef CSILK_SYNC_H
#define CSILK_SYNC_H

#ifdef CSILK_USE_URING
#include <pthread.h>
typedef pthread_mutex_t csilk_mutex_t;
typedef pthread_t csilk_thread_t;
typedef pthread_once_t csilk_once_t;
#define CSILK_ONCE_INIT PTHREAD_ONCE_INIT

static inline int
csilk_mutex_init(csilk_mutex_t* m)
{
	return pthread_mutex_init(m, NULL);
}
static inline void
csilk_mutex_lock(csilk_mutex_t* m)
{
	pthread_mutex_lock(m);
}
static inline void
csilk_mutex_unlock(csilk_mutex_t* m)
{
	pthread_mutex_unlock(m);
}
static inline void
csilk_mutex_destroy(csilk_mutex_t* m)
{
	pthread_mutex_destroy(m);
}
static inline void
csilk_once(csilk_once_t* control, void (*cb)(void))
{
	pthread_once(control, cb);
}
#else
#include <uv.h>
typedef uv_mutex_t csilk_mutex_t;
typedef uv_thread_t csilk_thread_t;
typedef uv_once_t csilk_once_t;
#define CSILK_ONCE_INIT UV_ONCE_INIT

static inline int
csilk_mutex_init(csilk_mutex_t* m)
{
	return uv_mutex_init(m);
}
static inline void
csilk_mutex_lock(csilk_mutex_t* m)
{
	uv_mutex_lock(m);
}
static inline void
csilk_mutex_unlock(csilk_mutex_t* m)
{
	uv_mutex_unlock(m);
}
static inline void
csilk_mutex_destroy(csilk_mutex_t* m)
{
	uv_mutex_destroy(m);
}
static inline void
csilk_once(csilk_once_t* control, void (*cb)(void))
{
	uv_once(control, cb);
}
#endif

#endif /* CSILK_SYNC_H */
