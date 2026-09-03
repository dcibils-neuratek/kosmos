#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "process.h"
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

    for (i = 0; i < PROCESS_MAX && n < max; i++) {
        struct process *p = &processes[i];

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
         * memory. The image is as big as the program; the heap and the
         * stacks are the same for everybody.
         *
         * Shared regions are deliberately not counted. A region's pages are
         * already charged to whoever created it, and charging every process
         * that maps one would have two processes sharing a surface paying
         * for it twice - the same reason `next_share` is counted apart from
         * `next_map`.
         */
        out[n].pages     = (uint32_t)p->mapped_pages;
        out[n].held      = (uint32_t)(p->mapped_pages + p->image_page_count
                                      + USER_HEAP_PAGES + USER_STACK_PAGES);
        out[n].caps      = thread_cap_count(p->thread);
        out[n].priority  = (p->thread != NULL)
                           ? thread_effective_priority(p->thread)
                           : 0u;

        out[n].owns      = (p->owns_console ? 1u : 0u)
                         | (p->owns_screen ? 2u : 0u)
                         | (p->owns_disk ? 4u : 0u)
                         | (p->owns_procctl ? 8u : 0u);

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

    for (i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].in_use) {
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

    p->next_map     = USER_MAP_VA;
    p->next_share   = USER_SHARE_VA;
    p->mapped_pages = 0;

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
    p->image = image;
    p->image_len = len;

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

struct process *process_spawn(struct process *parent, unsigned long arg)
{
    struct process *child;

    if (parent == NULL || parent->image == NULL) {
        return NULL;
    }

    child = process_create(parent->name, parent->image, parent->image_len, arg);

    if (child != NULL) {
        child->parent = parent;
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

        for (i = 0; i < PROCESS_MAX; i++) {
            struct process *c = &processes[i];

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
                 * twice. */
                process_reap(c);
                return code;
            }

            any = true;
        }

        if (!any) {
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
            return -2;
        }

        parent->waiter = thread_current();
        thread_block();
        parent->waiter = NULL;
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

    for (i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &processes[i];

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
            return;
        }
    }
}

bool process_grant_disk(struct process *p)
{
    struct blkdev dev;

    if (p == NULL || !hal_blk_init(&dev)) {
        return false;
    }

    p->owns_disk = true;
    return true;
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
     * Rounded up from pitch * height rather than from width * height * 4,
     * because the pitch is padded and the last row runs to the end of its
     * stride. Getting this wrong leaves the bottom row unmapped, and the
     * fault would arrive on whatever happened to draw near the bottom of
     * the screen rather than at the mapping.
     */
    bytes = (size_t)fb.pitch * fb.height;
    pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (as_map(p->space, USER_SCREEN_VA, (uintptr_t)fb.pixels,
               pages, MAP_USER_RW) != AS_OK) {
        return false;
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

    for (i = 0; i < PROCESS_MAX; i++) {
        struct process *c = &processes[i];

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

    for (i = 0; i < PROCESS_MAX; i++) {
        struct process *c = &processes[i];

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
         * call fails, and the check on the way back to EL0 ends it.
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

    for (i = 0; i < p->image_page_count; i++) {
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
        uint64_t *entry = as_page_entry(p->space, va);

        if (entry != NULL && (*entry & DESC_VALID) != 0) {
            pmm_free_page((void *)(uintptr_t)(*entry & DESC_ADDR_MASK));
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

void process_abandon(struct process *p)
{
    if (p == NULL || !p->in_use) {
        return;
    }

    if (p->thread != NULL) {
        thread_abandon(p->thread);
        p->thread = NULL;
    }

    release_memory(p);
    p->in_use = false;
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

    p->exit_code = code;
    p->exited = true;

    /* Whoever is waiting for this one, if anyone is. */
    if (p->parent != NULL && p->parent->waiter != NULL) {
        thread_wake(p->parent->waiter);
    }

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
     * Capabilities before memory. A shared region's pages come back only
     * when the last capability naming it is dropped, and this thread's are
     * about to stop existing - so a process that exits holding one would
     * leak it for the life of the machine.
     */
    ipc_caps_release(thread_current());

    release_memory(p);
    p->thread = NULL;

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
