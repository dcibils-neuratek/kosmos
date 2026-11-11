/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef PTHREAD_H
#define PTHREAD_H

/*
 * **POSIX threads, inside the process** (`docs/w3.md` W3a), on Kosmos
 * threads and the futex.
 *
 * For ported code only: this directory, `runtime/posix/include/`, is on
 * the include path of an image that links `runtime/posix/` and of nothing
 * else, so Kosmos's own kits, servers and applications cannot include it
 * by accident (`design.md` §17.3). No signals: `pthread_kill` and
 * `pthread_sigmask` are not here, and a port that calls them has a Kosmos
 * file saying what instead.
 */

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

typedef struct kosmos_pthread *pthread_t;

typedef struct {
    size_t stacksize;           /* 0: the slot's own */
    int    detached;
    void  *stackaddr;           /* the lowest address; `pthread_getattr_np`'s */
} pthread_attr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1

/* The mutex: `kosmos_lock`'s word, with an owner and a count for the
 * recursive and error-checking kinds. */
typedef struct {
    volatile uint32_t word;
    int               type;
    pthread_t         owner;
    unsigned          count;
} pthread_mutex_t;

typedef struct {
    int type;
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_RECURSIVE  1
#define PTHREAD_MUTEX_ERRORCHECK 2
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL

#define PTHREAD_MUTEX_INITIALIZER { 0, PTHREAD_MUTEX_NORMAL, 0, 0 }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { 0, PTHREAD_MUTEX_RECURSIVE, 0, 0 }

/* The condition variable: a sequence word every signal moves, waited on
 * with the value the waiter saw before it let the mutex go. */
typedef struct {
    volatile uint32_t seq;
    clockid_t         clock;
} pthread_cond_t;

typedef struct {
    clockid_t clock;
} pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER { 0, CLOCK_REALTIME }

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  changed;
    unsigned        readers;
    unsigned        writers_waiting;
    int             writing;
} pthread_rwlock_t;

typedef struct {
    int unused;
} pthread_rwlockattr_t;

#define PTHREAD_RWLOCK_INITIALIZER \
    { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0 }

typedef volatile uint32_t pthread_once_t;
#define PTHREAD_ONCE_INIT 0

typedef unsigned pthread_key_t;
#define PTHREAD_KEYS_MAX              128
#define PTHREAD_DESTRUCTOR_ITERATIONS 4

int       pthread_create(pthread_t *out, const pthread_attr_t *attr,
                         void *(*start)(void *), void *arg);
int       pthread_join(pthread_t t, void **result);
int       pthread_detach(pthread_t t);
pthread_t pthread_self(void);
int       pthread_equal(pthread_t a, pthread_t b);
void      pthread_exit(void *result) __attribute__((noreturn));

int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t bytes);
int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *bytes);
int pthread_attr_setdetachstate(pthread_attr_t *a, int state);
int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state);
int pthread_attr_getstack(const pthread_attr_t *a, void **lowest, size_t *bytes);

/* Where a thread's stack is, which JavaScriptCore's collector asks
 * before it scans one. */
int pthread_getattr_np(pthread_t t, pthread_attr_t *out);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *until);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_condattr_init(pthread_condattr_t *a);
int pthread_condattr_destroy(pthread_condattr_t *a);
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock);
int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock);

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a);
int pthread_rwlock_destroy(pthread_rwlock_t *l);
int pthread_rwlock_rdlock(pthread_rwlock_t *l);
int pthread_rwlock_tryrdlock(pthread_rwlock_t *l);
int pthread_rwlock_wrlock(pthread_rwlock_t *l);
int pthread_rwlock_trywrlock(pthread_rwlock_t *l);
int pthread_rwlock_unlock(pthread_rwlock_t *l);

int pthread_once(pthread_once_t *once, void (*init)(void));

int   pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int   pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int   pthread_setspecific(pthread_key_t key, const void *value);

__END_DECLS

#endif /* PTHREAD_H */
