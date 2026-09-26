/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "process.h"
#include "irq.h"
#include "spinlock.h"
#include "screen.h"
#include "syscall.h"
#include "sched.h"
#include "thread.h"
#include "ipc.h"
#include "mmu.h"
#include "page.h"
#include "pmm.h"
#include "panic.h"
#include "console.h"
#include "hal.h"
#include "pool.h"


/*
 * **The pool, which grows** (`kernel/pool.h`, `threads.md` step 1b).
 *
 * Thirty-two slots until 19 September 2026, and spawning once failed at
 * twenty-six. A slot for every megabyte of memory now, thirty-two made at
 * boot: a process costs about 2.3 MB when it exists (`process.h`), so memory
 * runs out long before slots do, and it is the machine that says no.
 */
static struct pool processes;
static unsigned next_id = 1;

static struct process *proc(unsigned i)
{
    return pool_at(&processes, i);
}

static unsigned procs_made(void)
{
    return pool_slots(&processes);
}

unsigned process_ceiling(void)
{
    return pool_ceiling(&processes);
}

void process_init(void)
{
    /* A zeroed slot is `in_use == false`: nothing to prepare. */
    pool_init(&processes, "processes", sizeof(struct process),
              pool_ceiling_for(PROCESS_RAM_EACH, PROCESS_BOOT_SLOTS),
              PROCESS_BOOT_SLOTS, NULL);

    /* Never fewer spaces than processes: see `as_pool_init`. */
    as_pool_init(pool_ceiling(&processes) + ADDRSPACE_SPARE,
                 PROCESS_BOOT_SLOTS + ADDRSPACE_SPARE);
}

struct process *process_current(void)
{
    /* Whichever process this thread is running. Not a global: with more than
     * one process a global names whichever ran last, which is the wrong
     * answer in exactly the situation the question is asked. */
    struct thread *t = thread_current();

    return (t != NULL) ? t->process : NULL;
}

unsigned process_count(void)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < procs_made(); i++) {
        if (proc(i)->in_use && !proc(i)->exited) {
            n++;
        }
    }

    return n;
}

/*
 * Slots occupied, which is not the same as processes running.
 *
 * A process that has exited keeps its slot until somebody waits for it -
 * that is what makes an exit code readable after the fact. `process_count`
 * deliberately does not count those, and reporting only that number was a
 * bug of exactly the kind ADDRSPACE_MAX was: spawning failed at twenty-six
 * while every report said five of thirty-two were in use, because twenty-one
 * slots were held by processes nobody had waited for and nothing counted
 * them.
 *
 * So both numbers are reported now. A pool that says fewer are in use than
 * are occupied is a pool that will run out for reasons nobody can see.
 */
/*
 * Every process, as a table. See `struct proc_info`.
 *
 * The kernel fills in what it knows and interprets none of it: a name is
 * whatever the process called itself, and what that name *means* is decided
 * where the tables live, in Lua.
 */
unsigned process_table(struct proc_info *out, unsigned max)
{
    unsigned i, n = 0;

    for (i = 0; i < procs_made() && n < max; i++) {
        struct process *p = proc(i);

        if (!p->in_use) {
            continue;
        }

        out[n].id        = p->id;
        out[n].state     = (p->thread != NULL) ? (uint32_t)p->thread->state : 0;
        out[n].exited    = p->exited ? 1u : 0u;
        out[n].exit_code = (int32_t)p->exit_code;
        /* From the thread while it exists, from the process afterwards. */
        if (p->thread != NULL) {
            p->ticks = p->thread->ticks;
        }

        out[n].ticks = p->ticks;
        /*
         * Everything the process holds, not only what it asked for.
         *
         * `mapped_pages` is the surfaces and buffers it took with SYS_MAP,
         * and reporting only that says zero for every process that has not
         * taken one - which is most of them, and none of which are using no
         * memory. The heap and the stacks are the same for everybody.
         *
         * **`image_page_count` is the writable half of the image, which is
         * the only half a process has to itself.** The read-only half is one
         * set of pages mapped into every address space, so charging each of
         * them 2.8 MB for it would be the same memory counted sixteen times
         * - and this column is what somebody reads to find out which process
         * is the expensive one.
         *
         * Shared regions are left out for the same reason, with one
         * difference worth stating: a region *is* charged, to whoever
         * created it, so the total is still somewhere. The read-only image
         * is charged to nobody, because nobody asked for it. `mem` reports
         * what the machine actually holds and is where the two reconcile.
         */
        out[n].pages     = (uint32_t)p->mapped_pages;
        out[n].held      = (uint32_t)(p->mapped_pages + p->image_page_count
                                      + USER_HEAP_PAGES + USER_STACK_PAGES);
        out[n].caps      = captable_count(&p->caps);
        out[n].priority  = (p->thread != NULL)
                           ? thread_effective_priority(p->thread)
                           : 0u;

        /* Its home, which does not move. See `struct proc_info`. */
        out[n].cpu       = (p->thread != NULL)
                           ? (uint32_t)p->thread->sched.cpu
                           : PROC_CPU_NONE;

        out[n].owns      = (p->owns_console ? 1u : 0u)
                         | (p->owns_screen ? 2u : 0u)
                         | (p->owns_disk ? 4u : 0u)
                         | (p->owns_procctl ? 8u : 0u)
                         | (p->owns_devices ? 16u : 0u);
        out[n].parent    = p->parent_id;

        memcpy(out[n].name, p->name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';

        n++;
    }

    return n;
}

/* A process says what it is. The kernel does not name anything: a spawned
 * child inherits its parent's name, which made every process in the system
 * "init" until this existed. */
void process_set_name(struct process *p, const char *name, size_t len)
{
    size_t i;

    if (p == NULL) {
        return;
    }

    if (len > sizeof(p->name) - 1) {
        len = sizeof(p->name) - 1;
    }

    for (i = 0; i < len; i++) {
        /* Printable only. A name reaches a screen and a log, and a control
         * character in one of those is a mess somebody has to debug. */
        char c = name[i];
        p->name[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }

    p->name[len] = '\0';
}

unsigned process_slots_used(void)
{
    unsigned n = 0;
    unsigned i;

    for (i = 0; i < procs_made(); i++) {
        if (proc(i)->in_use) {
            n++;
        }
    }

    return n;
}

/*
 * The process pool's lock.
 *
 * Same shape as the thread pool's and for the same reason: the scan and the
 * claim are one operation, and splitting them leaves a window where two
 * cores are handed the same slot. `in_use` is set here rather than by the
 * caller, which is what closes it.
 *
 * `process_count` reads the pool without taking this, deliberately. It is a
 * statistic - `procs` and `sysinfo` show it - and a count that is one out
 * because a process was being created while it was read is a count that was
 * true a microsecond earlier. Taking a lock to make a number briefly more
 * accurate would put every reader of `sysinfo` in the way of every process
 * that starts.
 */
static struct spinlock processes_lock = SPINLOCK("processes");

static struct process *alloc_process(void)
{
    unsigned long flags;
    unsigned i;

again:
    flags = spin_lock(&processes_lock);

    for (i = 0; i < procs_made(); i++) {
        if (!proc(i)->in_use) {
            /*
             * Zeroed here, inside the lock, and that is not tidiness.
             *
             * `process_create` used to `memset` the slot itself, right after
             * this returned - which cleared the very `in_use` flag that
             * claims it. The slot read as free again for the length of a
             * 160-byte memset, and a second core scanning in that window
             * took it: two processes on one slot, the second overwriting the
             * first's address space, and a translation fault at the user
             * text base a moment later with nothing to say why.
             *
             * It was caught by the suite the first time it ran, which is the
             * argument for locking the pools before anything contends for
             * them rather than after.
             */
            memset(proc(i), 0, sizeof(struct process));
            proc(i)->in_use = true;
            spin_unlock(&processes_lock, flags);
            return proc(i);
        }
    }

    spin_unlock(&processes_lock, flags);

    /* Every slot taken: one more slab, then look again - or, at the ceiling
     * or with no pages to spare, the refusal it always was. */
    if (pool_grow(&processes)) {
        goto again;
    }

    return NULL;
}

/*
 * A slot `alloc_process` claimed, given back by a create that failed.
 *
 * Under the lock that claimed it, so a scanner on another core sees the slot
 * free only after everything this create wrote into it. It was missing from
 * the six early refusals - an empty image, a bad header or read-only half, a
 * misaligned image, no address space, no pages for the writable half - which
 * returned `NULL` holding the slot, and `process_count` counted each as a
 * live process that never ran (`threads.md` step 0).
 */
static struct process *give_back(struct process *p)
{
    unsigned long flags = spin_lock(&processes_lock);

    p->in_use = false;
    spin_unlock(&processes_lock, flags);
    return NULL;
}

/*
 * The thread side of a process.
 *
 * Runs once, in kernel context, to install the address space and then hand
 * the CPU to user level. It never comes back: from that return onwards this
 * thread is
 * the process, and every return into the kernel is an exception.
 */
static void process_main(void *arg)
{
    struct process *p = arg;

    thread_current()->process = p;

    /* Recorded on the thread as well, because from here every switch has to
     * carry it: the space belongs to the thread, not to whoever set it last. */
    thread_current()->space = p->space;
    as_switch(p->space);

    /* Past the header, which is data rather than code. */
    thread_time_return(1);     /* from here on this is the process's time */
    enter_user(USER_TEXT_VA + USER_IMAGE_HEADER, USER_STACK_TOP, p->arg);
}

/* A run of pages back to the allocator, one at a time, as everything here
 * frees them: the allocator does not remember that a run was a run. */
static void free_pages(void *base, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        pmm_free_page((char *)base + i * PAGE_SIZE);
    }
}

/*
 * A thread's own block: one page, mapped at the bottom of its stack slot,
 * holding its own address in its first word (`USER_TBLOCK`). Written here
 * through the kernel's own view of the page, because the process cannot be
 * asked to do it - it would have to read `errno` to report a failure.
 */
static bool give_thread_a_block(struct process *p, struct thread *t,
                                unsigned index)
{
    void *page;

    if (!pmm_room_for_user(1)) {
        return false;
    }

    page = pmm_alloc_page();

    if (page == NULL) {
        return false;
    }

    memset(page, 0, PAGE_SIZE);
    *(unsigned long *)page = (unsigned long)USER_TBLOCK(index);

    if (as_map(p->space, (uintptr_t)USER_TBLOCK(index), virt_to_phys(page), 1,
               MAP_USER_RW) != AS_OK) {
        kputs("block: as_map refused index ");
        kputu(index);
        kputs("\n");
        pmm_free_page(page);
        return false;
    }

    t->tls_page = page;
    t->tls = (unsigned long)USER_TBLOCK(index);
    return true;
}

/*
 * **A thread of a process that is already running** (`threads.md` step 3).
 *
 * It begins where `process_main` begins - in kernel context, with the
 * address space installed - and reads where to go from its own slot, because
 * the thread does not exist when `SYS_THREAD_CREATE` is deciding those
 * numbers. From the `enter_user` below it is user code like any other, and
 * every return into the kernel is an exception.
 */
static void user_thread_main(void *arg)
{
    struct thread *self = thread_current();

    (void)arg;
    as_switch(self->space);
    thread_time_return(1);
    enter_user(self->user_entry, user_function_sp(USER_TSTACK_TOP(self->index)),
               self->user_arg);
}

/*
 * One more thread for `p`, entering at `entry` with `arg`, and its stack.
 * Returns its index, or a negative error. The caller is one of `p`'s own
 * threads; nothing else may make one.
 */
int process_thread_create(struct process *p, unsigned long entry,
                          unsigned long arg)
{
    unsigned long flags;
    struct thread *t;
    void *pages;
    unsigned index;
    uintptr_t top;

    if (p == NULL || !p->in_use || p->exited) {
        return -1;
    }

    if (!pmm_room_for_user(USER_STACK_PAGES)) {
        return -2;                          /* the reserve: `pmm.c` */
    }

    pages = pmm_alloc_contiguous(USER_STACK_PAGES);

    if (pages == NULL) {
        return -2;
    }

    memset(pages, 0, USER_STACK_PAGES * PAGE_SIZE);

    flags = spin_lock(&processes_lock);
    index = p->next_index++;
    spin_unlock(&processes_lock, flags);

    top = (uintptr_t)USER_TSTACK_TOP(index);

    /* The stack at the top of its slot, and the rest of the slot left
     * unmapped: an overflow faults rather than reaching a neighbour. */
    if (as_map(p->space, top - USER_STACK_PAGES * PAGE_SIZE,
               virt_to_phys(pages), USER_STACK_PAGES,
               MAP_USER_RW) != AS_OK) {
        free_pages(pages, USER_STACK_PAGES);
        return -2;
    }

    t = thread_create_suspended(p->name, user_thread_main, NULL);

    if (t == NULL) {
        (void)as_unmap(p->space, top - USER_STACK_PAGES * PAGE_SIZE,
                       USER_STACK_PAGES);
        free_pages(pages, USER_STACK_PAGES);
        return -3;
    }

    t->process = p;
    t->space = p->space;
    t->caps = &p->caps;
    t->index = index;
    t->user_entry = entry;
    t->user_arg = arg;
    t->user_stack_pages = pages;

    if (!give_thread_a_block(p, t, index)) {
        thread_abandon(t);
        (void)as_unmap(p->space, top - USER_STACK_PAGES * PAGE_SIZE,
                       USER_STACK_PAGES);
        free_pages(pages, USER_STACK_PAGES);
        return -2;
    }

    flags = spin_lock(&processes_lock);
    t->sibling = p->threads;
    p->threads = t;
    p->live_threads++;
    spin_unlock(&processes_lock, flags);

    thread_wake(t);
    return (int)index;
}

/* A thread of `p` by the index it was given, or NULL. */
struct thread *process_thread_at(struct process *p, unsigned index)
{
    unsigned long flags = spin_lock(&processes_lock);
    struct thread *t = p->threads;

    while (t != NULL && t->index != index) {
        t = t->sibling;
    }

    spin_unlock(&processes_lock, flags);
    return t;
}

/*
 * A thread of `p` that has ended: off the list, its stack given back, and
 * whoever was waiting for it woken with its code.
 */
void process_thread_ended(struct process *p, struct thread *t, int code)
{
    unsigned long flags;
    struct thread **link;
    struct thread *joiner;

    flags = spin_lock(&processes_lock);
    link = &p->threads;

    while (*link != NULL && *link != t) {
        link = &(*link)->sibling;
    }

    /* Left on the list, marked ended: its code is kept until a sibling waits
     * for it, and `alloc_thread` steps over the slot until then. */
    (void)link;
    t->exit_code = code;
    t->ended = true;
    p->live_threads--;
    joiner = t->joiner;
    t->joiner = NULL;
    spin_unlock(&processes_lock, flags);

    if (t->tls_page != NULL) {
        (void)as_unmap(p->space, (uintptr_t)USER_TBLOCK(t->index), 1);
        pmm_free_page(t->tls_page);
        t->tls_page = NULL;
    }

    if (t->user_stack_pages != NULL) {
        (void)as_unmap(p->space,
                       (uintptr_t)USER_TSTACK_TOP(t->index)
                       - USER_STACK_PAGES * PAGE_SIZE,
                       USER_STACK_PAGES);
        free_pages(t->user_stack_pages, USER_STACK_PAGES);
        t->user_stack_pages = NULL;
    }

    if (joiner != NULL) {
        thread_wake(joiner);
    }
}

/*
 * Waits for the thread with that index and answers with its code.
 *
 * A thread that has already ended is answered at once - which is the usual
 * way round, since a thread that does a little work finishes before whoever
 * made it gets round to asking. Its slot is handed back here, which is what
 * `ended` was keeping.
 */
int process_thread_wait(struct process *p, unsigned index)
{
    for (;;) {
        unsigned long flags = spin_lock(&processes_lock);
        struct thread **link = &p->threads;
        struct thread *t;
        int code;

        while (*link != NULL && (*link)->index != index) {
            link = &(*link)->sibling;
        }

        t = *link;

        if (t == NULL) {
            spin_unlock(&processes_lock, flags);
            return SYS_ERR_NO_CHILD;        /* no thread of this process */
        }

        if (t->ended) {
            code = t->exit_code;
            *link = t->sibling;
            t->sibling = NULL;
            t->ended = false;               /* the slot is the pool's again */
            spin_unlock(&processes_lock, flags);
            return code;
        }

        if (t->joiner != NULL && t->joiner != thread_current()) {
            spin_unlock(&processes_lock, flags);
            return SYS_ERR_DENIED;          /* somebody is already waiting */
        }

        t->joiner = thread_current();
        thread_block_and_release(&processes_lock, flags);
    }
}

/*
 * Every other thread of this process, gone, before anything it is running on
 * is freed (`threads.md` step 3).
 *
 * The first thread is the one that tears the process down, and it does that
 * here rather than trusting that no sibling is left: freeing an address
 * space under a running thread is the failure this ordering exists to
 * prevent. Marked killed first, so a sibling on its way back to user level
 * leaves instead; then waited for.
 */
/* The slots of threads that ended and were never waited for, back to the
 * pool: a process that ends takes its zombies with it. */
static void release_ended_threads(struct process *p)
{
    unsigned long flags = spin_lock(&processes_lock);
    struct thread *t = p->threads;

    while (t != NULL) {
        struct thread *next = t->sibling;

        t->sibling = NULL;
        t->ended = false;
        t = next;
    }

    p->threads = NULL;
    spin_unlock(&processes_lock, flags);
}

static void wait_for_siblings(struct process *p)
{
    unsigned long start = hal_ticks();

    /*
     * **Waited for, not killed.** Marking the process killed here sets the
     * flag the *exiting* thread's own return path reads, and that path calls
     * `process_exit` again - which on x86 was a reboot in the middle of the
     * suite rather than an error anybody could read. Ending siblings that
     * have not asked to end is step 6's work, with the waking and the
     * interrupt it needs; step 3 waits for threads that are leaving anyway,
     * and says so loudly if one does not.
     */
    while (p->live_threads > 0) {
        if (hal_ticks() - start > 5UL * TICK_HZ) {
            panic("process: a thread would not leave");
        }

        thread_yield();
    }
}

struct process *process_create(const char *name, const void *image,
                               size_t len, unsigned long arg)
{
    struct process *p = alloc_process();   /* zeroed and claimed */
    const uint64_t *header = image;
    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t rx_bytes;
    size_t rx_pages;
    size_t i;

    if (p == NULL) {
        return NULL;                        /* no slot, so none to give back */
    }

    captable_init(&p->caps);

    if (pages == 0) {
        return give_back(p);
    }

    /*
     * The header is checked rather than trusted, even though the only image
     * that exists is built alongside the kernel. A loader that assumes its
     * input is well formed is a loader that will be wrong exactly once, and
     * at M8 the images come off a disk.
     */
    if (len < USER_IMAGE_HEADER || header[0] != USER_IMAGE_MAGIC) {
        return give_back(p);
    }

    rx_bytes = (size_t)header[1];
    rx_pages = rx_bytes / PAGE_SIZE;

    /*
     * `rx_bytes > len` is the one that matters now the read-only half is
     * mapped rather than copied.
     *
     * `rx_pages > pages` only says the half fits in the image *rounded up to
     * a page*, which was enough while the tail of that page was zeroed on
     * the way in. Mapping in place has no such moment: an 80-byte blob whose
     * header claims a 4096-byte read-only half would have the rest of the
     * page - whatever the linker put after it - readable by the process.
     * So the half has to be bytes the image actually has.
     */
    if ((rx_bytes & PAGE_MASK) != 0 || rx_pages == 0 || rx_bytes > len) {
        return give_back(p);
    }

    /*
     * The read-only half is mapped where it lies; only the writable half is
     * copied.
     *
     * **This is the same physical memory in every process.** There is one
     * userland image, so `.text`, `.rodata` - the code, the fonts, the icons
     * and the Lua source of every program - was 2.8 MB copied per process,
     * sixteen times, and no process could tell the difference because none
     * of them can write a byte of it. A second mapping of the kernel's own
     * pages at `USER_TEXT_VA`, read-only and executable to the process,
     * costs nothing and is exactly as isolated: the permissions live in the
     * *mapping*, and every process has its own.
     *
     * The comment that stood here said the opposite - that the image's pages
     * are the kernel's alone, and giving them user permissions would give
     * them to everything at once. That is true of the *identity* map, which
     * is shared, and this does not touch it. `USER_TEXT_VA` is above RAM on
     * both boards, in a slot of the top-level table the kernel never uses,
     * so the tables built here belong to this address space alone.
     *
     * **And the alias is safe because both mappings carry the same memory
     * attributes**, not because they carry the same permissions. Different
     * permissions for the same physical page are the entire point. What
     * both architectures forbid is two mappings that disagree about
     * *cacheability* - ARM's rule is about Normal, inner-shareable,
     * write-back memory and x86's is about the PAT and MTRR types, and they
     * amount to the same requirement. Both mappings here are ordinary
     * write-back memory, so both are satisfied.
     *
     * The writable half is still copied, and has to be: two processes from
     * one image must not share their globals.
     */
    if (((uintptr_t)image & PAGE_MASK) != 0) {
        /*
         * Refused rather than rounded down, and rather than silently copying
         * instead. An image mapped from an unaligned address maps whatever
         * precedes it, which is a process running on somebody else's bytes -
         * a failure that would look like anything except its cause.
         * `bin2c.py` aligns what it generates; a loader that arrives later
         * has to align its buffer, and this is where it will find that out.
         */
        return give_back(p);
    }


    p->space = as_create();
    if (p->space == NULL) {
        return give_back(p);
    }

    p->image_page_count = pages - rx_pages;

    if (p->image_page_count > 0) {
        p->image_pages = pmm_alloc_contiguous(p->image_page_count);

        if (p->image_pages == NULL) {
            as_destroy(p->space);
            return give_back(p);
        }

        memcpy(p->image_pages, (const char *)image + rx_bytes, len - rx_bytes);

        /* The tail of the last page holds whatever the previous owner left,
         * which this process would otherwise be able to read. */
        memset((char *)p->image_pages + (len - rx_bytes), 0,
               p->image_page_count * PAGE_SIZE - (len - rx_bytes));
    }

    p->heap_pages = pmm_alloc_contiguous(USER_HEAP_PAGES);
    p->stack_pages = pmm_alloc_contiguous(USER_STACK_PAGES);

    if (p->heap_pages == NULL || p->stack_pages == NULL) {
        goto fail;
    }

    memset(p->heap_pages, 0, USER_HEAP_PAGES * PAGE_SIZE);
    memset(p->stack_pages, 0, USER_STACK_PAGES * PAGE_SIZE);

    /*
     * Three mappings and three different sets of permissions. Code is read
     * only and executable; everything else is writable and never executable.
     * A page is executable by exactly one exception level and writable by at
     * most one purpose.
     */
    /*
     * `image` is the userland blob the build linked in - a symbol in
     * `.rodata`, identity mapped, so its address already is its physical
     * address - while everything else mapped here came from the page
     * allocator and lives in the window. `virt_to_phys` answers for both,
     * which is why `mmu.h` makes it total over kernel pointers rather than
     * leaving each caller to know which kind it is holding.
     */
    if (as_map(p->space, USER_TEXT_VA, virt_to_phys(image),
               rx_pages, MAP_USER_RX) != AS_OK) {
        goto fail;
    }

    if (p->image_page_count > 0
        && as_map(p->space, USER_TEXT_VA + rx_bytes,
                  virt_to_phys(p->image_pages),
                  p->image_page_count, MAP_USER_RW) != AS_OK) {
        goto fail;
    }

    p->next_map     = USER_MAP_VA;
    p->next_share   = USER_SHARE_VA;
    p->mapped_pages = 0;

    if (as_map(p->space, USER_HEAP_VA, virt_to_phys(p->heap_pages),
               USER_HEAP_PAGES, MAP_USER_RW) != AS_OK) {
        goto fail;
    }

    if (as_map(p->space, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
               virt_to_phys(p->stack_pages), USER_STACK_PAGES,
               MAP_USER_RW) != AS_OK) {
        goto fail;
    }

    /*
     * `in_use` is already true - `alloc_process` set it when it claimed the
     * slot - and this says so rather than setting it again, because a reader
     * finding it set here would reasonably conclude the claim happens at
     * this line and that the window above is open.
     */
    p->exited = false;

    /* The id under the pool's lock, for the reason the thread id is: two
     * cores creating a process at once would otherwise be handed the same
     * one, and an id is what `procs` and `kill` name a process by. */
    {
        unsigned long idflags = spin_lock(&processes_lock);

        p->id = next_id++;
        spin_unlock(&processes_lock, idflags);
    }
    p->arg = arg;
    p->image = image;
    p->image_len = len;

    for (i = 0; i + 1 < PROCESS_NAME_MAX && name[i] != '\0'; i++) {
        p->name[i] = name[i];
    }
    p->name[i] = '\0';

    p->thread = thread_create_suspended(p->name, process_main, p);

    if (p->thread == NULL) {
        goto fail;
    }

    /* Its capabilities are the process's, from before it first runs. */
    p->thread->caps = &p->caps;
    p->thread->index = 0;
    p->next_index = 1;

    /* And its own block, as every thread has: slot zero's, the first
     * thread's stack being where it always was. */
    if (!give_thread_a_block(p, p->thread, 0)) {
        goto fail;
    }

    return p;

fail:
    if (p->stack_pages != NULL) {
        for (i = 0; i < USER_STACK_PAGES; i++) {
            pmm_free_page((char *)p->stack_pages + i * PAGE_SIZE);
        }
    }
    if (p->heap_pages != NULL) {
        for (i = 0; i < USER_HEAP_PAGES; i++) {
            pmm_free_page((char *)p->heap_pages + i * PAGE_SIZE);
        }
    }
    for (i = 0; p->image_pages != NULL && i < p->image_page_count; i++) {
        pmm_free_page((char *)p->image_pages + i * PAGE_SIZE);
    }
    as_destroy(p->space);
    return give_back(p);
}

struct process *process_spawn(struct process *parent, unsigned long arg)
{
    struct process *child;

    if (parent == NULL || parent->image == NULL) {
        return NULL;
    }

    child = process_create(parent->name, parent->image, parent->image_len, arg);

    if (child != NULL) {
        child->parent = parent;
        child->parent_id = parent->id;
    }

    return child;
}

int process_wait(struct process *parent, unsigned *id, bool nonblocking)
{
    unsigned i;

    if (parent == NULL) {
        return -1;
    }

    for (;;) {
        bool any = false;
        unsigned long flags = spin_lock(&processes_lock);

        parent->waiter = NULL;

        for (i = 0; i < procs_made(); i++) {
            struct process *c = proc(i);

            if (!c->in_use || c->parent != parent) {
                continue;
            }

            if (c->exited) {
                int code = c->exit_code;

                if (id != NULL) {
                    *id = c->id;
                }

                /* Reaped here, so a supervisor looping on wait does not have
                 * to remember to, and so the same child is not reported
                 * twice - under the lock `exited` was published under,
                 * which is what makes the slot this frees one that nothing
                 * is still writing to. */
                c->in_use = false;
                spin_unlock(&processes_lock, flags);
                return code;
            }

            any = true;
        }

        if (!any) {
            spin_unlock(&processes_lock, flags);
            return -1;      /* nothing left to wait for */
        }

        /*
         * Park until a child ends. Recorded on the parent so process_exit
         * knows who to wake; without it this would have to poll, and a
         * supervisor that polls is a supervisor that burns a core doing
         * nothing.
         */
        /* Asked not to block, and nothing has exited yet. Nought children
         * *ready*, which is a different answer from no children at all and
         * has to be distinguishable: a caller draining zombies must be able
         * to stop without being told its children have gone. */
        if (nonblocking) {
            spin_unlock(&processes_lock, flags);
            return -2;
        }

        /*
         * **Findable and blocked before the lock is let go**, which is the
         * whole of the fix and the same one IPC needed.
         *
         * This set `waiter` and called `thread_block` with nothing held. On
         * one core that was safe: a child cannot run between the scan above
         * and the block. On two it can, and there are two ways to lose the
         * wake - the child ends on another core after the scan and finds no
         * waiter yet, or finds the waiter and wakes a thread that has not
         * blocked yet, which `thread_wake` ignores. Either way the parent
         * sleeps for ever beside an exited child, and the shell's `sys.wait`
         * after every foreground command is this call.
         *
         * `process_exit` publishes and wakes under `processes_lock`, so it
         * either ran before the scan above - which then found the child - or
         * runs after this thread is blocked and named, where its wake
         * sticks. `thread_block_and_release` lets go of the lock at the one
         * instant that is true, and why that instant may come before the
         * switch rather than after it is written above it in `thread.c`.
         */
        parent->waiter = thread_current();
        thread_block_and_release(&processes_lock, flags);
    }
}

void process_grant_console(struct process *p)
{
    if (p != NULL) {
        p->owns_console = true;

        /*
         * **Not** promoted to SCHED_PRIO_INPUT, and the reason is worth
         * keeping.
         *
         * It looks right: the console owner is the one process allowed to
         * read the keyboard and the pointer, so it is what every keystroke
         * waits on, and priority following capability rather than being
         * asked for is the shape the rest of this system has. Nothing can
         * promote itself; it can only be handed something that comes with a
         * promotion.
         *
         * It was tried and it starves the machine. The console owner is not
         * only the input reader - it is also the *output* path, and every
         * program that prints asks it to. At the top band it outranks
         * everything it is serving, and strict priority means the things it
         * serves never run. `thread: three threads interleave` failed
         * within a minute of the change, which is the fairness property
         * that test exists to hold.
         *
         * **Priority inheritance is the answer, and it exists now.** A server runs
         * at the band of whoever is blocked waiting for it - `thread_inherit`,
         * called from `ipc_call` and `ipc_receive`, given back in `ipc_reply`.
         * So the console does not need promoting: it sits at NORMAL and
         * *becomes* urgent for exactly as long as something urgent is waiting
         * on it, and goes back to being ordinary the moment it answers.
         *
         * Which is why this promotion is not merely disabled but wrong. A
         * band says "this thread is always important". Inheritance says "it
         * is as important as whoever needs it", and for a server that is
         * both the input path and the print path, only the second is true.
         */
    }
}

void process_grant_procctl(struct process *p)
{
    if (p != NULL) {
        p->owns_procctl = true;
    }
}

/*
 * Hardware. See `SPAWN_DEVICES` for what this is and why a driver does not
 * get it - it is the authority to *mint* a device capability, not the
 * capability itself.
 *
 * **And the display band**, as the audio server and the compositor are
 * given it: a driver is on a deadline the hardware sets. At NORMAL a busy
 * desktop held the USB driver off its core for tens of milliseconds, and on
 * 24 September QEMU's xHCI dropped the camera's Transfer Events and the
 * C920's picture froze (`usb.md` §11); on the ThinkPad the same driver is
 * the USB mouse, which `sched.h` would put higher still. Diego: "Do 6i yes"
 * (`roadmap.md` 6i). Not INPUT, for `process_grant_audio`'s reason below:
 * a band that high is safe only while the thread blocks, and nothing yet
 * enforces that it does. At DISPLAY a driver that spins shares its core
 * with the compositor rather than taking it.
 */
void process_grant_devices(struct process *p)
{
    if (p != NULL) {
        p->owns_devices = true;
        thread_set_priority(p->thread, SCHED_PRIO_DISPLAY);
    }
}

/*
 * The right to play sound.
 *
 * Nothing is mapped and nothing is reserved: unlike the screen there are no
 * pages to hand over, because samples go through a syscall rather than into
 * a shared buffer. Which is a deliberate difference and not an oversight -
 * a period is a kilobyte and arrives every five milliseconds, so the copy
 * is cheap and the alternative is a shared ring that two sides have to
 * agree about under a deadline.
 *
 * False when the board has no sound device, so that a process asking for
 * audio on a machine without any finds out at spawn rather than at the
 * first silent `beep`.
 */
bool process_grant_audio(struct process *p)
{
    if (p == NULL || !hal_snd_present()) {
        return false;
    }

    p->owns_audio = true;

    /*
     * **Promoted to the display band, and it took three tries to earn it.**
     *
     * First refusal: the server spun, so a DISPLAY-band spinner starved the
     * clients that had real work to do before they could play anything. The
     * note here said a spinning server is the wrong shape and no band fixes
     * it, and that was right.
     *
     * Second: the spin became a sleep, and priority inheritance made the
     * question look moot - `ipc_call` lifts a server to its caller's band
     * for as long as the caller waits, so the server *became* urgent
     * whenever anybody needed it and was ordinary otherwise, which is
     * strictly better than a fixed band.
     *
     * Third, and the reason this is here now: **the samples stopped
     * travelling as messages.** A client writes into a shared ring and calls
     * nothing, so there is no call to inherit from - and the mechanism that
     * had been quietly holding this server up disappeared along with the
     * copying. That is a real cost of `CLAUDE.md`'s "control by message,
     * data by shared memory", and it is worth naming: inheritance only
     * works on a path somebody is blocked on, so the moment a data path
     * stops blocking, whatever was riding on it needs saying out loud.
     *
     * A band is honest here in a way it was not for the console. The console
     * is both the input path and the print path, so "always urgent" is true
     * of half its job and false of the other half. This server has one job,
     * it arrives every 5.8 milliseconds whether anybody asks or not, and the
     * work is bounded: mix at most a device-queue's worth of periods, then
     * block. A thread that cannot run long cannot starve anybody, which is
     * what made the first refusal true and makes this safe.
     *
     * **The input band, and the display band was measured to be wrong.**
     *
     * Equal priority is round robin, and this system's quantum is a tenth
     * of a second. So a server sharing a band with the program feeding it
     * can wait up to 100 ms for its turn - against a device holding 23 ms
     * of sound. That is the argument, and it is a structural one rather
     * than a measured one: moving this from the display band to here did
     * not change the underrun count, and the 106 ms stall that first
     * suggested it turned out to be teardown - unmapping a region and
     * dropping a capability, once, after the sound had stopped - measured
     * by a probe that ran after `close` instead of during play. During play
     * the worst gap is 24 ms, which is the device's whole buffer and a
     * different problem.
     *
     * It stays because the hazard is real even though it was not what was
     * being seen: a periodic server that round-robins with its own client
     * at a tenth of a second is one slow client away from a gap, and
     * nothing else in the design prevents it.
     *
     * That is also what priority inheritance had been hiding. A server
     * lifted by `ipc_call` does not wait for a turn, it *preempts* - the
     * caller's band arrives with the call. Take the calls away and equal
     * priority is not "as urgent as its client", it is "after its client,
     * for up to a quantum".
     *
     * **The display band, not the input band, and that was tried the hard
     * way.** Above its clients is the tidier argument and it was put at
     * `SCHED_PRIO_INPUT` for an afternoon. Then `refill` stopped blocking -
     * a separate bug, in Lua, in this same change - and a spin at the top
     * of a strict-priority scheduler took the desktop with it: the whole
     * user interface stopped responding while anything played.
     *
     * The lesson is not "the band was wrong". It is that **the safety of a
     * high band rests entirely on the thread blocking**, and that is a
     * property of code somebody can break in a different file. The display
     * band leaves the compositor able to fight back, which for a bug of
     * that shape is the difference between bad audio and no machine.
     *
     * Above the clients remains the right answer once there is something
     * enforcing the bound rather than a promise. A budget the scheduler
     * checks would be that; there is not one yet.
     *
     * The remaining discomfort is honest: a 100 ms quantum is a long time
     * for anything, and a band is being used to work around it. Bands are
     * the right answer for a periodic deadline; the quantum is a separate
     * question and `sched_prio.c` says it is a variable so that it can be
     * asked.
     */
    thread_set_priority(p->thread, SCHED_PRIO_DISPLAY);

    return true;
}

/*
 * Wake whoever holds the sound device, because the device asked.
 *
 * Called from the interrupt path. There is exactly one such process - that
 * is what `SPAWN_AUDIO` means and what makes per-application volume possible
 * - so there is no search to do beyond finding it, and no ambiguity about
 * who to wake.
 *
 * **This is what an interrupt is for.** The server was blocking with a
 * deadline and being woken by the timer, which is a poll wearing a
 * different hat: it asked the device whether it wanted anything at a rate
 * somebody had picked. Now the device says so, and the deadline it still
 * carries is a backstop rather than the mechanism.
 *
 * Only a thread that is *waiting* is touched. Waking a running thread is
 * meaningless, and waking one blocked on something else - a reply it is
 * owed, a child it is waiting for - would be a bug that presents as a
 * server returning from a call nobody answered.
 */
void process_wake_audio(void)
{
    unsigned i;

    for (i = 0; i < procs_made(); i++) {
        struct process *p = proc(i);

        if (p->in_use && p->owns_audio && p->thread != NULL
            && p->thread->state == THREAD_BLOCKED
            && p->thread->wake_at != 0) {
            /*
             * `wake_at` is the giveaway that this is the timed wait in
             * `ipc_receive`, and clearing it before waking is what makes the
             * receive report "nothing arrived" rather than believing a
             * message came - `ipc_timed_out` does the unlinking that goes
             * with it.
             */
            p->thread->wake_at = 0;
            ipc_timed_out(p->thread);
            thread_wake(p->thread);

            /*
             * **Every holder, not the first**, for the reason
             * `process_wake_net` sets out at length: init holds every grant
             * it passes on and comes first in this table, so stopping at the
             * first one woke init and left the server that wanted the wake
             * asleep until its own deadline.
             */
        }
    }
}

/*
 * **The disk, started once.** `hal_blk_init` starts the controller - an NVMe
 * drive reset and given new queues, or a virtio device taken back to status 0
 * and brought up again - and it was called for every `SYS_DISK_INFO` and
 * every grant: every `sys.disk()` anybody made, including the one the disk
 * server makes each time `/home/.super` is read, and the one This Machine
 * makes as it opens. QEMU's trace counted the NVMe controller started ten
 * times in a boot that ran `diskinfo` three times, and a virtio disk walked
 * through its whole start again for each; on x86 every NVMe start also spent
 * one of four MSI vectors and a mapping. It worked for as long as nothing was
 * in flight when it happened, which was all anything had tested.
 *
 * So `kmain` starts it here, beside sound, while it is the only thing running
 * - no other thread, no other processor started, no process - and what it
 * answered is kept. Nothing writes these afterwards, which is why reading
 * them takes no lock.
 */
static struct blkdev kept_disk;
static bool kept_disk_found;

void process_disk_start(void)
{
    kept_disk_found = hal_blk_init(&kept_disk);
}

bool process_disk(struct blkdev *out)
{
    if (!kept_disk_found) {
        return false;
    }

    if (out != NULL) {
        *out = kept_disk;
    }

    return true;
}

bool process_grant_disk(struct process *p)
{
    if (p == NULL || !process_disk(NULL)) {
        return false;
    }

    p->owns_disk = true;
    return true;
}

/*
 * The network.
 *
 * `hal_net_init` was already called at boot - the card needs its receive
 * buffers before the first frame arrives, not when somebody first asks - so
 * this only records who may use it. Asking the HAL again would re-run the
 * handshake on a device that is already running, which is a reset with
 * frames in flight.
 *
 * **It does not ask whether there is a card**, and did until 22 September.
 * The grant says *who holds the network*, and the kernel's part in that is
 * making sure it is one process; whether this machine has a card the kernel
 * can see is a different question, and `SYS_NET_INFO` is where it is asked
 * and answered. A machine whose network arrives on a USB Ethernet adapter
 * has no card here and one stack all the same (`usb.md` 7d) - and it needs
 * this flag, because `process_wake_net` finds the stack by it. Without the
 * change a frame taken off a USB adapter woke nobody, and a ping over it
 * came back in the stack's receive deadline, 104 ms, rather than the
 * network's.
 *
 * Nothing is reachable that was not: every syscall behind this flag asks
 * `hal_net_present` for itself and answers a machine with no card honestly.
 */
bool process_grant_net(struct process *p)
{
    if (p == NULL) {
        return false;
    }

    p->owns_net = true;
    return true;
}

/*
 * Wake whoever holds the network, because a frame arrived.
 *
 * The same shape as `process_wake_audio` next door, down to the `wake_at`
 * check that says this is the timed wait in `ipc_receive` rather than a
 * thread blocked on something it is owed.
 *
 * **Every one of them, not the first**, and that correction is worth the
 * paragraph. This said "there is exactly one such process - that is what
 * `SPAWN_NET` means", and there is not: a process can only pass on a grant
 * it holds, so **init holds it too**, and init comes first in this table. So
 * the wake went to init, which was blocked in its own timed receive and had
 * nothing to do with the frame, and the stack was left to find out when its
 * deadline came round.
 *
 * It cost 100 milliseconds on every round trip and nobody noticed, because
 * it looks exactly like a network that is slow: a ping over the kernel's own
 * virtio card came back in 103 ms, and with the stack's deadline shortened
 * by hand it came back in 6. Found on 22 September, in the USB Ethernet work
 * (`usb.md` 7d), by a wake that was added for a driver in userland and
 * changed nothing at all.
 *
 * Waking the others costs each of them one pass of a loop they were about to
 * make anyway. Waking the wrong one costs every packet a tenth of a second.
 *
 * Without any of it the stack has to come back and ask, which is a poll
 * wearing a different hat: it would look at the card at a rate somebody
 * picked, and the round-trip time it reported would be that rate rather than
 * the network's - which is precisely what it did.
 */
unsigned process_wake_net(void)
{
    unsigned i, woke = 0;

    for (i = 0; i < procs_made(); i++) {
        struct process *p = proc(i);

        if (p->in_use && p->owns_net && p->thread != NULL
            && p->thread->state == THREAD_BLOCKED
            && p->thread->wake_at != 0) {
            p->thread->wake_at = 0;
            ipc_timed_out(p->thread);
            thread_wake(p->thread);
            woke++;
        }
    }

    return woke;
}

bool process_grant_screen(struct process *p)
{
    struct fb fb;
    size_t bytes;
    size_t pages;

    if (p == NULL || !screen_get(&fb)) {
        return false;
    }

    /*
     * The same physical pages the board is scanning out, mapped a second
     * time into this process. Not a copy: a linear framebuffer's whole
     * value is that a store lands on the screen with nothing in between,
     * and a copy would need three megabytes somewhere and a flush after
     * every frame.
     *
     * **`fb.phys`, not `fb.pixels`.** They are the same number on a board
     * whose framebuffer is in RAM and are not on a board whose firmware
     * provided one; `struct fb` has the whole account.
     *
     * Rounded up from pitch * height rather than from width * height * 4,
     * because the pitch is padded and the last row runs to the end of its
     * stride. Getting this wrong leaves the bottom row unmapped, and the
     * fault would arrive on whatever happened to draw near the bottom of
     * the screen rather than at the mapping.
     */
    bytes = (size_t)fb.pitch * fb.height;
    pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /*
     * **A screen too large for its window is refused, and said.**
     *
     * The window was sixteen megabytes and nothing checked: a 3440x1440
     * framebuffer is 18.9, so the mapping ran past it and into
     * `USER_MAP_VA`, where the next surface the compositor asked for was
     * mapped over the bottom of the screen. The desktop drew its top three
     * quarters and the kernel's console showed through the rest, and
     * nothing anywhere said why (`USER_SCREEN_MAX`).
     *
     * A machine with no screen is a supported way to run, so this is a
     * refusal rather than a panic - and it is printed, because a desktop
     * that does not start is a question somebody will have to answer.
     */
    if (bytes > USER_SCREEN_MAX) {
        kputs("screen: this display needs more of a process's address space "
              "than the window for it holds; not handed over\n");
        return false;
    }

    /*
     * **`MAP_USER_FB`, not `MAP_USER_RW`**, and the difference is the
     * memory type rather than the permissions.
     *
     * These are the same physical pages the kernel mapped write-combining,
     * and a second mapping of them does not inherit that: the type lives in
     * the page table entry, so two mappings of one framebuffer can disagree
     * about it. This one did, and on a machine with a real cache it meant
     * the console was quick and the desktop was unusable - the console
     * draws through the kernel's mapping and the compositor through this
     * one. `arch/x86_64/mmu.h` has the account and the assertion that stops
     * the two drifting apart again.
     */
    if (as_map(p->space, USER_SCREEN_VA, fb.phys,
               pages, MAP_USER_FB) != AS_OK) {
        return false;
    }

    /*
     * **Read back, because "the constant is right" and "this call used it"
     * are two claims and only one of them was ever true.**
     *
     * `arch/` asserts at compile time that `MAP_USER_FB` carries the
     * framebuffer's memory type. That says nothing about whether this line
     * passed it - which is precisely the mistake that happened, and it went
     * unnoticed for as long as it did because every suite here runs under
     * an emulator that models no cache and so cannot tell the two types
     * apart. So the entry the mapping actually produced is asked.
     *
     * It reports rather than refusing. A desktop drawing through the wrong
     * memory type works and is slow, and a machine that says so on its own
     * boot log is worth much more than one that will not start.
     */
    {
        const uint64_t *entry = as_page_entry(p->space, USER_SCREEN_VA);

        if (entry == NULL || !mmu_entry_matches_framebuffer(*entry)) {
            kputs("screen: the compositor's mapping does not carry the "
                  "framebuffer's memory type; the desktop will be slow\n");
        }
    }

    p->owns_screen = true;

    /*
     * The compositor is what the eye is waiting for, so it outranks ordinary
     * work and is outranked by input. Same argument as the console: whoever
     * was handed the screen is the one drawing it, and nothing else needs to
     * be asked.
     */
    thread_set_priority(p->thread, SCHED_PRIO_DISPLAY);
    return true;
}

/*
 * Ends a child.
 *
 * Marks and unblocks; the process dies on its own next entry into the
 * kernel. It cannot be torn down from here, because the teardown ends with
 * the thread that performs it and this is not that thread - `process_exit`
 * says so with a panic rather than behaving like cleanup and acting like
 * suicide.
 */
/*
 * The same, for a process that holds SPAWN_PROCCTL.
 *
 * Written as its own function rather than as a NULL parent, because a NULL
 * parent already means something here - a process init did not start - and
 * a flag that turns a safety check off is the kind of parameter that gets
 * passed by accident. Two names, one of which no ordinary caller has.
 *
 * init itself is refused. Ending it ends the system, and doing that by
 * clicking a row in a task manager is not a power worth having; the machine
 * has a reset for that.
 */
int process_kill_any(unsigned id)
{
    unsigned i;

    for (i = 0; i < procs_made(); i++) {
        struct process *c = proc(i);

        if (!c->in_use || c->id != id) {
            continue;
        }

        if (c->parent == NULL) {
            return -1;              /* init, or something init did not start */
        }

        if (c->exited) {
            return 0;
        }

        return process_kill(c->parent, id);
    }

    return -1;
}

int process_kill(struct process *parent, unsigned id)
{
    unsigned i;

    for (i = 0; i < procs_made(); i++) {
        struct process *c = proc(i);

        if (!c->in_use || c->parent != parent || c->id != id) {
            continue;
        }

        if (c->exited) {
            return 0;               /* already gone; nothing to do */
        }

        c->killed = true;

        /*
         * A thread waiting on an endpoint is not running, so it cannot
         * notice the flag. Unblocking it here is what makes the kill take
         * effect on a process that is not spinning - it resumes, its IPC
         * call fails, and the check on the way back to user level ends it.
         */
        if (c->thread != NULL) {
            ipc_abort(c->thread);
        }

        return 0;
    }

    return -1;
}

bool process_should_die(void)
{
    struct process *p = thread_current()->process;

    return p != NULL && p->killed && !p->exited;
}

void process_start(struct process *p)
{
    if (p != NULL && p->thread != NULL) {
        thread_wake(p->thread);
    }
}

/* Frees the memory a process owns. Shared by exiting and abandoning, which
 * differ only in whose thread ends. */
static void release_memory(struct process *p)
{
    /* The first thread's own block, which no `process_thread_ended` will
     * reach: that one is for the threads it made. */
    if (p->thread != NULL && p->thread->tls_page != NULL) {
        pmm_free_page(p->thread->tls_page);
        p->thread->tls_page = NULL;
    }

    /* Before the thread is gone, so the figure outlives it. */
    if (p->thread != NULL) {
        p->ticks = p->thread->ticks;
    }

    size_t i;
    uintptr_t va;

    for (i = 0; i < USER_STACK_PAGES; i++) {
        pmm_free_page((char *)p->stack_pages + i * PAGE_SIZE);
    }

    for (i = 0; i < USER_HEAP_PAGES; i++) {
        pmm_free_page((char *)p->heap_pages + i * PAGE_SIZE);
    }

    /*
     * The writable half only. The read-only half is the kernel's own image,
     * mapped rather than copied, and freeing it would return the pages the
     * kernel is running out of.
     */
    for (i = 0; p->image_pages != NULL && i < p->image_page_count; i++) {
        pmm_free_page((char *)p->image_pages + i * PAGE_SIZE);
    }

    /*
     * And whatever it mapped with SYS_MAP and never unmapped.
     *
     * There is no list of those: they were allocated a page at a time and
     * the only record is in the page tables, so the way to find them is to
     * walk the range the bump pointer covers and free whatever is still
     * mapped. `as_destroy` will not do it - it says so - because it did not
     * allocate them and does not know who did.
     *
     * Without this a process that exits holding a surface leaks it, and the
     * leak is invisible: the pages are gone from the free count and nothing
     * points at them.
     */
    for (va = USER_MAP_VA; va < p->next_map; va += PAGE_SIZE) {
        uintptr_t phys = as_page_phys(p->space, va);

        if (phys != 0) {
            pmm_free_page(phys_to_virt(phys));
        }
    }

    p->next_map     = USER_MAP_VA;
    p->next_share   = USER_SHARE_VA;
    p->mapped_pages = 0;

    as_destroy(p->space);

    p->space = NULL;
    p->image_pages = NULL;
    p->heap_pages = NULL;
    p->stack_pages = NULL;
}

/*
 * A process that was made and never started, taken apart.
 *
 * **Its capabilities are released**, and they were not until 19 September
 * (`threads.md` step 1b): `sys_spawn` grants a child its capabilities before
 * it starts, and abandons it if a later grant fails - so a child abandoned
 * holding a region kept that region's reference, and the region was never
 * freed. The table was its thread's then and nothing looked at it; it is the
 * process's now, and taking a process apart includes it.
 *
 * And the slot goes back under the lock that claims them (`give_back`).
 */
void process_abandon(struct process *p)
{
    if (p == NULL || !p->in_use) {
        return;
    }

    if (p->thread != NULL) {
        /* Its block by hand, because `release_memory` reaches it through the
         * pointer this is about to clear. */
        if (p->thread->tls_page != NULL) {
            pmm_free_page(p->thread->tls_page);
            p->thread->tls_page = NULL;
        }

        thread_abandon(p->thread);
        p->thread = NULL;
    }

    release_ended_threads(p);
    ipc_caps_release(&p->caps);
    release_memory(p);
    (void)give_back(p);
}

void process_exit(struct process *p, int code)
{

    if (p == NULL || p->exited) {
        return;
    }

    if (thread_current()->process != p) {
        /*
         * Ending somebody else's process here would end *this* thread, not
         * theirs: the thread_exit at the bottom is unconditional and has to
         * be, because that is what exiting means. Saying so is better than
         * the alternative, which reads as cleanup and behaves as suicide.
         */
        panic("process_exit: only the running process may exit");
    }

    /* Its other threads first, before a byte of what they run on is freed
     * (`wait_for_siblings`). Nothing below may assume it is alone until this
     * has returned. */
    wait_for_siblings(p);

    /*
     * **Every death says so, not only the ones that fault.**
     *
     * A process that takes an exception is reported by `trap.c` with a
     * register dump. A process that merely *stops* - returned, was killed,
     * or ended because a Lua chunk raised and the runtime unwound - printed
     * nothing at all, and the log said nothing about the most interesting
     * thing that had happened.
     *
     * That is not hypothetical. Four applications on the first real machine
     * drew their windows and went silent, and `log error` found nothing,
     * because nothing was written when they went. Whether they died at all
     * was unanswerable from the one record the machine keeps.
     *
     * One line, at the moment it becomes true, with the name and the number
     * a person can match against `ps` and against the window that was left
     * behind.
     */
    kputs("process ");
    kputu((unsigned long)p->id);
    kputs(" (");
    kputs(p->name[0] != '\0' ? p->name : "?");
    kputs(") ended, code ");

    if (code < 0) {
        kputc('-');
        kputu((unsigned long)(-(long)code));
    } else {
        kputu((unsigned long)code);
    }

    kputc('\n');

    /*
     * Back to the kernel's own address space before the process's is taken
     * apart. The thread is still executing, and it is executing kernel code
     * mapped in both, but the moment as_destroy frees the tables the running
     * translation regime would be describing freed pages.
     */
    as_switch(NULL);
    thread_current()->process = NULL;
    thread_current()->space = NULL;

    /*
     * The endpoints it made, ended with it.
     *
     * Before its capabilities, because this is not about its own table:
     * those endpoints are in other processes' tables too, and some of those
     * processes are blocked on them. A server killed with a client waiting
     * for its answer left the client waiting for ever, and a registry
     * holding its name kept a capability that still resolved. Destroying
     * them wakes the first with an error and makes the second stale, which
     * is how anything else learns that this process has gone.
     *
     * And before `exited` is published below, so a parent whose wait
     * returns finds them already gone.
     */
    ipc_endpoints_release(p);

    /*
     * **Interrupt lines, and this one is not a leak but a live device.**
     *
     * A driver that exits or is killed leaves its hardware asserting and
     * nothing left to quieten it. `irq_release` masks each line on the way
     * out, which is what stops a level-triggered source from delivering for
     * ever into a claim nobody owns - and it wakes anything still blocked on
     * one, so a driver with a thread inside `SYS_IRQ_WAIT` can finish dying.
     *
     * Before the capabilities, because those name these.
     */
    irq_release_owned_by(p);

    /*
     * **And the pointer's buttons, for the same reason**: a driver that
     * reported one down and ended - killed, faulted, or gone before its
     * device said the button came up - leaves the desktop dragging for
     * good, because it was the only one who could ever have said so. No
     * movement and no buttons, reported on its behalf, is that release,
     * and the sleepers are woken as its own report would have woken them,
     * so the window manager sees the button come up now rather than at its
     * next deadline.
     *
     * The board keeps one set of buttons for every device a process drives,
     * so a second such driver's would come up too. There is one, the USB
     * driver.
     */
    if (p->moved_pointer) {
        (void)hal_pointer_move(0, 0, 0, 0);
        thread_wake_sleepers_now();
    }

    /* And any key it pressed and did not let go - a game controller's
     * button, held when its driver died - for the same reason. */
    if (p->pushed_keys && hal_key_release_all()) {
        thread_wake_sleepers_now();
    }

    /*
     * Capabilities before memory. A shared region's pages come back only
     * when the last capability naming it is dropped, and this thread's are
     * about to stop existing - so a process that exits holding one would
     * leak it for the life of the machine.
     */
    release_ended_threads(p);
    ipc_caps_release(&p->caps);

    release_memory(p);

    /*
     * **And only now is it exited**, published under the pool's lock with
     * the wake that goes with it.
     *
     * `exited` was set on this function's first line, before any of the
     * teardown above, and on one core nothing could see the difference: a
     * parent cannot run until this thread has switched away for good. On
     * two it runs at once: `process_wait` can find the child exited, reap
     * the slot and return while this thread is still on another core giving
     * back the child's capabilities and pages - so a parent that counts what
     * the machine holds the moment its wait returns counts a child that is
     * still leaving. And a slot reaped mid-teardown can be claimed by the
     * next spawn while this thread still writes to it: `release_memory`
     * reading the new process's pages, `p->thread = NULL` clearing its
     * thread.
     *
     * So everything that touches `p` happens first, and `exited` means what
     * `process.h` says - finished, holding only its exit code. The waiter
     * is read and woken under the lock `process_wait` blocks with, which is
     * what stops that wake being lost.
     *
     * `in_use` stays set. What is left is the exit code, and the moment a
     * process dies is exactly when somebody wants to know why.
     * `process_wait` or `process_reap` releases the slot.
     */
    {
        unsigned long flags = spin_lock(&processes_lock);

        p->thread = NULL;
        p->exit_code = code;
        p->exited = true;

        if (p->parent != NULL && p->parent->waiter != NULL) {
            thread_wake(p->parent->waiter);
        }

        spin_unlock(&processes_lock, flags);
    }

    /* Never returns. The thread's slot and stacks go back to the pool. */
    thread_exit();
}

void process_reap(struct process *p)
{
    unsigned long flags;

    if (p == NULL) {
        return;
    }

    /* Under the lock `exited` is published under, for the reason
     * `process_wait` gives. */
    flags = spin_lock(&processes_lock);

    if (p->in_use && p->exited) {
        p->in_use = false;
    }

    spin_unlock(&processes_lock, flags);
}

/*
 * Whether an address range belongs to the process.
 *
 * This is the check that stands between a syscall and an arbitrary read of
 * kernel memory. A process handing over a kernel pointer is not caught by
 * the MMU: the kernel dereferences it privileged, where that mapping is
 * valid
 * and privileged. Nothing about the hardware notices; only this does.
 *
 * It walks the process's own page tables rather than comparing against a
 * range, so it answers the question actually being asked, which is whether
 * that address is mapped *for this process* with the permission needed.
 */
static bool range_ok(const struct process *p, uintptr_t va, size_t len,
                     bool need_write)
{
    uintptr_t page;
    uintptr_t last;

    if (p == NULL || p->space == NULL) {
        return false;
    }

    if (len == 0) {
        return true;
    }

    /* Overflow would otherwise wrap a huge length into a small range and
     * pass a check it should fail. */
    if (va + len < va) {
        return false;
    }

    last = va + len - 1;

    for (page = va & ~(uintptr_t)PAGE_MASK;
         page <= (last & ~(uintptr_t)PAGE_MASK);
         page += PAGE_SIZE) {
        /*
         * Asked of the address space rather than decoded here.
         *
         * This is the check on every pointer a process hands the kernel,
         * and it used to read the descriptor's AP field directly - which
         * meant the most security-critical line in the kernel was also the
         * one most quietly tied to one processor. On a machine where those
         * bits mean something else it would not crash; it would answer
         * wrongly.
         */
        if (!as_user_may(p->space, page, need_write)) {
            return false;
        }
    }

    return true;
}

bool process_may_read(const struct process *p, uintptr_t va, size_t len)
{
    return range_ok(p, va, len, false);
}

bool process_may_write(const struct process *p, uintptr_t va, size_t len)
{
    return range_ok(p, va, len, true);
}
