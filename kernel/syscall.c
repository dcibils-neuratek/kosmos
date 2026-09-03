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
#include "sched.h"
#include "thread.h"
#include "ipc.h"
#include "memobj.h"
#include "trap.h"
#include "console.h"
#include "screen.h"
#include "cpu.h"
#include "pmm.h"
#include "page.h"
#include "kernel.h"
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
                        uintptr_t sender_ptr, bool nonblocking,
                        unsigned long timeout)
{
    struct message msg;
    struct thread *sender = NULL;
    int status;

    if (!process_may_write(p, msg_ptr, sizeof(msg))
        || !process_may_write(p, sender_ptr, sizeof(uint64_t))) {
        return SYS_ERR_FAULT;
    }

    status = ipc_receive(cap, &msg, &sender, nonblocking, timeout);

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

/*
 * Spawn: a process makes another like itself.
 *
 * The child gets the parent's image, a boot word, and whatever capabilities
 * the parent chose to pass. Nothing else. Authority flows from parent to
 * child and never sideways, which is what makes an init that starts servers
 * the same kind of thing as a shell that starts a program.
 *
 * Every capability is resolved against the *parent's* table, so a parent
 * cannot hand over what it does not hold, and the child's indices are its
 * own, in the order they were given.
 */
static long sys_spawn(struct process *p, unsigned long arg, uintptr_t caps_ptr,
                      size_t ncaps, unsigned long flags)
{
    struct process *child;
    size_t i;

    if (ncaps > CAPS_PER_THREAD) {
        return SYS_ERR_NO_ROOM;
    }

    if (ncaps > 0 && !process_may_read(p, caps_ptr, ncaps * sizeof(int32_t))) {
        return SYS_ERR_FAULT;
    }

    /*
     * Checked before anything is built. A parent can only pass on the
     * console if it holds it, or any process could promote itself by
     * spawning a child and asking it to print. Refusing here rather than
     * after means the refusal costs nothing and there is no half-built
     * process to unpick.
     */
    if ((flags & SPAWN_CONSOLE) != 0 && !p->owns_console) {
        return SYS_ERR_DENIED;
    }

    /* And the same for the screen. A process cannot promote itself by
     * spawning a child and asking it to draw. */
    if ((flags & SPAWN_DISK) != 0 && !p->owns_disk) {
        return SYS_ERR_DENIED;
    }

    if ((flags & SPAWN_PROCCTL) != 0 && !p->owns_procctl) {
        return SYS_ERR_DENIED;
    }

    if ((flags & SPAWN_AUDIO) != 0 && !p->owns_audio) {
        return SYS_ERR_DENIED;          /* cannot pass on what it has not got */
    }

    if ((flags & SPAWN_SCREEN) != 0 && !p->owns_screen) {
        return SYS_ERR_DENIED;
    }

    child = process_spawn(p, arg);
    if (child == NULL) {
        return SYS_ERR_NO_ROOM;
    }

    for (i = 0; i < ncaps; i++) {
        cap_t from = (cap_t)((const int32_t *)caps_ptr)[i];

        if (ipc_cap_grant(child->thread, from) < 0) {
            /* It would run without something it was meant to have, which is
             * a well defined and useless state. Abandoned rather than
             * exited: it has not started, and process_exit would end the
             * caller's thread instead of its own. */
            process_abandon(child);
            return SYS_ERR_NO_ROOM;
        }
    }

    if ((flags & SPAWN_CONSOLE) != 0) {
        process_grant_console(child);
    }

    if ((flags & SPAWN_DISK) != 0 && !process_grant_disk(child)) {
        process_abandon(child);
        return SYS_ERR_NO_ROOM;
    }

    if ((flags & SPAWN_PROCCTL) != 0) {
        process_grant_procctl(child);
    }

    if ((flags & SPAWN_AUDIO) != 0 && !process_grant_audio(child)) {
        /* Not fatal: a machine with no sound device still runs a program
         * that would have liked some, and it finds out by being silent. */
    }

    if ((flags & SPAWN_SCREEN) != 0 && !process_grant_screen(child)) {
        /* Asked for a screen and could not be given one. It would run
         * without something it was meant to have, which is well defined and
         * useless. Abandoned rather than exited: it has not started. */
        process_abandon(child);
        return SYS_ERR_NO_ROOM;
    }

    process_start(child);
    return (long)child->id;
}

/*
 * Where the screen is, and how big.
 *
 * Reporting only. The mapping happened at spawn, when the parent granted the
 * screen, so there is nothing to set up here and no live page table to
 * modify - a process that holds the screen has held it since before it ran.
 *
 * The pitch is in the answer and the width is not enough on its own. It is
 * padded, and a caller that multiplies width by four writes a sheared image.
 * That is `gfx.md` §19.3's trap, and handing back the real number is the
 * only way the other side can avoid it.
 */
static long sys_screen(struct process *p, uintptr_t out_ptr)
{
    struct screen_info info;
    struct fb fb;

    if (!p->owns_screen) {
        return SYS_ERR_DENIED;
    }

    if (!screen_get(&fb)) {
        return SYS_ERR_DENIED;
    }

    if (!process_may_write(p, out_ptr, sizeof(info))) {
        return SYS_ERR_FAULT;
    }

    info.address  = USER_SCREEN_VA;
    info.width    = fb.width;
    info.height   = fb.height;
    info.pitch    = fb.pitch;
    info.reserved = 0;

    *(struct screen_info *)out_ptr = info;
    return 0;
}

/*
 * Raw sectors, for the one process that serves the filesystem.
 *
 * Through a bounce buffer, and that is not laziness. The device is handed a
 * *physical* address, and a process's buffer is a user virtual one whose
 * pages need not be contiguous - so passing it straight through would be
 * the same bug the fw_cfg work hit, where the kernel being identity mapped
 * made a virtual address look like it worked until it was somebody else's.
 * The copy also means the device never writes into a process's address
 * space directly, which is one fewer thing to be careful about.
 *
 * One filesystem block per call. A Lua caller that wants more loops, and a
 * fixed buffer is what `CLAUDE.md` asks for anyway - there is no allocator
 * here to size one against the request.
 */
#define DISK_CHUNK  4096u

static _Alignas(16) uint8_t disk_bounce[DISK_CHUNK];

static long sys_disk(struct process *p, bool writing, uint64_t sector,
                     uintptr_t buf, size_t bytes)
{
    if (!p->owns_disk) {
        return SYS_ERR_DENIED;
    }

    if (bytes == 0 || bytes > DISK_CHUNK
        || (bytes % HAL_BLK_SECTOR) != 0) {
        return SYS_ERR_FAULT;
    }

    if (writing) {
        if (!process_may_read(p, buf, bytes)) {
            return SYS_ERR_FAULT;
        }

        memcpy(disk_bounce, (const void *)buf, bytes);

        if (!hal_blk_write(sector, disk_bounce, (uint32_t)bytes)) {
            return SYS_ERR_FAULT;
        }
    } else {
        if (!process_may_write(p, buf, bytes)) {
            return SYS_ERR_FAULT;
        }

        if (!hal_blk_read(sector, disk_bounce, (uint32_t)bytes)) {
            return SYS_ERR_FAULT;
        }

        memcpy((void *)buf, disk_bounce, bytes);
    }

    return (long)bytes;
}

/*
 * The machine, and how much of it is in use.
 *
 * Reads registers and counts pools. It decodes nothing: the raw ID registers
 * go out as they were read, and what they mean is userland's problem - which
 * is the same division `design.md` §1 draws everywhere else, and it means a
 * new processor needs no kernel change to be described properly.
 */
static long sys_sysinfo(struct process *p, uintptr_t out_ptr)
{
    struct sysinfo info;
    struct cpu_info cpu;
    struct memrange ram;
    struct fb fb;
    uint64_t el;

    if (!process_may_write(p, out_ptr, sizeof(info))) {
        return SYS_ERR_FAULT;
    }

    /*
     * Zeroed before anything is filled in, and it was not.
     *
     * Every field here used to be assigned unconditionally, so nothing
     * noticed. The audio fields are the first that are only set when the
     * board has the device - and on a board without one they were whatever
     * the stack happened to hold. `may_pass_audio()` read a nonzero period
     * size on a silent machine, asked to pass on authority init did not
     * have, and the shell never started.
     *
     * The bug that found it is the small one. This struct is copied whole
     * into a process's memory, so any field the kernel does not write is
     * *kernel stack* handed to userland - and a leak like that is invisible
     * until somebody looks for it. One memset is the whole fix, and it has
     * to be here rather than at each new field, because the next person to
     * add a conditional one will not read this comment either.
     */
    memset(&info, 0, sizeof(info));

    cpu_identify(&cpu);
    hal_ram_range(&ram);

    info.midr       = cpu.midr;
    info.mpidr      = cpu.mpidr;
    info.ctr        = cpu.ctr;
    info.pfr0       = cpu.pfr0;
    info.isar0      = cpu.isar0;
    info.mmfr0      = cpu.mmfr0;
    info.counter_hz = cpu.counter_hz;

    info.ram_base    = ram.base;
    info.ram_size    = ram.size;
    info.pages_total = (uint32_t)pmm_total_pages();
    info.pages_free  = (uint32_t)pmm_free_pages();

    {
        unsigned long idle, busy;

        thread_load(&idle, &busy);
        info.idle_ticks = idle;
        info.busy_ticks = busy;
    }

    info.epoch = (uint64_t)hal_rtc_seconds();

    if (hal_snd_present()) {
        info.audio_rate     = HAL_SND_RATE;
        info.audio_channels = HAL_SND_CHANNELS;
        info.audio_period   = HAL_SND_PERIOD_BYTES;
        info.audio_periods  = HAL_SND_PERIODS;
        info.audio_dry      = hal_snd_dry();
        info.audio_floor    = hal_snd_floor();
    }

    info.threads_used     = thread_count();
    info.threads_total    = THREAD_MAX;
    info.processes_used   = process_count();
    info.processes_held   = process_slots_used();
    info.processes_total  = PROCESS_MAX;
    info.regions_used     = memobj_in_use();
    info.regions_total    = memobj_total();
    info.endpoints_used   = ipc_endpoints_in_use();
    info.endpoints_total  = ENDPOINT_MAX;
    info.spaces_used      = as_count();
    info.spaces_total     = as_total();

    if (screen_get(&fb)) {
        info.screen_width  = fb.width;
        info.screen_height = fb.height;
        info.screen_pitch  = fb.pitch;
    } else {
        info.screen_width  = 0;
        info.screen_height = 0;
        info.screen_pitch  = 0;
    }

    /*
     * Through hal_keyboard_init, which is idempotent and returns whether
     * there is one. Not through the board's own header: `CLAUDE.md` puts no
     * hardware knowledge outside hal/, and a kernel file that includes
     * hal/qemu-virt/qemu-virt.h has quietly made the kernel board-specific.
     * The first draft of this did exactly that and would not compile, which
     * is the include path doing its job.
     */
    info.has_keyboard = hal_keyboard_init() ? 1u : 0u;

    /* One, and it will stay one until M7. `CLAUDE.md` has the code written
     * SMP-ready from the start, but written ready and actually running on
     * more than one core are different claims and this reports the second. */
    info.cpus       = 1;
    info.tick_hz    = TICK_HZ;
    info.page_size  = PAGE_SIZE;

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    info.current_el = (uint32_t)((el >> 2) & 3);

    *(struct sysinfo *)out_ptr = info;
    return 0;
}

/*
 * Pages, for the things that do not fit on a heap.
 *
 * A surface is the case this exists for: `gfx.md` §19.1 puts pixels in flat
 * bytes behind a userdata, and a full-screen one is 3.2 MB against a 2 MB
 * process heap that is small on purpose. So a process can ask the kernel for
 * pages directly, and they are its own - mapped only into its address space,
 * counted against its own budget, and returned when it exits whether or not
 * it remembered to unmap them.
 *
 * Not contiguous. Each page is allocated on its own and mapped where the
 * bump pointer says, because the CPU reaches them through the MMU and does
 * not care, and demanding four hundred contiguous pages of a bitmap
 * allocator is how an allocation fails on a machine with plenty free.
 *
 * The one caller that *would* care is a device reading the memory itself,
 * and when a shared surface has to be handed to a GPU that is the moment
 * this needs a contiguous variant - not before.
 */
static long sys_map(struct process *p, size_t pages)
{
    uintptr_t base;
    size_t i;

    if (pages == 0) {
        return SYS_ERR_NO_ROOM;
    }

    if (pages > USER_MAP_PAGES_MAX
        || p->mapped_pages + pages > USER_MAP_PAGES_MAX) {
        return SYS_ERR_NO_ROOM;
    }

    /* The addresses are never reused, so this is what stops a process that
     * maps and unmaps for long enough from walking into the next window. */
    if (p->next_map + pages * PAGE_SIZE > USER_MAP_END) {
        return SYS_ERR_NO_ROOM;
    }

    base = p->next_map;

    for (i = 0; i < pages; i++) {
        void *page = pmm_alloc_page();

        if (page == NULL) {
            break;
        }

        /* Zeroed before the process can see it. A fresh page holding
         * whatever the last owner left is how one process reads another's
         * memory without either of them doing anything wrong. */
        memset(page, 0, PAGE_SIZE);

        if (as_map(p->space, base + i * PAGE_SIZE, (uintptr_t)page,
                   1, MAP_USER_RW) != AS_OK) {
            pmm_free_page(page);
            break;
        }
    }

    if (i < pages) {
        /* Unwound rather than left half done. A partial mapping the caller
         * was told nothing about is worse than no mapping. */
        size_t j;

        for (j = 0; j < i; j++) {
            uintptr_t va = base + j * PAGE_SIZE;
            uint64_t *entry = as_page_entry(p->space, va);

            if (entry != NULL && (*entry & DESC_VALID) != 0) {
                void *page = (void *)(uintptr_t)(*entry & DESC_ADDR_MASK);

                (void)as_unmap(p->space, va, 1);
                pmm_free_page(page);
            }
        }

        return SYS_ERR_NO_ROOM;
    }

    p->next_map      = base + pages * PAGE_SIZE;
    p->mapped_pages += pages;

    return (long)base;
}

static long sys_unmap(struct process *p, uintptr_t va, size_t pages)
{
    size_t i;
    size_t freed = 0;

    /*
     * Only inside the region SYS_MAP hands out, and only what is actually
     * mapped. A process cannot use this to unmap its own code, its stack or
     * the screen: those are outside the range, and the check is the range
     * rather than a list of what is special.
     */
    if ((va & (PAGE_SIZE - 1)) != 0) {
        return SYS_ERR_FAULT;
    }

    /*
     * `pages` is bounded before it is multiplied. The loop below runs
     * `pages` times whatever the arithmetic says, and 2^52 + 1 pages
     * multiplies to 4096 - a request that looks like one page and iterates
     * four and a half quadrillion times. `sys_map` has always checked this
     * and this had not.
     */
    if (va < USER_MAP_VA || pages == 0 || pages > USER_MAP_PAGES_MAX
        || va + pages * PAGE_SIZE > p->next_map) {
        return SYS_ERR_FAULT;
    }

    for (i = 0; i < pages; i++) {
        uintptr_t at = va + i * PAGE_SIZE;
        uint64_t *entry = as_page_entry(p->space, at);

        if (entry == NULL || (*entry & DESC_VALID) == 0) {
            continue;           /* already gone; unmapping twice is not an error */
        }

        {
            void *page = (void *)(uintptr_t)(*entry & DESC_ADDR_MASK);

            (void)as_unmap(p->space, at, 1);
            pmm_free_page(page);
            freed++;
        }
    }

    p->mapped_pages -= (freed < p->mapped_pages) ? freed : p->mapped_pages;

    /* The address is not reused. See USER_MAP_VA: reclaiming it would need
     * an allocator, and there is 512 GB of it. */
    return 0;
}

static long sys_setname(struct process *p, uintptr_t ptr, size_t len)
{
    if (len > 64) {
        len = 64;
    }

    if (len > 0 && !process_may_read(p, ptr, len)) {
        return SYS_ERR_FAULT;
    }

    process_set_name(p, (const char *)ptr, len);
    return 0;
}

static long sys_proctable(struct process *p, uintptr_t out_ptr, size_t max)
{
    if (max == 0 || max > PROCESS_MAX) {
        max = PROCESS_MAX;
    }

    if (!process_may_write(p, out_ptr, max * sizeof(struct proc_info))) {
        return SYS_ERR_FAULT;
    }

    return (long)process_table((struct proc_info *)out_ptr, (unsigned)max);
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

    case SYS_SND_WRITE:
        if (!p->owns_audio) {
            result = SYS_ERR_DENIED;
        } else if (tf->x[1] == 0 || tf->x[1] > HAL_SND_PERIOD_BYTES) {
            result = SYS_ERR_FAULT;
        } else if (!process_may_read(p, tf->x[0], (size_t)tf->x[1])) {
            result = SYS_ERR_FAULT;
        } else {
            /* Full is not a failure. See SYS_SND_WRITE in syscall.h: a
             * caller ahead of the device is told so rather than blocked. */
            result = hal_snd_write((const void *)tf->x[0],
                                   (unsigned)tf->x[1]) ? 0 : SYS_NO_INPUT;
        }

        break;

    case SYS_SND_QUEUED:
        result = p->owns_audio ? (long)hal_snd_queued() : SYS_ERR_DENIED;
        break;

    case SYS_POWER:
        if (!p->owns_procctl) {
            result = SYS_ERR_DENIED;
        } else {
            /*
             * Said before it happens, because after it there is nobody to
             * say anything. The console is the kernel's here, so this
             * reaches the serial line whatever userland was doing.
             */
            kputs((tf->x[0] == 1) ? "\nkosmos: restarting\n"
                                  : "\nkosmos: powering off\n");

            if (tf->x[0] == 1) {
                hal_restart();
            } else {
                hal_power_off();
            }

            /* Only if the firmware refused. */
            result = SYS_ERR_DENIED;
        }

        break;

    case SYS_KEY_EVENT: {
        /*
         * Two out-parameters rather than a packed return, because a
         * keycode and a boolean are two facts and encoding them into one
         * integer would mean a decode ring on the other side for no gain.
         */
        unsigned code = 0;
        bool down = false;

        if (!p->owns_console) {
            result = SYS_ERR_DENIED;
        } else if (!process_may_write(p, tf->x[0], sizeof(unsigned))
                   || !process_may_write(p, tf->x[1], sizeof(unsigned))) {
            result = SYS_ERR_FAULT;
        } else if (!hal_key_event(&code, &down)) {
            result = SYS_NO_INPUT;
        } else {
            *(unsigned *)tf->x[0] = code;
            *(unsigned *)tf->x[1] = down ? 1u : 0u;
            result = 0;
        }

        break;
    }

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

    case SYS_POINTER: {
        /*
         * Guarded exactly as SYS_GETCHAR is, and for the same reason: input
         * has one reader. A second process polling the tablet would take
         * events the first never sees, and the two would disagree about
         * where the pointer is with nothing to say which was right.
         */
        struct pointer_state state;

        if (!p->owns_console) {
            result = SYS_ERR_DENIED;
        } else if (!process_may_write(p, tf->x[0], sizeof(struct pointer_info))) {
            result = SYS_ERR_FAULT;
        } else if (!hal_pointer_poll(&state)) {
            result = SYS_ERR_DENIED;    /* there is no pointer on this board */
        } else {
            struct pointer_info *out = (struct pointer_info *)tf->x[0];

            out->x       = state.x;
            out->y       = state.y;
            out->min_x   = state.min_x;
            out->max_x   = state.max_x;
            out->min_y   = state.min_y;
            out->max_y   = state.max_y;
            out->buttons = state.buttons;
            out->moved   = state.moved;
            result = 0;
        }
        break;
    }

    case SYS_SCREEN_TAKE:
        /*
         * "I am drawing the whole screen now; stop printing on it."
         *
         * Only the process that holds the screen may say it, which is the
         * same test SYS_SCREEN uses - a process that cannot draw has no
         * business deciding who else may.
         *
         * The kernel console keeps writing to the serial line either way.
         * Suspending it is about pixels, not about output: everything still
         * reaches the cable, which is what a system is debugged through.
         */
        if (!p->owns_screen) {
            result = SYS_ERR_DENIED;
        } else {
            if (tf->x[0] != 0) {
                console_screen_suspend();
            } else {
                console_screen_resume();
            }
            result = 0;
        }
        break;

    case SYS_KILL:
        /*
         * Only a parent may end a child, which is the authority
         * `process_wait` already implies. Nothing new is granted: a process
         * that started something may end it, and holding a capability to
         * somebody is not the same as being allowed to kill them.
         */
        if (p->owns_procctl) {
            /* Granted authority over every process. See SPAWN_PROCCTL: the
             * rule is not relaxed, one process is trusted with more than
             * it. */
            result = (process_kill_any((unsigned)tf->x[0]) == 0)
                     ? 0 : SYS_ERR_NO_CHILD;
        } else {
            result = (process_kill(p, (unsigned)tf->x[0]) == 0)
                     ? 0 : SYS_ERR_NO_CHILD;
        }
        break;

    case SYS_WAIT_INPUT:
        /*
         * Sleep until a key or the pointer moves, or until `ticks` have
         * passed, whichever is first.
         *
         * The whole point of this system call is *not* running. Everything
         * above this used to poll - the window manager asked the console for
         * keys, got none, yielded, and asked again - and one thread that is
         * always runnable keeps a core at a hundred per cent for ever, with
         * a processor meter that reads ninety per cent on an empty desktop
         * and is telling the truth.
         *
         * Restricted to the console owner exactly as `getchar` and the
         * pointer are, and for the same reason: input has one reader.
         *
         * The pending flag is checked before sleeping. An interrupt that
         * arrived between the last read and this call would otherwise be
         * slept through, which is the classic way to build a race that shows
         * up as one lost keystroke in a hundred.
         */
        if (!p->owns_console) {
            result = SYS_ERR_DENIED;
        } else if (hal_input_pending()) {
            result = 0;
        } else {
            /*
             * **Timer ticks, not counter ticks.**
             *
             * There are two clocks here and they differ by a factor of six
             * hundred thousand. `SYS_TICKS` hands out the physical counter,
             * which runs at CNTFRQ_EL0 - 62.5 MHz on this machine - and is
             * what every program uses for measuring. `hal_ticks` counts
             * scheduler ticks, at TICK_HZ, and is the only clock a sleep can
             * be against because it is the only one that interrupts.
             *
             * Passing one where the other was meant asks for a sleep of
             * ten thousand seconds and looks exactly like a hang. It was
             * written that way first.
             */
            thread_sleep_until(hal_ticks() + (unsigned long)tf->x[0]);
            result = 0;
        }
        break;

    case SYS_SLEEP:
        /*
         * The same sleep, for anybody, without the input half.
         *
         * Capped, because a sleep is the one call whose argument nobody
         * checks against anything: a program that computes a deadline
         * wrongly and asks for four billion ticks is asking to be gone for
         * a year and a half, and it looks exactly like a hang. An hour is
         * far longer than anything here waits for and far shorter than a
         * mistake.
         *
         * Zero ticks is a yield and is written as one. `thread_sleep_until`
         * already returns immediately for a deadline that has passed, so
         * this is only about saying what happens rather than relying on it.
         */
        if (tf->x[0] == 0) {
            thread_yield();
        } else {
            unsigned long ticks = (unsigned long)tf->x[0];

            if (ticks > (unsigned long)TICK_HZ * 3600UL) {
                ticks = (unsigned long)TICK_HZ * 3600UL;
            }

            thread_sleep_until(hal_ticks() + ticks);
        }

        result = 0;
        break;

    case SYS_LOG: {
        /*
         * What this machine has printed, kernel and processes together.
         *
         * No permission check, and that is a decision rather than an
         * oversight: this is what was already printed to a serial line
         * anybody watching could read, and to a screen anybody looking at
         * could see. Making it a capability would protect nothing and would
         * mean a log viewer had to be privileged, which is the wrong shape
         * for a thing whose whole job is to be looked at.
         *
         * If output ever carries something that should not be shared, the
         * fix is not to print it.
         */
        size_t max = (size_t)tf->x[1];

        if (max > 16384) {
            max = 16384;
        }

        if (!process_may_write(p, tf->x[0], max)) {
            result = SYS_ERR_FAULT;
            break;
        }

        result = (long)console_log((char *)tf->x[0], max);
        break;
    }

    case SYS_MEM_CREATE: {
        /*
         * A region two processes can share, and a capability naming it.
         *
         * No permission check, and none is needed: this allocates the
         * caller's own memory and hands it a name only the caller holds.
         * The authority is in the *passing* - a region reaches a second
         * process only by somebody sending the capability - and that is
         * checked where every other capability transfer is, in
         * `message_deliver`.
         *
         * Not mapped here. Creating and mapping are separate because the
         * process that creates a region is often not the one that draws
         * into it, and a create that also mapped would put pages in the
         * address space of a process that only wanted to hand them on.
         */
        struct memobj *m = memobj_create((size_t)tf->x[0]);

        if (m == NULL) {
            result = SYS_ERR_NO_ROOM;
            break;
        }

        result = ipc_install_memory(thread_current(), m);

        if (result < 0) {
            /*
             * The region was made and there is nowhere to put the
             * capability to it: this thread's table is full.
             *
             * Reported as itself rather than folded into NO_ROOM. The two
             * are completely different problems - one is a machine out of
             * memory, the other is one process holding too many things -
             * and saying "no room" for both cost an evening of looking at
             * the allocator while 117,000 pages sat free.
             */
            memobj_unref(m);            /* the create's own reference */
            result = SYS_ERR_NO_CAPS;
            break;
        }

        /* `install` took a reference of its own; the create's is spent. */
        memobj_unref(m);
        break;
    }

    case SYS_MEM_MAP: {
        /*
         * The region into this process's address space.
         *
         * At the same place any other mapping goes, and counted against the
         * same limit, so a process cannot map its way past what it is
         * allowed to have by asking for regions instead of pages.
         */
        struct memobj *m = ipc_resolve_memory(thread_current(),
                                              (cap_t)tf->x[0]);
        uintptr_t base;
        size_t i;

        if (m == NULL) {
            result = SYS_ERR_DENIED;
            break;
        }

        /*
         * Bounded by address, not by the SYS_MAP budget. Those pages are
         * charged to whoever created the region; charging them again to
         * everybody who maps it would mean a compositor and an app sharing
         * one surface pay for it twice, and the second one to ask would be
         * refused memory that is already allocated.
         */
        if (p->next_share + m->pages * PAGE_SIZE > USER_SHARE_END) {
            result = SYS_ERR_NO_ROOM;
            break;
        }

        base = p->next_share;

        for (i = 0; i < m->pages; i++) {
            if (as_map(p->space, base + i * PAGE_SIZE,
                       (uintptr_t)memobj_page(m, i),
                       1, MAP_USER_RW) != AS_OK) {
                break;
            }
        }

        if (i < m->pages) {
            size_t j;

            for (j = 0; j < i; j++) {
                as_unmap(p->space, base + j * PAGE_SIZE, 1);
            }

            result = SYS_ERR_NO_ROOM;
            break;
        }

        p->next_share += m->pages * PAGE_SIZE;

        result = (long)base;
        break;
    }

    case SYS_MEM_SIZE: {
        struct memobj *m = ipc_resolve_memory(thread_current(),
                                              (cap_t)tf->x[0]);

        result = (m == NULL) ? SYS_ERR_DENIED : (long)m->pages;
        break;
    }

    case SYS_DISK_INFO: {
        struct diskinfo info;
        struct blkdev dev;

        if (!process_may_write(p, (uintptr_t)tf->x[0], sizeof(info))) {
            result = SYS_ERR_FAULT;
            break;
        }

        /* Readable without holding the disk. It says whether there is one
         * and how big it is, which is not authority over it - and init has
         * to be able to ask before deciding whether to start a filesystem
         * server at all. */
        if (hal_blk_init(&dev)) {
            info.sectors     = dev.sectors;
            info.sector_size = dev.sector_size;
        } else {
            info.sectors     = 0;
            info.sector_size = 0;
        }

        info.reserved = 0;
        *(struct diskinfo *)(uintptr_t)tf->x[0] = info;
        result = 0;
        break;
    }

    case SYS_DISK_READ:
        result = sys_disk(p, false, tf->x[0], (uintptr_t)tf->x[1],
                          (size_t)tf->x[2]);
        break;

    case SYS_DISK_WRITE:
        result = sys_disk(p, true, tf->x[0], (uintptr_t)tf->x[1],
                          (size_t)tf->x[2]);
        break;

    case SYS_BOOT_OPT: {
        /*
         * What the machine was started with.
         *
         * Readable by anybody: it is a string somebody typed on the QEMU
         * command line, and treating it as a secret would be pretending it
         * is one. init is the only caller that has a use for it.
         */
        char name[64];
        char value[128];
        size_t len = (size_t)tf->x[2];

        if (!process_may_read(p, (uintptr_t)tf->x[0], 1)
            || !process_may_write(p, (uintptr_t)tf->x[1], len)) {
            result = SYS_ERR_FAULT;
            break;
        }

        {
            const char *from = (const char *)(uintptr_t)tf->x[0];
            size_t i;

            for (i = 0; i + 1 < sizeof(name) && from[i] != '\0'; i++) {
                name[i] = from[i];
            }

            name[i] = '\0';
        }

        if (!hal_boot_option(name, value, sizeof(value))) {
            result = 0;                 /* no such option; not an error */
            break;
        }

        {
            char *to = (char *)(uintptr_t)tf->x[1];
            size_t i;

            for (i = 0; i + 1 < len && value[i] != '\0'; i++) {
                to[i] = value[i];
            }

            to[i] = '\0';
            result = (long)i;
        }

        break;
    }

    case SYS_YIELD:
        thread_yield();
        result = 0;
        break;

    case SYS_TICKS: {
        /*
         * The physical counter, straight out of the register.
         *
         * Not a wall clock and not pretending to be one: it counts from
         * whenever the machine started, at CNTFRQ_EL0 Hz, and says nothing
         * about what time it is. A date is `/dev/clock`'s job.
         *
         * No permission check. It is not authority - every process can
         * already time itself by counting yields, only worse - and a
         * benchmark or a frame loop that has to ask for the clock is a
         * benchmark that measures the asking.
         *
         * isb first, or the read can be reordered ahead of whatever the
         * caller was timing. The read is cheap; the barrier is the part
         * that makes the answer mean anything.
         */
        uint64_t t;
        __asm__ volatile("isb" ::: "memory");
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
        result = (long)t;
        break;
    }

    case SYS_ENDPOINT:
        result = ipc_endpoint_create();
        break;

    case SYS_ENDPOINT_DESTROY:
        /*
         * The other half of SYS_ENDPOINT, which was missing.
         *
         * Endpoints are a pool of 96 and every program launched consumed
         * one for ever, because the kernel could destroy them and nothing
         * could ask it to. Ninety runs and the system was out.
         *
         * No permission check beyond the capability itself: the index is
         * resolved against this thread's own table, so a process can only
         * destroy an endpoint it was given, and everything blocked on it is
         * woken with an error rather than left waiting - which is the
         * behaviour M3 built and tested.
         */
        result = ipc_endpoint_destroy((cap_t)tf->x[0]);
        break;

    case SYS_SHARE_UNMAP: {
        /*
         * A shared region out of this process's share window.
         *
         * `SYS_UNMAP` cannot do this and should not: it is bounded to the
         * window `SYS_MAP` hands out and it *frees* what it unmaps, which is
         * correct there and catastrophic here. The pages under a shared
         * region belong to the memobj, and the memobj frees them when its
         * last capability goes. Freeing them from the mapper's side is the
         * double free that `USER_SHARE_VA` was introduced to prevent.
         *
         * So this clears page table entries and nothing else, and it exists
         * because giving a capability back has to mean losing access to it.
         * Without it `sys.release` dropped the right to *name* a region while
         * keeping the ability to read and write it, which is not a capability
         * system, it is a capability system with a hole in it.
         */
        uintptr_t va    = tf->x[0];
        size_t    pages = (size_t)tf->x[1];

        if ((va & (PAGE_SIZE - 1)) != 0 || pages == 0) {
            result = SYS_ERR_FAULT;
            break;
        }

        /*
         * And the count before the multiply, for the reason `user_range`
         * now gives: a page count large enough to wrap it comes back
         * inside the window and takes the loop in `as_unmap` with it.
         */
        if (pages > (USER_SHARE_END - USER_SHARE_VA) / PAGE_SIZE) {
            result = SYS_ERR_FAULT;
            break;
        }

        if (va < USER_SHARE_VA
            || va + pages * PAGE_SIZE > p->next_share) {
            result = SYS_ERR_FAULT;
            break;
        }

        result = (as_unmap(p->space, va, pages) == AS_OK) ? 0 : SYS_ERR_FAULT;
        break;
    }

    case SYS_SCHED_INFO: {
        /*
         * Readable by anyone. It says how the machine is scheduled, which
         * is not authority over anything and is exactly what a settings app
         * and `htop` both want.
         */
        struct schedinfo info;
        unsigned i;

        if (!process_may_write(p, (uintptr_t)tf->x[0], sizeof(info))) {
            result = SYS_ERR_FAULT;
            break;
        }

        memset(&info, 0, sizeof(info));

        info.policy     = sched_policy_index();
        info.policies   = sched_policy_count();
        info.quantum    = sched_get_quantum();
        info.tick_hz    = TICK_HZ;
        info.priorities = SCHED_PRIORITIES;

        if (info.policies > SCHED_POLICY_MAX) {
            info.policies = SCHED_POLICY_MAX;
        }

        for (i = 0; i < info.policies; i++) {
            const char *name = sched_policy_name(i);
            unsigned    j;

            for (j = 0; j + 1 < SCHED_NAME_MAX && name[j] != '\0'; j++) {
                info.name[i][j] = name[j];
            }
        }

        memcpy((void *)(uintptr_t)tf->x[0], &info, sizeof(info));
        result = 0;
        break;
    }

    case SYS_SCHED_SET:
        /*
         * Writable by anyone too, and that is a decision rather than an
         * oversight.
         *
         * Changing the quantum or the policy is tuning the machine you are
         * sitting at, not reaching into another process - and on a
         * single-user system there is nobody to defend it from. What is
         * deliberately *not* here is setting a priority: bands are handed
         * out by capability (`process_grant_screen` gives the compositor
         * DISPLAY) precisely so that nothing can promote itself, and a
         * syscall that let it would undo that in one line.
         *
         * If this ever needs an owner, the shape already exists: a spawn
         * grant, the way `SPAWN_DISK` hands the disk to exactly one child.
         */
        switch ((unsigned)tf->x[0]) {
        case SCHED_SET_QUANTUM:
            /*
             * Refused rather than clamped. The setter clamps too, because
             * it is called from inside the kernel as well, but a process
             * that asked for four hundred days should be told it did not
             * get them instead of being answered "yes" and given one
             * second.
             */
            if (tf->x[1] == 0 || tf->x[1] > SCHED_QUANTUM_MAX) {
                result = SYS_ERR_DENIED;
                break;
            }

            sched_set_quantum((unsigned)tf->x[1]);
            result = 0;
            break;

        case SCHED_SET_POLICY:
            result = sched_switch_to((unsigned)tf->x[1]) ? 0 : SYS_ERR_DENIED;
            break;

        default:
            result = SYS_ERR_DENIED;
            break;
        }
        break;

    case SYS_CAP_DROP:
        /*
         * A capability this thread holds, given back.
         *
         * The counterpart of receiving one, and until now there was none:
         * `ipc_caps_release` ran when a thread died and nothing else ever
         * released a slot. Sixteen received regions and a server was full
         * for good.
         *
         * No permission check beyond the index, for the same reason
         * SYS_ENDPOINT_DESTROY needs none: it resolves against this
         * thread's own table, so a process can only drop what it was given.
         */
        result = ipc_cap_drop(thread_current(), (cap_t)tf->x[0]);
        break;

    case SYS_CALL:
        result = sys_call(p, (cap_t)tf->x[0], tf->x[1], tf->x[2]);
        break;

    case SYS_RECEIVE:
        /*
         * x3 bit 0 asks not to block, the same way SYS_WAIT's x1 does, and
         * x4 is how many scheduler ticks to wait before giving up - zero
         * for the old behaviour, which is to wait for ever.
         *
         * Capped like `SYS_SLEEP` is and for the same reason: a deadline
         * computed wrongly is a server that stops answering for a year, and
         * it looks exactly like a hang.
         */
        {
            unsigned long timeout = (unsigned long)tf->x[4];

            if (timeout > (unsigned long)TICK_HZ * 3600UL) {
                timeout = (unsigned long)TICK_HZ * 3600UL;
            }

            result = sys_receive(p, (cap_t)tf->x[0], tf->x[1], tf->x[2],
                                 (tf->x[3] & 1u) != 0, timeout);
        }
        break;

    case SYS_REPLY:
        result = sys_reply(p, tf->x[0], tf->x[1]);
        break;

    case SYS_SPAWN:
        result = sys_spawn(p, tf->x[0], tf->x[1], (size_t)tf->x[2], tf->x[3]);
        break;

    case SYS_SCREEN:
        result = sys_screen(p, tf->x[0]);
        break;

    case SYS_SYSINFO:
        result = sys_sysinfo(p, tf->x[0]);
        break;

    case SYS_MAP:
        result = sys_map(p, (size_t)tf->x[0]);
        break;

    case SYS_UNMAP:
        result = sys_unmap(p, tf->x[0], (size_t)tf->x[1]);
        break;

    case SYS_SETNAME:
        result = sys_setname(p, tf->x[0], (size_t)tf->x[1]);
        break;

    case SYS_PROCTABLE:
        result = sys_proctable(p, tf->x[0], (size_t)tf->x[1]);
        break;

    case SYS_WAIT: {
        /*
         * x1 bit 0 asks not to block. A shell draining the processes it
         * spawned must not stop at the prompt for ten seconds because one
         * of them is still running.
         */
        unsigned id = 0;
        uintptr_t id_ptr = tf->x[0];

        if (id_ptr != 0 && !process_may_write(p, id_ptr, sizeof(uint64_t))) {
            result = SYS_ERR_FAULT;
            break;
        }

        result = process_wait(p, &id, (tf->x[1] & 1u) != 0);

        if (result == -2) {
            result = SYS_NO_CHILD_READY;
        } else if (result < 0) {
            result = SYS_ERR_NO_CHILD;
        } else if (id_ptr != 0) {
            *(uint64_t *)id_ptr = (uint64_t)id;
        }
        break;
    }

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
