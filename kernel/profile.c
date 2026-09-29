/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where every processor is, a tick at a time (`SYS_PROFILE`, `roadmap.md`'s
 * App Inspector, step 1).
 *
 * **Thread accounting, which is the kernel's to know**, and nothing more:
 * at each tick a processor notes the process it was running, the address it
 * was interrupted at, and whether that was a program, the kernel or idle.
 * What an address *means* - the Lua VM, a C kit, the libc - is decided in
 * userland, on the Mac, from the build's own symbols; the kernel does not
 * know what a symbol is any more than it knows what a file is.
 *
 * **A ring for each processor**, so a tick writes where only it writes and
 * the profiler drains them all: pages taken when a profile starts and given
 * back when it stops, so a machine that is not being profiled pays one
 * comparison a tick. A ring that fills - a profiler that stopped draining -
 * counts what it could not keep rather than overwriting what it has.
 *
 * **One profile at a time**, and only for a process spawned with
 * `SPAWN_PROFILE`. The one that started it drains and stops it; if it ends
 * without stopping, the next process with the right takes it over.
 */

#include "profile.h"

#include <string.h>

#include "panic.h"
#include "percpu.h"
#include "pmm.h"
#include "process.h"
#include "spinlock.h"
#include "syscall.h"
#include "thread.h"

/* Samples a ring holds: sixteen pages, sixteen seconds at 250 ticks a second. */
#define RING_PAGES   16u
#define RING_SAMPLES (RING_PAGES * 4096u / sizeof(struct profile_sample))

struct ring {
    struct spinlock lock;
    struct profile_sample *s;
    uint32_t head, tail;        /* written at head, read from tail */
    uint64_t lost;
};

static struct ring rings[NR_CPUS];

/* Read at every tick without the lock: a tick that sees it change late
 * records one sample more or one fewer, which is what a tick is worth. */
static volatile bool active;

static struct spinlock owner_lock;
static struct process *owner;
static unsigned owner_id;

void profile_tick(uint64_t pc, bool user)
{
    struct percpu *c;
    struct ring *r;
    struct thread *t;
    unsigned long flags;

    if (!active) {
        return;
    }

    c = this_cpu();
    r = &rings[c->index];
    t = c->current;
    flags = spin_lock(&r->lock);

    if (r->s != NULL) {
        uint32_t next = (r->head + 1) % RING_SAMPLES;

        if (next == r->tail) {
            r->lost++;
        } else {
            struct profile_sample *s = &r->s[r->head];

            s->pc = pc;
            s->cpu = (uint8_t)c->index;

            if (t == NULL || t == c->idle_thread) {
                s->pid = 0;
                s->thread = 0;
                s->where = PROFILE_IDLE;
            } else {
                s->pid = (t->process != NULL) ? t->process->id : 0;
                s->thread = (uint16_t)t->id;
                s->where = user ? PROFILE_USER : PROFILE_KERNEL;
            }

            r->head = next;
        }
    }

    spin_unlock(&r->lock, flags);
}

static bool owner_alive(void)
{
    return owner != NULL && owner->in_use && owner->id == owner_id;
}

static void rings_free(void)
{
    for (unsigned i = 0; i < NR_CPUS; i++) {
        unsigned long flags = spin_lock(&rings[i].lock);
        struct profile_sample *s = rings[i].s;

        rings[i].s = NULL;
        rings[i].head = rings[i].tail = 0;
        spin_unlock(&rings[i].lock, flags);

        if (s != NULL) {
            for (unsigned page = 0; page < RING_PAGES; page++) {
                pmm_free_page((uint8_t *)s + page * 4096u);
            }
        }
    }
}

static long start(struct process *p)
{
    unsigned long flags = spin_lock(&owner_lock);

    if (active && owner_alive() && owner != p) {
        spin_unlock(&owner_lock, flags);
        return SYS_ERR_BUSY;
    }

    owner = p;
    owner_id = p->id;
    spin_unlock(&owner_lock, flags);

    active = false;
    rings_free();

    for (unsigned i = 0; i < NR_CPUS; i++) {
        void *pages = pmm_alloc_contiguous(RING_PAGES);

        if (pages == NULL) {
            rings_free();
            return SYS_ERR_NO_ROOM;
        }

        memset(pages, 0, RING_PAGES * 4096u);
        flags = spin_lock(&rings[i].lock);
        rings[i].s = pages;
        rings[i].head = rings[i].tail = 0;
        rings[i].lost = 0;
        spin_unlock(&rings[i].lock, flags);
    }

    active = true;
    return 0;
}

/* Up to `max` samples, oldest first on each processor, into the caller's. */
static long drain(struct process *p, uintptr_t buf, size_t max)
{
    struct profile_sample *out = (struct profile_sample *)buf;
    size_t n = 0;

    if (max > 0 && !process_may_write(p, buf, max * sizeof(struct profile_sample))) {
        return SYS_ERR_FAULT;
    }

    for (unsigned i = 0; i < NR_CPUS && n < max; i++) {
        struct ring *r = &rings[i];

        /* A sample at a time under the lock: a tick is never held off for a
         * copy longer than one. */
        for (;;) {
            unsigned long flags = spin_lock(&r->lock);
            struct profile_sample s;

            if (r->s == NULL || r->tail == r->head || n == max) {
                spin_unlock(&r->lock, flags);
                break;
            }

            s = r->s[r->tail];
            r->tail = (r->tail + 1) % RING_SAMPLES;
            spin_unlock(&r->lock, flags);

            out[n++] = s;
        }
    }

    return (long)n;
}

long profile_call(struct process *p, unsigned long op, uintptr_t buf, size_t max)
{
    if (p == NULL || !p->owns_profile) {
        return SYS_ERR_DENIED;
    }

    if (op == PROFILE_START) {
        return start(p);
    }

    if (owner != p || !owner_alive()) {
        return SYS_ERR_DENIED;
    }

    if (op == PROFILE_READ) {
        return drain(p, buf, max);
    }

    if (op == PROFILE_LOST) {
        uint64_t lost = 0;

        for (unsigned i = 0; i < NR_CPUS; i++) {
            lost += rings[i].lost;
        }

        return (long)lost;
    }

    if (op == PROFILE_STOP) {
        active = false;
        rings_free();
        owner = NULL;
        return 0;
    }

    return SYS_ERR_BADCALL;
}
