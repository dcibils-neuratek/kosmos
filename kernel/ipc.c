/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ipc.h"
#include "pool.h"
#include "pmm.h"
#include "irq.h"
#include "spinlock.h"
#include "hal.h"
#include "thread.h"
#include "memobj.h"
#include "panic.h"


/*
 * There is no explicit "what is this thread waiting for" field, and that is
 * deliberate. Which of an endpoint's three queues a thread is on already
 * answers it exactly, and a second representation of the same fact is a
 * second thing to keep consistent with the first.
 */

struct endpoint {
    bool in_use;

    /*
     * One lock per endpoint, and that grain is deliberate.
     *
     * **No operation in this file ever touches two endpoints.** `resolve`
     * returns exactly one, and every queue splice below works on that one -
     * so a lock per endpoint has no ordering between two of them to get
     * wrong, and two conversations on two endpoints proceed on two cores
     * without meeting. A single lock for the whole subsystem would have been
     * one line shorter and would have made every message in the machine
     * queue behind every other, which in a microkernel is the machine.
     *
     * What it covers: the three queues, and the `ipc` block inside each
     * thread on them - `msg`, `peer`, `waiting_on`, `status`. Those are one
     * structure spread across several objects, which is what makes IPC the
     * hard subsystem: a single operation moves two threads and an endpoint
     * through states that have to change together or a message is delivered
     * twice, or to a thread that has since died.
     *
     * It nests *outside* the runqueue lock - `deliver` wakes a thread while
     * this is held, and waking takes that thread's queue lock. Never the
     * other way round, which is the whole of the ordering discipline here.
     */
    struct spinlock lock;

    /*
     * A generation number, bumped on every destroy.
     *
     * Slots are reused, so without this a capability outliving its endpoint
     * would quietly start naming whatever was created next in the same slot.
     * That is not a leak, it is a thread reaching something it was never
     * handed, which is the exact property capabilities exist to prevent.
     * With it, a stale capability fails as IPC_ERR_BAD_CAP.
     */
    unsigned generation;

    /*
     * The process that made it, and an endpoint ends with that process.
     *
     * Nothing else could tell. A capability names an endpoint and not
     * whoever receives on it, so a process that died without destroying its
     * own - killed, faulted, or unwound by an error nothing caught - left
     * them in the pool with nobody to answer: a client already waiting for a
     * reply waited for ever, and every capability to them still resolved.
     * `process_exit` destroys them now, which wakes the first and turns the
     * second stale.
     *
     * The maker rather than the receiver, and the difference matters. Every
     * server's endpoint is made by somebody else and handed over - four by
     * the kernel before init exists, the rest by init - so a server that
     * dies does not take its endpoint with it, which is what restarting one
     * on the same endpoint, `design.md` §10's level 2, needs.
     *
     * NULL for one a kernel thread made, as those four are; they end only
     * when something destroys them.
     */
    struct process *owner;

    /*
     * Three queues, and the third is the one that is easy to forget.
     *
     * Senders and receivers waiting to meet are the obvious two. A sender
     * whose message was already taken is waiting on the *receiver* rather
     * than on the endpoint, and would be in neither, so destroying the
     * endpoint would leave it blocked forever. The third list is what makes
     * "destroying an endpoint wakes everything blocked on it" true rather
     * than nearly true.
     *
     * **A thread is on exactly one of these at a time.** All three thread
     * through the same `ipc.next` field, so putting a thread on two of them
     * silently truncates whichever list was linked first. A sender moves
     * from `senders` to `awaiting_reply` when its message is collected; it
     * is never on both.
     */
    struct thread *senders;
    struct thread *receivers;
    struct thread *awaiting_reply;

    /*
     * **One thread that a caller arriving here should wake**, even though it
     * is not receiving.
     *
     * The window manager sleeps inside the console server's input wait and
     * collects its own messages afterwards, without blocking. So a caller on
     * its endpoint - an application's `commit`, its `poll` - used to sit in
     * `senders` until that sleep ran out: 11.5 ms a round trip on an idle
     * desktop, two per frame, which is the Super Nintendo's 43 frames a
     * second on the ThinkPad. `ipc_wait_for_caller` puts the sleeping
     * thread here and `ipc_call` wakes it. One, because one process serves
     * this; a second watcher is refused rather than silently replacing the
     * first.
     */
    struct thread *watcher;
};

/*
 * **The pool, which grows** (`kernel/pool.h`, `threads.md` step 1b): ninety-six
 * at boot, as there were, and an endpoint for every `ENDPOINT_RAM_EACH` of
 * memory at most - an endpoint is seventy-two bytes, so the ceiling costs
 * nothing until it is reached.
 */
#define ENDPOINT_BOOT_SLOTS 96u
#define ENDPOINT_RAM_EACH   (64u * 1024u)

static struct pool endpoints;

static struct endpoint *endpoint_at(unsigned i)
{
    return pool_at(&endpoints, i);
}

unsigned ipc_endpoints_total(void)
{
    return pool_ceiling(&endpoints);
}

/*
 * Single core and cooperative, so none of the queue surgery below can be
 * interrupted by anything that also touches it: the only interrupt handler
 * that exists counts timer ticks. When an interrupt can wake a thread, or
 * when SMP arrives at M6, every function here needs the endpoint locked for
 * the whole operation. Marked rather than left to be discovered.
 */

/*
 * Copies a message, and only as much of it as there is.
 *
 * A round trip moves a message five times: into the sender's slot, across to
 * the receiver, out to the receiver's buffer, back into the sender's slot as
 * a reply, and out to the caller. Copying the whole 512-byte buffer each
 * time rather than the bytes in use made the benchmark thirty-six times
 * slower, from 23.9 ticks to 849.8, which is what a benchmark is for.
 *
 * The length is clamped rather than trusted. It arrives from a process, and
 * the one field an attacker changes first is the one that says how much to
 * copy.
 */
static struct endpoint *resolve(struct thread *t, cap_t index);
static struct endpoint *resolve_as(struct thread *t, cap_t index,
                                   unsigned *generation);
static cap_t install(struct thread *t, struct endpoint *ep, unsigned generation);
static struct memobj *resolve_memory_as(struct thread *t, cap_t index,
                                        unsigned *generation);
static cap_t install_memory(struct thread *t, struct memobj *m,
                            unsigned generation);

static void message_copy(struct message *dst, const struct message *src)
{
    uint32_t n = src->length;

    if (n > MSG_BYTES) {
        n = MSG_BYTES;
    }

    dst->tag = src->tag;
    dst->cap_plus_one = src->cap_plus_one;
    dst->length = n;
    memcpy(dst->data, src->data, n);
}

/*
 * Delivers a message from one thread to another, translating any capability
 * travelling with it.
 *
 * The translation is the whole point and it happens exactly once, here. An
 * index means something only inside the table it came from, so the sender's
 * number is resolved against the sender's table and installed in the
 * receiver's, and what arrives is the receiver's own index for the same
 * endpoint. A sender cannot pass a capability it does not hold, because
 * resolve refuses; and it cannot guess one, because there is no number that
 * names anything it was not given.
 *
 * A transfer that cannot be completed - a bad index, or a full table on the
 * far side - arrives as MSG_NO_CAP rather than failing the send. The message
 * is still a message; the receiver checks whether the capability it expected
 * came with it, exactly as it checks any other field.
 */
static void message_deliver(struct thread *to, struct thread *from,
                            const struct message *src, struct message *dst)
{
    cap_t sending = message_get_cap(src);

    message_copy(dst, src);
    message_set_cap(dst, -1);

    if (sending >= 0) {
        unsigned generation = 0;
        struct endpoint *ep = resolve_as(from, sending, &generation);

        /* With the generation the sender's slot held, not the object's now:
         * see `install`. */
        if (ep != NULL) {
            message_set_cap(dst, install(to, ep, generation));
        } else {
            /*
             * Or a region of memory, which travels exactly the same way and
             * for exactly the same reason: an index means something only
             * inside the table it came from, so it is resolved against the
             * sender's and installed in the receiver's.
             *
             * This is what makes a shared surface shareable without a global
             * name for it. The compositor creates the region and sends the
             * capability; the application receives its own index for the
             * same pages and can map them. Nobody else can name them.
             */
            struct memobj *m = resolve_memory_as(from, sending, &generation);

            if (m != NULL) {
                cap_t got = install_memory(to, m, generation);

                message_set_cap(dst, got >= 0 ? got : -1);
            }
        }
    }
}

static void queue_push(struct thread **head, struct thread *t)
{
    struct thread **p = head;

    /* Appended rather than pushed, so waiting threads are served in the
     * order they arrived. A stack here would starve whoever came first. */
    while (*p != NULL) {
        p = &(*p)->ipc.next;
    }

    t->ipc.next = NULL;
    *p = t;
}

static struct thread *queue_pop(struct thread **head)
{
    struct thread *t = *head;

    if (t == NULL) {
        return NULL;
    }

    *head = t->ipc.next;
    t->ipc.next = NULL;
    return t;
}

static void queue_remove(struct thread **head, struct thread *t)
{
    struct thread **p = head;

    while (*p != NULL) {
        if (*p == t) {
            *p = t->ipc.next;
            t->ipc.next = NULL;
            return;
        }
        p = &(*p)->ipc.next;
    }
}

/*
 * Takes a thread out of whatever it was waiting for.
 *
 * For killing. A thread blocked in IPC is not running, so it cannot notice
 * that it has been killed; waking it without unlinking it would leave a
 * pointer to it in an endpoint's queue, and that queue would later hand out
 * a thread that no longer exists.
 *
 * `waiting_on` records the endpoint in every one of the three blocking
 * paths, and `queue_remove` is safe on a queue that does not contain the
 * thread - so removing from all three is both correct and simpler than
 * remembering which one it is on.
 *
 * Clearing `waiting_on` is what makes a later reply to this thread fail
 * cleanly: `ipc_reply` refuses a sender that is not waiting for anything.
 * That is not a complete answer - a replier holds a raw thread pointer, so
 * once this slot is reused a stale reply could reach the thread that
 * inherits it - and it is the same leak `sys_receive` already records
 * against handing thread pointers to a process. Killing makes it reachable
 * rather
 * than introducing it.
 */
void ipc_abort(struct thread *t)
{
    struct endpoint *ep;

    if (t == NULL) {
        return;
    }

    /*
     * A thread that dies watching an endpoint must not stay its watcher: the
     * next caller would wake a thread slot that belongs to somebody else by
     * then. Either of the two a wait can watch.
     */
    for (unsigned s = 0; s < IPC_WATCH_MAX; s++) {
        struct endpoint *w = t->ipc.watching[s];
        unsigned long wflags;

        if (w == NULL) {
            continue;
        }

        wflags = spin_lock(&w->lock);

        if (w->watcher == t) {
            w->watcher = NULL;
        }

        t->ipc.watching[s] = NULL;
        spin_unlock(&w->lock, wflags);
    }

    ep = t->ipc.waiting_on;

    if (ep == NULL) {
        return;                     /* not blocked on an endpoint */
    }

    {
        unsigned long epflags = spin_lock(&ep->lock);

        /* Re-checked under the lock: `waiting_on` was read without one,
         * and between the read and the lock this thread may already have
         * been taken off by a timeout or a reply. Removing it from three
         * queues it is no longer on is harmless; clearing `waiting_on` and
         * waking it a second time is not. */
        if (t->ipc.waiting_on != ep) {
            spin_unlock(&ep->lock, epflags);
            return;
        }

        queue_remove(&ep->senders, t);
        queue_remove(&ep->receivers, t);
        queue_remove(&ep->awaiting_reply, t);

        t->ipc.waiting_on = NULL;
        t->ipc.status = IPC_ERR_GONE;
        thread_wake(t);

        spin_unlock(&ep->lock, epflags);
    }
}

/*
 * A blocked receiver whose deadline arrived: unlink it, and say nothing came.
 *
 * Called from the timer, not from the thread itself, and that is the whole
 * point. `thread_wake_sleepers` makes the thread *ready*; it does not run
 * until the scheduler gets to it, and in that window it is still sitting on
 * the endpoint's receiver queue. A sender arriving there would be handed a
 * thread that has already given up, so the message would go into a receive
 * that is about to return "nothing arrived" and the sender would wait for a
 * reply nobody is going to send.
 *
 * Unlinking here closes the window instead of narrowing it. `ipc_receive`
 * removes itself too, on the path where it was woken with no message, and
 * `queue_remove` is safe on a queue that does not hold the thread - so the
 * two together are belt and braces rather than a conflict.
 *
 * `status` is left alone: `ipc_receive` set it to `IPC_NO_MESSAGE` before
 * blocking, and that is exactly what this means.
 */
void ipc_timed_out(struct thread *t)
{
    struct endpoint *ep;

    if (t == NULL) {
        return;
    }

    ep = t->ipc.waiting_on;

    if (ep == NULL) {
        return;                     /* a plain sleep, not a receive */
    }

    {
        unsigned long epflags = spin_lock(&ep->lock);

        /* Same re-check as `ipc_abort`, and the same reason. */
        if (t->ipc.waiting_on != ep) {
            spin_unlock(&ep->lock, epflags);
            return;
        }

        queue_remove(&ep->senders, t);
        queue_remove(&ep->receivers, t);
        queue_remove(&ep->awaiting_reply, t);

        t->ipc.waiting_on = NULL;

        spin_unlock(&ep->lock, epflags);
    }
}

/*
 * A new slab's endpoints: zeroed already, which is unused with empty queues,
 * except the lock, whose holder has to say nobody. Their generation starts at
 * zero and only ever goes up.
 */
static void endpoint_fresh(void *object)
{
    struct endpoint *ep = object;

    ep->lock.locked = 0;
    ep->lock.holder = SPIN_NOBODY;
    ep->lock.name   = "endpoint";
}

void ipc_init(void)
{
    pool_init(&endpoints, "endpoints", sizeof(struct endpoint),
              pool_ceiling_for(ENDPOINT_RAM_EACH, ENDPOINT_BOOT_SLOTS),
              ENDPOINT_BOOT_SLOTS, endpoint_fresh);
}

unsigned ipc_endpoints_in_use(void)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < pool_slots(&endpoints); i++) {
        if (endpoint_at(i)->in_use) {
            n++;
        }
    }

    return n;
}

/*
 * The table itself (`ipc.h`): made empty, and counted.
 *
 * The lock is initialised field by field rather than by `SPINLOCK()`, which
 * is an initialiser for a declaration and not for a table that lives inside a
 * process slot or a thread slot and is made again every time one is reused.
 */
void captable_init(struct captable *c)
{
    memset(c->first, 0, sizeof c->first);
    c->chunk = NULL;
    c->chunks = 0;
    c->lock.locked = 0;
    c->lock.holder = SPIN_NOBODY;
    c->lock.name = "caps";
}

unsigned captable_limit(void)
{
    return (unsigned)(CAPS_INLINE + CAPS_CHUNKS_MAX * CAPS_PER_CHUNK);
}

/* Slots this table has now: the ones in it, and its chunks. */
static unsigned table_slots(const struct captable *c)
{
    return (unsigned)(CAPS_INLINE + c->chunks * CAPS_PER_CHUNK);
}

/*
 * Slot `index`, or NULL when this table has no such slot - which is the whole
 * of the bounds check, since a table's size is no longer a constant anybody
 * can compare against. The lock is held by the caller.
 */
static struct cap *cap_slot(struct captable *c, cap_t index)
{
    unsigned n;

    if (index < 0) {
        return NULL;
    }

    if ((unsigned)index < CAPS_INLINE) {
        return &c->first[index];
    }

    n = (unsigned)index - (unsigned)CAPS_INLINE;

    if (n / CAPS_PER_CHUNK >= c->chunks) {
        return NULL;
    }

    return &c->chunk[n / CAPS_PER_CHUNK][n % CAPS_PER_CHUNK];
}

/*
 * One more chunk of slots: the page of chunk pointers as well, if this is the
 * first. The pages are allocated outside the lock, as a pool's slab is, and
 * attached under it; a chunk two cores both made has one of them given back.
 *
 * Against the reserve like any other page a program is given: a process that
 * leaks capabilities meets the machine's wall rather than a number's.
 */
static bool captable_grow(struct captable *c)
{
    struct cap **dir = NULL;
    struct cap *chunk = NULL;
    unsigned long flags;
    bool want_dir;
    bool added = false;

    flags = spin_lock(&c->lock);
    want_dir = (c->chunk == NULL);

    if (c->chunks >= CAPS_CHUNKS_MAX) {
        spin_unlock(&c->lock, flags);
        return false;
    }

    spin_unlock(&c->lock, flags);

    if (!pmm_room_for_user(want_dir ? 2 : 1)) {
        return false;
    }

    if (want_dir) {
        dir = pmm_alloc_page();

        if (dir == NULL) {
            return false;
        }

        memset(dir, 0, PAGE_SIZE);
    }

    chunk = pmm_alloc_page();

    if (chunk == NULL) {
        if (dir != NULL) {
            pmm_free_page(dir);
        }

        return false;
    }

    memset(chunk, 0, PAGE_SIZE);
    flags = spin_lock(&c->lock);

    if (c->chunk == NULL && dir != NULL) {
        c->chunk = dir;
        dir = NULL;
    }

    if (c->chunk != NULL && c->chunks < CAPS_CHUNKS_MAX) {
        c->chunk[c->chunks] = chunk;
        c->chunks++;
        chunk = NULL;
        added = true;
    }

    spin_unlock(&c->lock, flags);

    if (dir != NULL) {
        pmm_free_page(dir);
    }

    if (chunk != NULL) {
        pmm_free_page(chunk);
    }

    return added;
}

unsigned captable_count(struct captable *c)
{
    unsigned long flags = spin_lock(&c->lock);
    unsigned i, n = 0;

    for (i = 0; i < table_slots(c); i++) {
        if (cap_slot(c, (cap_t)i)->kind != CAP_NONE) {
            n++;
        }
    }

    spin_unlock(&c->lock, flags);
    return n;
}

/*
 * The endpoint a capability index names, or NULL if it names nothing. This
 * is the entire access check: a bounds test and a generation match.
 *
 * `generation`, when asked for, is the one the *slot* holds, which is what a
 * capability travelling onward must carry (`install`).
 */
static struct endpoint *resolve_as(struct thread *t, cap_t index,
                                   unsigned *generation)
{
    struct captable *c = t->caps;
    struct endpoint *ep = NULL;
    struct cap *slot;
    unsigned long flags;

    flags = spin_lock(&c->lock);
    slot = cap_slot(c, index);

    /* Empty, or a region of memory, or an interrupt line: not an endpoint. */
    if (slot != NULL && slot->kind == CAP_ENDPOINT) {
        struct endpoint *e = slot->endpoint;

        /* Destroyed and the slot reused since, if the generations differ. */
        if (e != NULL && e->in_use && slot->generation == e->generation) {
            ep = e;

            if (generation != NULL) {
                *generation = slot->generation;
            }
        }
    }

    spin_unlock(&c->lock, flags);
    return ep;
}

static struct endpoint *resolve(struct thread *t, cap_t index)
{
    return resolve_as(t, index, NULL);
}

/* The generation `index` holds in `t`'s table, read under its lock. */
static unsigned slot_generation(struct thread *t, cap_t index)
{
    unsigned long flags = spin_lock(&t->caps->lock);
    struct cap *slot = cap_slot(t->caps, index);
    unsigned g = (slot != NULL) ? slot->generation : 0;

    spin_unlock(&t->caps->lock, flags);
    return g;
}

/*
 * An endpoint into a free slot, **with the generation it is known by** - the
 * one the capability it came from held, not the endpoint's generation now.
 *
 * They are the same number unless the endpoint was destroyed while its
 * capability was on its way: resolved against the sender's table, then torn
 * down by its owner on another core, and its slot perhaps made a new
 * endpoint of somebody else's - all before the install. Reading the
 * endpoint's generation here would give the receiver a capability that
 * matches the stranger. Carrying the sender's gives it one that is stale on
 * arrival, which is the right answer for something that no longer exists.
 */
static cap_t install(struct thread *t, struct endpoint *ep, unsigned generation)
{
    struct captable *c = t->caps;
    unsigned long flags;
    cap_t i;

again:
    flags = spin_lock(&c->lock);

    for (i = 0; (unsigned)i < table_slots(c); i++) {
        struct cap *slot = cap_slot(c, i);

        if (slot->kind == CAP_NONE) {
            slot->kind = CAP_ENDPOINT;
            slot->endpoint = ep;
            slot->memory = NULL;
            slot->irq = NULL;
            slot->generation = generation;
            spin_unlock(&c->lock, flags);
            return i;
        }
    }

    spin_unlock(&c->lock, flags);

    /* Every slot taken: one more chunk and look again, or refuse. */
    if (captable_grow(c)) {
        goto again;
    }

    return IPC_ERR_NO_SPACE;
}

/*
 * The same two operations for a region of memory.
 *
 * Deliberately not one pair of functions with a kind argument: the two
 * kinds have nothing in common except the table they live in, and a shared
 * `resolve` would return something the caller has to test the type of
 * anyway - which is where a capability of one kind gets used as the other.
 */
static struct memobj *resolve_memory_as(struct thread *t, cap_t index,
                                        unsigned *generation)
{
    struct captable *c = t->caps;
    struct memobj *m = NULL;
    struct cap *slot;
    unsigned long flags;

    flags = spin_lock(&c->lock);
    slot = cap_slot(c, index);

    if (slot != NULL && slot->kind == CAP_MEMORY) {
        struct memobj *r = slot->memory;

        /* Freed and the slot reused since, if the generations differ. */
        if (r != NULL && r->in_use && slot->generation == r->generation) {
            m = r;

            if (generation != NULL) {
                *generation = slot->generation;
            }
        }
    }

    spin_unlock(&c->lock, flags);
    return m;
}

struct memobj *ipc_resolve_memory(struct thread *t, cap_t index)
{
    return resolve_memory_as(t, index, NULL);
}

/*
 * An interrupt line, out of and into a capability table.
 *
 * The same shape as the memory pair above and for the same reasons, with one
 * difference worth naming: **there is no reference count.** A line belongs to
 * the process that claimed it and is released when that process ends, so the
 * number of capabilities naming it does not decide its lifetime. If a driver
 * passes one to another process and then dies, the line goes and the other
 * capability stops resolving - which is the right answer, since the device
 * is unowned at that point and nothing is left to quieten it.
 *
 * The generation is what makes that safe rather than a dangling pointer: a
 * released slot counts up, so a capability naming the claim that used to be
 * there fails to match the one that is.
 */
struct irq_line *ipc_resolve_irq(struct thread *t, cap_t index)
{
    struct captable *c = t->caps;
    struct irq_line *line = NULL;
    struct cap *slot;
    unsigned long flags;

    flags = spin_lock(&c->lock);
    slot = cap_slot(c, index);

    if (slot != NULL && slot->kind == CAP_IRQ) {
        struct irq_line *l = slot->irq;

        /* Released and the slot reused since, if the generations differ. */
        if (l != NULL && l->in_use && slot->generation == l->generation) {
            line = l;
        }
    }

    spin_unlock(&c->lock, flags);
    return line;
}

cap_t ipc_install_irq(struct thread *t, struct irq_line *line)
{
    struct captable *c;
    unsigned long flags;
    cap_t i;

    if (t == NULL || line == NULL) {
        return -1;
    }

    c = t->caps;

again:
    flags = spin_lock(&c->lock);

    for (i = 0; (unsigned)i < table_slots(c); i++) {
        struct cap *slot = cap_slot(c, i);

        if (slot->kind == CAP_NONE) {
            slot->kind = CAP_IRQ;
            slot->endpoint = NULL;
            slot->memory = NULL;
            slot->irq = line;
            slot->generation = line->generation;
            spin_unlock(&c->lock, flags);
            return i;
        }
    }

    spin_unlock(&c->lock, flags);

    if (captable_grow(c)) {
        goto again;
    }

    return -1;
}

/*
 * A region into a free slot, with a reference - **taken only if the region
 * is still the one `generation` names** (`memobj_ref_as`).
 *
 * Every install takes a slot and a reference, and there is deliberately
 * no dedupe here.
 *
 * There was, for about an hour: handing back an existing index for a
 * region the thread already held, so that a client resending one buffer
 * in a loop did not spend a slot per call. It returned that index
 * without taking a reference, which is defensible on its own and is
 * wrong in company - `SYS_MEM_CREATE` installs and then unrefs on the
 * stated grounds that `install` took a reference of its own. When the
 * dedupe fired there, that unref took the count to zero and freed the
 * region its caller had just created, whose pages then went back to the
 * allocator while a capability still named them.
 *
 * The symptom was a read that reported writing 1811 bytes into a region
 * that stayed full of zeroes, and only for regions created late - late
 * being when a pool slot had been recycled and a stale capability could
 * match it.
 *
 * `ipc_cap_drop` is what makes the dedupe unnecessary: a server that
 * gives a buffer back does not accumulate them, so there is nothing to
 * deduplicate. An optimisation that trades a correct reference count
 * for a slot is not one.
 */
static cap_t install_memory(struct thread *t, struct memobj *m,
                            unsigned generation)
{
    struct captable *c = t->caps;
    unsigned long flags;
    bool gone = false;
    cap_t i;

again:
    flags = spin_lock(&c->lock);

    for (i = 0; (unsigned)i < table_slots(c); i++) {
        struct cap *slot = cap_slot(c, i);

        if (slot->kind == CAP_NONE) {
            /* The region pool's lock inside this one: the order is table,
             * then regions, and nothing takes them the other way round. */
            if (!memobj_ref_as(m, generation)) {
                gone = true;    /* freed on the way here: nothing to name */
                break;
            }

            slot->kind = CAP_MEMORY;
            slot->endpoint = NULL;
            slot->memory = m;
            slot->irq = NULL;
            slot->generation = generation;
            spin_unlock(&c->lock, flags);
            return i;
        }
    }

    spin_unlock(&c->lock, flags);

    if (gone) {
        return IPC_ERR_BAD_CAP;
    }

    if (captable_grow(c)) {
        goto again;
    }

    return IPC_ERR_NO_SPACE;
}

cap_t ipc_install_memory(struct thread *t, struct memobj *m)
{
    return install_memory(t, m, m->generation);
}

/*
 * A slot emptied, and the region it held - if it held one it still counts
 * in - handed back for the caller to let go of **after** the table's lock is
 * released: letting go of the last reference frees every page of a region,
 * which may be nine hundred of them, and a lock that masks interrupts is not
 * held across that.
 *
 * A stale capability is dropped without touching the object. The slot may
 * name a region that was freed and whose pool entry has since been taken by
 * another one. Unreffing then decrements a stranger, and the stranger's owner
 * watches its pages go back to the allocator while it still holds a
 * capability to them. The generation is exactly the check that tells the two
 * apart, and it is why the field is there. Letting go afterwards is safe for
 * the same reason: this slot's reference is still counted until then, so the
 * region cannot be freed by anybody else in between.
 */
static struct memobj *empty_slot(struct cap *slot)
{
    struct memobj *m = NULL;

    if (slot->kind == CAP_MEMORY
        && slot->memory != NULL
        && slot->generation == slot->memory->generation) {
        m = slot->memory;
    }

    slot->kind = CAP_NONE;
    slot->endpoint = NULL;
    slot->memory = NULL;
    slot->irq = NULL;
    slot->generation = 0;

    return m;
}

/*
 * One capability, released.
 *
 * The other half of receiving one, and it was missing in exactly the way
 * `SYS_ENDPOINT_DESTROY` was missing before it: a thread could be given a
 * region and had no way to give it back, so a server that took a buffer per
 * request ran out of slots and every later request failed. The endpoint
 * pool had this same shape and the same cure.
 *
 * Dropping is not destroying. The region's pages go when the *last*
 * capability to it is dropped, so a server letting go of its own has no
 * effect on the client still holding one - which is the property that makes
 * it safe for a server to drop unconditionally when it is done.
 */
int ipc_cap_drop(struct thread *t, cap_t index)
{
    struct captable *c;
    struct memobj *m;
    unsigned long flags;

    struct cap *slot;

    if (t == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    c = t->caps;
    flags = spin_lock(&c->lock);
    slot = cap_slot(c, index);

    if (slot == NULL || slot->kind == CAP_NONE) {
        spin_unlock(&c->lock, flags);
        return IPC_ERR_BAD_CAP;
    }

    m = empty_slot(slot);
    spin_unlock(&c->lock, flags);

    memobj_unref(m);
    return 0;
}

/*
 * Everything a table holds, released: a process's, when it ends.
 *
 * Only memory needs this. An endpoint capability going stale is harmless -
 * the generation check catches it - but a region's pages are only freed
 * when the last capability to it is dropped, so a process that ends without
 * dropping its own leaks them for the life of the machine.
 *
 * Emptied under the lock and let go of after it, a slot's worth at a time,
 * for the reason `empty_slot` gives. **And the chunks go back**: a table that
 * grew past the slots inside it holds pages, and a process that ends gives
 * every page back.
 */
void ipc_caps_release(struct captable *c)
{
    unsigned long flags;
    struct cap **chunk;
    unsigned chunks, i;

    if (c == NULL) {
        return;
    }

    flags = spin_lock(&c->lock);
    chunks = table_slots(c);
    spin_unlock(&c->lock, flags);

    for (i = 0; i < chunks; i++) {
        struct memobj *m;

        flags = spin_lock(&c->lock);
        m = empty_slot(cap_slot(c, (cap_t)i));
        spin_unlock(&c->lock, flags);
        memobj_unref(m);
    }

    flags = spin_lock(&c->lock);
    chunk = c->chunk;
    chunks = c->chunks;
    c->chunk = NULL;
    c->chunks = 0;
    spin_unlock(&c->lock, flags);

    for (i = 0; i < chunks; i++) {
        pmm_free_page(chunk[i]);
    }

    if (chunk != NULL) {
        pmm_free_page(chunk);
    }
}

/*
 * **The slot is claimed under its own lock**, and it was not until 19
 * September (`threads.md` step 0).
 *
 * It was a test of `in_use` and a store of `true`, with nothing between two
 * cores doing both at once - and `SYS_ENDPOINT_CREATE` is a syscall, so two
 * programs starting on two cores reach it together. Both saw the slot free,
 * both claimed it, and two servers had one endpoint: a message sent to one
 * received by the other. It is the race `alloc_process` and `memobj_create`
 * were each fixed for, a third time.
 *
 * The endpoint's own lock rather than a lock for the pool, because it is the
 * one `teardown` holds when it gives a slot back: claiming and releasing a
 * slot are then ordered by the same lock, and nothing new is needed. A scan
 * takes each lock in turn, briefly; creating an endpoint is rare.
 */
cap_t ipc_endpoint_create(void)
{
    struct thread *self = thread_current();
    unsigned i;

again:
    for (i = 0; i < pool_slots(&endpoints); i++) {
        struct endpoint *ep = endpoint_at(i);
        unsigned long flags = spin_lock(&ep->lock);
        cap_t index;

        if (ep->in_use) {
            spin_unlock(&ep->lock, flags);
            continue;
        }

        ep->in_use = true;
        ep->owner = self->process;
        ep->senders = NULL;
        ep->receivers = NULL;
        ep->watcher = NULL;
        ep->awaiting_reply = NULL;
        spin_unlock(&ep->lock, flags);

        index = install(self, ep, ep->generation);

        if (index < 0) {
            flags = spin_lock(&ep->lock);
            ep->in_use = false;
            ep->owner = NULL;
            spin_unlock(&ep->lock, flags);
        }

        return index;
    }

    /* Every endpoint taken: one more slab and look again, or refuse. */
    if (pool_grow(&endpoints)) {
        goto again;
    }

    return IPC_ERR_NO_SPACE;
}

cap_t ipc_cap_grant(struct thread *to, cap_t from_index)
{
    unsigned generation = 0;
    struct endpoint *ep = resolve_as(thread_current(), from_index, &generation);

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    /* The index the recipient gets is unrelated to the one the granter used.
     * An index is meaningful only inside the table it came from, which is
     * what stops one from being guessed or forged elsewhere. */
    return install(to, ep, generation);
}

/* Wakes a thread out of an IPC wait with a result. */
static void deliver(struct thread *t, int status)
{
    t->ipc.status = status;
    t->ipc.waiting_on = NULL;
    thread_wake(t);
}

/*
 * Everything blocked on an endpoint woken with IPC_ERR_GONE, and the endpoint
 * gone. The caller holds its lock.
 *
 * The trap roadmap.md names for M3. Everything blocked on this endpoint has
 * to be woken with an error, or it waits forever for a server that no longer
 * exists, and a server that cannot be restarted takes the design's whole
 * recovery story with it.
 *
 * The whole teardown under one hold, and it has to be. Emptying three queues
 * and then invalidating the endpoint is one operation: a sender arriving
 * between the last `queue_pop` and `in_use = false` would join a queue nobody
 * will ever drain, and block for ever on a server that no longer exists.
 * That is the exact failure this was written to prevent, one core later.
 */
static void teardown(struct endpoint *ep)
{
    struct thread *t;

    while ((t = queue_pop(&ep->senders)) != NULL) {
        deliver(t, IPC_ERR_GONE);
    }

    while ((t = queue_pop(&ep->receivers)) != NULL) {
        deliver(t, IPC_ERR_GONE);
    }

    while ((t = queue_pop(&ep->awaiting_reply)) != NULL) {
        deliver(t, IPC_ERR_GONE);
    }

    /* A watcher is woken as well: what it was watching is gone, and the
     * deadline it would otherwise sleep to is somebody else's business. */
    if (ep->watcher != NULL) {
        t = ep->watcher;
        ep->watcher = NULL;
        irq_wake_watcher(t);            /* the lines' lock too: see ipc_call */
    }

    /*
     * Bumped before the slot is freed, so every capability naming it is
     * already stale by the time the slot can be handed out again.
     */
    ep->generation++;
    ep->owner = NULL;
    ep->in_use = false;
}

int ipc_endpoint_destroy(cap_t index)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    unsigned long epflags;
    unsigned held;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    epflags = spin_lock(&ep->lock);

    /*
     * Looked at again with the lock held. `resolve` read without it, and
     * since then the process that made this endpoint may have ended on
     * another core and taken it down - and the slot may already be a new
     * endpoint belonging to somebody else, which destroying would end.
     */
    held = slot_generation(self, index);

    if (!ep->in_use || held != ep->generation) {
        spin_unlock(&ep->lock, epflags);
        return IPC_ERR_BAD_CAP;
    }

    teardown(ep);
    spin_unlock(&ep->lock, epflags);

    /*
     * The granter's own capability is cleared; the others go stale on their
     * next use, which is what the generation check is for. Only if the slot
     * still holds *this* capability: another thread of the process may have
     * dropped the index and filled it with something else since the check.
     * An endpoint's slot holds no region, so there is nothing to let go of.
     */
    {
        unsigned long cflags = spin_lock(&self->caps->lock);
        struct cap *slot = cap_slot(self->caps, index);

        if (slot != NULL && slot->kind == CAP_ENDPOINT && slot->endpoint == ep
            && slot->generation == held) {
            (void)empty_slot(slot);
        }

        spin_unlock(&self->caps->lock, cflags);
    }

    return IPC_OK;
}

/*
 * Every endpoint a process made, destroyed as it ends. See `owner`.
 *
 * Read without the lock first, and that is safe for one reason: an
 * endpoint's owner only becomes `p` when `p` creates it, and `p` is the
 * process ending, on this thread, so it is creating nothing. Whatever else
 * the unlocked read sees is not `p` and stays that way, so the lock is taken
 * only for the endpoints that are.
 */
void ipc_endpoints_release(struct process *p)
{
    unsigned i;

    if (p == NULL) {
        return;
    }

    for (i = 0; i < pool_slots(&endpoints); i++) {
        struct endpoint *ep = endpoint_at(i);
        unsigned long epflags;

        if (ep->owner != p) {
            continue;
        }

        epflags = spin_lock(&ep->lock);

        if (ep->in_use && ep->owner == p) {
            teardown(ep);
        }

        spin_unlock(&ep->lock, epflags);
    }
}

/*
 * Whether an index still names something, without using it.
 *
 * `resolve` is the whole answer - the bounds test and generation match every
 * operation here makes - and nothing is sent, received or dropped. That is
 * the point: a holder of somebody else's endpoint cannot find out by calling
 * it, which blocks on one that is live, or by receiving on it, which could
 * take a message meant for its server.
 */
int ipc_cap_check(struct thread *t, cap_t index)
{
    if (t == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    if (resolve(t, index) != NULL || ipc_resolve_memory(t, index) != NULL) {
        return IPC_OK;
    }

    return IPC_ERR_BAD_CAP;
}

int ipc_call(cap_t index, const struct message *msg, struct message *reply)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    struct thread *receiver;
    unsigned long  epflags;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    message_copy(&self->ipc.msg, msg);

    /*
     * From here to the block, this endpoint is this core's.
     *
     * **The section has to span the block, not end at it.** Everything
     * between here and `thread_block_and_release` is one indivisible move:
     * the message is handed over, the receiver is woken, and this thread
     * goes onto a list where a reply can find it. Split at any point and a
     * receiver running on another core replies to a sender that is not yet
     * on the list - which is not a corruption but a message that vanishes.
     *
     * The copy above is outside on purpose: it touches only this thread's
     * own buffer and is the largest thing in the function.
     */
    epflags = spin_lock(&ep->lock);

    receiver = queue_pop(&ep->receivers);

    if (receiver != NULL) {
        /*
         * Somebody is already waiting. Hand the message straight over rather
         * than parking it anywhere: this is the rendezvous, and it is why
         * there is no buffer in the kernel.
         *
         * The message is already gone, so this thread goes straight onto the
         * reply queue and never touches the sender queue at all.
         */
        message_deliver(receiver, self, msg, &receiver->ipc.msg);
        receiver->ipc.peer = self;

        /*
         * And it runs at this thread's band until it answers.
         *
         * Before `deliver`, which wakes it: the wake enqueues it, and a
         * priority queue puts a thread where its band says at the moment it
         * is enqueued. Boosting afterwards would put it in the right band
         * for the *next* time it runs, which is one request too late.
         */
        /*
         * The common case is a peer - a server and its client both at
         * NORMAL - and it is settled here without a call. `thread_inherit`
         * lives in `thread.c` and nothing inlines across files, so asking it
         * to decide "no" was a call on every message.
         */
        if (self->sched.effective > receiver->sched.inherited) {
            thread_inherit(receiver, self);
        }

        /*
         * **Findable before the receiver is woken, and the order is the
         * fix.** This thread went onto the reply queue and named its
         * endpoint *after* `deliver`, and on one core that was harmless: the
         * receiver could not run until this thread had blocked.
         *
         * On several it runs at once. `ipc_reply` reads `waiting_on` with no
         * lock - it is how it finds the lock to take - so a receiver on
         * another core that answered quickly read NULL, said "nobody is
         * waiting", and dropped the reply. The caller then blocked for ever
         * on an answer already given. That was the whole desktop stopping
         * within two seconds of spreading its threads, found by reading every
         * thread out of the frozen machine: the window manager blocked on
         * the console, and the console asleep with nothing left to answer.
         *
         * Named first, the unlocked read finds this endpoint and waits on
         * its lock, which this thread holds until it has blocked.
         */
        queue_push(&ep->awaiting_reply, self);
        self->ipc.waiting_on = ep;

        deliver(receiver, IPC_OK);
    } else {
        /*
         * Nobody home. Wait to be collected, on the sender queue and only
         * there. It moves to awaiting_reply in ipc_receive, when its message
         * is actually taken. Being on both at once would corrupt them both,
         * because they share the `ipc.next` link.
         */
        queue_push(&ep->senders, self);
        self->ipc.waiting_on = ep;

        /*
         * **And a watcher is told there is somebody to collect.** Under the
         * lock it checked the queue under, so it cannot look, find nothing,
         * and sleep through this: it is either already blocked and findable
         * here, or it has not looked yet and will find this thread queued.
         *
         * **And under the interrupt lines' lock** (`irq_wake_watcher`): a
         * watcher in `irq_wait_any` lets this endpoint's lock go before it
         * blocks, holding that one until it has, so a wake without it could
         * land in between and be lost.
         */
        if (ep->watcher != NULL) {
            struct thread *w = ep->watcher;

            ep->watcher = NULL;
            irq_wake_watcher(w);
        }
    }

    /* Either way it is on a queue this endpoint can find, and names it, so
     * destroying the endpoint reaches it and a reply finds its lock. */

    /* Blocked and findable become true together, and the endpoint is let go
     * at exactly that moment. `thread.c` explains why the release can happen
     * before the switch here and could not in a kernel whose threads
     * migrate. */
    thread_block_and_release(&ep->lock, epflags);

    /* Woken: with a reply, or with an error because the endpoint died. */
    if (self->ipc.status != IPC_OK) {
        return self->ipc.status;
    }

    message_copy(reply, &self->ipc.msg);
    return IPC_OK;
}

int ipc_receive(cap_t index, struct message *msg, struct thread **sender,
                bool nonblocking, unsigned long timeout)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    struct thread *s;
    unsigned long  epflags;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    /* The same section `ipc_call` takes, for the same reason: this either
     * collects a waiting sender or joins the receiver queue, and both are
     * moves another core must not see half of. */
    epflags = spin_lock(&ep->lock);

    s = queue_pop(&ep->senders);

    if (s != NULL) {
        /*
         * A sender was already waiting. It stays blocked, but what it is
         * waiting for has changed: it wanted a receiver and now it wants a
         * reply, so it moves from one queue to the other. `queue_pop` above
         * already took it off the sender queue.
         */
        message_deliver(self, s, &s->ipc.msg, msg);
        *sender = s;
        s->ipc.peer = self;

        /* The other way round: this thread collected a message that was
         * already waiting, so it takes on that sender's band. */
        if (s->sched.effective > self->sched.inherited) {
            thread_inherit(self, s);
        }

        queue_push(&ep->awaiting_reply, s);
        spin_unlock(&ep->lock, epflags);
        return IPC_OK;
    }

    if (nonblocking) {
        /*
         * A server that has something else to do cannot afford to park here.
         * The console is the case that forced this: it blocks inside `read`
         * waiting for a line, and while it is blocked it is answering
         * nobody - so a program that runs for a while and wants to ask
         * whether Control-C was pressed waits for a line that only arrives
         * when somebody types one.
         *
         * With this it can pump: serve whatever has arrived, then go back to
         * waiting. The alternative was a second thread in the server, and
         * there are no threads inside a process.
         */
        spin_unlock(&ep->lock, epflags);
        return IPC_NO_MESSAGE;
    }

    /*
     * Wait for a message, and optionally not for ever.
     *
     * **A server that sleeps on a timer is deaf**, and that is not a
     * hypothesis: the audio server was changed to `sleep(1)` between
     * refills, which stopped it burning a quarter of the machine and also
     * stopped it answering anybody until the timer got round to it. A
     * client sending twelve periods a pass paid a whole tick for each one,
     * so the Music window fed the device at two thirds of the rate it
     * drained - 45 ms in `feed` for twelve round trips that should be
     * microseconds.
     *
     * The two halves already existed and had never been put together: a
     * thread blocked on an endpoint is `THREAD_BLOCKED`, and
     * `thread_wake_sleepers` wakes any blocked thread whose `wake_at` has
     * arrived. Setting both means whichever comes first wins, which is what
     * every event loop in the world actually wants.
     *
     * `status` is set to `IPC_NO_MESSAGE` first so that waking can be told
     * apart from being sent to: a real delivery overwrites it.
     */
    self->ipc.status = IPC_NO_MESSAGE;

    if (timeout != 0) {
        /* Scheduler ticks in, a counter deadline out - `thread.h` says why
         * the kernel keeps deadlines in the clock that does not stretch
         * when a tick is missed. */
        self->wake_at = thread_deadline_in(timeout);
    }

    queue_push(&ep->receivers, self);
    self->ipc.waiting_on = ep;

    /* On the queue and blocked together; the endpoint goes at that instant. */
    thread_block_and_release(&ep->lock, epflags);

    self->wake_at = 0;

    if (self->ipc.status == IPC_NO_MESSAGE) {
        /*
         * The timer, not a sender. Nothing has been handed over, so this
         * thread has to take itself off the queue it is still on - leaving
         * it there would let a later sender deliver into a thread that has
         * moved on, which is the worst kind of bug this file can have.
         *
         * Retaken, because the lock was let go at the block: this is a
         * second visit to the endpoint rather than the tail of the first,
         * and between the two a sender may have arrived. The status is
         * re-read inside for exactly that reason - a delivery that landed
         * while this was reacquiring is a real message, not a timeout.
         */
        unsigned long again = spin_lock(&ep->lock);

        if (self->ipc.status == IPC_NO_MESSAGE) {
            queue_remove(&ep->receivers, self);
            self->ipc.waiting_on = NULL;
            spin_unlock(&ep->lock, again);
            return IPC_NO_MESSAGE;
        }

        spin_unlock(&ep->lock, again);
    }

    if (self->ipc.status != IPC_OK) {
        return self->ipc.status;
    }

    message_copy(msg, &self->ipc.msg);
    *sender = self->ipc.peer;
    return IPC_OK;
}

/*
 * **Until somebody calls this endpoint, the deadline passes, or - with
 * `or_input` - a key or the pointer arrives.** Whichever is first.
 *
 * It does not receive. The caller collects with an ordinary non-blocking
 * `ipc_receive` afterwards, which is what the window manager already does
 * every pass; this only ends the sleep in front of it. That is why a
 * receiver's timed wait is not the answer: the thread that sleeps here is
 * the console server, answering the window manager's input request, and the
 * endpoint it watches is the window manager's.
 *
 * **No lost wakeup.** The queue is looked at under the endpoint's lock, the
 * watcher is recorded under it, and the thread blocks and lets it go in one
 * step (`thread_block_and_release`). A caller on another core takes the same
 * lock to queue itself, so it either arrives first and is seen, or arrives
 * after and finds a blocked, findable watcher to wake. Input is looked at
 * again after `wake_on_input` is set, for the same reason one layer down.
 *
 * Returns IPC_OK however it ended - which of the three is not something the
 * caller can act on differently, since it looks at input and messages
 * anyway - or IPC_ERR_BAD_CAP, or IPC_ERR_NO_SPACE when somebody else is
 * already watching.
 */
int ipc_wait_for_caller(cap_t index, unsigned long ticks, bool or_input)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    unsigned long epflags;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    if (ticks == 0) {
        return IPC_OK;
    }

    epflags = spin_lock(&ep->lock);

    if (ep->senders != NULL) {
        spin_unlock(&ep->lock, epflags);
        return IPC_OK;                  /* somebody is already there */
    }

    if (ep->watcher != NULL && ep->watcher != self) {
        spin_unlock(&ep->lock, epflags);
        return IPC_ERR_NO_SPACE;
    }

    ep->watcher = self;
    self->ipc.watching[0] = ep;
    self->wake_at = thread_deadline_in(ticks);
    self->wake_on_input = or_input;

    if (or_input && hal_input_pending()) {
        ep->watcher = NULL;
        self->ipc.watching[0] = NULL;
        self->wake_on_input = false;
        self->wake_at = 0;
        spin_unlock(&ep->lock, epflags);
        return IPC_OK;
    }

    thread_block_and_release(&ep->lock, epflags);

    /* Woken by a caller, the timer, input, or the endpoint going: whichever
     * did it may or may not have cleared the watcher, so this does. */
    epflags = spin_lock(&ep->lock);

    if (ep->watcher == self) {
        ep->watcher = NULL;
    }

    self->ipc.watching[0] = NULL;
    spin_unlock(&ep->lock, epflags);

    self->wake_on_input = false;
    self->wake_at = 0;

    return IPC_OK;
}

struct endpoint *ipc_endpoint_lock(struct thread *t, cap_t index,
                                   unsigned long *flags)
{
    struct endpoint *ep = resolve(t, index);

    if (ep == NULL) {
        return NULL;
    }

    *flags = spin_lock(&ep->lock);

    /* Looked at again under the lock, as `ipc_endpoint_destroy` looks: the
     * endpoint may have been taken down since `resolve` read it. */
    if (!ep->in_use || slot_generation(t, index) != ep->generation) {
        spin_unlock(&ep->lock, *flags);
        return NULL;
    }

    return ep;
}

void ipc_endpoint_unlock(struct endpoint *ep, unsigned long flags)
{
    spin_unlock(&ep->lock, flags);
}

bool ipc_endpoint_has_caller(const struct endpoint *ep)
{
    return ep->senders != NULL;
}

bool ipc_endpoint_watch(struct endpoint *ep, struct thread *t, unsigned slot)
{
    if (ep->watcher != NULL && ep->watcher != t) {
        return false;
    }

    ep->watcher = t;
    t->ipc.watching[slot] = ep;
    return true;
}

void ipc_endpoint_unwatch(struct endpoint *ep, struct thread *t, unsigned slot)
{
    if (ep->watcher == t) {
        ep->watcher = NULL;
    }

    if (t->ipc.watching[slot] == ep) {
        t->ipc.watching[slot] = NULL;
    }
}

struct endpoint *ipc_endpoint_peek(struct thread *t, cap_t index)
{
    return resolve(t, index);
}

int ipc_reply(struct thread *sender, const struct message *msg)
{
    struct endpoint *ep;

    if (sender == NULL) {
        return IPC_ERR_NO_PEER;
    }

    ep = sender->ipc.waiting_on;

    if (ep == NULL) {
        /* Not waiting for anything, so there is nothing to answer. Replying
         * twice to the same sender lands here rather than corrupting it. */
        return IPC_ERR_NO_PEER;
    }

    {
        unsigned long epflags = spin_lock(&ep->lock);

        /*
         * Read again with the lock held, because the first read was not.
         *
         * `waiting_on` is what names the endpoint, so it has to be read
         * before there is a lock to take - the classic order problem, and
         * the answer is the classic one: take the lock the unlocked read
         * pointed at, then check that the read is still true. Between the
         * two, the sender may have been woken by a timeout or by the
         * endpoint being destroyed, and replying to it then would deliver
         * into a thread that has moved on.
         */
        if (sender->ipc.waiting_on != ep) {
            spin_unlock(&ep->lock, epflags);
            return IPC_ERR_NO_PEER;
        }

        queue_remove(&ep->awaiting_reply, sender);

        message_deliver(sender, thread_current(), msg, &sender->ipc.msg);
        deliver(sender, IPC_OK);

        spin_unlock(&ep->lock, epflags);
    }

    /*
     * Done on somebody's behalf, so back to its own band.
     *
     * Cleared rather than unwound: a server handling two requests at once -
     * which a coroutine server does - would need a stack of borrowed bands
     * to be exact, and the error either way lasts one request. Erring
     * downward is the safe direction: a server that stays high starves the
     * machine, and one that drops early is merely slower for a moment.
     */
    /* Likewise: a server that borrowed nothing has nothing to give back. */
    {
        struct thread *me = thread_current();

        if (me != NULL && me->sched.inherited != 0) {
            thread_disinherit(me);
        }
    }

    return IPC_OK;
}
