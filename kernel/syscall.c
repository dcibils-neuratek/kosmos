/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The syscall dispatcher.
 *
 * Every one of these runs on behalf of a process that is not trusted, on the
 * kernel's side of a boundary the hardware enforces. Two rules follow, and
 * they are the whole of the file's discipline:
 *
 * **Every pointer is checked before it is touched.** A process handing over
 * a kernel address is not caught by the MMU, because the kernel dereferences
 * it privileged, where that mapping is valid. `process_may_read`
 * and `process_may_write` are the only thing between a syscall and an
 * arbitrary read of kernel memory.
 *
 * **Every failure is a return value.** A syscall never panics on anything a
 * process can cause. A process that passes nonsense gets an error; a kernel
 * that panics on nonsense hands any process the power to stop the machine.
 */

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "syscall.h"
#include "irq.h"
#include "process.h"
#include "sched.h"
#include "smp.h"
#include "thread.h"
#include "ipc.h"
#include "memobj.h"
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

/*
 * `colour` is 0xAARRGGBB, and zero means "whatever the console is already
 * using" - so a caller with no opinion passes nothing new and nothing about
 * its output changes. It is a third argument rather than a mode because the
 * console has several writers; `console.h` has the argument in full.
 *
 * **Every caller must pass three registers**, including the ones that do not
 * care, because an unset register holds whatever the caller left in it. That
 * is why `kosmos_write` uses `sys3` with an explicit zero and why the four
 * assembly test programs zero it by hand.
 */
/*
 * **Is this a range a driver may be handed?**
 *
 * Lifted out of `SYS_DEV_MAP` so it can be asked a question without a
 * process and a syscall frame around it - which is the whole of what
 * `tests/tests.c` needs in order to check the one decision here that is
 * about safety rather than about mechanics.
 *
 * Three refusals, and the middle one is the point:
 *
 *   - Nothing, or absurdly much: a bound that catches a wrong number.
 *   - **Anything overlapping RAM.** Physical memory mapped uncached into a
 *     process is an alias for somebody else's pages that bypasses their
 *     cache - a way to corrupt them and a way to watch them. Nothing
 *     legitimate wants it: a driver's buffers come from `SYS_MEM_CREATE`
 *     with `MEM_CONTIGUOUS`, mapped normally, with `SYS_MEM_PHYS` saying
 *     where they are. The two calls do not overlap, so a driver that asks
 *     this one for RAM has made a mistake and should be told about it.
 *   - A range that wraps, tested before the overlap arithmetic, because an
 *     end that wrapped compares small and would sail straight through it.
 */
/*
 * One number with two names: the ABI's, which a driver passes, and the
 * board's, which the HAL answers to. Kept separate so `hal/` does not include
 * the syscall header, and held together here so they cannot drift.
 */
_Static_assert(IRQ_WAIT_ENDPOINTS_MAX == IPC_WATCH_MAX,
               "a wait watches as many endpoints as a thread has slots for");
_Static_assert(DEV_PL061_POWER_KEY == HAL_DEV_PL061_POWER_KEY,
               "a device kind must mean the same thing on both sides");
_Static_assert(DEV_XHCI == HAL_DEV_XHCI,
               "a device kind must mean the same thing on both sides");
_Static_assert(DEV_INTEL_BACKLIGHT == HAL_DEV_INTEL_BACKLIGHT,
               "a device kind must mean the same thing on both sides");

bool dev_range_ok(uintptr_t phys, size_t pages)
{
    struct memrange ram;

    if (pages == 0 || pages > DEV_MAP_PAGES_MAX) {
        return false;
    }

    if ((phys & (PAGE_SIZE - 1)) != 0) {
        return false;
    }

    if (phys + pages * PAGE_SIZE < phys) {
        return false;
    }

    hal_ram_range(&ram);

    if (phys < (uintptr_t)ram.base + (uintptr_t)ram.size
        && (uintptr_t)ram.base < phys + pages * PAGE_SIZE) {
        return false;
    }

    return true;
}

/*
 * **How many pages this process may have mapped at once.**
 *
 * The flat guard for everybody, and the screen's worth on top for the one
 * process that holds the screen. `process.h` has the count behind twelve.
 *
 * Asked each time rather than stored, because the framebuffer is the
 * kernel's fact and a copy of it in the process would be a second answer to
 * one question. A full screen is counted as `max(pitch, width * 4) * height`,
 * so a pitch padded wider than the pixels is paid for, and so the count is
 * right whichever unit `pitch` is written in.
 */
static size_t map_budget(const struct process *p)
{
    (void)p;

    /*
     * **The machine's memory, and not a number a process may have.**
     *
     * It was 48 MB for everybody and the screen's worth on top for whoever
     * held the screen - "a guard against one process running away", raised
     * once already when a maximised window at 1920x1080 went one per cent
     * over it. A number chosen in advance is wrong on every machine but the
     * one it was chosen for (`threads.md` step 1b), so what is left is the
     * window's own size - addresses run out where `USER_MAP_END` says - and
     * `pmm_room_for_user`, which keeps a runaway out of the reserve the
     * kernel needs to start the process that ends it.
     */
    return (USER_MAP_END - USER_MAP_VA) / PAGE_SIZE;
}

static long sys_write(struct process *p, uintptr_t ptr, size_t len,
                      unsigned long colour)
{
    const char *s = (const char *)ptr;

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
     * one running, and it is here rather than out at user level. When
     * another thread
     * can change this address space, the check and the copy have to become
     * one operation.
     */
    kwrite_colour(s, (unsigned long)len, colour);

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
     * The sender goes back to the process as a kernel pointer, which is a
     * leak: a
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

    if (ncaps > captable_limit()) {
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

    /* The strongest of them, so it is refused the same way: a parent that
     * was not given hardware cannot give it away. */
    if ((flags & SPAWN_DEVICES) != 0 && !p->owns_devices) {
        return SYS_ERR_DENIED;
    }

    if ((flags & SPAWN_NET) != 0 && !p->owns_net) {
        return SYS_ERR_DENIED;      /* cannot pass on what it has not got */
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

    if ((flags & SPAWN_DEVICES) != 0) {
        process_grant_devices(child);
    }

    if ((flags & SPAWN_NET) != 0 && !process_grant_net(child)) {
        /* Not fatal, and the same judgement as sound rather than the screen:
         * a machine with no card still runs a stack, and the stack finds out
         * by `SYS_NET_INFO` reporting no card. A machine with no network is
         * a machine, where a window manager with no screen is nothing. */
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
 * Up to 124 KB a call: thirty-one pages, which is also the most one USB read
 * moves, so both disks a filesystem stands on take the same size. It was one
 * filesystem block a call, and kfs made a system call for every 4 KB it read
 * or wrote - Disk Benchmark measured that as most of every run (storage at
 * full speed, `testing.md` 18.64). Still one fixed buffer, as `CLAUDE.md`
 * asks, since there is no allocator here to size one against the request;
 * what it costs is 120 KB more of the kernel's .bss. `SYS_DISK_INFO` says the
 * number, so nothing above has to know it.
 */
#define DISK_CHUNK  (31u * 4096u)

static _Alignas(16) uint8_t disk_bounce[DISK_CHUNK];

/* And the network's, for the same reason. One frame; nothing here queues. */
static _Alignas(16) uint8_t net_bounce[HAL_NET_FRAME];

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

    info.cpu_arch   = cpu_arch();
    info.cpu_words  = cpu_raw(&cpu, info.cpu_raw, CPU_RAW_WORDS);
    info.counter_hz = cpu.counter_hz;
    info.log_origin = console_log_origin();

    info.ram_base    = ram.base;
    info.ram_size    = ram.size;
    info.pages_total = (uint32_t)pmm_total_pages();
    info.pages_free  = (uint32_t)pmm_free_pages();

    {
        unsigned long idle, busy;

        thread_load(&idle, &busy);
        info.idle_ticks = idle;
        info.busy_ticks = busy;

        /*
         * And the split. `CPUS_MAX` is the room in the struct.
         *
         * The bound is `thread_cpu_count()` and not `info.cpus`, which is
         * the same number and is **assigned seventy lines below this** -
         * so reading it here bounded the loop by zero, `cpu[]` stayed as
         * `memset` left it, and every core reported 0% busy with two
         * spinners running. Found by the program written to show it, which
         * is what an instrument is for.
         */
        /*
         * And the bound is `smp_online`, not `thread_cpu_count`.
         *
         * They are different questions and this is the one that has an
         * answer per core: `smp_online` counts processors running kernel
         * code, every one of which takes its own timer interrupt and charges
         * its own idle or busy tick. `thread_cpu_count` is how many are
         * *given* work, which is all of them unless a boot option asks for
         * fewer - and that is a policy rather than a capability: a secondary
         * has its own runqueue and takes work from it whenever anything is
         * placed there.
         *
         * Bounding by the smaller of the two reported three cores as zero
         * when they were measurably idle, which is a different claim.
         */
        for (unsigned c = 0; c < smp_online() && c < CPUS_MAX; c++) {
            unsigned long ci, cb;

            thread_load_cpu(c, &ci, &cb);
            info.cpu[c].idle_ticks = ci;
            info.cpu[c].busy_ticks = cb;
        }
    }

    info.epoch = (uint64_t)hal_rtc_seconds();

    if (hal_snd_present()) {
        info.audio_rate     = HAL_SND_RATE;
        info.audio_channels = HAL_SND_CHANNELS;
        info.audio_period   = HAL_SND_PERIOD_BYTES;
        info.audio_periods  = HAL_SND_PERIODS;
        info.audio_dry      = hal_snd_dry();
        info.audio_floor    = hal_snd_floor();
        info.audio_wakes    = hal_snd_wakes();
    }

    /* Outside the sound branch, which is where it briefly was - and a
     * machine with a network card and no speaker then reported no network.
     * Two devices, two questions. */
    info.net_mtu = hal_net_present() ? HAL_NET_MTU : 0u;

    {
        struct hal_battery b;

        if (hal_battery_read(&b)) {
            info.battery_known       = 1u;
            info.battery_present     = b.present ? 1u : 0u;
            info.battery_charging    = b.charging ? 1u : 0u;
            info.battery_discharging = b.discharging ? 1u : 0u;
            info.battery_on_ac       = b.on_ac ? 1u : 0u;
            info.battery_critical    = b.critical ? 1u : 0u;
            info.battery_percent     = b.percent;
        }
    }

    info.threads_used     = thread_count();
    info.threads_total    = thread_ceiling();   /* what the pool may grow to */
    info.processes_used   = process_count();
    info.processes_held   = process_slots_used();
    info.processes_total  = process_ceiling();  /* what the pool may grow to */
    info.regions_used     = memobj_in_use();
    info.regions_total    = memobj_total();
    info.endpoints_used   = ipc_endpoints_in_use();
    info.endpoints_total  = ipc_endpoints_total();
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

    /*
     * What the board's bus enumeration found, driven or not.
     *
     * Every other device field here answers presence, and presence cannot
     * tell a machine with no sound card from one whose card nothing claims.
     * The board fills this; nothing in the kernel decodes an id.
     */
    info.bus_found = hal_bus_scan(info.bus, BUS_DEVICES_MAX);
    info.bus_count = info.bus_found < BUS_DEVICES_MAX ? info.bus_found
                                                     : BUS_DEVICES_MAX;

    /*
     * What the firmware calls the machine, copied out of the board's copy.
     * Terminated here too: this struct crosses into a process, and a string
     * with no end is a read past it on the other side.
     */
    {
        struct hal_machine m;

        _Static_assert(sizeof(m.vendor) == sizeof(info.machine_vendor)
                       && sizeof(m.product) == sizeof(info.machine_product)
                       && sizeof(m.version) == sizeof(info.machine_version)
                       && sizeof(m.source) == sizeof(info.machine_source),
                       "sysinfo's machine names are the HAL's size");

        (void)hal_machine_ident(&m);

        memcpy(info.machine_vendor, m.vendor, sizeof(info.machine_vendor));
        memcpy(info.machine_product, m.product, sizeof(info.machine_product));
        memcpy(info.machine_version, m.version, sizeof(info.machine_version));
        memcpy(info.machine_source, m.source, sizeof(info.machine_source));

        info.machine_vendor[sizeof(info.machine_vendor) - 1]   = '\0';
        info.machine_product[sizeof(info.machine_product) - 1] = '\0';
        info.machine_version[sizeof(info.machine_version) - 1] = '\0';
        info.machine_source[sizeof(info.machine_source) - 1]   = '\0';
    }

    /*
     * And where the screen came from, when there is one, in the words the
     * boot log already has - copied up to its field and terminated, for the
     * same reason as the names above.
     */
    if (info.screen_width > 0) {
        const char *source = hal_fb_describe();
        unsigned i = 0;

        while (source != NULL && source[i] != '\0'
               && i + 1 < sizeof(info.screen_source)) {
            info.screen_source[i] = source[i];
            i++;
        }

        info.screen_source[i] = '\0';
    }

    /*
     * Three numbers, because there are three questions.
     *
     *   cpus_present   what the machine has, from the firmware
     *   cpus_online    how many are running kernel code and taking ticks
     *   cpus           how many are given new threads
     *
     * On a machine that started every processor and was given no boot
     * option they are one number three times, and that is the point: a gap
     * between the first two is a processor that never arrived, and a gap
     * between the last two is `opt/kosmos/smp` asking for fewer.
     *
     * Two of these were one number for a while, and the collapse was the
     * bug: a parked core and a working one both reported as "not
     * scheduling", so nothing could tell a secondary that had died from one
     * that was simply not being asked to do anything.
     */
    info.cpus         = thread_cpu_count();
    info.cpus_online  = smp_online();
    info.cpus_present = hal_cpu_count();
    info.tick_hz    = TICK_HZ;
    info.page_size  = PAGE_SIZE;

    info.current_el = (uint32_t)cpu_current_el();

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
    size_t budget = map_budget(p);

    if (pages == 0) {
        return SYS_ERR_NO_ROOM;
    }

    if (pages > budget || p->mapped_pages + pages > budget) {
        return SYS_ERR_NO_ROOM;
    }

    /* The addresses are never reused, so this is what stops a process that
     * maps and unmaps for long enough from walking into the next window. */
    if (p->next_map + pages * PAGE_SIZE > USER_MAP_END) {
        return SYS_ERR_NO_ROOM;
    }

    /* Against the kernel's reserve, once for the whole request rather than
     * page by page: a refusal is one answer, not a half-served map. */
    if (!pmm_room_for_user(pages)) {
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
            uintptr_t phys = as_page_phys(p->space, va);

            if (phys != 0) {
                (void)as_unmap(p->space, va, 1);
                pmm_free_page((void *)phys);
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
     *
     * The bound is `map_budget`, the one `sys_map` takes. The process holding
     * the screen may map more than the flat allowance in a single call, and a
     * bound tighter here than there would leave such a mapping impossible to
     * give back.
     */
    if (va < USER_MAP_VA || pages == 0 || pages > map_budget(p)
        || va + pages * PAGE_SIZE > p->next_map) {
        return SYS_ERR_FAULT;
    }

    for (i = 0; i < pages; i++) {
        uintptr_t at = va + i * PAGE_SIZE;
        uintptr_t phys = as_page_phys(p->space, at);

        if (phys == 0) {
            continue;           /* already gone; unmapping twice is not an error */
        }

        (void)as_unmap(p->space, at, 1);
        pmm_free_page((void *)phys);
        freed++;
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

/*
 * Every process, into the caller's buffer of `max`. The caller sizes it -
 * the pool grows, so no number here could be right - and one that fills it
 * asks again with a larger one (`sys_user.c`).
 */
static long sys_proctable(struct process *p, uintptr_t out_ptr, size_t max)
{
    if (max == 0 || max > process_ceiling()) {
        max = process_ceiling();
    }

    if (!process_may_write(p, out_ptr, max * sizeof(struct proc_info))) {
        return SYS_ERR_FAULT;
    }

    return (long)process_table((struct proc_info *)out_ptr, (unsigned)max);
}

void syscall_dispatch(struct syscall_frame *sc)
{
    struct process *p = process_current();
    unsigned long number = sc->number;
    long result;

    if (p == NULL) {
        /* A syscall from something that is not a process. Nothing issues
         * one, so reaching here means the entry path routed something
         * wrongly. */
        sc->result = (uint64_t)(long)SYS_ERR_BADCALL;
        return;
    }

    switch (number) {
    case SYS_EXIT:
        /* Never returns. The thread and everything the process owned go
         * back to their pools. */
        process_exit(p, (int)sc->arg[0]);
        return;

    case SYS_WRITE:
        result = sys_write(p, sc->arg[0], (size_t)sc->arg[1], sc->arg[2]);
        break;

    case SYS_SND_WRITE:
        if (!p->owns_audio) {
            result = SYS_ERR_DENIED;
        } else if (sc->arg[1] == 0 || sc->arg[1] > HAL_SND_PERIOD_BYTES) {
            result = SYS_ERR_FAULT;
        } else if (!process_may_read(p, sc->arg[0], (size_t)sc->arg[1])) {
            result = SYS_ERR_FAULT;
        } else {
            /* Full is not a failure. See SYS_SND_WRITE in syscall.h: a
             * caller ahead of the device is told so rather than blocked. */
            result = hal_snd_write((const void *)sc->arg[0],
                                   (unsigned)sc->arg[1]) ? 0 : SYS_NO_INPUT;
        }

        break;

    case SYS_SND_QUEUED:
        result = p->owns_audio ? (long)hal_snd_queued() : SYS_ERR_DENIED;
        break;

    /*
     * The network, three calls, and every one of them behind `owns_net`.
     *
     * A bounce buffer for both directions, like the disk's and for the same
     * reason: the driver hands the device a pointer, and a pointer into a
     * process's address space is not one the device can use - the ring lives
     * in the kernel's identity-mapped memory and the process's does not.
     * Copying is also what makes the length check mean something, because a
     * process cannot change the bytes after they were checked.
     */
    case SYS_NET_INFO: {
        struct netinfo info;
        struct netdev card;

        if (!p->owns_net) {
            result = SYS_ERR_DENIED;
            break;
        }

        if (!process_may_write(p, sc->arg[0], sizeof(info))) {
            result = SYS_ERR_FAULT;
            break;
        }

        memset(&info, 0, sizeof(info));

        /* `hal_net_present` rather than a second `hal_net_init`: the card
         * came up at boot and bringing a running device up again is a reset
         * with frames in flight. The MAC is read back through the same
         * `out` struct the boot used. */
        if (hal_net_present() && hal_net_info(&card)) {
            memcpy(info.mac, card.mac, sizeof(info.mac));
            info.mtu     = card.mtu;
            info.present = 1;
        }

        memcpy((void *)sc->arg[0], &info, sizeof(info));
        result = 0;
        break;
    }

    case SYS_NET_SEND:
        if (!p->owns_net) {
            result = SYS_ERR_DENIED;
        } else if (sc->arg[1] == 0 || sc->arg[1] > HAL_NET_FRAME) {
            result = SYS_ERR_FAULT;
        } else if (!process_may_read(p, sc->arg[0], (size_t)sc->arg[1])) {
            result = SYS_ERR_FAULT;
        } else {
            memcpy(net_bounce, (const void *)sc->arg[0], (size_t)sc->arg[1]);

            result = hal_net_send(net_bounce, (unsigned)sc->arg[1])
                     ? 0 : SYS_NO_INPUT;
        }

        break;

    case SYS_NET_RECV: {
        int got;

        if (!p->owns_net) {
            result = SYS_ERR_DENIED;
            break;
        }

        if (sc->arg[1] == 0 || sc->arg[1] > HAL_NET_FRAME) {
            result = SYS_ERR_FAULT;
            break;
        }

        if (!process_may_write(p, sc->arg[0], (size_t)sc->arg[1])) {
            result = SYS_ERR_FAULT;
            break;
        }

        got = hal_net_recv(net_bounce, (unsigned)sc->arg[1]);

        if (got > 0) {
            memcpy((void *)sc->arg[0], net_bounce, (size_t)got);
            result = got;
        } else if (got < 0) {
            result = SYS_ERR_FAULT;     /* the buffer is smaller than a frame */
        } else {
            result = SYS_NO_INPUT;      /* nothing waiting, which is usual */
        }

        break;
    }

    case SYS_POWER:
        if (!p->owns_procctl) {
            result = SYS_ERR_DENIED;
        } else {
            /*
             * Said before it happens, because after it there is nobody to
             * say anything. The console is the kernel's here, so this
             * reaches the serial line whatever userland was doing.
             */
            kputs((sc->arg[0] == 1) ? "\nkosmos: restarting\n"
                                  : "\nkosmos: powering off\n");

            if (sc->arg[0] == 1) {
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
        } else if (!process_may_write(p, sc->arg[0], sizeof(unsigned))
                   || !process_may_write(p, sc->arg[1], sizeof(unsigned))) {
            result = SYS_ERR_FAULT;
        } else if (!hal_key_event(&code, &down)) {
            result = SYS_NO_INPUT;
        } else {
            *(unsigned *)sc->arg[0] = code;
            *(unsigned *)sc->arg[1] = down ? 1u : 0u;
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

    case SYS_PTR_SPEED:
        /*
         * Not guarded by `owns_console`, and deliberately: this reads and
         * writes a *setting*, not the input stream. Taking a keystroke
         * somebody else is waiting for is theft; making the pointer move
         * faster is a preference, and a program run from a shell has no
         * console of its own to prove it deserves one.
         */
        result = (long)hal_pointer_speed((unsigned)sc->arg[0]);
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
        } else if (!process_may_write(p, sc->arg[0], sizeof(struct pointer_info))) {
            result = SYS_ERR_FAULT;
        } else if (!hal_pointer_poll(&state)) {
            result = SYS_ERR_DENIED;    /* there is no pointer on this board */
        } else {
            struct pointer_info *out = (struct pointer_info *)sc->arg[0];

            out->x       = state.x;
            out->y       = state.y;
            out->min_x   = state.min_x;
            out->max_x   = state.max_x;
            out->min_y   = state.min_y;
            out->max_y   = state.max_y;
            out->buttons = state.buttons;
            out->moved   = state.moved;

            /*
             * And what the buttons *did*, which the state above cannot
             * say. Drained here because this call is already guarded to
             * one reader: a second process taking transitions the first
             * never sees is the same disagreement the comment above
             * refuses for the position, and worse, because a transition
             * taken is gone.
             */
            {
                struct pointer_edge edges[POINTER_EDGES_MAX];
                unsigned n = hal_pointer_edges(edges, POINTER_EDGES_MAX);
                unsigned i;

                for (i = 0; i < n; i++) {
                    out->edges[i].x = edges[i].x;
                    out->edges[i].y = edges[i].y;
                    out->edges[i].buttons = edges[i].buttons;
                }

                out->nedges = n;
                out->dropped = hal_pointer_edges_dropped();
            }

            result = 0;
        }
        break;
    }

    case SYS_POINTER_MOVE: {
        /*
         * A driver's pointing device, added to the pointer. `syscall.h` has
         * why this is a call and why device authority is what gates it.
         *
         * **Clamped to fifteen bits a report**, which no device comes near: a
         * USB boot report says -127 to 127. The board multiplies by its speed
         * in 64 bits either way, so the clamp is not arithmetic safety - it
         * makes a count a driver got wrong one the pointer survives rather
         * than one it is trusted with.
         *
         * **And the sleepers woken**, which is the half an interrupt would
         * have done by itself. The i8042's arrives in the trap handler, which
         * wakes whoever waits for input when the board says there is some; a
         * report from a process arrives here instead, and without this the
         * window manager would see where the pointer went only when its own
         * deadline came round.
         */
        enum { COUNT_MAX = 32767 };
        long dx = (long)sc->arg[0];
        long dy = (long)sc->arg[1];

        dx = dx > COUNT_MAX ? COUNT_MAX : dx < -COUNT_MAX ? -COUNT_MAX : dx;
        dy = dy > COUNT_MAX ? COUNT_MAX : dy < -COUNT_MAX ? -COUNT_MAX : dy;

        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
        } else if (!hal_pointer_move((int)dx, (int)dy,
                                     (uint32_t)sc->arg[2])) {
            result = SYS_ERR_NO_DEVICE;
        } else {
            p->moved_pointer = true;
            thread_wake_sleepers_now();
            result = 0;
        }
        break;
    }

    case SYS_NET_WAKE:
        /*
         * A driver process saying a frame is in the ring it shares with the
         * stack. The kernel knows nothing about the ring: what it does is
         * what it already does for its own card, which is cut short the one
         * process that holds the network's timed receive.
         */
        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
        } else {
            /*
             * **How many were woken**, rather than 0 for "asked". A wake
             * that reaches nobody is a mechanism that silently does nothing,
             * and this one did: see `process_wake_net`.
             */
            result = (long)process_wake_net();
        }
        break;

    case SYS_SET_TLS:
        /*
         * Kept with the thread and loaded now, so the caller can use it on
         * the instruction after this one rather than after its next switch.
         */
        thread_set_tls(sc->arg[0]);
        result = 0;
        break;

    case SYS_THREAD_CREATE:
        result = process_thread_create(p, sc->arg[0], sc->arg[1]);

        if (result < 0) {
            result = (result == -2) ? SYS_ERR_NO_ROOM : SYS_ERR_NO_ROOM;
        }

        break;

    case SYS_THREAD_EXIT:
        /*
         * The caller, and only the caller. A thread that is its process's
         * first is the process ending, which is `SYS_EXIT` - answered as
         * such rather than leaving a process with no first thread.
         */
        if (thread_current() == p->thread) {
            process_exit(p, (int)sc->arg[0]);
            result = 0;                     /* not reached */
        } else {
            process_thread_ended(p, thread_current(), (int)sc->arg[0]);
            thread_exit();                  /* does not return */
            result = 0;
        }

        break;

    case SYS_THREAD_WAIT:
        result = process_thread_wait(p, (unsigned)sc->arg[0]);
        break;

    case SYS_KEY_PUSH:
        /*
         * A driver's key - `syscall.h` has why. Device authority, as the
         * pointer's movement is, and the sleepers woken as it wakes them:
         * a key from a process arrives here rather than in the trap handler
         * that wakes whoever waits for input.
         */
        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
        } else if ((unsigned long)sc->arg[0] > KEY_PUSH_MOST) {
            result = SYS_ERR_NO_DEVICE;     /* no key has that number */
        } else if (!hal_key_push((unsigned)sc->arg[0], sc->arg[1] != 0)) {
            result = SYS_ERR_NO_ROOM;       /* the queue is full */
        } else {
            p->pushed_keys = true;
            thread_wake_sleepers_now();
            result = 0;
        }
        break;

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
            if (sc->arg[0] != 0) {
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
            result = (process_kill_any((unsigned)sc->arg[0]) == 0)
                     ? 0 : SYS_ERR_NO_CHILD;
        } else {
            result = (process_kill(p, (unsigned)sc->arg[0]) == 0)
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
             * The argument is in **scheduler ticks**, at TICK_HZ, which is
             * what every timeout in this system has always been counted in
             * and what `sys.sleep` and `fs.wait_input` mean by a tick.
             *
             * `thread_deadline_in` turns it into a counter deadline, and it
             * is the only place in the kernel that knows both clocks.
             * Deadlines used to be `hal_ticks()` plus this number, which
             * had two faults: `hal_ticks` counts interrupts *taken*, so a
             * machine that misses one runs every sleep long by exactly the
             * amount it was already struggling; and a count of interrupts
             * cannot be handed to a comparator, which is what a one-shot
             * timer needs.
             *
             * Passing the counter where scheduler ticks were meant still
             * asks for a sleep of ten thousand seconds and still looks
             * exactly like a hang. It was written that way first, and the
             * cap under SYS_SLEEP below is what stops it being permanent.
             */
            thread_wait_input_until(
                thread_deadline_in((unsigned long)sc->arg[0]));
            result = 0;
        }
        break;

    case SYS_WAIT_INPUT_OR_CALL:
        /*
         * `SYS_WAIT_INPUT`, and a caller on the watched endpoint as well -
         * syscall.h has why. The same owner check and the same look at
         * input first; the rest, including looking again once the wake
         * condition is set, is `ipc_wait_for_caller`'s.
         */
        if (!p->owns_console) {
            result = SYS_ERR_DENIED;
        } else if (hal_input_pending()) {
            result = 0;
        } else {
            unsigned long ticks = (unsigned long)sc->arg[0];

            if (ticks > (unsigned long)TICK_HZ * 3600UL) {
                ticks = (unsigned long)TICK_HZ * 3600UL;
            }

            result = ipc_wait_for_caller((cap_t)sc->arg[1], ticks, true);
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
        if (sc->arg[0] == 0) {
            thread_yield();
        } else {
            unsigned long ticks = (unsigned long)sc->arg[0];

            if (ticks > (unsigned long)TICK_HZ * 3600UL) {
                ticks = (unsigned long)TICK_HZ * 3600UL;
            }

            thread_sleep_until(thread_deadline_in(ticks));
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
        /* The whole ring, at most. Capped rather than trusted because the
         * caller names the length and the buffer, and a caller that asked
         * for more than exists would be handed whatever follows it. */
        size_t max = (size_t)sc->arg[1];

        if (max > CONSOLE_LOG_BYTES) {
            max = CONSOLE_LOG_BYTES;
        }

        if (!process_may_write(p, sc->arg[0], max)) {
            result = SYS_ERR_FAULT;
            break;
        }

        result = (long)console_log((char *)sc->arg[0], max);
        break;
    }

    case SYS_FIRMWARE: {
        /*
         * The firmware's AML, a window of one table at a time.
         *
         * No permission check, for `SYS_LOG`'s reason: this is the firmware's
         * description of the machine's own hardware, which every operating
         * system that boots on it reads, and not a secret. The tables that
         * could hold one - MSDM's licence key - are never kept
         * (`hal_firmware_table`), and that is where the line is drawn rather
         * than here.
         *
         * The index is bounded before it is narrowed, so a huge one is past
         * the last table rather than wrapped round to the first.
         */
        struct hal_firmware_table t;
        unsigned long index = (unsigned long)sc->arg[0];
        unsigned long offset = (unsigned long)sc->arg[1];
        size_t max = (size_t)sc->arg[3];

        if (index >= 65536u || !hal_firmware_table((unsigned)index, &t)) {
            result = SYS_ERR_NO_DEVICE;
            break;
        }

        if (offset >= t.length || max == 0) {
            result = 0;
            break;
        }

        if (max > t.length - offset) {
            max = t.length - offset;
        }

        if (!process_may_write(p, sc->arg[2], max)) {
            result = SYS_ERR_FAULT;
            break;
        }

        memcpy((void *)sc->arg[2], t.bytes + offset, max);
        result = (long)max;
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
        /*
         * **`arg[1]` is flags, and bit 0 asks for one physical run.**
         *
         * Added as a second argument rather than a second syscall because
         * what differs is a constraint on the allocation, not the nature of
         * the thing: a DMA buffer is pages a process maps, exactly like
         * every other region, and giving it its own object would duplicate
         * the mapping, unmapping and freeing that already work.
         *
         * Callers that pass nothing get zero and the old behaviour, which
         * is what every existing one wants - a window's double buffer has
         * no business asking for a run and would sometimes fail if it did.
         */
        struct memobj *m = memobj_create((size_t)sc->arg[0],
                                         (sc->arg[1] & MEM_CONTIGUOUS) != 0);

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
                                              (cap_t)sc->arg[0]);
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

    case SYS_DEV_MAP: {
        /*
         * A window of a device's registers, into this process's address
         * space, with the memory type that makes a register a register.
         *
         * **Gated on device authority, like `SYS_MEM_PHYS`**, and this is
         * the stronger of the two: knowing where something lives is a head
         * start, and this is the reaching itself. A process holding it can
         * drive any device on the machine, which is why the grant exists at
         * all and why it is one flag rather than a list - a list of which
         * devices would be a policy, and policy does not belong here. The
         * *server* that hands drivers their windows is where that belongs,
         * and it is a process.
         *
         * **RAM is refused**, and the check is cheap and worth having.
         * Physical memory mapped uncached into a process is an alias for
         * somebody else's pages that bypasses their cache, which is both a
         * way to corrupt them and a way to watch them. Nothing legitimate
         * wants it: a driver's buffers come from `SYS_MEM_CREATE` with
         * `MEM_CONTIGUOUS`, which is mapped normally and whose physical
         * address `SYS_MEM_PHYS` reports. So the two calls do not overlap,
         * and a driver that asks this one for RAM has made a mistake it
         * should be told about rather than allowed to debug.
         */
        uintptr_t phys = (uintptr_t)sc->arg[0];
        size_t pages = (size_t)sc->arg[1];
        uintptr_t base;
        size_t i;

        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
            break;
        }

        if (!dev_range_ok(phys, pages)) {
            result = SYS_ERR_DENIED;
            break;
        }

        if (p->next_share + pages * PAGE_SIZE > USER_SHARE_END) {
            result = SYS_ERR_NO_ROOM;
            break;
        }

        base = p->next_share;

        for (i = 0; i < pages; i++) {
            if (as_map(p->space, base + i * PAGE_SIZE, phys + i * PAGE_SIZE,
                       1, MAP_USER_DEVICE) != AS_OK) {
                break;
            }
        }

        if (i < pages) {
            size_t j;

            for (j = 0; j < i; j++) {
                as_unmap(p->space, base + j * PAGE_SIZE, 1);
            }

            result = SYS_ERR_NO_ROOM;
            break;
        }

        p->next_share += pages * PAGE_SIZE;

        result = (long)base;
        break;
    }

    case SYS_DEV_FIND: {
        /*
         * Where a device of this kind is, if the board has one.
         *
         * **Gated on device authority**, the same as mapping it: an address
         * is most of the way to reaching a device, and there is no reason a
         * process that may not drive hardware should be able to survey it.
         *
         * "Nothing of that kind" is its own answer rather than a denial. A
         * driver on a board without its device is the ordinary case - the
         * power button on a PC - and it should be able to tell that apart
         * from having been refused.
         */
        struct hal_device found;
        struct dev_info info = { 0 };
        uintptr_t out_ptr = (uintptr_t)sc->arg[2];

        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
            break;
        }

        if (!process_may_write(p, out_ptr, sizeof(info))) {
            result = SYS_ERR_FAULT;
            break;
        }

        if (!hal_device_find((unsigned)sc->arg[0], (unsigned)sc->arg[1],
                             &found)) {
            result = SYS_ERR_NO_DEVICE;
            break;
        }

        info.kind  = (uint32_t)sc->arg[0];
        info.intid = (uint32_t)found.intid;
        info.line  = (uint32_t)found.line;
        info.base  = (uint64_t)found.base;
        info.size  = (uint64_t)found.size;
        info.where = (uint32_t)found.where;

        *(struct dev_info *)out_ptr = info;
        result = 0;
        break;
    }

    case SYS_IRQ_CLAIM: {
        /*
         * A line, claimed, and handed back as a capability.
         *
         * **Gated on device authority**, like the two mappings: this is the
         * right to take an interrupt out of the kernel's hands, and a
         * process that could take the wrong one could stop the machine. The
         * board decides which numbers are wrong, in `hal_irq_available`,
         * because it is the only thing that knows which it spends.
         *
         * Installed into the capability table, so the driver names its line
         * by an index into its own table from here on and `owns_devices` is
         * not consulted again. That is what makes it possible for a devices
         * server to hold the authority one day and hand a driver the
         * capability - the driver never needing the authority at all.
         */
        struct irq_line *line;
        cap_t index;

        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
            break;
        }

        line = irq_claim((unsigned)sc->arg[0], p);

        if (line == NULL) {
            result = SYS_ERR_DENIED;
            break;
        }

        index = ipc_install_irq(thread_current(), line);

        if (index < 0) {
            /* No slot for it, so the claim goes back rather than being held
             * by a process with no way to name it. */
            irq_release(line);
            result = SYS_ERR_NO_CAPS;
            break;
        }

        result = (long)index;
        break;
    }

    case SYS_IRQ_WAIT: {
        /*
         * No device-authority check, and that is deliberate: holding the
         * capability *is* the authority. The grant was spent at the claim.
         *
         * The deadline is capped at an hour for `SYS_SLEEP`'s reason: a
         * driver that computed it wrongly would otherwise wait a year and
         * look exactly like a hang. Zero is for ever.
         */
        unsigned long ticks = (unsigned long)sc->arg[1];

        if (ticks > (unsigned long)TICK_HZ * 3600UL) {
            ticks = (unsigned long)TICK_HZ * 3600UL;
        }

        result = irq_wait(ipc_resolve_irq(thread_current(),
                                          (cap_t)sc->arg[0]), ticks);
        break;
    }

    case SYS_IRQ_ACK:
        result = irq_ack(ipc_resolve_irq(thread_current(),
                                         (cap_t)sc->arg[0]));
        break;

    case SYS_IRQ_WAIT_ANY: {
        /*
         * `SYS_IRQ_WAIT` on several capabilities, each resolved exactly as
         * that call resolves its one: holding them is the authority. A number
         * that names no line in this process's table refuses the whole wait,
         * rather than waiting on fewer lines than the driver believes.
         *
         * The array is read once, into the kernel, before anything waits, so
         * a process that rewrote it during the wait would change nothing.
         *
         * And the endpoints, `arg[3]` pointing at `arg[4]` of them, each one
         * watched when it is not negative and resolved by `ipc.c` as every
         * endpoint is: a number that names none refuses the wait.
         *
         * **An array rather than two arguments**, which is what they were
         * until 22 September. A syscall has five, all five were spoken for,
         * and the xHCI driver needed a third endpoint - the network stack's
         * frames beside the disk server's writes and `/dev/blocks`
         * (`usb.md` 7d). Reading them out of the caller's memory costs one
         * `process_may_read` and takes the ceiling off.
         *
         * **No lines and some endpoints is a wait**: a driver with no
         * hardware to watch still answers its clients, and polling them
         * would be wakes a second on a machine where nothing is happening.
         */
        uintptr_t at = (uintptr_t)sc->arg[0];
        unsigned long count = (unsigned long)sc->arg[1];
        unsigned long ticks = (unsigned long)sc->arg[2];
        uintptr_t eps_at = (uintptr_t)sc->arg[3];
        unsigned long ends = (unsigned long)sc->arg[4];
        int endpoints[IRQ_WAIT_ENDPOINTS_MAX];
        struct irq_line *set[IRQ_WAIT_ANY_MAX];
        unsigned long i;

        if ((count == 0 && ends == 0) || count > IRQ_WAIT_ANY_MAX
            || ends > IRQ_WAIT_ENDPOINTS_MAX) {
            result = SYS_ERR_DENIED;
            break;
        }

        if ((count > 0 && !process_may_read(p, at, count * sizeof(long)))
            || (ends > 0 && !process_may_read(p, eps_at,
                                              ends * sizeof(long)))) {
            result = SYS_ERR_FAULT;
            break;
        }

        for (i = 0; i < count; i++) {
            long cap = ((const long *)at)[i];

            set[i] = (cap < 0 || cap > INT_MAX)
                   ? NULL
                   : ipc_resolve_irq(thread_current(), (cap_t)cap);
        }

        if (ticks > (unsigned long)TICK_HZ * 3600UL) {
            ticks = (unsigned long)TICK_HZ * 3600UL;
        }

        for (i = 0; i < ends; i++) {
            long cap = ((const long *)eps_at)[i];

            endpoints[i] = cap < 0 ? -1 : cap > INT_MAX ? INT_MAX : (int)cap;
        }

        result = irq_wait_any(set, (unsigned)count, ticks, endpoints,
                              (unsigned)ends);
        break;
    }

    case SYS_MEM_SIZE: {
        struct memobj *m = ipc_resolve_memory(thread_current(),
                                              (cap_t)sc->arg[0]);

        result = (m == NULL) ? SYS_ERR_DENIED : (long)m->pages;
        break;
    }

    case SYS_MEM_PHYS: {
        /*
         * Where a region begins in physical memory - the one number a
         * driver cannot work out and cannot do without, since hardware is
         * told where its rings are in the bus's addresses and a process
         * only ever sees its own.
         *
         * **Gated on device authority, and that is not ceremony.** A
         * process that can learn where things physically live has a head
         * start on reaching them: it turns "somewhere in 684 MB" into an
         * address, which is most of the work of using any of the ways a
         * kernel can be persuaded to touch memory on somebody's behalf.
         * Nothing here is hostile yet and this is the cheapest moment to
         * decide that it will not be.
         *
         * Refused for a scattered region by `memobj_phys` rather than here,
         * because that is where the reason lives.
         */
        struct memobj *m;

        if (!p->owns_devices) {
            result = SYS_ERR_DENIED;
            break;
        }

        m = ipc_resolve_memory(thread_current(), (cap_t)sc->arg[0]);

        if (m == NULL) {
            result = SYS_ERR_DENIED;
            break;
        }

        result = (long)memobj_phys(m);

        if (result == 0) {
            result = SYS_ERR_DENIED;    /* not one run; see memobj_phys */
        }

        break;
    }

    case SYS_DISK_INFO: {
        struct diskinfo info;
        struct blkdev dev;

        if (!process_may_write(p, (uintptr_t)sc->arg[0], sizeof(info))) {
            result = SYS_ERR_FAULT;
            break;
        }

        /* Readable without holding the disk. It says whether there is one
         * and how big it is, which is not authority over it - and init has
         * to be able to ask before deciding whether to start a filesystem
         * server at all. Answered from what boot kept, and never by starting
         * the controller again (`process_disk_start` says why). */
        if (process_disk(&dev)) {
            info.sectors     = dev.sectors;
            info.sector_size = dev.sector_size;
        } else {
            info.sectors     = 0;
            info.sector_size = 0;
        }

        info.most = DISK_CHUNK;
        *(struct diskinfo *)(uintptr_t)sc->arg[0] = info;
        result = 0;
        break;
    }

    case SYS_DISK_READ:
        result = sys_disk(p, false, sc->arg[0], (uintptr_t)sc->arg[1],
                          (size_t)sc->arg[2]);
        break;

    case SYS_DISK_WRITE:
        result = sys_disk(p, true, sc->arg[0], (uintptr_t)sc->arg[1],
                          (size_t)sc->arg[2]);
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
        size_t len = (size_t)sc->arg[2];

        if (!process_may_read(p, (uintptr_t)sc->arg[0], 1)
            || !process_may_write(p, (uintptr_t)sc->arg[1], len)) {
            result = SYS_ERR_FAULT;
            break;
        }

        {
            const char *from = (const char *)(uintptr_t)sc->arg[0];
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
            char *to = (char *)(uintptr_t)sc->arg[1];
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
         * whenever the machine started, at whatever rate the board's
         * counter runs, and says nothing
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
        result = (long)cpu_cycles();
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
        result = ipc_endpoint_destroy((cap_t)sc->arg[0]);
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
        uintptr_t va    = sc->arg[0];
        size_t    pages = (size_t)sc->arg[1];

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

        /*
         * **And the address space comes back, when it was the last thing
         * handed out.**
         *
         * `next_share` only ever climbed. Unmapping returned the *pages*
         * to whoever owned them and kept the *addresses* spent for ever,
         * so a process that maps and unmaps in a loop marches up the
         * window until nothing more will fit - and then cannot map
         * anything, ever again, however little memory the machine is
         * using.
         *
         * That is not a slow leak in a corner. The filesystem server maps
         * the caller's whole buffer on every `read_into`, and the video
         * player's buffer is four megabytes, so one ten-second film spends
         * more than a gigabyte of window. Diego found it on 21 September:
         * "the video player ran once with the mp4 mjpeg video but not a
         * second time... it looks something remained in memory". What
         * remained was addresses.
         *
         * **LIFO, and that is a deliberate half-measure.** A free list
         * over the window would need somewhere to keep the holes, and this
         * kernel has no allocator to keep them in; a bitmap would be 128 KB
         * a process for a 4 GB window. Lowering the mark when the topmost
         * mapping is the one going back costs two lines and one branch,
         * and it is exactly the shape every server here actually has: map
         * the caller's buffer, answer, unmap, wait for the next request.
         * One at a time, so the top is always the one being returned.
         *
         * A server that held two and released the older first keeps the
         * old behaviour for that range - no worse than before, and it
         * recovers the moment the newer one goes.
         */
        if (result == 0 && va + pages * PAGE_SIZE == p->next_share) {
            p->next_share = va;
        }

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

        if (!process_may_write(p, (uintptr_t)sc->arg[0], sizeof(info))) {
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

        memcpy((void *)(uintptr_t)sc->arg[0], &info, sizeof(info));
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
        switch ((unsigned)sc->arg[0]) {
        case SCHED_SET_QUANTUM:
            /*
             * Refused rather than clamped. The setter clamps too, because
             * it is called from inside the kernel as well, but a process
             * that asked for four hundred days should be told it did not
             * get them instead of being answered "yes" and given one
             * second.
             */
            if (sc->arg[1] == 0 || sc->arg[1] > SCHED_QUANTUM_MAX) {
                result = SYS_ERR_DENIED;
                break;
            }

            sched_set_quantum((unsigned)sc->arg[1]);
            result = 0;
            break;

        case SCHED_SET_POLICY:
            result = sched_switch_to((unsigned)sc->arg[1]) ? 0 : SYS_ERR_DENIED;
            break;

        case SCHED_SET_MY_BAND:
            /*
             * **Down only, and the comparison is the whole security of it.**
             *
             * A process may give up a band it was handed and may not take
             * one it was not, so the rule above - nothing promotes itself -
             * is untouched. Asking for a higher band is refused rather than
             * ignored, for the reason the quantum is: a caller that is told
             * "yes" and given nothing has no way to find out.
             */
            if ((unsigned)sc->arg[1] >= thread_current()->sched.priority) {
                result = SYS_ERR_DENIED;
                break;
            }

            thread_set_priority(thread_current(), (unsigned)sc->arg[1]);
            result = 0;
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
        result = ipc_cap_drop(thread_current(), (cap_t)sc->arg[0]);
        break;

    case SYS_CAP_CHECK:
        /*
         * Whether a capability this thread holds still names something.
         *
         * For a holder of somebody else's endpoint - the /app registry holds
         * one for every name it answers to - which had no way to ask:
         * calling it blocks on one that is live, and receiving on it could
         * take a message meant for its server. No permission check, for
         * SYS_CAP_DROP's reason: the index resolves against this thread's
         * own table.
         */
        result = ipc_cap_check(thread_current(), (cap_t)sc->arg[0]);
        break;

    case SYS_CALL:
        result = sys_call(p, (cap_t)sc->arg[0], sc->arg[1], sc->arg[2]);
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
            unsigned long timeout = (unsigned long)sc->arg[4];

            if (timeout > (unsigned long)TICK_HZ * 3600UL) {
                timeout = (unsigned long)TICK_HZ * 3600UL;
            }

            result = sys_receive(p, (cap_t)sc->arg[0], sc->arg[1], sc->arg[2],
                                 (sc->arg[3] & 1u) != 0, timeout);
        }
        break;

    case SYS_REPLY:
        result = sys_reply(p, sc->arg[0], sc->arg[1]);
        break;

    case SYS_SPAWN:
        result = sys_spawn(p, sc->arg[0], sc->arg[1], (size_t)sc->arg[2], sc->arg[3]);
        break;

    case SYS_SCREEN:
        result = sys_screen(p, sc->arg[0]);
        break;

    case SYS_SYSINFO:
        result = sys_sysinfo(p, sc->arg[0]);
        break;

    case SYS_MAP:
        result = sys_map(p, (size_t)sc->arg[0]);
        break;

    case SYS_UNMAP:
        result = sys_unmap(p, sc->arg[0], (size_t)sc->arg[1]);
        break;

    case SYS_SETNAME:
        result = sys_setname(p, sc->arg[0], (size_t)sc->arg[1]);
        break;

    case SYS_PROCTABLE:
        result = sys_proctable(p, sc->arg[0], (size_t)sc->arg[1]);
        break;

    case SYS_WAIT: {
        /*
         * x1 bit 0 asks not to block. A shell draining the processes it
         * spawned must not stop at the prompt for ten seconds because one
         * of them is still running.
         */
        unsigned id = 0;
        uintptr_t id_ptr = sc->arg[0];

        if (id_ptr != 0 && !process_may_write(p, id_ptr, sizeof(uint64_t))) {
            result = SYS_ERR_FAULT;
            break;
        }

        result = process_wait(p, &id, (sc->arg[1] & 1u) != 0);

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
    sc->result = (uint64_t)result;
}
