#ifndef KERNEL_IPC_H
#define KERNEL_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

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
 * A message is a fixed-size register-sized payload plus a tag.
 *
 * `design.md` §14 makes the tag mandatory: with no types in a system of tens
 * of thousands of lines of Lua, a message that does not say what it is
 * becomes a silent nil three layers down. The kernel does not interpret it,
 * it only insists it is there.
 *
 * Eight words because that is a cache line and change, and because anything
 * larger wants shared memory rather than a copy. The Lua table serialiser
 * that fills these arrives with userland at M4; the kernel moves bytes and
 * has no opinion about their shape.
 */
#define MSG_WORDS   8

struct message {
    uint64_t tag;
    uint64_t word[MSG_WORDS];
};

/* Results. Negative is failure, so `if (ipc_call(...) < 0)` reads correctly. */
#define IPC_OK              0
#define IPC_ERR_BAD_CAP    (-1)     /* the index names nothing, or something stale */
#define IPC_ERR_GONE       (-2)     /* the endpoint was destroyed while waiting */
#define IPC_ERR_NO_PEER    (-3)     /* replying to a thread that is not waiting */
#define IPC_ERR_NO_SPACE   (-4)     /* out of endpoints, or out of capability slots */

/*
 * Capabilities are indices into a per-thread table, never global identifiers.
 *
 * `design.md` §4.3: a thread cannot name what it was not handed. There is no
 * global table to enumerate and no identifier to guess, so the check on every
 * operation is a bounds check rather than a permission lookup. At M4 the
 * table moves from the thread to the process, which is where the design puts
 * it; nothing about the interface changes.
 */
#define CAPS_PER_THREAD     16

typedef int cap_t;

/* Prepares the endpoint pool. Called once, before any thread uses IPC. */
void ipc_init(void);

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
int ipc_receive(cap_t index, struct message *msg, struct thread **sender);

/* Answer a sender obtained from ipc_receive, unblocking it. */
int ipc_reply(struct thread *sender, const struct message *msg);

/* For tests and inspection. */
unsigned ipc_endpoints_in_use(void);

#endif /* KERNEL_IPC_H */
