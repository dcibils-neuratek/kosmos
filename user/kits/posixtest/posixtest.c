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
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The one Kosmos call here: how many threads the kernel has handed out,
 * to see that detached threads are collected. */
long kosmos_posixtest_fresh_index(void);

#define WORKERS     8
#define DETACHED    1000
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

/*
 * **Memory** (`docs/w3.md` W3b): what bmalloc and JavaScriptCore do with
 * it - a large range reserved with no access, pages in it given access as
 * they are used, given back with their access kept, and pieces unmapped.
 */
#define GB4 (4UL * 1024 * 1024 * 1024)

static int memory_checks(char *out, unsigned long size)
{
    unsigned char *big, *page, *small, *f;
    long ps = sysconf(_SC_PAGESIZE);
    unsigned long i;

    /* 11: the page size, and 4 GB with no access - addresses, not pages */
    if (ps != 4096 || getpagesize() != 4096) {
        SAY("posix: the page size is %ld", ps);
    }

    big = mmap(NULL, GB4, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if (big == MAP_FAILED) {
        SAY("posix: 4 GB with no access was refused (%d)", errno);
    }

    /* 12: three pages in the middle made writable, and used */
    page = big + GB4 / 2;

    if (mprotect(page, 3 * 4096, PROT_READ | PROT_WRITE) != 0) {
        SAY("posix: mprotect to read and write failed (%d)", errno);
    }

    for (i = 0; i < 3 * 4096; i++) {
        if (page[i] != 0) {
            SAY("posix: a page made writable did not read zero");
        }

        page[i] = (unsigned char)(i * 7);
    }

    /* 13: read only, and still what was written */
    if (mprotect(page, 4096, PROT_READ) != 0 || page[1] != 7 || page[4095] != (unsigned char)(4095 * 7)) {
        SAY("posix: mprotect to read only lost what the page held (%d)", errno);
    }

    /* 14: MADV_DONTNEED - the second page reads zero, and is writable still */
    if (madvise(page + 4096, 4096, MADV_DONTNEED) != 0) {
        SAY("posix: madvise(MADV_DONTNEED) failed (%d)", errno);
    }

    if (page[4096] != 0 || page[8191] != 0) {
        SAY("posix: a page given back with MADV_DONTNEED did not read zero");
    }

    page[4096] = 0x42;

    if (page[4096] != 0x42 || page[2 * 4096 + 1] != (unsigned char)((2 * 4096 + 1) * 7)) {
        SAY("posix: after MADV_DONTNEED the pages beside it were not as they were");
    }

    /* 15: the middle page unmapped, and the two around it still there */
    if (munmap(page + 4096, 4096) != 0) {
        SAY("posix: munmap of a middle page failed (%d)", errno);
    }

    if (page[1] != 7 || page[2 * 4096 + 1] != (unsigned char)((2 * 4096 + 1) * 7)) {
        SAY("posix: the pages around an unmapped one lost what they held");
    }

    page[2 * 4096] = 9;

    /* 16: the refusals - writable and executable, a file, the unmapped
     * middle protected, an address that was never this process's */
    if (mmap(NULL, 4096, PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) != MAP_FAILED
        || errno != EACCES) {
        SAY("posix: writable and executable at once was not refused with EACCES");
    }

    if (mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, 3, 0) != MAP_FAILED || errno != ENODEV) {
        SAY("posix: a file's mapping was not refused with ENODEV");
    }

    if (mprotect(page, 3 * 4096, PROT_READ) == 0 || errno != ENOMEM) {
        SAY("posix: mprotect across an unmapped page was not refused with ENOMEM");
    }

    if (munmap((void *)(uintptr_t)0x1000, 4096) == 0 || errno != EINVAL) {
        SAY("posix: munmap of an address never mapped was not refused with EINVAL");
    }

    /* 17: MAP_FIXED over what the process has - zeroed, and the new access */
    f = mmap(page + 2 * 4096, 4096, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (f != page + 2 * 4096 || f[0] != 0) {
        SAY("posix: MAP_FIXED over a page of its own did not give it new and zeroed");
    }

    f[0] = 1;

    /* 18: an ordinary mapping, zeroed and writable throughout */
    small = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (small == MAP_FAILED) {
        SAY("posix: 64 KB read and write was refused (%d)", errno);
    }

    for (i = 0; i < 65536; i += 512) {
        if (small[i] != 0) {
            SAY("posix: a new mapping did not read zero");
        }

        small[i] = 1;
    }

    if (munmap(small, 65536) != 0 || munmap(big, GB4) != 0) {
        SAY("posix: munmap of a whole mapping failed (%d)", errno);
    }

    snprintf(out, size, "memory: 4 GB reserved, protected, discarded, unmapped "
             "in the middle, fixed, refused");
    return 0;
}

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

    /* 8: detached threads collected - their indices come back. A
     * thousand, each ending at once, and then the heap used: a record
     * written after its thread was collected corrupts malloc's links, and
     * the churn is where that shows (`testing.md` 18.769). */
    for (i = 0; i < DETACHED; i++) {
        pthread_attr_t a;
        pthread_t d;

        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

        /* EAGAIN while ended threads wait for the reaper is the kernel's
         * ceiling, and a refusal a port must expect; it has to pass once
         * they are collected. On x86 under QEMU, whose processors take
         * turns, nine hundred were ahead of it. */
        {
            int r, tries = 0;

            while ((r = pthread_create(&d, &a, ends_at_once, NULL)) == EAGAIN && tries < 200) {
                tries++;
                pause_ms(10);
            }

            if (r != 0) {
                SAY("posix: detached thread %d not made (%d, after %d waits)", i, r, tries);
            }
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

    {
        void *held[64];
        int j;

        for (i = 0; i < 64; i++) {
            for (j = 0; j < 64; j++) {
                held[j] = malloc(32 + (size_t)((i * 64 + j) % 7) * 48);

                if (held[j] == NULL) {
                    SAY("posix: the heap refused after the detached threads");
                }

                memset(held[j], j, 32);
            }

            for (j = 63; j >= 0; j--) {
                free(held[j]);
            }
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

    {
        char memory[160];

        if (memory_checks(memory, sizeof memory) != 0) {
            snprintf(out, size, "%s", memory);
            return 1;
        }

        snprintf(out, size, "posix: counted %ld, ring %d, timed out, once %d, keys %d, "
                 "recursive, stacks, reaped, rwlock %s, joined %ld; %s",
                 counted, ring_passes, once_ran, destroyed,
                 most_inside > 1 ? "shared" : "one at a time", (long)(intptr_t)got,
                 memory);
    }

    return 0;
}
