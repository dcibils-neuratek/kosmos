#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdbool.h>
/* For USER_VA_BASE, which the address macros below expand to. A header
 * whose macros do not compile on their own is a header that works only in
 * the order it happens to be included. */
#include "mmu.h"
#include <stddef.h>
#include <stdint.h>

struct addrspace;
struct thread;

/*
 * A process: an address space, a thread running in it at EL0, and the
 * capabilities it was handed.
 *
 * `design.md` §2's isolation comes from the hardware, not from the language.
 * A process runs at EL0 with its own page table root, and every kernel
 * mapping is AP=00, which is EL1 read/write and no EL0 access at all. Lua
 * inside it is not sandboxed and does not need to be: if it breaks its own
 * language sandbox it breaks itself, and nothing else.
 *
 * One thread per process for now. Several is a scheduler question rather
 * than a new mechanism, and nothing needs it yet.
 */

/*
 * How many processes there can be, ever.
 *
 * Eight until M6, which was enough for init, three servers and a shell with
 * two to spare. The app server needs more than that on its own: a compositor
 * and a window manager, then a terminal, an inspector and whatever is being
 * looked at. Thirty-two leaves room to run out of something more interesting
 * than slots.
 *
 * The cost is not the slot, which is 128 bytes of .bss. It is what a process
 * is made of, and that is charged only when one exists:
 *
 *   image     a private copy, so it can be mapped read-only and executable
 *             without the original being. About 240 KB today.
 *   heap      2 MB, fixed, because `design.md` §5.2 wants a bounded one: a
 *             small heap collects quickly and the maximum GC pause is what
 *             decides whether the system stutters.
 *   stack     64 KB.
 *
 * So roughly 2.3 MB a process, and thirty-two of them would be 74 MB of the
 * 512 this machine has. That is a real number rather than a comfortable one,
 * and it is the heap that dominates - which is the same 2 MB that stops a
 * full-screen surface fitting in one. Both get solved by the same change.
 */
#define PROCESS_MAX         32
#define PROCESS_NAME_MAX    16

/*
 * Where a process's own memory sits. Has to agree with user/include/kosmos.h
 * and user/user.ld; they are two halves of one contract.
 *
 * The gaps between the regions are the point. A stack that grows past its
 * end, or a heap that runs off its top, lands in unmapped space and faults
 * rather than in whatever happened to be next.
 */
#define USER_TEXT_VA     USER_VA_BASE                       /* 0x80000000 */
#define USER_HEAP_VA     (USER_VA_BASE + 0x01000000UL)      /* 0x81000000 */
#define USER_HEAP_PAGES  512                                /* 2 MB       */
#define USER_STACK_TOP   (USER_VA_BASE + 0x02000000UL)      /* 0x82000000 */
#define USER_STACK_PAGES 16                                 /* 64 KB      */

/*
 * Where the framebuffer lands in a process that holds the screen.
 *
 * Above the stack and nowhere near it. Three megabytes of it, mapped from
 * the same physical pages the board is scanning out - not a copy, because a
 * copy would need somewhere to put three megabytes and would then need
 * flushing, and the whole point of a linear framebuffer is that there is
 * nothing between the write and the screen.
 */
#define USER_SCREEN_VA   (USER_VA_BASE + 0x03000000UL)      /* 0x83000000 */

/*
 * Where SYS_MAP puts pages a process asks for, growing upward.
 *
 * A surface does not fit in the heap and must not: the heap is 2 MB on
 * purpose, because `design.md` §5.2 wants collections short and the maximum
 * GC pause is what decides whether the system stutters. A full-screen
 * surface is 3.2 MB, and a compositor's backbuffer is full-screen by
 * definition. Putting them on the Lua heap would mean either a heap too
 * large to collect quickly or a compositor that cannot exist.
 *
 * The address is bumped and never reused, up to `USER_MAP_END`. There is
 * 512 GB of address space below the 39-bit limit and a surface is a few
 * megabytes, so a process would have to allocate tens of thousands of them
 * to run out of *space* while the pages themselves are returned and reused
 * normally. Tracking freed ranges to reuse addresses would need an
 * allocator, which is the thing this kernel does not have.
 */
#define USER_MAP_VA      (USER_VA_BASE + 0x04000000UL)      /* 0x84000000 */

/*
 * How much a process may map this way, in pages.
 *
 * 48 MB, and it was 16 MB until a maximised window at 1920x1080 hit it.
 *
 * The old number said "enough for a double-buffered full-screen surface
 * with room over", and that was true of the screen it was written for. A
 * full screen at 1024x768 is 3 MB; at 1920x1080 it is 8.3 MB, so the
 * compositor's backbuffer plus one maximised window is 4009 pages against a
 * 4096 cap - over by less than one per cent, which is the worst way for a
 * limit to be wrong because it works everywhere except the case you built
 * the system for.
 *
 * The reasoning that came with the old number does not survive either. It
 * was that thirty-two processes at 16 MB is 512 MB, which is all the RAM -
 * so the cap was set where every process could take all of it at once. That
 * is sizing for a case that never happens, and it is not what stops memory
 * being exhausted: `pmm_alloc_page` returning NULL is. This is a guard
 * against one process running away, and the number to pick is a generous
 * multiple of what the largest legitimate one needs - a compositor wanting
 * four full screens at 1080p.
 */
#define USER_MAP_PAGES_MAX  12288

/*
 * Where a shared region lands, and deliberately not in the window above.
 *
 * The two windows differ in who owns the pages, which is the whole reason
 * they cannot be one window. A page in the SYS_MAP window was allocated by
 * this process and belongs to it, so `release_memory` walks that range on
 * the way out and hands back whatever is still mapped, and `SYS_UNMAP`
 * lets the process do the same by hand. A page in *this* window belongs to
 * a `memobj` that some other process may also have mapped, and is freed
 * only when the last capability to it is dropped.
 *
 * Sharing one window makes the exit path free pages it does not own: two
 * processes mapping one region both free its pages, and the second one
 * panics the machine with a double free. That is how this was found. The
 * quieter half of the same bug is worse - `SYS_UNMAP` would let a process
 * hand a region's pages back to the allocator while the other process is
 * still drawing into them, which is a use-after-free reachable from Lua.
 *
 * So the rule is a range and not a flag: the ranges say who frees what, and
 * the two pieces of code that free pages by walking a range are bounded by
 * the window whose pages they own. `as_destroy` still tears the page tables
 * down, which is all that has to happen here - the mapping goes, the pages
 * stay, and `memobj_unref` decides their fate.
 */
#define USER_SHARE_VA    (USER_VA_BASE + 0x100000000UL)     /* 0x180000000 */

/*
 * Where each bump pointer has to stop.
 *
 * Neither address is reused, so both walk upwards for the life of the
 * process, and a process that maps and unmaps in a loop walks a long way.
 * Without a ceiling the SYS_MAP pointer eventually reaches the shared
 * window and starts handing out addresses that belong to it - and then the
 * exit walk, bounded by `next_map`, frees a region's pages after all, which
 * is the bug this window was added to prevent, arriving by a longer road.
 *
 * 4 GB each. A surface is a few megabytes, so this is tens of thousands of
 * them before a long-lived process is told no; the 39-bit address space is
 * 512 GB, so the room costs nothing.
 */
#define USER_MAP_END     (USER_VA_BASE + 0x100000000UL)     /* 0x180000000 */
#define USER_SHARE_END   (USER_VA_BASE + 0x200000000UL)     /* 0x280000000 */

/*
 * The image header. Sixteen bytes at the front: a magic number, then how
 * many bytes are read-only and executable. Without the second field the
 * kernel could only map an image one way, and one way that works for both
 * code and data is writable and executable, which is W^X thrown away for
 * want of a number.
 */
#define USER_IMAGE_MAGIC  0x534f4d534f4bUL
#define USER_IMAGE_HEADER 16

/*
 * **Only `exited` and `exit_code` are safe to read once a process is
 * running.** Everything else - the address space above all - is torn down by
 * process_exit, and a process runs whenever the scheduler says so. Reading
 * `space` after creating a process is reading a pointer another thread is
 * entitled to have already freed.
 *
 * That was found by a test doing exactly that and faulting on a NULL
 * address space once in five runs. The lock that would make this safe goes
 * here; until there is one, anything inspecting a live process has to mask
 * interrupts, which is what stops it being preempted mid-look.
 */
struct process {
    bool              in_use;
    unsigned          id;
    char              name[PROCESS_NAME_MAX];

    struct addrspace *space;
    struct thread    *thread;

    /* The physical pages behind each region, so they can be returned when
     * the process dies. */
    void             *image_pages;
    size_t            image_page_count;
    void             *heap_pages;
    void             *stack_pages;

    unsigned long     arg;      /* the one word it is told at entry */

    /*
     * The image this process was made from, so it can make another like
     * itself. One image exists today and the kernel could keep it in a
     * global; recording it per process is what makes a second image a
     * change to whoever loads them rather than to spawn.
     */
    const void       *image;
    size_t            image_len;

    /* Who spawned it, and the thread of theirs waiting for it to end. A
     * process with no parent was started by whoever is playing init. */
    struct process   *parent;
    struct thread    *waiter;

    /*
     * Whether this process may touch the serial port.
     *
     * A boolean standing in for a capability, and it is written down as such
     * rather than dressed up. A device should be reached the way everything
     * else is - by name, through a capability the process was handed - and
     * that needs a capability that names a device rather than an endpoint.
     * Until then this enforces the property that matters: exactly one
     * process owns the console, and everything else has to ask it.
     *
     * Without it the console server is ceremony: any process could print, so
     * nothing would depend on going through it, and the design would be
     * demonstrated by convention rather than by the machine.
     */
    bool              owns_console;

    /*
     * The screen. A boolean like the console and temporary for the same
     * reason: a device should be named by a capability the process holds,
     * and that needs a capability that names a device rather than an
     * endpoint. Until then, whoever spawns decides, which is at least the
     * right shape - authority flows from parent to child and never sideways.
     */
    bool              owns_screen;

    /* Raw sectors. The strongest grant in the system - it is every file on
     * the machine, under every namespace - so it goes to one process. */
    bool              owns_disk;

    /* May end any process, not only its own children. One process holds
     * this: the task manager. */
    bool              owns_procctl;

    /*
     * Pages this process asked for with SYS_MAP: where the next one goes,
     * and how many it holds. The count is both the budget and what
     * `release_memory` walks to give them back - a process that exits
     * without unmapping must not leak them.
     */
    uintptr_t         next_map;
    size_t            mapped_pages;


    /*
     * The same for shared regions, in the window that says the pages are
     * not this process's to free. Counted separately as well as mapped
     * separately: a region's pages are already charged to whoever created
     * it, and charging every process that maps it would mean two processes
     * sharing one surface pay for it twice.
     */
    uintptr_t         next_share;

    /*
     * Timer ticks this process was charged, kept here as well as on its
     * thread because the thread goes when the process exits and the number
     * does not stop being true.
     *
     * Reporting the thread's directly meant an exited process read as zero
     * ticks, and anything computing a delta against an earlier reading got
     * a negative percentage - which is how `htop` came to show -939%.
     */
    unsigned long     ticks;

    int               exit_code;
    bool              exited;

    /*
     * Somebody has ended this process; it has not noticed yet.
     *
     * A process cannot be torn down from another thread's context -
     * `process_exit` panics if it is not the running one, and it has to,
     * because the teardown ends with the thread that calls it. So a kill
     * marks, and the process dies on its own next entry into the kernel:
     * a syscall, or the timer interrupt, whichever comes first.
     *
     * That bounds the wait at one timer period even for a process that has
     * stopped making syscalls entirely, which is exactly the case a kill is
     * for - `/bin/spin.lua` is an EL0 loop that yields to nothing.
     */
    bool              killed;
};

/* Prepares the pool. Once, before any process exists. */
void process_init(void);

/*
 * Creates a process from a blob of position-independent user code, copied
 * into fresh pages and mapped at USER_TEXT_VA. Returns NULL when the pool is
 * full or there are no pages.
 *
 * The blob is copied rather than mapped in place because the image's own
 * pages are the kernel's, mapped EL1-only, and shared by every process. A
 * process gets its own copy so it can be given EL0 permissions without
 * handing them to anybody else.
 */
/*
 * Builds a process but does not start it. Its capabilities are granted after
 * this and before process_start, which is the only order that works: a
 * runnable process runs, and a process that runs before it has been handed
 * anything finds an empty capability table and exits.
 */
struct process *process_create(const char *name, const void *image,
                               size_t len, unsigned long arg);

/*
 * Hands this process the serial port. Called before process_start, by
 * whoever is playing init.
 *
 * Nothing stops two processes being given it, and nothing should have to:
 * whoever hands out devices decides, and that is the whole of the model. In
 * the running system exactly one process gets it, and their output would
 * interleave if two did.
 */
void process_grant_console(struct process *p);

/*
 * Hands a process the screen: marks it the owner and maps the framebuffer
 * into its address space at USER_SCREEN_VA.
 *
 * Must be called before process_start, like every other capability - a
 * process that is runnable is running, and one that starts before its
 * mappings are in place faults on the first pixel.
 *
 * False when there is no display, or when the mapping did not fit. Not a
 * panic: a machine booted without a screen is a supported way to run and the
 * caller decides what to do about it.
 */
bool process_grant_screen(struct process *p);

/*
 * Hands a process the disk. Like the console and unlike the screen, there is
 * nothing to map: the sectors are reached through syscalls, so this is a
 * flag and no more.
 *
 * False when there is no block device, which the caller is expected to
 * survive rather than treat as fatal.
 */
bool process_grant_disk(struct process *p);

/* Hands a process authority over every other one. Like the console, this is
 * a flag and nothing to map. Always succeeds; there is no device to be
 * absent. */
void process_grant_procctl(struct process *p);


/* Makes it runnable. Nothing may touch its address space afterwards without
 * masking interrupts: from here it can exit at any moment. */
void process_start(struct process *p);

/* The process the current thread belongs to, or NULL in a kernel thread. */
struct process *process_current(void);

/*
 * Ends the *calling* process: unmaps and frees everything it owned, and ends
 * its thread. Never returns.
 *
 * It is only ever correct for the process the current thread is running.
 * Calling it on another one would end the caller's thread rather than the
 * target's, which is a mistake that reads as cleanup and behaves as suicide.
 * To discard a process that was built and never started, use
 * process_abandon.
 *
 * The slot itself survives, holding the exit code, until somebody reaps it.
 * Everything expensive is already gone by then, so what is left is a few
 * bytes recording how it ended. Freeing it at the same moment would mean the
 * only record of why a process died disappears at the instant it dies, which
 * is exactly when somebody wants to look.
 */
void process_exit(struct process *p, int code);

/*
 * A child of `parent`, from the same image, with `arg` as its boot word.
 * Not started: the caller grants its capabilities first, exactly as the
 * kernel does for its own.
 */
struct process *process_spawn(struct process *parent, unsigned long arg);

/*
 * Waits for any exited child. Blocks until one has.
 *
 * Returns its exit code and fills `*id`, or a negative result when the
 * caller has no children at all. The child is reaped, so a supervisor that
 * waits in a loop does not have to remember to.
 */
/*
 * Waits for a child and returns its exit code, reaping it.
 *
 * `nonblocking` makes it return -2 when children exist but none has exited,
 * which is distinct from -1 for no children at all. A shell draining the
 * processes it spawned needs to stop without being told it has none.
 */
int process_wait(struct process *parent, unsigned *id, bool nonblocking);

/*
 * Ends a child. Returns 0, or an error when `id` is not this process's
 * child.
 *
 * Only a parent may, which is the same authority `process_wait` already
 * implies and needs no new one: a process that started something may end it,
 * and nothing else may. It does not take effect here - see `killed`.
 */
int process_kill(struct process *parent, unsigned id);

/* Ends any process, for a caller the kernel has granted SPAWN_PROCCTL.
 * Refuses init: ending it ends the system. */
int process_kill_any(unsigned id);

/* Whether the running process has been killed and should now exit. Asked on
 * the way out of the kernel, which is the only safe moment. */
bool process_should_die(void);

/*
 * Discards a process that was created and never started.
 *
 * The counterpart to process_exit, for the caller cleaning up after
 * something that has not run: it frees the same memory without ending
 * anybody's thread.
 */
void process_abandon(struct process *p);

/* Releases an exited process's slot. Until this, `exited` and `exit_code`
 * can be read. Reaping a process that has not exited does nothing. */
void process_reap(struct process *p);

unsigned process_count(void);

/* Slots occupied, including processes that have exited and not been waited
 * for. Always at least process_count(); the difference is what has not been
 * reaped, and it is what actually runs the pool out. */
unsigned process_slots_used(void);

struct proc_info;

/* Fills `out` with up to `max` entries, one per occupied slot, and returns
 * how many. */
unsigned process_table(struct proc_info *out, unsigned max);

/* A process says what it is. Truncated to fit and stripped of anything
 * unprintable. */
void process_set_name(struct process *p, const char *name, size_t len);

/*
 * Whether a process may read, or write, `len` bytes at `va`.
 *
 * Every pointer a syscall is handed has to go through this. A process that
 * passes a kernel address is not misbehaving in a way the MMU catches: the
 * kernel dereferences it at EL1, where the mapping is perfectly valid, and
 * the check is the only thing standing between a syscall and an arbitrary
 * read of kernel memory. `design.md` §4.3's whole argument is that a process
 * reaches exactly what it was handed.
 */
bool process_may_read(const struct process *p, uintptr_t va, size_t len);
bool process_may_write(const struct process *p, uintptr_t va, size_t len);

#endif /* KERNEL_PROCESS_H */
