#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ipc.h"
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
 * against handing thread pointers to EL0. Killing makes it reachable rather
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

    queue_remove(&ep->senders, t);
    queue_remove(&ep->receivers, t);
    queue_remove(&ep->awaiting_reply, t);

    t->ipc.waiting_on = NULL;
    t->ipc.status = IPC_ERR_GONE;
    thread_wake(t);
}

void ipc_init(void)
{
    unsigned i;

    for (i = 0; i < ENDPOINT_MAX; i++) {
        endpoints[i].in_use = false;
        endpoints[i].senders = NULL;
        endpoints[i].receivers = NULL;
        endpoints[i].awaiting_reply = NULL;
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

int ipc_endpoint_destroy(cap_t index)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    struct thread *t;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    /*
     * The trap roadmap.md names for this milestone. Everything blocked on
     * this endpoint has to be woken with an error, or it waits forever for a
     * server that no longer exists, and a server that cannot be restarted
     * takes the design's whole recovery story with it.
     */
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
    ep->in_use = false;

    /* The granter's own capability is cleared; the others go stale on their
     * next use, which is what the generation check is for. */
    self->caps[index].kind = CAP_NONE;
    self->caps[index].endpoint = NULL;

    return IPC_OK;
}

int ipc_call(cap_t index, const struct message *msg, struct message *reply)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    struct thread *receiver;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

    message_copy(&self->ipc.msg, msg);

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
        deliver(receiver, IPC_OK);
        queue_push(&ep->awaiting_reply, self);
    } else {
        /*
         * Nobody home. Wait to be collected, on the sender queue and only
         * there. It moves to awaiting_reply in ipc_receive, when its message
         * is actually taken. Being on both at once would corrupt them both,
         * because they share the `ipc.next` link.
         */
        queue_push(&ep->senders, self);
    }

    /* Either way it is on a queue this endpoint can find, so destroying the
     * endpoint reaches it. */
    self->ipc.waiting_on = ep;
    thread_block();

    /* Woken: with a reply, or with an error because the endpoint died. */
    if (self->ipc.status != IPC_OK) {
        return self->ipc.status;
    }

    message_copy(reply, &self->ipc.msg);
    return IPC_OK;
}

int ipc_receive(cap_t index, struct message *msg, struct thread **sender,
                bool nonblocking)
{
    struct thread *self = thread_current();
    struct endpoint *ep = resolve(self, index);
    struct thread *s;

    if (ep == NULL) {
        return IPC_ERR_BAD_CAP;
    }

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
        queue_push(&ep->awaiting_reply, s);
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
        return IPC_NO_MESSAGE;
    }

    queue_push(&ep->receivers, self);
    self->ipc.waiting_on = ep;
    thread_block();

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

    queue_remove(&ep->awaiting_reply, sender);

    message_deliver(sender, thread_current(), msg, &sender->ipc.msg);
    deliver(sender, IPC_OK);

    return IPC_OK;
}
