/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A futex: sleep while a word says what you saw** (`docs/w2.md` part 1).
 *
 * `futex_wait` puts the calling thread to sleep while the 32-bit word at an
 * address in its own process still holds `expected`; `futex_wake` wakes up
 * to `count` threads sleeping on that address, oldest first. Every lock and
 * condition variable in a process stands on the pair - a lock is taken by
 * an atomic instruction in user code when nobody holds it, and only a
 * thread that finds it held comes here.
 *
 * **The one rule that makes it correct**: the word is read, and the thread
 * queued, under the same lock `futex_wake` takes. A waker changes the word
 * *before* it calls wake, so either the waiter's read sees the change and
 * it returns at once, or the waiter is already queued when the waker
 * looks. A wake cannot fall between the two.
 *
 * **Where the waiters are.** A fixed table of buckets, found by a hash of
 * (address space, address), each a lock and a list. The waiter *is* the
 * thread - `struct thread`'s `futex` fields are the link - so there is no
 * pool here to size or to run out of, and no allocation at all.
 *
 * **Private to a process.** Two processes cannot meet on a futex, because
 * the key holds the address space. One shared through a region would need
 * (region, offset) as its key; nothing asks for it yet (`w2.md`, *Not in
 * W2*).
 *
 * **The word is read through the page tables, not through the user
 * mapping.** Another thread of the process may unmap the page at any
 * moment, and a kernel load from a user address that has just gone would
 * be a kernel fault. Asking the address space for the page's physical
 * address and reading it through the kernel's map of RAM cannot fault: an
 * address with no page answers `SYS_ERR_FAULT`, and a page freed a moment
 * after it was looked up is still RAM, read harmlessly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "futex.h"
#include "mmu.h"
#include "page.h"
#include "process.h"
#include "spinlock.h"
#include "syscall.h"
#include "thread.h"

#define FUTEX_BUCKETS 256

struct futex_bucket {
    struct spinlock lock;
    struct thread  *head;
    struct thread  *tail;
};

static struct futex_bucket buckets[FUTEX_BUCKETS] = {
    [0 ... FUTEX_BUCKETS - 1] = { .lock = SPINLOCK("futex") },
};

static struct futex_bucket *bucket_for(const struct addrspace *as, uintptr_t va)
{
    uint64_t h = ((uint64_t)(uintptr_t)as >> 4) ^ ((uint64_t)va >> 2);

    h *= 0x9e3779b97f4a7c15ULL;
    return &buckets[h >> 56];
}

/* Takes `t` off its bucket's list; the bucket's lock is held. */
static void unlink(struct futex_bucket *b, struct thread *t)
{
    struct thread **at = &b->head;
    struct thread *prev = NULL;

    while (*at != NULL && *at != t) {
        prev = *at;
        at = &(*at)->futex.next;
    }

    if (*at == NULL) {
        return;
    }

    *at = t->futex.next;

    if (b->tail == t) {
        b->tail = prev;
    }

    t->futex.next = NULL;
    t->futex.bucket = NULL;
}

long futex_wait(struct process *p, uintptr_t va, uint32_t expected,
                unsigned long wait_ticks)
{
    struct thread *me = thread_current();
    struct futex_bucket *b;
    unsigned long flags;
    uintptr_t page;
    uint32_t now;

    if ((va & 3) != 0 || !process_may_write(p, va, sizeof(uint32_t))) {
        return SYS_ERR_FAULT;
    }

    b = bucket_for(p->space, va);
    flags = spin_lock(&b->lock);

    page = as_page_phys(p->space, va & ~(uintptr_t)(PAGE_SIZE - 1));

    if (page == 0) {
        spin_unlock(&b->lock, flags);
        return SYS_ERR_FAULT;
    }

    now = __atomic_load_n((const uint32_t *)phys_to_virt(page + (va & (PAGE_SIZE - 1))),
                          __ATOMIC_ACQUIRE);

    if (now != expected) {
        spin_unlock(&b->lock, flags);
        return SYS_NO_CHANGE;
    }

    me->futex.as = p->space;
    me->futex.va = va;
    me->futex.next = NULL;
    me->futex.bucket = b;
    me->futex.result = 0;

    if (b->tail != NULL) {
        b->tail->futex.next = me;
    } else {
        b->head = me;
    }
    b->tail = me;

    /*
     * The deadline before the block, and safe there: the tick's walk of
     * the sleepers looks only at a thread that is BLOCKED, and this one is
     * not until `thread_block_and_release` says so under its queue's lock -
     * after which a timer that finds it waits on this bucket's lock in
     * `futex_timed_out` until the block has released it.
     */
    me->wake_at = wait_ticks != 0 ? thread_deadline_in(wait_ticks) : 0;

    thread_block_and_release(&b->lock, flags);

    me->wake_at = 0;
    return me->futex.result;
}

long futex_wake(struct process *p, uintptr_t va, unsigned long count)
{
    struct futex_bucket *b;
    struct thread *t, *next, *woken = NULL, **tail = &woken;
    unsigned long flags;
    long n = 0;

    if ((va & 3) != 0 || !process_may_read(p, va, sizeof(uint32_t))) {
        return SYS_ERR_FAULT;
    }

    b = bucket_for(p->space, va);
    flags = spin_lock(&b->lock);

    for (t = b->head; t != NULL && (unsigned long)n < count; t = next) {
        next = t->futex.next;

        if (t->futex.as == p->space && t->futex.va == va) {
            unlink(b, t);
            t->futex.result = 0;
            *tail = t;
            tail = &t->futex.next;
            n++;
        }
    }

    spin_unlock(&b->lock, flags);

    /*
     * Woken outside the bucket's lock, since a wake takes the thread's
     * queue lock. The chain is read before each wake: once woken, a thread
     * may run, wait again and reuse its link.
     */
    for (t = woken; t != NULL; t = next) {
        next = t->futex.next;
        t->futex.next = NULL;
        thread_wake(t);
    }

    return n;
}

/*
 * A waiter whose deadline came, or that is being killed: off its bucket,
 * told it timed out. A thread already taken by a waker is left with the
 * waker's answer - and the waker wakes it. Safe on a thread that is not
 * waiting on a futex at all, which is every caller's other case.
 */
bool futex_timed_out(struct thread *t)
{
    struct futex_bucket *b = __atomic_load_n(&t->futex.bucket, __ATOMIC_ACQUIRE);
    unsigned long flags;
    bool taken = false;

    if (b == NULL) {
        return false;
    }

    flags = spin_lock(&b->lock);

    if (t->futex.bucket == b) {
        unlink(b, t);
        t->futex.result = SYS_TIMED_OUT;
        taken = true;
    }

    spin_unlock(&b->lock, flags);
    return taken;
}

/* How many threads are listed on every bucket together. For the kernel
 * suite, which asks it after a process that left a waiter has ended. */
unsigned futex_listed(void)
{
    unsigned i, n = 0;

    for (i = 0; i < FUTEX_BUCKETS; i++) {
        unsigned long flags = spin_lock(&buckets[i].lock);
        struct thread *t;

        for (t = buckets[i].head; t != NULL; t = t->futex.next) {
            n++;
        }

        spin_unlock(&buckets[i].lock, flags);
    }

    return n;
}
