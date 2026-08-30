#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "process.h"
#include "thread.h"
#include "mmu.h"
#include "page.h"
#include "pmm.h"
#include "panic.h"
#include "console.h"

/* In arch/aarch64/el0.S. Does not return. */
void enter_el0(uintptr_t entry, uintptr_t user_sp, unsigned long arg);

static struct process processes[PROCESS_MAX];
static unsigned next_id = 1;


void process_init(void)
{
    unsigned i;

    for (i = 0; i < PROCESS_MAX; i++) {
        processes[i].in_use = false;
    }
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

    for (i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].in_use && !processes[i].exited) {
            n++;
        }
    }

    return n;
}

static struct process *alloc_process(void)
{
    unsigned i;

    for (i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].in_use) {
            return &processes[i];
        }
    }

    return NULL;
}

/*
 * The thread side of a process.
 *
 * Runs once, in kernel context, to install the address space and then hand
 * the CPU to EL0. It never comes back: from the eret onwards this thread is
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
    enter_el0(USER_TEXT_VA + USER_IMAGE_HEADER, USER_STACK_TOP, p->arg);
}

struct process *process_create(const char *name, const void *image,
                               size_t len, unsigned long arg)
{
    struct process *p = alloc_process();
    const uint64_t *header = image;
    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    size_t rx_bytes;
    size_t rx_pages;
    size_t i;

    if (p == NULL || pages == 0) {
        return NULL;
    }

    /*
     * The header is checked rather than trusted, even though the only image
     * that exists is built alongside the kernel. A loader that assumes its
     * input is well formed is a loader that will be wrong exactly once, and
     * at M8 the images come off a disk.
     */
    if (len < USER_IMAGE_HEADER || header[0] != USER_IMAGE_MAGIC) {
        return NULL;
    }

    rx_bytes = (size_t)header[1];
    rx_pages = rx_bytes / PAGE_SIZE;

    if ((rx_bytes & PAGE_MASK) != 0 || rx_pages == 0 || rx_pages > pages) {
        return NULL;
    }

    memset(p, 0, sizeof(*p));

    p->space = as_create();
    if (p->space == NULL) {
        return NULL;
    }

    /*
     * Copied rather than mapped where it sits. The image's pages inside the
     * kernel are the kernel's: EL1-only, and shared by every process. Giving
     * them EL0 permissions would give them to everything at once, and two
     * processes from the same image would share their writable data.
     */
    p->image_pages = pmm_alloc_contiguous(pages);
    if (p->image_pages == NULL) {
        as_destroy(p->space);
        return NULL;
    }
    p->image_page_count = pages;

    memcpy(p->image_pages, image, len);

    /* The tail of the last page holds whatever the previous owner left,
     * which this process would otherwise be able to read. */
    memset((char *)p->image_pages + len, 0, pages * PAGE_SIZE - len);

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
    if (as_map(p->space, USER_TEXT_VA, (uintptr_t)p->image_pages,
               rx_pages, MAP_USER_RX) != AS_OK) {
        goto fail;
    }

    if (pages > rx_pages
        && as_map(p->space, USER_TEXT_VA + rx_bytes,
                  (uintptr_t)p->image_pages + rx_bytes,
                  pages - rx_pages, MAP_USER_RW) != AS_OK) {
        goto fail;
    }

    if (as_map(p->space, USER_HEAP_VA, (uintptr_t)p->heap_pages,
               USER_HEAP_PAGES, MAP_USER_RW) != AS_OK) {
        goto fail;
    }

    if (as_map(p->space, USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
               (uintptr_t)p->stack_pages, USER_STACK_PAGES,
               MAP_USER_RW) != AS_OK) {
        goto fail;
    }

    p->in_use = true;
    p->exited = false;
    p->id = next_id++;
    p->arg = arg;

    for (i = 0; i + 1 < PROCESS_NAME_MAX && name[i] != '\0'; i++) {
        p->name[i] = name[i];
    }
    p->name[i] = '\0';

    p->thread = thread_create_suspended(p->name, process_main, p);

    if (p->thread == NULL) {
        p->in_use = false;
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
    for (i = 0; i < pages; i++) {
        pmm_free_page((char *)p->image_pages + i * PAGE_SIZE);
    }
    as_destroy(p->space);
    p->in_use = false;
    return NULL;
}

void process_grant_console(struct process *p)
{
    if (p != NULL) {
        p->owns_console = true;
    }
}

void process_start(struct process *p)
{
    if (p != NULL && p->thread != NULL) {
        thread_wake(p->thread);
    }
}

void process_exit(struct process *p, int code)
{
    size_t i;

    if (p == NULL || p->exited) {
        return;
    }

    p->exit_code = code;
    p->exited = true;

    /*
     * Back to the kernel's own address space before the process's is taken
     * apart. The thread is still executing, and it is executing kernel code
     * mapped in both, but the moment as_destroy frees the tables the running
     * translation regime would be describing freed pages.
     */
    if (thread_current()->process == p) {
        as_switch(NULL);
        thread_current()->process = NULL;
        thread_current()->space = NULL;
    }

    for (i = 0; i < USER_STACK_PAGES; i++) {
        pmm_free_page((char *)p->stack_pages + i * PAGE_SIZE);
    }

    for (i = 0; i < USER_HEAP_PAGES; i++) {
        pmm_free_page((char *)p->heap_pages + i * PAGE_SIZE);
    }

    for (i = 0; i < p->image_page_count; i++) {
        pmm_free_page((char *)p->image_pages + i * PAGE_SIZE);
    }

    as_destroy(p->space);

    p->space = NULL;
    p->thread = NULL;
    p->image_pages = NULL;
    p->heap_pages = NULL;
    p->stack_pages = NULL;

    /*
     * `in_use` stays set. Everything expensive is gone; what is left is the
     * exit code, and the moment a process dies is exactly when somebody
     * wants to know why. process_reap releases the slot.
     */

    /* Never returns. The thread's slot and stacks go back to the pool. */
    thread_exit();
}

void process_reap(struct process *p)
{
    if (p == NULL || !p->in_use || !p->exited) {
        return;
    }

    p->in_use = false;
}

/*
 * Whether an address range belongs to the process.
 *
 * This is the check that stands between a syscall and an arbitrary read of
 * kernel memory. A process handing over a kernel pointer is not caught by
 * the MMU: the kernel dereferences it at EL1, where that mapping is valid
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
        uint64_t *entry = as_page_entry(p->space, page);
        uint64_t ap;

        if (entry == NULL || (*entry & 1) == 0) {
            return false;       /* not mapped in this process */
        }

        ap = (*entry >> 6) & 3;

        /* AP=01 is EL0 read/write; AP=11 is EL0 read-only. Anything else has
         * no EL0 access at all, which means it is the kernel's. */
        if (ap != 1 && ap != 3) {
            return false;
        }

        if (need_write && ap != 1) {
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
