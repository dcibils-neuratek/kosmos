/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_H
#define KOSMOS_H

#include <stddef.h>
#include <stdint.h>

#include "syscall.h"

/*
 * What a process can reach.
 *
 * Every one of these is one syscall instruction and there is nothing else:
 * no shared memory, no global tree, no ambient authority. A process reaches exactly
 * what it was handed, which is `design.md` §4.3's argument made concrete by
 * the fact that there is no other door.
 *
 * The stubs are inline assembly rather than a library call because a syscall
 * is not a function call: the arguments are already where the ABI wants them
 * and the only work is naming the number and issuing the instruction.
 *
 * **They are the only thing in this header that knows which machine it is
 * on**, and the one `#if` below is the only one in the whole userland: the
 * fifty thousand lines above this layer reach the kernel through `sys0` to
 * `sys5` and through nothing else, which is why moving them to a second
 * architecture is a header rather than a project.
 */

#if defined(__x86_64__)
#include "syscall-x86_64.h"
#else
#include "syscall-aarch64.h"
#endif


__attribute__((noreturn))
static inline void kosmos_exit(int code)
{
    (void)sys1(SYS_EXIT, code);

    /* SYS_EXIT does not return. Spinning is better than running off the end
     * of whatever called this. */
    for (;;) { }
}

/*
 * A run of text on the console, in a colour.
 *
 * `colour` is 0xAARRGGBB and zero means the console's own default, so
 * `kosmos_write` below is this with no opinion. The colour travels with the
 * bytes rather than being a mode: the console has several writers and a
 * set-print-restore pair from two of them at once puts one's colour on the
 * other's line. `kernel/console.h` has the argument in full.
 *
 * `sys3` and not `sys2`, even for the plain form, because the kernel now
 * reads a third register and an unset one holds whatever was left in it.
 */
static inline long kosmos_write_colour(const char *s, size_t len,
                                       unsigned long colour)
{
    return sys3(SYS_WRITE, (long)(uintptr_t)s, (long)len, (long)colour);
}

static inline long kosmos_write(const char *s, size_t len)
{
    return kosmos_write_colour(s, len, 0);
}

/* One byte from the console, or negative when nothing is waiting. Only the
 * process that owns the console may ask; everything else is refused. */
static inline long kosmos_getchar(void)
{
    return sys0(SYS_GETCHAR);
}

/*
 * The next key transition, or SYS_NO_INPUT when there is none.
 *
 * The companion to `kosmos_getchar`, and gated the same way: only the
 * process that owns the console may ask, because a process that can watch
 * every key is a keylogger. Everything else asks the console server.
 */
/*
 * Stop the machine (0) or start it again (1). Does not return when it
 * works, so a return is a refusal - see SYS_POWER.
 */
/*
 * One period of PCM: 44100 Hz, stereo, signed sixteen-bit little-endian.
 *
 * 0 when it was taken, SYS_NO_INPUT when the queue is full - which is a
 * fact rather than a failure. See SYS_SND_WRITE.
 */
static inline long kosmos_snd_write(const void *pcm, unsigned long bytes)
{
    return sys2(SYS_SND_WRITE, (long)(uintptr_t)pcm, (long)bytes);
}

/*
 * The network card: what it is, and frames both ways.
 *
 * Only the process holding the card may call these - `SPAWN_NET` - and what
 * crosses is an Ethernet frame with its header on it, exactly as it goes on
 * the wire. `SYS_NET_RECV` answers `SYS_NO_INPUT` when nothing is waiting,
 * which is not an error and is the usual answer.
 */
static inline long kosmos_net_info(struct netinfo *out)
{
    return sys1(SYS_NET_INFO, (long)(uintptr_t)out);
}

static inline long kosmos_net_send(const void *frame, unsigned long bytes)
{
    return sys2(SYS_NET_SEND, (long)(uintptr_t)frame, (long)bytes);
}

static inline long kosmos_net_recv(void *frame, unsigned long max)
{
    return sys2(SYS_NET_RECV, (long)(uintptr_t)frame, (long)max);
}

/* Periods the device has not finished with: the refill deadline, measured. */
static inline long kosmos_snd_queued(void)
{
    return sys0(SYS_SND_QUEUED);
}

static inline long kosmos_power(unsigned long what)
{
    return sys1(SYS_POWER, (long)what);
}

static inline long kosmos_key_event(unsigned *code, unsigned *down)
{
    return sys2(SYS_KEY_EVENT, (long)(uintptr_t)code, (long)(uintptr_t)down);
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
    /*
     * `sys4`, which is what this was spelled out as - the same five
     * registers and the same instruction, written again. It was the only
     * hand-written stub left in this header and the only line above the
     * syscall layer that named a register, which is how the second
     * architecture found it: everything else in fifty thousand lines
     * compiled for x86-64 unchanged and this did not.
     */
    return sys4(SYS_SPAWN, (long)arg, (long)(uintptr_t)caps,
                (long)ncaps, (long)flags);
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
 * Stop running for `ticks` scheduler ticks - a hundredth of a second each.
 *
 * The difference from `kosmos_yield` is the whole point: yielding goes to
 * the back of the band and comes straight back, so a loop around it is a
 * spin and the thread never stops being runnable. This one leaves the
 * runqueue entirely until the timer puts it back.
 */
static inline void kosmos_sleep(unsigned long ticks)
{
    (void)sys1(SYS_SLEEP, (long)ticks);
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

/*
 * A capability back. The region's pages survive while anyone else holds one,
 * so a server may drop what it was handed the moment it has finished.
 */
/* How the machine is scheduled, and changing it. */
static inline long kosmos_sched_info(void *out)
{
    return sys1(SYS_SCHED_INFO, (long)(uintptr_t)out);
}

static inline long kosmos_sched_set(long what, long value)
{
    return sys2(SYS_SCHED_SET, what, value);
}

static inline long kosmos_cap_drop(long cap)
{
    return sys1(SYS_CAP_DROP, cap);
}

/*
 * 0 while the capability names an endpoint or a region, negative once what
 * it named has gone - and an endpoint goes with the process that made it.
 * Asked without using it: nothing is sent, received or dropped.
 */
static inline long kosmos_cap_check(long cap)
{
    return sys1(SYS_CAP_CHECK, cap);
}

/*
 * A shared region out of the share window. The pages are the region's and
 * are not freed here; this is losing sight of them, not disposing of them.
 */
static inline long kosmos_share_unmap(unsigned long address,
                                      unsigned long pages)
{
    return sys2(SYS_SHARE_UNMAP, (long)address, (long)pages);
}

static inline long kosmos_call(long cap, const struct message *msg,
                               struct message *reply)
{
    return sys3(SYS_CALL, cap, (long)(uintptr_t)msg, (long)(uintptr_t)reply);
}

/* `nonblocking` returns SYS_NO_MESSAGE rather than parking when nobody is
 * waiting - for a server that has something else to be getting on with. */
/*
 * `timeout` is in scheduler ticks: 0 waits for ever, anything else gives up
 * and returns SYS_NO_MESSAGE when nothing has arrived by then.
 *
 * The combination is what a server loop actually wants - answer whoever
 * calls, but be back by the next deadline whether or not anybody did. A
 * server that sleeps on a timer instead cannot answer while it sleeps, and
 * every one of its callers pays a tick.
 */
static inline long kosmos_receive(long cap, struct message *msg,
                                  uint64_t *sender, int nonblocking,
                                  unsigned long timeout)
{
    return sys5(SYS_RECEIVE, cap, (long)(uintptr_t)msg,
                (long)(uintptr_t)sender, nonblocking ? 1 : 0, (long)timeout);
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

/* What the machine was started with, from the firmware. Zero when there is
 * no such option, which is the ordinary case. */
static inline long kosmos_boot_option(const char *name, char *out,
                                      unsigned long max)
{
    return sys3(SYS_BOOT_OPT, (unsigned long)(uintptr_t)name,
                (unsigned long)(uintptr_t)out, max);
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

/* Read (0) or set how far the pointer moves per count. Answers the speed
 * in force, or zero on a board whose pointer is absolute. */
static inline long kosmos_pointer_speed(unsigned units)
{
    return sys1(SYS_PTR_SPEED, (long)units);
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

/*
 * Where the image, the heap and the stack are, and **the base comes from
 * the build** rather than being written here a third time.
 *
 * It was `0x80000000` on this line, matching `USER_VA_BASE` in the kernel's
 * `arch/<name>/mmu.h` and `. = 0x80000000` in `user/user.ld` - three copies
 * of one number, agreeing because nobody had changed one. The comment
 * below already records what that shape cost once, with the heap size. The
 * second architecture is what changed one: x86-64 puts a process at 1 GB,
 * because 0x80000000 is exactly the first address its default code model
 * cannot reach.
 *
 * What that looked like: every one of the twelve boot stages passed, init
 * started at ring 3, and the first thing it touched was `0x81000018` - the
 * old base plus the heap offset - in an address space where the heap is at
 * `0x41000000`.
 *
 * So the Makefile computes it once and hands it to the compiler and the
 * linker both. Two places still know the number, and they are the
 * irreducible two: a linker script cannot read a C header. An `#error`
 * rather than a default, because a default is what silently produced the
 * fault above.
 */
#ifndef KOSMOS_USER_BASE
#error "the build must say where a process is mapped: -DKOSMOS_USER_BASE"
#endif

#define USER_TEXT       ((unsigned long)KOSMOS_USER_BASE)
#define USER_HEAP       (USER_TEXT + 0x01000000UL)
/*
 * The heap's size, and it must be the same number the *kernel* used.
 *
 * `kernel/process.h` maps `USER_HEAP_PAGES` pages at this address, and
 * `heap_init` here is told how much of it to manage. These were two
 * independent constants - 512 pages there and a literal 2 MB here - and
 * they agreed only because nobody had ever changed one.
 *
 * Changing one is exactly what `make DOOM=1` used to do, and what that
 * looked like was Doom saying "Unable to allocate 5 MiB of RAM for zone" on
 * a process whose kernel-side mapping was twelve megabytes: the pages were
 * there and the allocator had been told they were not. The other direction
 * is worse and is what x86-64 shipped for a day - the userland told it had
 * twelve megabytes of which the kernel mapped two, so `malloc` handed out
 * addresses inside its own arena that were not mapped, and never called
 * `grow()` because it believed it still had room.
 *
 * **Nothing overrides it any more.** `runtime/libc/malloc.c` grows the heap
 * by asking the kernel for another arena, so the size here is where a
 * process *starts* rather than what it is limited to, and the default is
 * what every process gets. Kept as a `#ifndef` rather than including
 * `kernel/process.h`, because that header is the kernel's own structures
 * and userland has no business seeing them - the shared fact is one number,
 * so one number is what is shared.
 */
#ifndef USER_HEAP_PAGES
#define USER_HEAP_PAGES 512                     /* design.md 5.2: ~2 MB */
#endif

#define USER_HEAP_SIZE  ((unsigned long)USER_HEAP_PAGES * 4096UL)
#define USER_STACK_END  (USER_TEXT + 0x02000000UL)

#endif /* KOSMOS_H */
