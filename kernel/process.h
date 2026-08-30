#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdbool.h>
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
 * Where a process's own memory starts, and how it is laid out.
 *
 * Code at the bottom of the user region, then a gap, then the stack. The gap
 * is not decoration: it means a stack that grows past its guard runs into
 * nothing rather than into the code that is running.
 */
#define USER_TEXT_VA    USER_VA_BASE
#define USER_STACK_TOP  (USER_VA_BASE + 0x00100000UL)   /* 1 MB above */
#define USER_STACK_PAGES 4

struct process {
    bool              in_use;
    unsigned          id;
    char              name[PROCESS_NAME_MAX];

    struct addrspace *space;
    struct thread    *thread;

    /* Physical pages backing the image and the stack, so they can be
     * returned when the process dies. */
    void             *text_pages;
    size_t            text_page_count;
    void             *stack_pages;

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
struct process *process_create(const char *name, const void *code, size_t len);

/* The process the current thread belongs to, or NULL in a kernel thread. */
struct process *process_current(void);

/*
 * Ends a process: unmaps and frees everything it owned, and ends its thread.
 * Called from the syscall path on exit, and from the fault path when a
 * process does something it may not. Never returns if it is the caller's own.
 *
 * The slot itself survives, holding the exit code, until somebody reaps it.
 * Everything expensive is already gone by then, so what is left is a few
 * bytes recording how it ended. Freeing it at the same moment would mean the
 * only record of why a process died disappears at the instant it dies, which
 * is exactly when somebody wants to look.
 */
void process_exit(struct process *p, int code);

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
