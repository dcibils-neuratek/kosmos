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

#define PROCESS_MAX         8
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

    int               exit_code;
    bool              exited;
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
int process_wait(struct process *parent, unsigned *id);

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
