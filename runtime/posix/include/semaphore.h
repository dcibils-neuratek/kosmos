/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef POSIX_SEMAPHORE_H
#define POSIX_SEMAPHORE_H

/*
 * **Unnamed semaphores, inside one process** (`docs/w5.md` W5e, for Skia's
 * thread pool): a count and the futex (`pthread.c`). `sem_init` with
 * `pshared` set is refused - a semaphore between processes would be a
 * word two processes share, and nothing here hands one over - and named
 * semaphores (`sem_open`) do not exist: a global name is what Kosmos has
 * none of.
 */

#include <sys/cdefs.h>
#include <stdint.h>
#include <time.h>

__BEGIN_DECLS

typedef struct {
    volatile uint32_t count;
    volatile uint32_t waiters;
} sem_t;

#define SEM_VALUE_MAX 0x7fffffff

int sem_init(sem_t *s, int pshared, unsigned int value);
int sem_destroy(sem_t *s);
int sem_post(sem_t *s);
int sem_wait(sem_t *s);
int sem_trywait(sem_t *s);
int sem_timedwait(sem_t *s, const struct timespec *until);  /* CLOCK_REALTIME */
int sem_getvalue(sem_t *s, int *value);

__END_DECLS

#endif /* POSIX_SEMAPHORE_H */
