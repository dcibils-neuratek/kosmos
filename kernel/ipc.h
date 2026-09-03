#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;
struct memobj;

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
 * How many capabilities a thread may hold at once.
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
 * abort. A slot is 32 bytes, so that was 1.5 KB more per thread and 73 KB
 * more `.bss` across the pool - and `state.md` already records what `.bss`
 * growth does here: the thread stacks and their guard pages have to stay
 * inside the first 2 MB of RAM, which is the only part mapped a page at a
 * time. Doubling is enough for what applications actually hold and leaves
 * that alone.
 *
 * It is still a *limit*, and deliberately: a process that leaks capabilities
 * should hit a wall rather than grow without bound. `SYS_CAP_DROP` is how a
 * program stays under it.
 */
#define CAPS_PER_THREAD     32

/*
 * How many endpoints exist. Here rather than only in ipc.c because
 * SYS_SYSINFO reports "in use, of this many", and half of that pair is
 * useless without the other.
 *
 * Raised with the process pool, because they are spent together: a server
 * needs one to be reachable at, and a client that brokers a private
 * connection needs another. Thirty-two was one per process and change;
 * ninety-six is three apiece, which is what a system where processes hand
 * each other capabilities actually uses.
 */
#define ENDPOINT_MAX        96

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
cap_t ipc_install_memory(struct thread *t, struct memobj *m);

/* Drops everything a thread holds. Only memory needs it - an endpoint
 * capability going stale is harmless, a region's pages are not. */
/* One capability back. Dropping is not destroying: see ipc.c. */
int  ipc_cap_drop(struct thread *t, cap_t index);

void ipc_caps_release(struct thread *t);

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
int ipc_reply(struct thread *sender, const struct message *msg);

/* For tests and inspection. */
unsigned ipc_endpoints_in_use(void);

#endif /* KERNEL_IPC_H */
