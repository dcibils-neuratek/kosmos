/*
 * The syscall dispatcher.
 *
 * Every one of these runs on behalf of a process that is not trusted, on the
 * kernel's side of a boundary the hardware enforces. Two rules follow, and
 * they are the whole of the file's discipline:
 *
 * **Every pointer is checked before it is touched.** A process handing over
 * a kernel address is not caught by the MMU, because the kernel dereferences
 * it at EL1 where that mapping is valid and privileged. `process_may_read`
 * and `process_may_write` are the only thing between a syscall and an
 * arbitrary read of kernel memory.
 *
 * **Every failure is a return value.** A syscall never panics on anything a
 * process can cause. A process that passes nonsense gets an error; a kernel
 * that panics on nonsense hands any process the power to stop the machine.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "syscall.h"
#include "process.h"
#include "thread.h"
#include "ipc.h"
#include "trap.h"
#include "console.h"
#include "hal.h"

/*
 * Moving a message across the boundary, in each direction.
 *
 * Both copy only the bytes the message says it has, because copying all 512
 * every time is what made the IPC round trip thirty-six times slower than it
 * needed to be. Both clamp the length first: on the way in it came from a
 * process, and on the way out it is about to be written into one.
 */
static void copy_message_in(struct message *dst, const struct message *src)
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

static void copy_message_out(struct message *dst, const struct message *src)
{
    uint32_t n = (src->length > MSG_BYTES) ? MSG_BYTES : src->length;

    dst->tag = src->tag;
    dst->cap_plus_one = src->cap_plus_one;
    dst->length = n;
    memcpy(dst->data, src->data, n);
}

/* Longest a single write may be. Bounded because the process chooses the
 * length, and an unbounded one is an unbounded time with interrupts masked
 * inside a polled UART. */
#define WRITE_MAX   4096

static long sys_write(struct process *p, uintptr_t ptr, size_t len)
{
    const char *s = (const char *)ptr;
    size_t i;

    /*
     * Only the process that owns the console. Everything else reaches it the
     * way it reaches anything else: by asking whoever serves it.
     *
     * This is what stops the console server being ceremony. If any process
     * could print, nothing would depend on going through it and the design
     * would hold by convention rather than because the machine says so.
     */
    if (!p->owns_console) {
        return SYS_ERR_DENIED;
    }

    if (len > WRITE_MAX) {
        len = WRITE_MAX;
    }

    if (!process_may_read(p, ptr, len)) {
        return SYS_ERR_FAULT;
    }

    /*
     * Checked once and then read a byte at a time, which is safe only
     * because nothing can unmap those pages in between: this process is the
     * one running, and it is here rather than at EL0. When another thread
     * can change this address space, the check and the copy have to become
     * one operation.
     */
    for (i = 0; i < len; i++) {
        kputc(s[i]);
    }

    return (long)len;
}

static long sys_call(struct process *p, cap_t cap, uintptr_t msg_ptr,
                     uintptr_t reply_ptr)
{
    /*
     * One buffer for both directions.
     *
     * Two would be clearer and would put 4 KB on a 16 KB exception stack.
     * ipc_call copies the request into the calling thread's own slot before
     * it blocks and fills the reply in afterwards, so the request is already
     * gone from here by the time the reply arrives and the two never need to
     * exist at once.
     */
    struct message m;
    int status;

    if (!process_may_read(p, msg_ptr, sizeof(m))
        || !process_may_write(p, reply_ptr, sizeof(m))) {
        return SYS_ERR_FAULT;
    }

    /*
     * Copied in before the call and out after, rather than handing the
     * kernel's IPC path a pointer into the process. The process is not
     * running while its message is in flight, but a second thread in the
     * same address space would be, and the copy is what makes the message
     * the kernel acts on the one that was checked.
     *
     * Only the bytes in use, and the length is clamped first. It comes from
     * the process, so it is the one field to distrust before anything is
     * copied on the strength of it.
     */
    copy_message_in(&m, (const struct message *)msg_ptr);

    status = ipc_call(cap, &m, &m);

    if (status != IPC_OK) {
        return status;
    }

    copy_message_out((struct message *)reply_ptr, &m);
    return IPC_OK;
}

static long sys_receive(struct process *p, cap_t cap, uintptr_t msg_ptr,
                        uintptr_t sender_ptr)
{
    struct message msg;
    struct thread *sender = NULL;
    int status;

    if (!process_may_write(p, msg_ptr, sizeof(msg))
        || !process_may_write(p, sender_ptr, sizeof(uint64_t))) {
        return SYS_ERR_FAULT;
    }

    status = ipc_receive(cap, &msg, &sender);

    if (status != IPC_OK) {
        return status;
    }

    copy_message_out((struct message *)msg_ptr, &msg);

    /*
     * The sender goes back to EL0 as a kernel pointer, which is a leak: a
     * process learns where a struct thread lives. It is written down rather
     * than hidden because it is temporary. At M5 a reply is a capability
     * like everything else, and a process will hold an index into its own
     * table instead of an address it could never have guessed but can now
     * simply read.
     */
    *(uint64_t *)sender_ptr = (uint64_t)(uintptr_t)sender;
    return IPC_OK;
}

static long sys_reply(struct process *p, uintptr_t sender, uintptr_t msg_ptr)
{
    struct message msg;

    if (!process_may_read(p, msg_ptr, sizeof(msg))) {
        return SYS_ERR_FAULT;
    }

    copy_message_in(&msg, (const struct message *)msg_ptr);

    /*
     * `sender` is whatever the process passed. ipc_reply checks that it is a
     * thread actually waiting for a reply, which is what stops a forged
     * value from doing anything: the worst a process can do with a made-up
     * pointer is get IPC_ERR_NO_PEER back.
     *
     * That is thin, and it is the reason the token becomes a capability at
     * M5. A value that is only safe because of what the callee checks is
     * one audit away from not being safe.
     */
    return ipc_reply((struct thread *)sender, &msg);
}

void syscall_dispatch(struct trapframe *tf)
{
    struct process *p = process_current();
    unsigned long number = tf->x[8];
    long result;

    if (p == NULL) {
        /* An SVC from something that is not a process. Nothing issues one,
         * so reaching here means the vector routed something wrongly. */
        tf->x[0] = (uint64_t)(long)SYS_ERR_BADCALL;
        return;
    }

    switch (number) {
    case SYS_EXIT:
        /* Never returns. The thread and everything the process owned go
         * back to their pools. */
        process_exit(p, (int)tf->x[0]);
        return;

    case SYS_WRITE:
        result = sys_write(p, tf->x[0], (size_t)tf->x[1]);
        break;

    case SYS_GETCHAR:
        /*
         * One byte, or SYS_NO_INPUT when none is waiting. Non-blocking,
         * because the alternative is a syscall that parks a thread until the
         * UART interrupts, and there is no UART interrupt yet: the receive
         * path is polled until the terminal at M6 gives it a reason not to
         * be. The console server yields between polls, which costs a
         * scheduling slot rather than the machine.
         */
        if (!p->owns_console) {
            result = SYS_ERR_DENIED;
        } else {
            int c = hal_getchar();
            result = (c == HAL_NO_INPUT) ? SYS_NO_INPUT : (long)c;
        }
        break;

    case SYS_YIELD:
        thread_yield();
        result = 0;
        break;

    case SYS_ENDPOINT:
        result = ipc_endpoint_create();
        break;

    case SYS_CALL:
        result = sys_call(p, (cap_t)tf->x[0], tf->x[1], tf->x[2]);
        break;

    case SYS_RECEIVE:
        result = sys_receive(p, (cap_t)tf->x[0], tf->x[1], tf->x[2]);
        break;

    case SYS_REPLY:
        result = sys_reply(p, tf->x[0], tf->x[1]);
        break;

    default:
        /* An unknown number is an error, not a panic. A process must not be
         * able to stop the machine by guessing. */
        result = SYS_ERR_BADCALL;
        break;
    }

    /* Into the frame rather than into x0 directly: the eret restores every
     * register from here, so this is where a return value lives. */
    tf->x[0] = (uint64_t)result;
}
