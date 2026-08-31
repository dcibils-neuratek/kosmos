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

static inline long sys4(long n, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
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

/*
 * Makes another process from this one's image.
 *
 * `caps` are indices in *this* process's table; the child gets its own, in
 * the same order. A parent cannot pass what it does not hold. Returns the
 * child's id, or negative.
 */
static inline long kosmos_spawn(unsigned long arg, const int *caps,
                                unsigned long ncaps, unsigned long flags)
{
    register long x8 __asm__("x8") = SYS_SPAWN;
    register long x0 __asm__("x0") = (long)arg;
    register long x1 __asm__("x1") = (long)(uintptr_t)caps;
    register long x2 __asm__("x2") = (long)ncaps;
    register long x3 __asm__("x3") = (long)flags;
    __asm__ volatile("svc #0"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return x0;
}

/*
 * Blocks until any child ends, and returns its exit code. Negative when
 * there are no children left to wait for.
 *
 * `nonblocking` returns SYS_NO_CHILD_READY instead of waiting when children
 * exist but none has exited - which is what draining them looks like.
 */
static inline long kosmos_wait(uint64_t *id, int nonblocking)
{
    return sys2(SYS_WAIT, (long)(uintptr_t)id, nonblocking ? 1 : 0);
}

static inline void kosmos_yield(void)
{
    (void)sys0(SYS_YIELD);
}

/*
 * The monotonic counter, in CNTFRQ_EL0 ticks.
 *
 * Not a date. It counts from whenever the machine started and is only good
 * for measuring how long something took; `design.md` §4.4's `/dev/clock` is
 * where a date comes from, and it is a capability rather than this.
 */
static inline unsigned long kosmos_ticks(void)
{
    return (unsigned long)sys0(SYS_TICKS);
}

/*
 * Where the screen is in this process, and how big.
 *
 * `struct screen_info` comes from kernel/syscall.h, which this header
 * already includes for the numbers: it is the ABI, so it belongs to both
 * sides by definition and is written once rather than twice.
 *
 * Negative when this process was not handed the screen, which is the normal
 * answer for every process but one.
 */
static inline long kosmos_screen(struct screen_info *out)
{
    return sys1(SYS_SCREEN, (long)(uintptr_t)out);
}

/*
 * The machine, and how much of it is in use. See `struct sysinfo` in
 * kernel/syscall.h: raw ID registers plus pool counts, decoded by whoever
 * asked rather than by the kernel.
 */
static inline long kosmos_sysinfo(struct sysinfo *out)
{
    return sys1(SYS_SYSINFO, (long)(uintptr_t)out);
}

/*
 * Pages of this process's own, for the things that do not fit on its heap.
 *
 * The heap is 2 MB and deliberately so; a full-screen surface is 3.2 MB.
 * These come straight from the kernel, zeroed, mapped only here, and are
 * returned when this process exits whether or not it remembers to unmap
 * them. Negative on refusal - there is a per-process budget, because one
 * process asking for everything is the failure this stops.
 */
static inline long kosmos_map(unsigned long pages)
{
    return sys1(SYS_MAP, (long)pages);
}

static inline long kosmos_unmap(unsigned long address, unsigned long pages)
{
    return sys2(SYS_UNMAP, (long)address, (long)pages);
}

/* A process says what it is. The kernel names nothing: a spawned child
 * inherits its parent's name, so without this every process is "init". */
static inline long kosmos_setname(const char *name, unsigned long len)
{
    return sys2(SYS_SETNAME, (long)(uintptr_t)name, (long)len);
}

/* Every process, into `out`. Returns how many were written. */
static inline long kosmos_proctable(struct proc_info *out, unsigned long max)
{
    return sys2(SYS_PROCTABLE, (long)(uintptr_t)out, (long)max);
}

/* The granule those come in. Has to match PAGE_SIZE in the kernel; it is the
 * unit the syscall counts in. */
#define KOSMOS_PAGE_SIZE    4096UL

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

/*
 * Destroys an endpoint and wakes everything blocked on it with an error.
 * Only one this process holds: the index is resolved against its own table.
 */
static inline long kosmos_endpoint_destroy(long cap)
{
    return sys1(SYS_ENDPOINT_DESTROY, cap);
}

static inline long kosmos_call(long cap, const struct message *msg,
                               struct message *reply)
{
    return sys3(SYS_CALL, cap, (long)(uintptr_t)msg, (long)(uintptr_t)reply);
}

/* `nonblocking` returns SYS_NO_MESSAGE rather than parking when nobody is
 * waiting - for a server that has something else to be getting on with. */
static inline long kosmos_receive(long cap, struct message *msg,
                                  uint64_t *sender, int nonblocking)
{
    return sys4(SYS_RECEIVE, cap, (long)(uintptr_t)msg,
                (long)(uintptr_t)sender, nonblocking ? 1 : 0);
}

/*
 * A region of memory two processes can share, named by a capability.
 *
 * Create, then send the capability in a message; the far side maps it. The
 * kernel translates the index exactly as it does for an endpoint, so what
 * arrives is the receiver's own name for the same pages and nobody else can
 * refer to them.
 */
static inline long kosmos_mem_create(unsigned long pages)
{
    return sys1(SYS_MEM_CREATE, (long)pages);
}

static inline long kosmos_mem_map(long cap)
{
    return sys1(SYS_MEM_MAP, cap);
}

static inline long kosmos_mem_size(long cap)
{
    return sys1(SYS_MEM_SIZE, cap);
}

/*
 * The disk, in sectors.
 *
 * `kosmos_disk_info` is readable by anybody: it says whether there is a
 * device and how big, which is not authority over it. The other two need
 * the grant, and exactly one process has it.
 */
static inline long kosmos_disk_info(struct diskinfo *out)
{
    return sys1(SYS_DISK_INFO, (unsigned long)(uintptr_t)out);
}

static inline long kosmos_disk_read(unsigned long sector, void *buf,
                                    unsigned long bytes)
{
    return sys3(SYS_DISK_READ, sector, (unsigned long)(uintptr_t)buf, bytes);
}

static inline long kosmos_disk_write(unsigned long sector, const void *buf,
                                     unsigned long bytes)
{
    return sys3(SYS_DISK_WRITE, sector, (unsigned long)(uintptr_t)buf, bytes);
}

/* The most recent bytes this machine printed, kernel and processes alike. */
static inline long kosmos_log(char *out, unsigned long max)
{
    return sys2(SYS_LOG, (long)(uintptr_t)out, (long)max);
}

/*
 * Sleeps until input arrives or `ticks` scheduler ticks have passed.
 *
 * Scheduler ticks, at TICK_HZ, and not the counter `kosmos_ticks` returns -
 * the two differ by a factor of six hundred thousand on this machine, and
 * the wrong one is a sleep of several hours that reads as a hang.
 */
static inline long kosmos_wait_input(unsigned long ticks)
{
    return sys1(SYS_WAIT_INPUT, (long)ticks);
}

/* Ends a child. Takes effect at that process's next entry into the kernel,
 * which is at most one timer period away. */
static inline long kosmos_kill(unsigned long id)
{
    return sys1(SYS_KILL, (long)id);
}

/* Takes the screen from the kernel console, or gives it back. */
static inline long kosmos_screen_take(int take)
{
    return sys1(SYS_SCREEN_TAKE, take ? 1 : 0);
}

static inline long kosmos_pointer(struct pointer_info *out)
{
    return sys1(SYS_POINTER, (long)(uintptr_t)out);
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
