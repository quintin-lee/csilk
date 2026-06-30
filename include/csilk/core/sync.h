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

typedef pthread_cond_t csilk_cond_t;
static inline int
csilk_cond_init(csilk_cond_t* cond)
{
	return pthread_cond_init(cond, NULL);
}
static inline void
csilk_cond_signal(csilk_cond_t* cond)
{
	pthread_cond_signal(cond);
}
static inline void
csilk_cond_wait(csilk_cond_t* cond, csilk_mutex_t* mutex)
{
	pthread_cond_wait(cond, mutex);
}
static inline void
csilk_cond_destroy(csilk_cond_t* cond)
{
	pthread_cond_destroy(cond);
}
#else
#include <csilk/core/sys_io.h>
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

typedef uv_cond_t csilk_cond_t;
static inline int
csilk_cond_init(csilk_cond_t* cond)
{
	return uv_cond_init(cond);
}
static inline void
csilk_cond_signal(csilk_cond_t* cond)
{
	uv_cond_signal(cond);
}
static inline void
csilk_cond_wait(csilk_cond_t* cond, csilk_mutex_t* mutex)
{
	uv_cond_wait(cond, mutex);
}
static inline void
csilk_cond_destroy(csilk_cond_t* cond)
{
	uv_cond_destroy(cond);
}
#endif

#endif /* CSILK_SYNC_H */
