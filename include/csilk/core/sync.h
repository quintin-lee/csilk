#pragma once
/**
 * @file csilk/core/sync.h
 * @brief Backend-agnostic synchronization primitives.
 *
 * Provides a thin, inline abstraction over either POSIX threads (when
 * CSILK_USE_URING is defined) or libuv (otherwise).  The public type and
 * function names are identical across both backends, so callers can write
 * portable locking code without caring which threading layer is compiled in.
 *
 * @copyright MIT License
 */

#ifdef CSILK_USE_URING
#include <pthread.h>
/** @brief Mutex type (pthread_mutex_t under the uring backend). */
typedef pthread_mutex_t csilk_mutex_t;
/** @brief Thread handle type (pthread_t under the uring backend). */
typedef pthread_t csilk_thread_t;
/** @brief One-time initializer control type (pthread_once_t). */
typedef pthread_once_t csilk_once_t;
/** @brief Initializer for a csilk_once_t. */
#define CSILK_ONCE_INIT PTHREAD_ONCE_INIT

/**
 * @brief Initialize a mutex.
 * @param[in,out] m Mutex to initialize.
 * @return 0 on success, or a pthread error number on failure.
 */
static inline int
csilk_mutex_init(csilk_mutex_t* m)
{
    return pthread_mutex_init(m, NULL);
}
/**
 * @brief Lock a mutex, blocking until acquired.
 * @param[in,out] m Mutex to lock.
 */
static inline void
csilk_mutex_lock(csilk_mutex_t* m)
{
    pthread_mutex_lock(m);
}
/**
 * @brief Unlock a previously locked mutex.
 * @param[in,out] m Mutex to unlock.
 */
static inline void
csilk_mutex_unlock(csilk_mutex_t* m)
{
    pthread_mutex_unlock(m);
}
/**
 * @brief Destroy a mutex.
 * @param[in,out] m Mutex to destroy.
 */
static inline void
csilk_mutex_destroy(csilk_mutex_t* m)
{
    pthread_mutex_destroy(m);
}
/**
 * @brief Run @p cb exactly once, guarded by @p control.
 * @param[in,out] control One-time control object.
 * @param[in] cb Callback invoked exactly once across all callers.
 */
static inline void
csilk_once(csilk_once_t* control, void (*cb)(void))
{
    pthread_once(control, cb);
}

/** @brief Condition variable type (pthread_cond_t under the uring backend). */
typedef pthread_cond_t csilk_cond_t;
/**
 * @brief Initialize a condition variable.
 * @param[in,out] cond Condition variable to initialize.
 * @return 0 on success, or a pthread error number on failure.
 */
static inline int
csilk_cond_init(csilk_cond_t* cond)
{
    return pthread_cond_init(cond, NULL);
}
/**
 * @brief Signal one waiter on a condition variable.
 * @param[in,out] cond Condition variable to signal.
 */
static inline void
csilk_cond_signal(csilk_cond_t* cond)
{
    pthread_cond_signal(cond);
}
/**
 * @brief Wake all waiters on a condition variable.
 * @param[in,out] cond Condition variable to broadcast.
 */
static inline void
csilk_cond_broadcast(csilk_cond_t* cond)
{
    pthread_cond_broadcast(cond);
}
/**
 * @brief Wait on a condition variable, atomically releasing the mutex.
 * @param[in,out] cond Condition variable to wait on.
 * @param[in,out] mutex Mutex currently held by the caller.
 */
static inline void
csilk_cond_wait(csilk_cond_t* cond, csilk_mutex_t* mutex)
{
    pthread_cond_wait(cond, mutex);
}
/**
 * @brief Wait on a condition variable with a timeout in nanoseconds.
 * @param[in,out] cond Condition variable to wait on.
 * @param[in,out] mutex Mutex currently held by the caller.
 * @param timeout_ns Timeout duration in nanoseconds.
 * @return 0 on success, or a non-zero error/timeout code.
 */
static inline int
csilk_cond_timedwait(csilk_cond_t* cond, csilk_mutex_t* mutex, uint64_t timeout_ns)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    ts.tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mutex, &ts);
}
/**
 * @brief Destroy a condition variable.
 * @param[in,out] cond Condition variable to destroy.
 */
static inline void
csilk_cond_destroy(csilk_cond_t* cond)
{
    pthread_cond_destroy(cond);
}

typedef void (*csilk_thread_cb)(void* arg);

static inline int
csilk_thread_create(csilk_thread_t* tid, csilk_thread_cb cb, void* arg)
{
    return pthread_create(tid, NULL, (void* (*)(void*))cb, arg);
}

static inline int
csilk_thread_join(csilk_thread_t* tid)
{
    return pthread_join(*tid, NULL);
}

static inline csilk_thread_t
csilk_thread_self(void)
{
    return pthread_self();
}

static inline void
csilk_thread_yield(void)
{
    sched_yield();
}

#ifdef __linux__
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
static inline int
csilk_thread_setaffinity(csilk_thread_t* tid, char* cpuset, char* oldmask, int maxcpu)
{
    (void)oldmask;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < maxcpu; i++) {
        if (cpuset[i]) {
            CPU_SET(i, &set);
        }
    }
    return pthread_setaffinity_np(*tid, sizeof(set), &set);
}
#else
static inline int
csilk_thread_setaffinity(csilk_thread_t* tid, char* cpuset, char* oldmask, int maxcpu)
{
    (void)tid;
    (void)cpuset;
    (void)oldmask;
    (void)maxcpu;
    return 0;
}
#endif

/**
 * @brief Thread barrier type (pthread_barrier_t under the uring backend).
 *
 * @warning Barrier instances shared across threads MUST be heap-allocated (via calloc/malloc)
 *          and freed after csilk_barrier_wait(). Never declare csilk_barrier_t on the stack
 *          when shared across threads to avoid use-after-free conditions.
 */
typedef pthread_barrier_t csilk_barrier_t;

/**
 * @brief Initialize a thread barrier.
 *
 * @param[in,out] barrier Barrier to initialize (must be heap-allocated).
 * @param[in]     count   Number of threads that must call csilk_barrier_wait() before synchronization completes.
 * @return 0 on success, or a pthread error number on failure.
 */
static inline int
csilk_barrier_init(csilk_barrier_t* barrier, unsigned int count)
{
    return pthread_barrier_init(barrier, NULL, count);
}

/**
 * @brief Synchronize participating threads at the barrier.
 *
 * Blocks calling threads until @p count threads have called csilk_barrier_wait().
 *
 * @param[in,out] barrier Barrier handle.
 * @return 0 or PTHREAD_BARRIER_SERIAL_THREAD for the serial thread, or an error number.
 */
static inline int
csilk_barrier_wait(csilk_barrier_t* barrier)
{
    return pthread_barrier_wait(barrier);
}

/**
 * @brief Destroy a thread barrier.
 *
 * @param[in,out] barrier Barrier handle to destroy.
 */
static inline void
csilk_barrier_destroy(csilk_barrier_t* barrier)
{
    pthread_barrier_destroy(barrier);
}
#else
#include <csilk/core/sys_io.h>
/** @brief Mutex type (uv_mutex_t under the libuv backend). */
typedef uv_mutex_t csilk_mutex_t;
/** @brief Thread handle type (uv_thread_t under the libuv backend). */
typedef uv_thread_t csilk_thread_t;
/** @brief One-time initializer control type (uv_once_t). */
typedef uv_once_t csilk_once_t;
/** @brief Initializer for a csilk_once_t. */
#define CSILK_ONCE_INIT UV_ONCE_INIT

/**
 * @brief Initialize a mutex.
 * @param[in,out] m Mutex to initialize.
 * @return 0 on success, or a libuv error code on failure.
 */
static inline int
csilk_mutex_init(csilk_mutex_t* m)
{
    return uv_mutex_init(m);
}
/**
 * @brief Lock a mutex, blocking until acquired.
 * @param[in,out] m Mutex to lock.
 */
static inline void
csilk_mutex_lock(csilk_mutex_t* m)
{
    uv_mutex_lock(m);
}
/**
 * @brief Unlock a previously locked mutex.
 * @param[in,out] m Mutex to unlock.
 */
static inline void
csilk_mutex_unlock(csilk_mutex_t* m)
{
    uv_mutex_unlock(m);
}
/**
 * @brief Destroy a mutex.
 * @param[in,out] m Mutex to destroy.
 */
static inline void
csilk_mutex_destroy(csilk_mutex_t* m)
{
    uv_mutex_destroy(m);
}
/**
 * @brief Run @p cb exactly once, guarded by @p control.
 * @param[in,out] control One-time control object.
 * @param[in] cb Callback invoked exactly once across all callers.
 */
static inline void
csilk_once(csilk_once_t* control, void (*cb)(void))
{
    uv_once(control, cb);
}

/** @brief Condition variable type (uv_cond_t under the libuv backend). */
typedef uv_cond_t csilk_cond_t;
/**
 * @brief Initialize a condition variable.
 * @param[in,out] cond Condition variable to initialize.
 * @return 0 on success, or a libuv error code on failure.
 */
static inline int
csilk_cond_init(csilk_cond_t* cond)
{
    return uv_cond_init(cond);
}
/**
 * @brief Signal one waiter on a condition variable.
 * @param[in,out] cond Condition variable to signal.
 */
static inline void
csilk_cond_signal(csilk_cond_t* cond)
{
    uv_cond_signal(cond);
}
/**
 * @brief Wake all waiters on a condition variable.
 * libuv has no broadcast primitive, so this loops signalling up to 64 times.
 * @param[in,out] cond Condition variable to broadcast.
 */
static inline void
csilk_cond_broadcast(csilk_cond_t* cond)
{
    /* libuv has no broadcast; signal all waiters in a loop */
    for (int i = 0; i < 64; i++) {
        uv_cond_signal(cond);
    }
}
/**
 * @brief Wait on a condition variable, atomically releasing the mutex.
 * @param[in,out] cond Condition variable to wait on.
 * @param[in,out] mutex Mutex currently held by the caller.
 */
static inline void
csilk_cond_wait(csilk_cond_t* cond, csilk_mutex_t* mutex)
{
    uv_cond_wait(cond, mutex);
}
/**
 * @brief Wait on a condition variable with a timeout in nanoseconds.
 * @param[in,out] cond Condition variable to wait on.
 * @param[in,out] mutex Mutex currently held by the caller.
 * @param timeout_ns Timeout duration in nanoseconds.
 * @return 0 on success, or a libuv error code (UV_ETIMEDOUT) on timeout.
 */
static inline int
csilk_cond_timedwait(csilk_cond_t* cond, csilk_mutex_t* mutex, uint64_t timeout_ns)
{
    return uv_cond_timedwait(cond, mutex, timeout_ns);
}
/**
 * @brief Destroy a condition variable.
 * @param[in,out] cond Condition variable to destroy.
 */
static inline void
csilk_cond_destroy(csilk_cond_t* cond)
{
    uv_cond_destroy(cond);
}

typedef uv_thread_cb csilk_thread_cb;

static inline int
csilk_thread_create(csilk_thread_t* tid, csilk_thread_cb cb, void* arg)
{
    return uv_thread_create(tid, cb, arg);
}

static inline int
csilk_thread_join(csilk_thread_t* tid)
{
    return uv_thread_join(tid);
}

static inline csilk_thread_t
csilk_thread_self(void)
{
    return uv_thread_self();
}

static inline void
csilk_thread_yield(void)
{
    sched_yield();
}

static inline int
csilk_thread_setaffinity(csilk_thread_t* tid, char* cpuset, char* oldmask, int maxcpu)
{
    return uv_thread_setaffinity(tid, cpuset, oldmask, maxcpu);
}

/**
 * @brief Thread barrier type (uv_barrier_t under the libuv backend).
 *
 * @warning Barrier instances shared across threads MUST be heap-allocated (via calloc/malloc)
 *          and freed after csilk_barrier_wait(). Never declare csilk_barrier_t on the stack
 *          when shared across threads to avoid use-after-free conditions.
 */
typedef uv_barrier_t csilk_barrier_t;

/**
 * @brief Initialize a thread barrier.
 *
 * @param[in,out] barrier Barrier to initialize (must be heap-allocated).
 * @param[in]     count   Number of threads that must call csilk_barrier_wait() before synchronization completes.
 * @return 0 on success, or a libuv error code on failure.
 */
static inline int
csilk_barrier_init(csilk_barrier_t* barrier, unsigned int count)
{
    return uv_barrier_init(barrier, count);
}

/**
 * @brief Synchronize participating threads at the barrier.
 *
 * Blocks calling threads until @p count threads have called csilk_barrier_wait().
 *
 * @param[in,out] barrier Barrier handle.
 * @return 0 or non-zero for the serial thread, or a negative libuv error code.
 */
static inline int
csilk_barrier_wait(csilk_barrier_t* barrier)
{
    return uv_barrier_wait(barrier);
}

/**
 * @brief Destroy a thread barrier.
 *
 * @param[in,out] barrier Barrier handle to destroy.
 */
static inline void
csilk_barrier_destroy(csilk_barrier_t* barrier)
{
    uv_barrier_destroy(barrier);
}
#endif
