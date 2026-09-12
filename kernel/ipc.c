/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ipc.h"
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
};

static struct endpoint endpoints[ENDPOINT_MAX];

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
static cap_t install(struct thread *t, struct endpoint *ep);

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
        struct endpoint *ep = resolve(from, sending);

        if (ep != NULL) {
            message_set_cap(dst, install(to, ep));
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
            struct memobj *m = ipc_resolve_memory(from, sending);

            if (m != NULL) {
                message_set_cap(dst, ipc_install_memory(to, m));
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

void ipc_init(void)
{
    unsigned i;

    for (i = 0; i < ENDPOINT_MAX; i++) {
        endpoints[i].in_use = false;
        endpoints[i].senders = NULL;
        endpoints[i].receivers = NULL;
        endpoints[i].awaiting_reply = NULL;

        endpoints[i].lock.locked = 0;
        endpoints[i].lock.holder = SPIN_NOBODY;
        endpoints[i].lock.name   = "endpoint";

        /* generation is deliberately not reset: it only ever goes up. */
    }
}

unsigned ipc_endpoints_in_use(void)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < ENDPOINT_MAX; i++) {
        if (endpoints[i].in_use) {
            n++;
        }
    }

    return n;
}

/* The endpoint a capability index names, or NULL if it names nothing. This
 * is the entire access check: a bounds test and a generation match. */
static struct endpoint *resolve(struct thread *t, cap_t index)
{
    struct endpoint *ep;

    if (index < 0 || index >= CAPS_PER_THREAD) {
        return NULL;
    }

    if (t->caps[index].kind != CAP_ENDPOINT) {
        return NULL;            /* empty, or a region of memory */
    }

    ep = t->caps[index].endpoint;

    if (ep == NULL || !ep->in_use) {
        return NULL;
    }

    if (t->caps[index].generation != ep->generation) {
        return NULL;    /* destroyed and the slot reused since */
    }

    return ep;
}

static cap_t install(struct thread *t, struct endpoint *ep)
{
    cap_t i;

    for (i = 0; i < CAPS_PER_THREAD; i++) {
        if (t->caps[i].kind == CAP_NONE) {
            t->caps[i].kind = CAP_ENDPOINT;
            t->caps[i].endpoint = ep;
            t->caps[i].memory = NULL;
            t->caps[i].generation = ep->generation;
            return i;
        }
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
struct memobj *ipc_resolve_memory(struct thread *t, cap_t index)
{
    struct memobj *m;

    if (index < 0 || index >= CAPS_PER_THREAD) {
        return NULL;
    }

    if (t->caps[index].kind != CAP_MEMORY) {
        return NULL;
    }

    m = t->caps[index].memory;

    if (m == NULL || !m->in_use) {
        return NULL;
    }

    if (t->caps[index].generation != m->generation) {
        return NULL;    /* freed and the slot reused since */
    }

    return m;
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
    struct irq_line *line;

    if (index < 0 || index >= CAPS_PER_THREAD) {
        return NULL;
    }

    if (t->caps[index].kind != CAP_IRQ) {
        return NULL;
    }

    line = t->caps[index].irq;

    if (line == NULL || !line->in_use) {
        return NULL;
    }

    if (t->caps[index].generation != line->generation) {
        return NULL;    /* released and the slot reused since */
    }

    return line;
}

cap_t ipc_install_irq(struct thread *t, struct irq_line *line)
{
    cap_t i;

    if (t == NULL || line == NULL) {
        return -1;
    }

    for (i = 0; i < CAPS_PER_THREAD; i++) {
        if (t->caps[i].kind == CAP_NONE) {
            t->caps[i].kind = CAP_IRQ;
            t->caps[i].endpoint = NULL;
            t->caps[i].memory = NULL;
            t->caps[i].irq = line;
            t->caps[i].generation = line->generation;
            return i;
        }
    }

    return -1;
}

cap_t ipc_install_memory(struct thread *t, struct memobj *m)
{
    cap_t i;

    /*
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
    for (i = 0; i < CAPS_PER_THREAD; i++) {
        if (t->caps[i].kind == CAP_NONE) {
            t->caps[i].kind = CAP_MEMORY;
            t->caps[i].endpoint = NULL;
            t->caps[i].memory = m;
            t->caps[i].generation = m->generation;
            memobj_ref(m);
            return i;
        }
    }

    return IPC_ERR_NO_SPACE;
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
    if (t == NULL || index < 0 || index >= CAPS_PER_THREAD) {
        return IPC_ERR_BAD_CAP;
    }

    if (t->caps[index].kind == CAP_NONE) {
        return IPC_ERR_BAD_CAP;
    }

    /*
     * A stale capability is dropped without touching the object.
     *
     * The slot may name a region that was freed and whose pool entry has
     * since been taken by another one. Unreffing then decrements a stranger,
     * and the stranger's owner watches its pages go back to the allocator
     * while it still holds a capability to them. The generation is exactly
     * the check that tells the two apart, and it is why the field is there.
     */
    if (t->caps[index].kind == CAP_MEMORY
        && t->caps[index].memory != NULL
        && t->caps[index].generation == t->caps[index].memory->generation) {
        memobj_unref(t->caps[index].memory);
    }

    t->caps[index].kind = CAP_NONE;
    t->caps[index].endpoint = NULL;
    t->caps[index].memory = NULL;
    t->caps[index].irq = NULL;
    t->caps[index].generation = 0;

    return 0;
}

/*
 * Everything a thread holds, released.
 *
 * Only memory needs this. An endpoint capability going stale is harmless -
 * the generation check catches it - but a region's pages are only freed
 * when the last capability to it is dropped, so a thread that ends without
 * dropping its own leaks them for the life of the machine.
 */
void ipc_caps_release(struct thread *t)
{
    cap_t i;

    if (t == NULL) {
        return;
    }

    for (i = 0; i < CAPS_PER_THREAD; i++) {
        /* Generation-checked, for the reason `ipc_cap_drop` gives: a stale
         * slot names a pool entry, not the region that used to be in it. */
        if (t->caps[i].kind == CAP_MEMORY
            && t->caps[i].memory != NULL
            && t->caps[i].generation == t->caps[i].memory->generation) {
            memobj_unref(t->caps[i].memory);
        }

        t->caps[i].kind = CAP_NONE;
        t->caps[i].endpoint = NULL;
        t->caps[i].memory = NULL;
        t->caps[i].irq = NULL;
    }
}

cap_t ipc_endpoint_create(void)
{
    struct thread *self = thread_current();
    unsigned i;

    for (i = 0; i < ENDPOINT_MAX; i++) {
        if (!endpoints[i].in_use) {
            cap_t index;

            endpoints[i].in_use = true;
            endpoints[i].owner = self->process;
            endpoints[i].senders = NULL;
            endpoints[i].receivers = NULL;
            endpoints[i].awaiting_reply = NULL;

            index = install(self, &endpoints[i]);

            if (index < 0) {
                endpoints[i].in_use = false;
                return index;
            }

            return index;
        }
    }

    return IPC_ERR_NO_SPACE;
}

cap_t ipc_cap_grant(struct thread *to, cap_t from_index)
{
    struct endpoint *ep = resolve(thread_current(), from_index);

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    /* The index the recipient gets is unrelated to the one the granter used.
     * An index is meaningful only inside the table it came from, which is
     * what stops one from being guessed or forged elsewhere. */
    return install(to, ep);
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
    if (!ep->in_use || self->caps[index].generation != ep->generation) {
        spin_unlock(&ep->lock, epflags);
        return IPC_ERR_BAD_CAP;
    }

    teardown(ep);
    spin_unlock(&ep->lock, epflags);

    /* The granter's own capability is cleared; the others go stale on their
     * next use, which is what the generation check is for. */
    self->caps[index].kind = CAP_NONE;
    self->caps[index].endpoint = NULL;

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

    for (i = 0; i < ENDPOINT_MAX; i++) {
        struct endpoint *ep = &endpoints[i];
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
