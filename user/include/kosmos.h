#ifndef KOSMOS_H
#define KOSMOS_H

#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

/*
 * What a process can reach.
 *
 * Every one of these is an `svc #0`, and there is nothing else: no shared
 * memory, no global tree, no ambient authority. A process reaches exactly
 * what it was handed, which is `design.md` §4.3's argument made concrete by
 * the fact that there is no other door.
 *
 * The stubs are inline assembly rather than a library call because a syscall
 * is not a function call: the arguments are already where the ABI wants them
 * and the only work is naming the number and issuing the instruction.
 */

static inline long sys0(long n)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys1(long n, long a)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static inline long sys2(long n, long a, long b)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}

static inline long sys3(long n, long a, long b, long c)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    __asm__ volatile("svc #0"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

__attribute__((noreturn))
static inline void kosmos_exit(int code)
{
    (void)sys1(SYS_EXIT, code);

    /* SYS_EXIT does not return. Spinning is better than running off the end
     * of whatever called this. */
    for (;;) { }
}

static inline long kosmos_write(const char *s, size_t len)
{
    return sys2(SYS_WRITE, (long)(uintptr_t)s, (long)len);
}

/* One byte from the console, or negative when nothing is waiting. Only the
 * process that owns the console may ask; everything else is refused. */
static inline long kosmos_getchar(void)
{
    return sys0(SYS_GETCHAR);
}

static inline void kosmos_yield(void)
{
    (void)sys0(SYS_YIELD);
}

/*
 * The message the kernel moves.
 *
 * Has to match kernel/ipc.h byte for byte: the syscall copies
 * sizeof(struct message) in each direction, and a disagreement about the
 * layout would be read as a disagreement about the contents. The two are one
 * definition written twice, which is the sort of thing that should be
 * checked rather than trusted - there is a _Static_assert on the size in
 * user/lib/sys_user.c.
 */
#define MSG_BYTES   2048

struct message {
    uint64_t tag;
    uint32_t cap_plus_one;      /* a capability travelling with it, +1 */
    uint32_t length;
    uint8_t  data[MSG_BYTES];
};

static inline long kosmos_endpoint(void)
{
    return sys0(SYS_ENDPOINT);
}

static inline long kosmos_call(long cap, const struct message *msg,
                               struct message *reply)
{
    return sys3(SYS_CALL, cap, (long)(uintptr_t)msg, (long)(uintptr_t)reply);
}

static inline long kosmos_receive(long cap, struct message *msg,
                                  uint64_t *sender)
{
    return sys3(SYS_RECEIVE, cap, (long)(uintptr_t)msg,
                (long)(uintptr_t)sender);
}

static inline long kosmos_reply(uint64_t sender, const struct message *msg)
{
    return sys2(SYS_REPLY, (long)sender, (long)(uintptr_t)msg);
}

/*
 * The address space a process is given. Fixed rather than negotiated,
 * because there is nothing yet to negotiate with; `design.md` §9.2's
 * manifest is what decides this at M5, and it will decide it per process.
 */
/*
 * The image header the kernel reads before mapping. Sixteen bytes: a magic
 * number, then how much of the image is read-only and executable.
 */
#define USER_IMAGE_MAGIC    0x534f4d534f4bUL
#define USER_IMAGE_HEADER   16

#define USER_TEXT       0x80000000UL
#define USER_HEAP       0x81000000UL
#define USER_HEAP_SIZE  (2UL * 1024 * 1024)     /* design.md 5.2: ~2 MB */
#define USER_STACK_END  0x82000000UL

#endif /* KOSMOS_H */
