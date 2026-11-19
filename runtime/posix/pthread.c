/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **POSIX threads on Kosmos threads and the futex** (`docs/w3.md` W3a).
 *
 * A `pthread_t` is a `struct kosmos_pthread` on the heap: the Kosmos
 * thread's index, what it was started with, what it ended with, its keys'
 * values and its thread-variable copy. A thread finds its own through a
 * `_Thread_local`, set first thing by the trampoline every thread starts in.
 *
 * **Joining is the kernel's wait** (`SYS_THREAD_WAIT`), which also gives the
 * thread's index back. **A detached thread** has nobody to wait for it, and
 * a thread the kernel has not collected keeps its index - so the first
 * detach in a process starts a reaper, a Kosmos thread of the library's own,
 * which waits for each detached thread as it ends and frees what it had.
 * Whether a thread is joinable, detached or ended is one word, exchanged, so
 * a detach racing the end has exactly one side that collects.
 *
 * **Mutexes are `kosmos_lock`** - the same three states and the same word -
 * with an owner and a count beside it for the recursive and
 * error-checking kinds; `trylock` and `timedlock` are written here against
 * that protocol. **A condition variable** is a sequence word every signal
 * moves: a waiter reads it, lets the mutex go and sleeps while it still
 * holds what was read, so a signal between the two is never lost.
 *
 * Deadlines arrive as a `struct timespec` on a clock and leave as
 * scheduler ticks, the futex's unit - the conversion is `ticks_until`, and
 * it is the only one (`CLAUDE.md`, *Two clocks*).
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kosmos.h"
#include "posix.h"
#include "tls.h"

#define JOINABLE 0u
#define DETACHED 1u
#define ENDED    2u

struct kosmos_pthread {
    unsigned long index;            /* the Kosmos thread's, by itself */
    unsigned long index_given;      /* the same, as its creator was told */
    void       *(*start)(void *);
    void         *arg;
    void         *result;
    volatile uint32_t state;        /* JOINABLE, DETACHED or ENDED */
    void         *tls_made;         /* `kosmos_tls_begin`'s, given back at the end */
    uintptr_t     stack_low;
    size_t        stack_bytes;
    struct {
        uint32_t    seq;            /* the key's `seq` when this was set */
        const void *value;
    } values[PTHREAD_KEYS_MAX];
    struct kosmos_pthread *next_reaped;
    struct thread_destructor *thread_destructors;  /* newest first */
};

/* A `thread_local` object's destructor, as C++ registers it. */
struct thread_destructor {
    void (*destructor)(void *);
    void *object;
    struct thread_destructor *next;
};

static _Thread_local struct kosmos_pthread *self_;

/* The first thread's, made by the constructor below rather than on the
 * heap: `pthread_self` on it works before anything has been allocated. */
static struct kosmos_pthread first_thread;

__attribute__((constructor)) static void adopt_first_thread(void)
{
    first_thread.index = 0;
    first_thread.state = JOINABLE;
    first_thread.stack_low = USER_MAIN_STACK_TOP - USER_MAIN_STACK_PAGES * 4096UL;
    first_thread.stack_bytes = USER_MAIN_STACK_PAGES * 4096UL;
    self_ = &first_thread;
}

/*
 *--------------------------------------------------------------------------
 * Time: a deadline on a clock, as scheduler ticks from now.
 */

/*
 * Ticks until `until` on `clock`, rounded up and never nought - nought is
 * "for ever" to the futex. Nought back means the deadline has passed.
 */
static unsigned long ticks_until(clockid_t clock, const struct timespec *until)
{
    struct timespec now;
    long long ns;

    if (clock_gettime(clock, &now) != 0) {
        return 0;
    }

    ns = (long long)(until->tv_sec - now.tv_sec) * 1000000000LL
         + (until->tv_nsec - now.tv_nsec);

    if (ns <= 0) {
        return 0;
    }

    return posix_ticks_for_ns(ns);
}

static bool bad_deadline(const struct timespec *until)
{
    return until == NULL || until->tv_nsec < 0 || until->tv_nsec >= 1000000000L;
}

/*
 *--------------------------------------------------------------------------
 * Keys.
 */

static struct {
    uint32_t seq;                   /* odd while the key is in use */
    void   (*destructor)(void *);
} keys[PTHREAD_KEYS_MAX];

static kosmos_lock_t keys_lock = KOSMOS_LOCK_INIT;

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    unsigned i;

    kosmos_lock(&keys_lock);

    for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
        if ((keys[i].seq & 1u) == 0) {
            keys[i].seq++;
            keys[i].destructor = destructor;
            kosmos_unlock(&keys_lock);
            *key = i;
            return 0;
        }
    }

    kosmos_unlock(&keys_lock);
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t key)
{
    int r = EINVAL;

    kosmos_lock(&keys_lock);

    if (key < PTHREAD_KEYS_MAX && (keys[key].seq & 1u) != 0) {
        keys[key].seq++;            /* every thread's value for it is stale now */
        keys[key].destructor = NULL;
        r = 0;
    }

    kosmos_unlock(&keys_lock);
    return r;
}

void *pthread_getspecific(pthread_key_t key)
{
    struct kosmos_pthread *me = pthread_self();

    if (key >= PTHREAD_KEYS_MAX || me->values[key].seq != keys[key].seq) {
        return NULL;
    }

    return (void *)me->values[key].value;
}

int pthread_setspecific(pthread_key_t key, const void *value)
{
    struct kosmos_pthread *me = pthread_self();

    if (key >= PTHREAD_KEYS_MAX || (keys[key].seq & 1u) == 0) {
        return EINVAL;
    }

    me->values[key].seq = keys[key].seq;
    me->values[key].value = value;
    return 0;
}

/* Every key's destructor over this thread's values, as POSIX says: again
 * while a destructor set a value, up to the iterations' limit. */
static void run_destructors(struct kosmos_pthread *me)
{
    unsigned round, i;

    for (round = 0; round < PTHREAD_DESTRUCTOR_ITERATIONS; round++) {
        bool any = false;

        for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
            void (*d)(void *) = keys[i].destructor;
            void *v;

            if (d == NULL || me->values[i].seq != keys[i].seq
                || me->values[i].value == NULL) {
                continue;
            }

            v = (void *)me->values[i].value;
            me->values[i].value = NULL;
            d(v);
            any = true;
        }

        if (!any) {
            return;
        }
    }
}

/*
 * **A `thread_local` object's destructor** (`docs/w6.md` W6b): C++
 * registers one through `__cxa_thread_atexit` the first time a thread
 * reaches an object with a non-trivial destructor, and it runs as that
 * thread ends - newest first, the reverse of construction, and before the
 * keys' destructors, as glibc orders them. A destructor that reaches a new
 * `thread_local` registers it, and it runs in its turn.
 *
 * The first thread's are kept and never run, as `__cxa_atexit`'s are
 * (`process.c`): it ends only when the process does, and Nebula takes
 * back everything at once.
 */
int __cxa_thread_atexit(void (*destructor)(void *), void *object, void *image)
{
    struct kosmos_pthread *me = pthread_self();
    struct thread_destructor *d;

    (void)image;

    if (me == &first_thread) {
        return 0;
    }

    d = malloc(sizeof *d);

    if (d == NULL) {
        return -1;
    }

    d->destructor = destructor;
    d->object = object;
    d->next = me->thread_destructors;
    me->thread_destructors = d;
    return 0;
}

static void run_thread_destructors(struct kosmos_pthread *me)
{
    struct thread_destructor *d;

    while ((d = me->thread_destructors) != NULL) {
        me->thread_destructors = d->next;
        d->destructor(d->object);
        free(d);
    }
}

/*
 *--------------------------------------------------------------------------
 * The reaper: collects detached threads as they end.
 */

static kosmos_lock_t reap_lock = KOSMOS_LOCK_INIT;
static struct kosmos_pthread *reap_list;
static volatile uint32_t reap_count;
static bool reaper_started;

static void reaper(unsigned long arg)
{
    (void)arg;

    for (;;) {
        struct kosmos_pthread *t;
        uint32_t seen = __atomic_load_n(&reap_count, __ATOMIC_ACQUIRE);

        kosmos_lock(&reap_lock);
        t = reap_list;

        if (t != NULL) {
            reap_list = t->next_reaped;
        }

        kosmos_unlock(&reap_lock);

        if (t == NULL) {
            (void)kosmos_futex_wait(&reap_count, seen, 0);
            continue;
        }

        (void)kosmos_thread_wait(t->index);
        free(t);
    }
}

/* The reaper, the first time a process detaches anything. */
static int start_reaper(void)
{
    int r = 0;

    kosmos_lock(&reap_lock);

    if (!reaper_started) {
        if (kosmos_thread_start(reaper, 0) < 0) {
            r = EAGAIN;
        } else {
            reaper_started = true;
        }
    }

    kosmos_unlock(&reap_lock);
    return r;
}

static void hand_to_reaper(struct kosmos_pthread *t)
{
    kosmos_lock(&reap_lock);
    t->next_reaped = reap_list;
    reap_list = t;
    kosmos_unlock(&reap_lock);
    __atomic_add_fetch(&reap_count, 1, __ATOMIC_RELEASE);
    (void)kosmos_futex_wake(&reap_count, 1);
}

/*
 *--------------------------------------------------------------------------
 * Threads.
 */

pthread_t pthread_self(void)
{
    return self_ != NULL ? self_ : &first_thread;
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

/* The end of every thread: its keys, its variables, and then whoever
 * collects it. After `kosmos_tls_end` nothing here touches `self_`. */
static void __attribute__((noreturn)) finish(struct kosmos_pthread *me, void *result)
{
    uint32_t was;

    me->result = result;
    run_thread_destructors(me);
    run_destructors(me);
    kosmos_tls_end(me->tls_made);

    was = __atomic_exchange_n(&me->state, ENDED, __ATOMIC_ACQ_REL);

    if (was == DETACHED) {
        hand_to_reaper(me);
    }

    kosmos_thread_exit(0);
    __builtin_unreachable();
}

static void trampoline(unsigned long arg)
{
    struct kosmos_pthread *me = (struct kosmos_pthread *)(uintptr_t)arg;

    /*
     * The kernel's block first - its first word is its own address, at the
     * bottom of this thread's slot, which says the index and the stack -
     * and then this thread's copy of the image's variables, before
     * anything reads one.
     */
    uintptr_t block = (uintptr_t)kosmos_tls();

    me->index = (block - USER_THREAD_SLOTS) / USER_THREAD_SLOT;
    me->stack_low = block + USER_THREAD_STACK_FLOOR;
    me->stack_bytes = USER_THREAD_STACK;
    me->tls_made = kosmos_tls_begin();
    self_ = me;

    finish(me, me->start(me->arg));
}

void pthread_exit(void *result)
{
    struct kosmos_pthread *me = pthread_self();

    if (me == &first_thread) {
        /* No other way out of the first thread than the process's. */
        exit(0);
    }

    finish(me, result);
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg)
{
    struct kosmos_pthread *t;
    long index;

    if (attr != NULL && attr->stacksize > USER_THREAD_STACK) {
        return EINVAL;              /* the slot is all a thread's stack can be */
    }

    if (attr != NULL && attr->detached && start_reaper() != 0) {
        return EAGAIN;
    }

    t = calloc(1, sizeof(*t));

    if (t == NULL) {
        return EAGAIN;
    }

    t->start = start;
    t->arg = arg;
    t->state = (attr != NULL && attr->detached) ? DETACHED : JOINABLE;

    index = kosmos_thread_start(trampoline, (unsigned long)(uintptr_t)t);

    if (index < 0) {
        free(t);
        return EAGAIN;
    }

    /*
     * **A detached thread's record is not touched again from here.** It
     * may have ended and been collected already, its record freed: the
     * first version wrote `index_given` here for every thread, and one of
     * the 200 detached threads that end at once was sometimes gone before
     * the write - into a free chunk, and malloc's next `bin_remove` wrote
     * through what it had overwritten (`testing.md` 18.769). Only a
     * joinable thread is joined by its index, and its record lives until
     * it is.
     */
    if (attr == NULL || !attr->detached) {
        t->index_given = (unsigned long)index;
    }

    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **result)
{
    if (t == NULL || t == pthread_self()) {
        return t == NULL ? ESRCH : EDEADLK;
    }

    if (t == &first_thread || __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == DETACHED) {
        return EINVAL;
    }

    /* The kernel's wait, by the index the creator was told, which is
     * written before the `pthread_t` was handed out. */
    (void)kosmos_thread_wait(t->index_given);

    if (result != NULL) {
        *result = t->result;
    }

    free(t);
    return 0;
}

int pthread_detach(pthread_t t)
{
    uint32_t joinable = JOINABLE;

    if (t == NULL || t == &first_thread) {
        return t == NULL ? ESRCH : EINVAL;
    }

    if (start_reaper() != 0) {
        return EAGAIN;
    }

    if (__atomic_compare_exchange_n(&t->state, &joinable, DETACHED, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return 0;                   /* it hands itself to the reaper as it ends */
    }

    if (joinable == ENDED) {
        t->index = t->index_given;
        hand_to_reaper(t);          /* it ended first: collected for it */
        return 0;
    }

    return EINVAL;                  /* detached already */
}

/*
 *--------------------------------------------------------------------------
 * Attributes.
 */

int pthread_attr_init(pthread_attr_t *a)
{
    a->stacksize = 0;
    a->detached = PTHREAD_CREATE_JOINABLE;
    a->stackaddr = NULL;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *a)
{
    (void)a;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *a, size_t bytes)
{
    if (bytes > USER_THREAD_STACK) {
        return EINVAL;
    }

    a->stacksize = bytes;
    return 0;
}

/*
 * **A thread's stack is its slot.** Each thread is born at the bottom of
 * a slot of its own, and the slot's place is how the thread finds itself
 * (`pthread_self`); a stack the caller allocated somewhere else would be
 * a thread that could not. So a creator may say how much, never where.
 */
int pthread_attr_setstack(pthread_attr_t *a, void *lowest, size_t bytes)
{
    (void)a;
    (void)lowest;
    (void)bytes;
    return ENOTSUP;
}

int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *bytes)
{
    *bytes = a->stacksize != 0 ? a->stacksize : USER_THREAD_STACK;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *a, int state)
{
    if (state != PTHREAD_CREATE_JOINABLE && state != PTHREAD_CREATE_DETACHED) {
        return EINVAL;
    }

    a->detached = state;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *a, int *state)
{
    *state = a->detached;
    return 0;
}

/* A thread's stack as it is: its lowest address and its size, which
 * `pthread_attr_getstack` reads back. */
int pthread_getattr_np(pthread_t t, pthread_attr_t *out)
{
    if (t == NULL) {
        return ESRCH;
    }

    out->stacksize = t->stack_bytes;
    out->stackaddr = (void *)t->stack_low;
    out->detached = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == DETACHED;
    return 0;
}

/*
 * **Stopped, read and let go, by Nebula** (`pthread.h` has why). By the
 * index the thread wrote into its own record before running anything of
 * its caller's (`trampoline`): a collector stops only threads that have
 * registered themselves with it, and so have run. Nebula's refusals - the
 * thread is the caller, or a resume has no suspend to undo - are EDEADLK
 * and EINVAL; a thread that is not there is ESRCH.
 */
_Static_assert(sizeof(pthread_regs_np) == sizeof(struct kosmos_regs),
               "pthread_regs_np is struct kosmos_regs under another name");
#if defined(__x86_64__)
_Static_assert(__builtin_offsetof(pthread_regs_np, rsp) == __builtin_offsetof(struct kosmos_regs, rsp)
               && __builtin_offsetof(pthread_regs_np, rip) == __builtin_offsetof(struct kosmos_regs, rip),
               "pthread_regs_np's fields where the kernel's are");
#else
_Static_assert(__builtin_offsetof(pthread_regs_np, sp) == __builtin_offsetof(struct kosmos_regs, sp)
               && __builtin_offsetof(pthread_regs_np, pc) == __builtin_offsetof(struct kosmos_regs, pc),
               "pthread_regs_np's fields where the kernel's are");
#endif

int pthread_suspend_np(pthread_t t)
{
    if (t == NULL) {
        return ESRCH;
    }
    if (t == pthread_self()) {
        return EDEADLK;
    }
    return kosmos_thread_suspend(t->index) == 0 ? 0 : ESRCH;
}

int pthread_resume_np(pthread_t t)
{
    if (t == NULL || t == pthread_self()) {
        return t == NULL ? ESRCH : EINVAL;
    }
    return kosmos_thread_resume(t->index) == 0 ? 0 : EINVAL;
}

int pthread_getregs_np(pthread_t t, pthread_regs_np *out)
{
    struct kosmos_regs regs;

    if (t == NULL || out == NULL) {
        return t == NULL ? ESRCH : EINVAL;
    }
    if (kosmos_thread_regs(t->index, &regs) != 0) {
        return ESRCH;
    }
    memcpy(out, &regs, sizeof(regs));
    return 0;
}

int pthread_attr_getstack(const pthread_attr_t *a, void **lowest, size_t *bytes)
{
    if (a->stackaddr == NULL) {
        return EINVAL;              /* a creator's attribute: no stack yet */
    }

    *lowest = a->stackaddr;
    *bytes = a->stacksize;
    return 0;
}

int sched_yield(void)
{
    kosmos_yield();
    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Mutexes, on `kosmos_lock`'s word.
 */

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    a->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }

    a->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    *type = a->type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
    m->word = 0;
    m->type = a != NULL ? a->type : PTHREAD_MUTEX_DEFAULT;
    m->owner = NULL;
    m->count = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    return m->word != 0 ? EBUSY : 0;
}

static kosmos_lock_t *word_of(pthread_mutex_t *m)
{
    return (kosmos_lock_t *)(void *)&m->word;
}

/* The owner's side of a lock already held: a recursive mutex counts, an
 * error-checking one refuses. -1: not the owner, go and take it. */
static int held_by_me(pthread_mutex_t *m)
{
    if (m->type == PTHREAD_MUTEX_NORMAL || m->owner != pthread_self()) {
        return -1;
    }

    if (m->type == PTHREAD_MUTEX_RECURSIVE) {
        m->count++;
        return 0;
    }

    return EDEADLK;
}

static void taken(pthread_mutex_t *m)
{
    m->owner = pthread_self();
    m->count = 1;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    int r = held_by_me(m);

    if (r >= 0) {
        return r;
    }

    kosmos_lock(word_of(m));
    taken(m);
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    uint32_t free_word = 0;
    int r = held_by_me(m);

    if (r == EDEADLK) {
        return EBUSY;
    }

    if (r >= 0) {
        return r;
    }

    if (!__atomic_compare_exchange_n(&m->word, &free_word, 1, false,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return EBUSY;
    }

    taken(m);
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *m, const struct timespec *until)
{
    uint32_t c = 0;
    int r = held_by_me(m);

    if (r >= 0) {
        return r;
    }

    if (__atomic_compare_exchange_n(&m->word, &c, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        taken(m);
        return 0;
    }

    if (bad_deadline(until)) {
        return EINVAL;
    }

    /* `kosmos_lock`'s slow path, with the futex's wait bounded. */
    if (c != 2) {
        c = __atomic_exchange_n(&m->word, 2, __ATOMIC_ACQUIRE);
    }

    while (c != 0) {
        unsigned long ticks = ticks_until(CLOCK_REALTIME, until);

        if (ticks == 0) {
            return ETIMEDOUT;
        }

        (void)kosmos_futex_wait(&m->word, 2, ticks);
        c = __atomic_exchange_n(&m->word, 2, __ATOMIC_ACQUIRE);
    }

    taken(m);
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (m->type != PTHREAD_MUTEX_NORMAL) {
        if (m->owner != pthread_self()) {
            return EPERM;
        }

        if (--m->count != 0) {
            return 0;
        }
    }

    m->owner = NULL;
    kosmos_unlock(word_of(m));
    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Condition variables.
 */

int pthread_condattr_init(pthread_condattr_t *a)
{
    a->clock = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t clock)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        return EINVAL;
    }

    a->clock = clock;
    return 0;
}

int pthread_condattr_getclock(const pthread_condattr_t *a, clockid_t *clock)
{
    *clock = a->clock;
    return 0;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{
    c->seq = 0;
    c->clock = a != NULL ? a->clock : CLOCK_REALTIME;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
    (void)c;
    return 0;
}

/*
 * The mutex let go and taken back around one futex wait on the sequence
 * as it was read *before* letting go: a signal after the read moves it,
 * and the wait returns at once rather than sleeping through it. The
 * recursive count is kept across, as POSIX asks.
 */
static int cond_wait(pthread_cond_t *c, pthread_mutex_t *m, unsigned long ticks,
                     bool timed)
{
    uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    unsigned count = m->count;
    long r;

    if (m->type != PTHREAD_MUTEX_NORMAL && m->owner != pthread_self()) {
        return EPERM;
    }

    m->owner = NULL;
    m->count = 0;
    kosmos_unlock(word_of(m));

    r = kosmos_futex_wait(&c->seq, seq, ticks);

    kosmos_lock(word_of(m));
    m->owner = pthread_self();
    m->count = count != 0 ? count : 1;

    return (timed && r == SYS_TIMED_OUT) ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    return cond_wait(c, m, 0, false);
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m,
                           const struct timespec *until)
{
    unsigned long ticks;

    if (bad_deadline(until)) {
        return EINVAL;
    }

    ticks = ticks_until(c->clock, until);

    if (ticks == 0) {
        return ETIMEDOUT;
    }

    return cond_wait(c, m, ticks, true);
}

int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    (void)kosmos_futex_wake(&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_RELEASE);
    (void)kosmos_futex_wake(&c->seq, ~0UL >> 1);
    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Reader-writer locks: a mutex and a condition, writers preferred so a
 * stream of readers cannot hold one off for ever.
 */

int pthread_rwlock_init(pthread_rwlock_t *l, const pthread_rwlockattr_t *a)
{
    (void)a;
    pthread_mutex_init(&l->lock, NULL);
    pthread_cond_init(&l->changed, NULL);
    l->readers = 0;
    l->writers_waiting = 0;
    l->writing = 0;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    return (l->readers != 0 || l->writing) ? EBUSY : 0;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);

    while (l->writing || l->writers_waiting != 0) {
        pthread_cond_wait(&l->changed, &l->lock);
    }

    l->readers++;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    int r = EBUSY;

    pthread_mutex_lock(&l->lock);

    if (!l->writing && l->writers_waiting == 0) {
        l->readers++;
        r = 0;
    }

    pthread_mutex_unlock(&l->lock);
    return r;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);
    l->writers_waiting++;

    while (l->writing || l->readers != 0) {
        pthread_cond_wait(&l->changed, &l->lock);
    }

    l->writers_waiting--;
    l->writing = 1;
    pthread_mutex_unlock(&l->lock);
    return 0;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    int r = EBUSY;

    pthread_mutex_lock(&l->lock);

    if (!l->writing && l->readers == 0) {
        l->writing = 1;
        r = 0;
    }

    pthread_mutex_unlock(&l->lock);
    return r;
}

int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    pthread_mutex_lock(&l->lock);

    if (l->writing) {
        l->writing = 0;
    } else if (l->readers != 0) {
        l->readers--;
    }

    pthread_cond_broadcast(&l->changed);
    pthread_mutex_unlock(&l->lock);
    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Once: 0 not yet, 1 running, 2 done; the losers sleep on the word.
 */

int pthread_once(pthread_once_t *once, void (*init)(void))
{
    uint32_t c = 0;

    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) {
        return 0;
    }

    if (__atomic_compare_exchange_n(once, &c, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        init();
        __atomic_store_n(once, 2, __ATOMIC_RELEASE);
        (void)kosmos_futex_wake(once, ~0UL >> 1);
        return 0;
    }

    while ((c = __atomic_load_n(once, __ATOMIC_ACQUIRE)) != 2) {
        (void)kosmos_futex_wait(once, c, 0);
    }

    return 0;
}

/*
 * **Priorities are Kosmos's bands**, granted by what a program declares
 * (`kosmos: needs audio`) and never asked for through here. So a thread's
 * policy reads as SCHED_OTHER at 0, setting that is accepted since it
 * changes nothing, and anything else is EPERM - POSIX's word for a
 * priority the caller may not have. WebKit's `Thread::changePriority`
 * then leaves the thread where it is.
 */
int pthread_getschedparam(pthread_t t, int *policy, struct sched_param *param)
{
    if (t == NULL || policy == NULL || param == NULL) {
        return EINVAL;
    }

    *policy = SCHED_OTHER;
    param->sched_priority = 0;
    return 0;
}

int pthread_setschedparam(pthread_t t, int policy, const struct sched_param *param)
{
    if (t == NULL || param == NULL) {
        return EINVAL;
    }

    return policy == SCHED_OTHER && param->sched_priority == 0 ? 0 : EPERM;
}

int sched_get_priority_min(int policy)
{
    return policy == SCHED_OTHER || policy == SCHED_FIFO || policy == SCHED_RR
         ? 0 : (errno = EINVAL, -1);
}

int sched_get_priority_max(int policy)
{
    return sched_get_priority_min(policy);
}

/*
 *--------------------------------------------------------------------------
 * Semaphores (`semaphore.h`): a count, taken by compare-and-swap, and a
 * futex wait on it while it is nought. `waiters` is raised before the wait
 * reads the count again, so a post either sees a waiter and wakes it or
 * lands first and the wait does not sleep.
 */

int sem_init(sem_t *s, int pshared, unsigned int value)
{
    if (pshared != 0) {
        errno = ENOSYS;
        return -1;
    }

    if (value > SEM_VALUE_MAX) {
        errno = EINVAL;
        return -1;
    }

    s->count = value;
    s->waiters = 0;
    return 0;
}

int sem_destroy(sem_t *s)
{
    (void)s;
    return 0;
}

int sem_post(sem_t *s)
{
    uint32_t c = __atomic_load_n(&s->count, __ATOMIC_RELAXED);

    do {
        if (c >= SEM_VALUE_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    } while (!__atomic_compare_exchange_n(&s->count, &c, c + 1, true,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));

    if (__atomic_load_n(&s->waiters, __ATOMIC_ACQUIRE) != 0) {
        (void)kosmos_futex_wake(&s->count, 1);
    }

    return 0;
}

/* One taken if there is one: true, or false with the count at nought. */
static bool sem_take(sem_t *s)
{
    uint32_t c = __atomic_load_n(&s->count, __ATOMIC_RELAXED);

    while (c != 0) {
        if (__atomic_compare_exchange_n(&s->count, &c, c - 1, true,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return true;
        }
    }

    return false;
}

/* Until one is taken, or `until` on CLOCK_REALTIME passes (NULL: for
 * ever) - the ticks counted again from the deadline after every wake, so
 * an early one does not stretch it. */
static int sem_wait_until(sem_t *s, const struct timespec *until)
{
    while (!sem_take(s)) {
        unsigned long ticks = 0;
        long r;

        if (until != NULL && (ticks = ticks_until(CLOCK_REALTIME, until)) == 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        __atomic_add_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
        r = kosmos_futex_wait(&s->count, 0, ticks);
        __atomic_sub_fetch(&s->waiters, 1, __ATOMIC_ACQ_REL);
        (void)r;
    }

    return 0;
}

int sem_wait(sem_t *s)
{
    return sem_wait_until(s, NULL);
}

int sem_trywait(sem_t *s)
{
    if (sem_take(s)) {
        return 0;
    }

    errno = EAGAIN;
    return -1;
}

int sem_timedwait(sem_t *s, const struct timespec *until)
{
    if (sem_take(s)) {
        return 0;
    }

    if (bad_deadline(until)) {
        errno = EINVAL;
        return -1;
    }

    return sem_wait_until(s, until);
}

int sem_getvalue(sem_t *s, int *value)
{
    *value = (int)__atomic_load_n(&s->count, __ATOMIC_RELAXED);
    return 0;
}

/*
 *--------------------------------------------------------------------------
 * Barriers: `count` threads, and a round that ends when the last arrives.
 * The round's number is what a waiter watches, so a thread woken for any
 * other reason waits again, and one that comes round to the next use of the
 * barrier before the others have left cannot be mistaken for the last.
 */

int pthread_barrier_init(pthread_barrier_t *b, const pthread_barrierattr_t *a, unsigned count)
{
    (void)a;

    if (count == 0) {
        return EINVAL;
    }

    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->passed, NULL);
    b->count = count;
    b->waiting = 0;
    b->round = 0;
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *b)
{
    return b->waiting != 0 ? EBUSY : 0;
}

int pthread_barrier_wait(pthread_barrier_t *b)
{
    unsigned round;

    pthread_mutex_lock(&b->lock);
    round = b->round;

    if (++b->waiting == b->count) {
        b->waiting = 0;
        b->round++;
        pthread_cond_broadcast(&b->passed);
        pthread_mutex_unlock(&b->lock);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }

    while (b->round == round) {
        pthread_cond_wait(&b->passed, &b->lock);
    }

    pthread_mutex_unlock(&b->lock);
    return 0;
}
