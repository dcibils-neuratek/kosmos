/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spinlock.h"

struct thread;
struct memobj;
struct process;

/*
 * Synchronous IPC, and the capability table that names it.
 *
 * Rendezvous with no buffering in the kernel, L4 style, as `design.md` §4.2
 * decided. The sender blocks until a receiver takes the message; the
 * receiver blocks until a sender arrives. Nothing is queued anywhere except
 * the threads themselves, which means there is no buffer to size, no
 * backpressure policy to invent, and no queue to overflow. It is also faster
 * on the hot path, because a send is a direct switch to the receiver.
 *
 * The cost is that code written over synchronous IPC normally turns into
 * ugly state machines. Coroutines are what answer that, in userland, from
 * M5. The kernel stays this simple on purpose.
 *
 * There is one detail here that is easy to leave out and ruins server
 * restart: **when an endpoint is destroyed, every thread blocked on it has
 * to be woken with an error.** Otherwise they wait forever for a server that
 * no longer exists. `roadmap.md` names it as this milestone's trap and it is
 * why `ipc_endpoint_destroy` is as long as it is.
 */

/*
 * A message is a tag and a run of bytes.
 *
 * `design.md` §14 makes the tag mandatory: with no types in a system of tens
 * of thousands of lines of Lua, a message that does not say what it is
 * becomes a silent nil three layers down. The kernel does not interpret it,
 * it only insists it is there.
 *
 * The bytes are a serialised Lua value, and **the kernel has no opinion
 * about that at all**. It copies them. That is what makes `design.md` §1's
 * thesis affordable: the protocol between servers can be the data model of
 * the language precisely because the thing in the middle does not need to
 * understand it. An IDL would have to.
 *
 * 2 KB. A namespace read or a drawing command is tens of bytes, and 512 was
 * enough for those and not for the thing that turned out to matter: hot
 * reload sends a server's source, and a server is more than 512 bytes of
 * Lua. Anything genuinely large still wants shared memory rather than a
 * copy, and `gfx.md` §19.4 designs that path separately.
 *
 * The size costs memory rather than time, because only `length` bytes are
 * ever copied. What it does cost is stack: a syscall holding two of these
 * would put 4 KB on a 16 KB exception stack, which is why `sys_call` uses
 * one buffer for the request and the reply.
 */
#define MSG_BYTES   2048

struct message {
    uint64_t tag;

    /*
     * A capability travelling with the message.
     *
     * Out of band rather than inside the serialised bytes, and it has to be:
     * a capability is an index into the *sender's* table and means nothing in
     * the receiver's. The kernel translates it on delivery, so what arrives
     * is the receiver's own index for the same endpoint. Serialising it as
     * data would send a number that names something else on the other side.
     *
     * This is what lets userland do its own mounting. Without it only the
     * kernel can hand out capabilities, so only the kernel decides what a
     * process may reach, and `design.md` §4.4 puts that decision in the
     * namespace server, which passes on what it holds.
     *
     * **Stored as index plus one, so that zero means none.** `struct message
     * m = {0}` is how every message in this codebase is written, and a field
     * whose safe value is not zero is a field that will be wrong at whichever
     * construction site was written in a hurry. With -1 for none, a zeroed
     * message would have quietly transferred the sender's capability 0 on
     * every send.
     */
    uint32_t cap_plus_one;

    uint32_t length;            /* bytes of `data` in use */
    uint8_t  data[MSG_BYTES];
};

/*
 * A capability is an index into a per-thread table, never a global
 * identifier. `design.md` §4.3: a thread cannot name what it was not handed.
 * Declared here rather than below because the helpers under it need the type.
 */
typedef int cap_t;


/* The two ends of the +1 encoding, so nothing else has to know about it. */
static inline void message_set_cap(struct message *m, cap_t c)
{
    m->cap_plus_one = (c >= 0) ? (uint32_t)(c + 1) : 0u;
}

/* The capability that came with the message, or negative for none. */
static inline cap_t message_get_cap(const struct message *m)
{
    return (m->cap_plus_one == 0) ? -1 : (cap_t)(m->cap_plus_one - 1);
}

/* Results. Negative is failure, so `if (ipc_call(...) < 0)` reads correctly. */
#define IPC_OK              0
#define IPC_ERR_BAD_CAP    (-1)     /* the index names nothing, or something stale */
#define IPC_ERR_GONE       (-2)     /* the endpoint was destroyed while waiting */
#define IPC_ERR_NO_PEER    (-3)     /* replying to a thread that is not waiting */
#define IPC_ERR_NO_SPACE   (-4)     /* out of endpoints, or out of capability slots */
#define IPC_ERR_TOO_BIG    (-5)     /* the value does not fit in a message */
#define IPC_ERR_BAD_VALUE  (-6)     /* a value that cannot cross a boundary */
#define IPC_NO_MESSAGE     (-7)     /* nobody was waiting, and blocking was refused */

/*
 * How many capabilities a thread may hold.
 *
 * There is no global table to enumerate and no identifier to guess, so the
 * check on every operation is a bounds check rather than a permission
 * lookup.
 */
/*
 * How many capabilities a table holds: a process's, or a kernel thread's.
 *
 * Sixteen, until a graphical application turned out to need more than that
 * on its own. A PDF viewer holds its console, its `/dev/wm` endpoint, the
 * filesystem, its window's shared region, a read buffer, the buffers a page
 * is decoded through, and a region per embedded font - and every one of them
 * is a slot. It ran out mid-page and the failure arrived as `NO_ROOM`, which
 * reads as "out of memory" and sent two rounds of debugging at the
 * allocator. There were 117,000 free pages at the time.
 *
 * Thirty-two, because the shape of what runs here changed: the userland this
 * number was chosen for was a shell and three servers.
 *
 * Sixty-four was tried first and panicked the benchmark image with a data
 * abort, because `.bss` growth then pushed the thread stacks' guard pages out
 * of the first 2 MB of RAM, the only part mapped a page at a time. **That
 * wall is gone** - `mmu_init` maps a page at a time as far as the image
 * reaches, "and grows by itself the next time the image does" - so the
 * number is the limit it says it is and not a layout accident.
 *
 * It is still a *limit*, and deliberately: a process that leaks capabilities
 * should hit a wall rather than grow without bound. `SYS_CAP_DROP` is how a
 * program stays under it.
 */
#define CAPS_PER_TABLE      32

struct endpoint;
struct memobj;
struct irq_line;

#define CAP_NONE      0
#define CAP_ENDPOINT  1
#define CAP_MEMORY    2

/*
 * A hardware interrupt line, claimed by a driver. `kernel/irq.h` is the
 * argument; what matters here is that it is a capability like the other two,
 * so a driver names a line by an index into its own table and cannot reach
 * one it was not given.
 */
#define CAP_IRQ       3

/*
 * **A capability table, and whose it is.**
 *
 * A capability names one of three kinds of thing: an endpoint, a region of
 * memory two processes share, or an interrupt line. The kind is stored
 * rather than inferred, so a slot holding one can never be read as another -
 * which is the mistake a union without a tag invites, and which would be a
 * process handing out a pointer to somebody's pixels as a place to send
 * messages. The generation is checked for all three, against the same
 * hazard: a slot whose object was destroyed and replaced.
 *
 * **The table is the process's**, which is where `design.md` always put it,
 * since 19 September (`threads.md` step 1). It was the thread's - a comment
 * said it would move "at M4", and it never did - which is the same thing
 * while a process has one thread and the wrong thing as soon as it has two:
 * a capability one thread received would be a number its sibling could not
 * use. So a process's threads all point at their process's table, and a
 * kernel thread, which belongs to no process, points at one of its own.
 *
 * **With a lock**, because two threads on two cores can now reach one table
 * at once. Before, the only writer from outside was a delivery, under the
 * endpoint's lock, and the thread itself needed none. Taken after an
 * endpoint's lock and before the region pool's, never the other way round,
 * and never held across freeing pages.
 */
struct cap {
    unsigned char    kind;      /* CAP_NONE, CAP_ENDPOINT, CAP_MEMORY, CAP_IRQ */
    struct endpoint *endpoint;
    struct memobj   *memory;
    struct irq_line *irq;
    unsigned         generation;
};

struct captable {
    struct spinlock lock;
    struct cap      slot[CAPS_PER_TABLE];
};

/* An empty table with its lock ready: for a process, or a kernel thread. */
void captable_init(struct captable *c);

/* How many slots hold something, for `sysinfo`. */
unsigned captable_count(struct captable *c);

/*
 * How many endpoints there can be: the pool's ceiling, for `SYS_SYSINFO`'s
 * "in use, of this many". It was ninety-six, compiled in - three a process -
 * and grows now (`ipc.c`, `threads.md` step 1b).
 */
unsigned ipc_endpoints_total(void);

/* Prepares the endpoint pool. Called once, before any thread uses IPC. */
void ipc_init(void);

/* Unblocks a thread and unlinks it from whatever queue it was on.
 * For killing: a blocked thread cannot notice anything by itself. */
void ipc_abort(struct thread *t);

/* A blocked receiver whose deadline has arrived. Called from the timer,
 * before the thread is made runnable, so that no sender can be handed a
 * thread that is about to give up. */
void ipc_timed_out(struct thread *t);

/* Capabilities to shared memory: the same two operations endpoints have. */
struct memobj *ipc_resolve_memory(struct thread *t, cap_t index);

/* The same pair for an interrupt line. `kernel/irq.h` is the argument, and
 * `ipc.c` has the one difference: a line has no reference count, because it
 * belongs to the process that claimed it rather than to its capabilities. */
struct irq_line;
struct irq_line *ipc_resolve_irq(struct thread *t, cap_t index);
cap_t ipc_install_irq(struct thread *t, struct irq_line *line);
cap_t ipc_install_memory(struct thread *t, struct memobj *m);

/* Drops everything a thread holds. Only memory needs it - an endpoint
 * capability going stale is harmless, a region's pages are not. */
/* One capability back. Dropping is not destroying: see ipc.c. */
int  ipc_cap_drop(struct thread *t, cap_t index);

void ipc_caps_release(struct captable *c);

/*
 * A new endpoint, with a capability to it installed in the calling thread.
 * Returns the index, or a negative error.
 */
cap_t ipc_endpoint_create(void);

/*
 * Hands a capability for the same endpoint to another thread, and returns
 * the index it will use. The two indices are unrelated: an index is
 * meaningful only inside the table it came from, which is the whole point.
 */
cap_t ipc_cap_grant(struct thread *to, cap_t from_index);

/*
 * Destroys the endpoint and wakes everything blocked on it with
 * IPC_ERR_GONE, including senders already waiting for a reply.
 *
 * Every capability naming it becomes stale rather than dangling: the slot
 * carries a generation number, so a capability that outlives its endpoint
 * fails cleanly instead of addressing whatever is created next.
 */
int ipc_endpoint_destroy(cap_t index);

/*
 * Destroys every endpoint `p` made, as `ipc_endpoint_destroy` would: an
 * endpoint ends with the process that made it. Called by `process_exit`.
 */
void ipc_endpoints_release(struct process *p);

/*
 * IPC_OK while `index` names a live endpoint or region, IPC_ERR_BAD_CAP once
 * it names nothing. Touches nothing, so a holder of somebody else's endpoint
 * can ask whether it is still there without calling it.
 */
int ipc_cap_check(struct thread *t, cap_t index);

/*
 * Send and wait for the reply. Blocks until a receiver takes the message and
 * answers it. This is the operation a client uses, and the one the round
 * trip benchmark measures.
 */
int ipc_call(cap_t index, const struct message *msg, struct message *reply);

/*
 * Wait for a message. Blocks until a sender arrives. `sender` comes back
 * holding whoever sent it, which is the token `ipc_reply` needs; it is not a
 * capability and cannot be stored or passed on.
 */
/* `timeout` is in scheduler ticks: 0 waits for ever, anything else returns
 * IPC_NO_MESSAGE if nothing has arrived by then. Ignored when nonblocking,
 * which already returns immediately. */
int ipc_receive(cap_t index, struct message *msg, struct thread **sender,
                bool nonblocking, unsigned long timeout);

/* Answer a sender obtained from ipc_receive, unblocking it. */
/*
 * Sleep until a caller arrives on `index`, `ticks` scheduler ticks pass, or -
 * with `or_input` - input arrives. Collects nothing; see ipc.c.
 */
int ipc_wait_for_caller(cap_t index, unsigned long ticks, bool or_input);

/*
 * For `irq_wait_any`'s endpoints, and nothing else: an endpoint locked by the
 * capability `t` holds for it, whether a caller is queued there, and a
 * watcher recorded or taken off - the last three under the lock the first
 * took. `irq.c` has why that wait holds an endpoint's lock and the lines'.
 */
struct endpoint;
struct endpoint *ipc_endpoint_lock(struct thread *t, cap_t index,
                                   unsigned long *flags);
void ipc_endpoint_unlock(struct endpoint *ep, unsigned long flags);
bool ipc_endpoint_has_caller(const struct endpoint *ep);
bool ipc_endpoint_watch(struct endpoint *ep, struct thread *t, unsigned slot);
void ipc_endpoint_unwatch(struct endpoint *ep, struct thread *t,
                          unsigned slot);

/*
 * Which endpoint `index` names, without its lock, or NULL: only to put two
 * endpoints in the order their locks are taken, and to see that they are not
 * the same one. Never read through - the endpoint may go the moment after.
 */
struct endpoint *ipc_endpoint_peek(struct thread *t, cap_t index);

int ipc_reply(struct thread *sender, const struct message *msg);

/* For tests and inspection. */
unsigned ipc_endpoints_in_use(void);

#endif /* KERNEL_IPC_H */
