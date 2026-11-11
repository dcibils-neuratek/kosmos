/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The POSIX layer's own checks** (`docs/w3.md`), in an image of their own,
 * `posixtest.elf`, as `cxxtest.elf` holds C++'s. Written the way a port
 * would write them - `<pthread.h>`, nothing of Kosmos's - so what is held
 * is what WebKit and curl will see.
 *
 * `kosmos_posixtest_run` writes one sentence: every check's result in
 * order when all of them held, or the first that did not and why.
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The one Kosmos call here: how many threads the kernel has handed out,
 * to see that detached threads are collected. */
long kosmos_posixtest_fresh_index(void);

#define WORKERS     8
#define COUNT_EACH  20000

static pthread_mutex_t counted_lock = PTHREAD_MUTEX_INITIALIZER;
static long counted;

static void *counts(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < COUNT_EACH; i++) {
        pthread_mutex_lock(&counted_lock);
        counted++;
        pthread_mutex_unlock(&counted_lock);
    }

    return NULL;
}

/* A token handed round a ring of threads under one mutex and condition. */
#define RING       4
#define RING_PASSES 1000

static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_turn = PTHREAD_COND_INITIALIZER;
static int ring_holder;
static int ring_passes;

static void *passes(void *arg)
{
    int me = (int)(intptr_t)arg;

    pthread_mutex_lock(&ring_lock);

    for (;;) {
        while (ring_holder != me && ring_passes < RING_PASSES) {
            pthread_cond_wait(&ring_turn, &ring_lock);
        }

        if (ring_passes >= RING_PASSES) {
            break;
        }

        ring_passes++;
        ring_holder = (me + 1) % RING;
        pthread_cond_broadcast(&ring_turn);
    }

    pthread_mutex_unlock(&ring_lock);
    return NULL;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static volatile int once_ran;

static void run_once(void)
{
    once_ran++;
    sched_yield();                  /* the others arrive while it runs */
}

static void *calls_once(void *arg)
{
    (void)arg;
    pthread_once(&once, run_once);
    return NULL;
}

static pthread_key_t key;
static volatile int destroyed;

static void destroy(void *value)
{
    if (value == (void *)&destroyed) {
        __atomic_add_fetch(&destroyed, 1, __ATOMIC_RELAXED);
    }
}

static void *sets_a_key(void *arg)
{
    (void)arg;
    pthread_setspecific(key, (void *)&destroyed);
    return pthread_getspecific(key) == &destroyed ? (void *)1 : NULL;
}

static pthread_mutex_t recursive;

static void *tries(void *arg)
{
    (void)arg;
    return pthread_mutex_trylock(&recursive) == EBUSY ? (void *)1 : NULL;
}

/* Whether this thread's own local is inside the stack it is told it has. */
static int my_stack_holds_me(void)
{
    pthread_attr_t a;
    void *low;
    size_t size;
    char here;

    if (pthread_getattr_np(pthread_self(), &a) != 0
        || pthread_attr_getstack(&a, &low, &size) != 0) {
        return 0;
    }

    return (uintptr_t)&here >= (uintptr_t)low && (uintptr_t)&here < (uintptr_t)low + size;
}

static void *checks_stack(void *arg)
{
    (void)arg;
    return my_stack_holds_me() ? (void *)1 : NULL;
}

static void *ends_at_once(void *arg)
{
    return arg;
}

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static volatile int inside, most_inside, writer_alone = 1, rw_done;

static void *reads(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < 200; i++) {
        int n;

        pthread_rwlock_rdlock(&rw);
        n = __atomic_add_fetch(&inside, 1, __ATOMIC_ACQ_REL);

        if (n > most_inside) {
            most_inside = n;
        }

        sched_yield();
        __atomic_sub_fetch(&inside, 1, __ATOMIC_ACQ_REL);
        pthread_rwlock_unlock(&rw);
    }

    __atomic_add_fetch(&rw_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void *writes(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < 100; i++) {
        pthread_rwlock_wrlock(&rw);

        if (__atomic_load_n(&inside, __ATOMIC_ACQUIRE) != 0) {
            writer_alone = 0;
        }

        sched_yield();
        pthread_rwlock_unlock(&rw);
    }

    __atomic_add_fetch(&rw_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* A pause, on a timed wait nobody signals: `nanosleep` is W3c's. */
static void pause_ms(int ms)
{
    pthread_condattr_t ca;
    pthread_cond_t c;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    struct timespec until;

    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&c, &ca);
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += (long)ms * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&m);
    (void)pthread_cond_timedwait(&c, &m, &until);
    pthread_mutex_unlock(&m);
}

#define SAY(...) do { snprintf(out, size, __VA_ARGS__); return 1; } while (0)

int kosmos_posixtest_run(char *out, unsigned long size)
{
    pthread_t t[WORKERS];
    void *got;
    int i, ok;

    /* 1: eight threads counting under one mutex */
    for (i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], NULL, counts, NULL) != 0) {
            SAY("posix: create failed at %d", i);
        }
    }

    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], NULL);
    }

    if (counted != (long)WORKERS * COUNT_EACH) {
        SAY("posix: counted %ld of %ld", counted, (long)WORKERS * COUNT_EACH);
    }

    /* 2: a token round a ring of four, a thousand times */
    for (i = 0; i < RING; i++) {
        pthread_create(&t[i], NULL, passes, (void *)(intptr_t)i);
    }

    /* Watched rather than joined at once: a wake lost leaves the ring
     * asleep for good, and that is to be a sentence, not a hang. */
    {
        int seen = -1, still = 0;

        while (still < 100) {               /* five seconds without a pass */
            int now;

            pthread_mutex_lock(&ring_lock);
            now = ring_passes;
            pthread_mutex_unlock(&ring_lock);

            if (now >= RING_PASSES) {
                break;
            }

            still = now == seen ? still + 1 : 0;
            seen = now;
            pause_ms(50);
        }

        if (still >= 100) {
            SAY("posix: the ring stopped at %d of %d passes - a wake was lost",
                seen, RING_PASSES);
        }
    }

    for (i = 0; i < RING; i++) {
        pthread_join(t[i], NULL);
    }

    if (ring_passes != RING_PASSES) {
        SAY("posix: the ring passed %d of %d", ring_passes, RING_PASSES);
    }

    /* 3: a timed wait nobody signals, on the monotonic clock */
    {
        pthread_condattr_t ca;
        pthread_cond_t c;
        pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        struct timespec start, until, end;
        long long waited;
        int r;

        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&c, &ca);
        clock_gettime(CLOCK_MONOTONIC, &start);
        until = start;
        until.tv_nsec += 30000000;          /* 30 ms */

        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }

        pthread_mutex_lock(&m);
        r = pthread_cond_timedwait(&c, &m, &until);
        pthread_mutex_unlock(&m);
        clock_gettime(CLOCK_MONOTONIC, &end);
        waited = (long long)(end.tv_sec - start.tv_sec) * 1000000000LL
                 + (end.tv_nsec - start.tv_nsec);

        if (r != ETIMEDOUT || waited < 30000000LL) {
            SAY("posix: the timed wait said %d after %lld us", r, waited / 1000);
        }
    }

    /* 4: once, under eight at once */
    for (i = 0; i < WORKERS; i++) {
        pthread_create(&t[i], NULL, calls_once, NULL);
    }

    for (i = 0; i < WORKERS; i++) {
        pthread_join(t[i], NULL);
    }

    if (once_ran != 1) {
        SAY("posix: once ran %d times", once_ran);
    }

    /* 5: a key's destructor, once for each thread that set it */
    pthread_key_create(&key, destroy);

    for (i = 0, ok = 0; i < 4; i++) {
        pthread_create(&t[i], NULL, sets_a_key, NULL);
    }

    for (i = 0; i < 4; i++) {
        pthread_join(t[i], &got);
        ok += got != NULL;
    }

    if (ok != 4 || destroyed != 4 || pthread_getspecific(key) != NULL) {
        SAY("posix: keys held %d, destroyed %d", ok, destroyed);
    }

    /* 6: a recursive mutex, held twice and refused to another */
    {
        pthread_mutexattr_t ma;

        pthread_mutexattr_init(&ma);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&recursive, &ma);
        pthread_mutex_lock(&recursive);
        pthread_mutex_lock(&recursive);
        pthread_create(&t[0], NULL, tries, NULL);
        pthread_join(t[0], &got);
        pthread_mutex_unlock(&recursive);
        ok = got != NULL && pthread_mutex_trylock(&recursive) == 0;
        pthread_mutex_unlock(&recursive);
        pthread_mutex_unlock(&recursive);

        if (!ok || pthread_mutex_unlock(&recursive) != EPERM) {
            SAY("posix: the recursive mutex did not hold");
        }
    }

    /* 7: every thread's stack where it is said to be */
    pthread_create(&t[0], NULL, checks_stack, NULL);
    pthread_join(t[0], &got);

    if (!my_stack_holds_me() || got == NULL) {
        SAY("posix: a stack is not where it is said to be (first %d, another %d)",
            my_stack_holds_me(), got != NULL);
    }

    /* 8: detached threads collected - their indices come back */
    for (i = 0; i < 200; i++) {
        pthread_attr_t a;
        pthread_t d;

        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

        if (pthread_create(&d, &a, ends_at_once, NULL) != 0) {
            SAY("posix: detached thread %d not made", i);
        }
    }

    {
        long fresh = -1;

        for (i = 0; i < 20; i++) {
            fresh = kosmos_posixtest_fresh_index();

            if (fresh >= 0 && fresh < 16) {
                break;
            }

            pause_ms(50);
        }

        if (fresh < 0 || fresh >= 16) {
            SAY("posix: detached threads not collected (a new thread is %ld)", fresh);
        }
    }

    /* 9: readers together, a writer alone */
    for (i = 0; i < 3; i++) {
        pthread_create(&t[i], NULL, reads, NULL);
    }

    pthread_create(&t[3], NULL, writes, NULL);

    /* Watched as the ring is: the rwlock sleeps on a condition too. */
    for (i = 0; i < 400 && __atomic_load_n(&rw_done, __ATOMIC_ACQUIRE) < 4; i++) {
        pause_ms(50);
    }

    if (__atomic_load_n(&rw_done, __ATOMIC_ACQUIRE) < 4) {
        SAY("posix: the rwlock's threads stopped, %d of 4 done - a wake was lost",
            __atomic_load_n(&rw_done, __ATOMIC_ACQUIRE));
    }

    for (i = 0; i < 4; i++) {
        pthread_join(t[i], NULL);
    }

    if (!writer_alone) {
        SAY("posix: a writer was not alone");
    }

    /* 10: what a thread ends with, given to its joiner */
    pthread_create(&t[0], NULL, ends_at_once, (void *)(intptr_t)42);
    pthread_join(t[0], &got);

    if ((intptr_t)got != 42) {
        SAY("posix: joined %ld", (long)(intptr_t)got);
    }

    snprintf(out, size, "posix: counted %ld, ring %d, timed out, once %d, keys %d, "
             "recursive, stacks, reaped, rwlock %s, joined %ld",
             counted, ring_passes, once_ran, destroyed,
             most_inside > 1 ? "shared" : "one at a time", (long)(intptr_t)got);
    return 0;
}
