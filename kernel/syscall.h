/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_SYSCALL_H
#define KERNEL_SYSCALL_H


/*
 * The syscall interface.
 *
 * A number and up to six arguments in registers, and the result back in
 * one. **Which registers is the architecture's business and is written down
 * once, at the bottom of this file**, beside the entry stubs that implement
 * it - AArch64 borrows the Linux convention (`svc #0`, the number in x8,
 * arguments in x0-x5) and x86-64 borrows System V's, and the reason for
 * borrowing in both cases is the same: it is the one every tool and every
 * reader already knows. Nothing else about either is inherited.
 *
 * The numbering and the meaning below are the interface. The registers are
 * how it is spelled.
 *
 * SYS_TICKS is the one that looks out of place, and is not. `design.md` §4.4
 * makes the *clock* a capability - `/dev/clock`, asked for by name, handed
 * over or not - and that stays true: what a program wants is a date, and a
 * date comes from a server. But the server has to read the counter from
 * somewhere, and a monotonic tick is the kind of thing that genuinely cannot
 * be a message: it has to be sampled where the code being timed runs, or the
 * sample measures the sampling. So the raw counter is a syscall and the wall
 * clock stays a capability, which is the same split the design already makes
 * between entering the kernel and everything else.
 *
 * It also retires a weakness. Without it a process cannot read the counter
 * at all, and Lua's string-hash seed came off a stack address for want of
 * anything better.
 *
 * These are not the interface Kosmos ends up with. `design.md` §4.4 has
 * processes reaching resources by name through a namespace, and at M5 most of
 * what is here becomes `list`, `read` and `write` against a path. What
 * survives is what genuinely cannot be a message: entering and leaving the
 * kernel, and the capability operations that everything else is expressed in.
 */

#define SYS_EXIT        0   /* (code)                    never returns */
#define SYS_WRITE       1   /* (ptr, len, colour)     -> bytes written */
#define SYS_YIELD       2   /* ()                                      */
#define SYS_ENDPOINT    3   /* ()                     -> cap or error  */
#define SYS_CALL        4   /* (cap, msg, reply)      -> 0 or error    */
#define SYS_RECEIVE     5   /* (cap, msg, &sender, flags) -> 0 or error */
#define SYS_REPLY       6   /* (sender, msg)          -> 0 or error    */
#define SYS_GETCHAR     7   /* ()                     -> byte, or -1    */
#define SYS_SPAWN       8   /* (arg, caps, ncaps, flags) -> child id     */
#define SYS_WAIT        9   /* (&id)                  -> exit code       */
#define SYS_TICKS      10   /* ()                     -> monotonic ticks  */
#define SYS_SCREEN     11   /* (&info)                -> 0 or error       */
#define SYS_SYSINFO    12   /* (&info)                -> 0 or error       */
#define SYS_MAP        13   /* (pages)                -> address or error  */
#define SYS_UNMAP      14   /* (address, pages)       -> 0 or error       */
#define SYS_SETNAME    15   /* (ptr, len)             -> 0 or error       */
#define SYS_PROCTABLE  16   /* (&entries, max)        -> count or error   */
#define SYS_ENDPOINT_DESTROY 17 /* (cap)              -> 0 or error       */
#define SYS_POINTER    18   /* (&state)               -> 0 or error       */
#define SYS_SCREEN_TAKE 19  /* (take)                 -> 0 or error       */
#define SYS_KILL       20   /* (id)                   -> 0 or error       */
#define SYS_WAIT_INPUT 21   /* (timer ticks)          -> 0                 */
#define SYS_LOG        22   /* (buffer, max)          -> bytes or error    */
#define SYS_MEM_CREATE 23   /* (pages, flags)         -> cap or error       */
#define SYS_MEM_MAP    24   /* (cap)                  -> address or error   */
#define SYS_MEM_SIZE   25   /* (cap)                  -> pages or error     */

#define SYS_DISK_INFO  26   /* (out struct)           -> 0 or error         */
#define SYS_DISK_READ  27   /* (sector, buf, bytes)   -> bytes or error     */
#define SYS_DISK_WRITE 28   /* (sector, buf, bytes)   -> bytes or error     */

#define SYS_BOOT_OPT   29   /* (name, out, max)       -> length or error   */
#define SYS_CAP_DROP   30   /* (cap)                  -> 0 or error         */
#define SYS_SHARE_UNMAP 31  /* (address, pages)       -> 0 or error         */
#define SYS_SCHED_INFO 32   /* (&info)                -> 0 or error         */

/*
 * **What a driver outside the kernel needs, and nothing else does.**
 *
 * `docs/drivers.md` is the argument: a USB stack is 1500 to 2500 lines
 * before enumeration, and `make size` says 2747 remain before the kernel's
 * smoke alarm - so it cannot live here, and a driver at EL0 needs two
 * things the kernel has never handed out.
 *
 * `SYS_MEM_PHYS` is where a region begins in physical memory, because
 * hardware is told where its rings are in the bus's addresses. Refused
 * unless the region was made with `MEM_CONTIGUOUS`, since reporting a base
 * for scattered pages would point hardware at somebody else's heap.
 */
#define SYS_MEM_PHYS   44   /* (cap)                  -> address or error   */

/*
 * `SYS_DEV_MAP` is the other half, and the one that makes a driver possible
 * at all: a device's registers, in the driver's own address space.
 *
 * The kernel has always mapped MMIO for itself - `MAP_DEVICE`, a whole 2 MB
 * block of it at boot - and has never had a way to hand a window of it to a
 * process. That is the difference between "a driver is kernel code" and "a
 * driver is a server you handed a capability to".
 *
 * The memory type is the point rather than a detail. Device-nGnRnE on ARM,
 * PCD|PWT on x86: a store to a register happens once, in the order it was
 * written, and is not merged with its neighbour. Mapped as ordinary memory a
 * doorbell write can sit in a cache line waiting for company, and the device
 * waits for ever.
 */
#define SYS_DEV_MAP    45   /* (phys, pages)          -> address or error   */

/*
 * How much of one, at most.
 *
 * A PCI BAR for the kind of device this is for is small: xHCI's is typically
 * 64 KB, NVMe's 16, an I/O APIC's one page. Four megabytes is far above all
 * of them and far below anything that would exhaust the share window, so it
 * is a bound that catches a wrong number rather than a budget anybody is
 * meant to plan against.
 */
#define DEV_MAP_PAGES_MAX  1024u

/*
 * **And being told the device has something to say**, which is the third of
 * `drivers.md`'s primitives and the one a driver cannot work around. Without
 * it a driver polls, which on a USB controller means either a thread that
 * never sleeps or a latency nobody wants.
 *
 * Three calls, which is where L4, seL4 and QNX all ended up:
 *
 *   `SYS_IRQ_CLAIM` takes a line and gives back a *capability*, so a driver
 *   names its interrupt by an index into its own table. Refused for a number
 *   the board spends on itself - the tick above all, since a process able to
 *   claim it could stop the machine scheduling.
 *
 *   `SYS_IRQ_WAIT` blocks until one arrives, or returns at once if one
 *   arrived while the driver was busy. A count rather than a flag, so an
 *   interrupt during servicing is not lost. **With `ticks` it waits no
 *   longer than that** and says `SYS_NO_INTERRUPT`, so a driver whose
 *   device never interrupts is told rather than hung; zero waits for ever.
 *
 *   `SYS_IRQ_ACK` unmasks. The kernel masks the line on delivery because a
 *   level-triggered source is still asserted when the handler returns, and
 *   unmasked it would arrive again before the driver - a process - could run
 *   at all. **On an MSI there is nothing to mask and this costs a syscall
 *   and changes nothing**, which is correct rather than missing: the device
 *   wrote to the local APIC and nothing is left asserted.
 *
 * And a fourth, arrived with the first driver that has more than one device
 * and one thread to wait with: `SYS_IRQ_WAIT_ANY`, further down, is the
 * second call's wait on several lines at once.
 */
#define SYS_IRQ_CLAIM  46   /* (intid)                -> cap or error       */
#define SYS_IRQ_WAIT   47   /* (cap, ticks)           -> 0, none, or error  */
#define SYS_IRQ_ACK    48   /* (cap)                  -> 0 or error         */

/*
 * **Where a device is**, asked of the board by a process holding device
 * authority. `CLAUDE.md` puts every hardware address in `hal/` and init is
 * userland too, so a driver cannot be *given* an address by anybody but the
 * kernel, and the kernel only relays what the board says. The address then
 * exists in userland solely as a value a driver was handed.
 *
 * **`index` is which one of that kind**, from zero, because "where is the
 * xHCI controller" has more than one answer on the machine this is for: a
 * laptop of the ThinkPad's generation is expected to carry one in its chipset
 * and another for its USB-C ports. `SYS_ERR_NO_DEVICE` past the last, so a
 * driver asks for 0, 1, 2 until it is told there are no more.
 */
#define SYS_DEV_FIND   49   /* (kind, index, &info)   -> 0 or error         */

/*
 * Whether a physical range may be handed to a driver at all - size,
 * alignment, and above all that it is not RAM. Declared here rather than
 * left static so the suite can ask it directly: the mapping mechanics are
 * `as_map`'s and already tested, and this is the part that is about safety.
 *
 * **Behind two guards, because this header is shared twice over.**
 * Everything else in here is numbers, which is why userland can include it
 * without including anything else - and why *assembly* can: the entry stubs
 * at the bottom of this file are `.S`, and a C declaration in front of them
 * makes the assembler read `typedef long int ptrdiff_t` and say `unknown
 * mnemonic`, which is a genuinely baffling error message to meet.
 *
 * So: not for userland, which would pay three headers for a declaration it
 * never uses, and not for the assembler, which cannot read one at all.
 */
#if !defined(KOSMOS_USER) && !defined(__ASSEMBLER__)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool dev_range_ok(uintptr_t phys, size_t pages);
#endif

/* Flags for SYS_MEM_CREATE's second argument. Zero is the old behaviour and
 * what every caller but a driver wants. */
#define MEM_CONTIGUOUS  1u
#define SYS_SCHED_SET  33   /* (what, value)          -> 0 or error         */

/*
 * The next key transition: a keycode and whether it went down.
 *
 * Separate from SYS_GETCHAR rather than folded into it, because they are
 * different questions. A character is what a key *means* - shifted, mapped,
 * with an arrow spread over three bytes - and is what a terminal wants. A
 * transition is what the key *did*, and is what anything that cares about a
 * key being held has to have; no stream of characters can express it.
 *
 * Both come off the same pass through the device, so the two never
 * disagree about what happened.
 *
 * Gated on owning the console, exactly as SYS_GETCHAR is. That is not
 * ceremony: a process that can ask which keys are being pressed is a
 * keylogger, so this goes to the one process the kernel already trusts with
 * input, and everything else asks it.
 */
#define SYS_KEY_EVENT  34   /* (&code, &down)         -> 0, or SYS_NO_INPUT */

/*
 * Stop the machine, or start it again.
 *
 * Gated on `owns_procctl`, which is the same authority `SYS_KILL` uses to
 * end any process - and that is the right comparison rather than a
 * convenient one: turning the machine off ends every process on it, so the
 * right to do it is the right to end them all at once. Nothing new is
 * granted, and a program that may not kill a process it did not start
 * certainly may not do this.
 *
 * Does not return when it works.
 */
#define SYS_POWER      35   /* (0 off, 1 restart)     -> does not return   */

/*
 * One period of PCM, queued. 44100 Hz, stereo, signed sixteen-bit - the
 * format is `hal.h`'s and is not negotiable here.
 *
 * Returns 0 when it was taken and SYS_NO_INPUT when the queue is full,
 * which is not an error: a caller that is ahead of the device should be
 * told so rather than blocked, because blocking in an audio path is how a
 * frame gets missed somewhere else.
 */
#define SYS_SND_WRITE  36   /* (ptr, len)             -> 0, full, or error */

/*
 * How many periods the device has not finished with.
 *
 * The deadline, as a number. `roadmap.md` M11a promises a measurement
 * rather than a bound, and this is the measurement: at zero the device has
 * run dry and the next sound has a click in it.
 */
/*
 * The largest period any board here uses, so userland can size a buffer
 * without including the HAL. A number rather than the real one because the
 * real one is the board's: `sys.info().audio_period` says what this machine
 * actually uses, and this is only the ceiling.
 */
#define HAL_SND_PERIOD_BYTES_MAX 8192

/*
 * The rate every source is converted to, which userland needs in order to
 * do the converting. The board fixes it in `hal.h`; this is the ABI's copy
 * of the same number, and `sys.info().audio_rate` reports what the machine
 * actually has. They agree today because there is one board.
 */
#define HAL_SND_RATE 44100u

#define SYS_SND_QUEUED 37   /* ()                     -> periods in flight */

/*
 * Sleep for a number of scheduler ticks.
 *
 * `SYS_WAIT_INPUT` has done this since M6 and does it well, but only for
 * the one process that owns the console, because it also wakes on a key.
 * Everything else that wanted to wait had `SYS_YIELD`, which does not wait
 * at all: it goes to the back of its band and comes straight back, so a
 * thread "waiting" this way is runnable for ever and a core is gone.
 *
 * That was measured rather than reasoned about. Playing a tone put the
 * audio server at 26% and the program feeding it at 63%, against 8% for
 * Doom - which draws a 320x200 frame thirty-five times a second and is
 * cheap precisely because it *waits* in between. Two spinning threads cost
 * eight times what rendering Doom costs.
 *
 * No permission check. A thread choosing not to run is the one request
 * that cannot be used against anybody: it gives the machine back.
 *
 * Ticks, not the physical counter - the trap `SYS_WAIT_INPUT` documents at
 * length, and the reason this takes the same units as that one rather than
 * milliseconds, which would read as an invitation to sleep for less than
 * a tick and get a tick anyway.
 */
#define SYS_SLEEP      38   /* (ticks)                -> 0                  */

/*
 * The network card: what it is, and frames both ways.
 *
 * **Frames, not packets.** What crosses here is an Ethernet frame with its
 * header on it, exactly as it goes on the wire, because that is all the
 * driver knows - `hal.h` says why. Addresses, protocols and checksums are
 * the stack's, and the stack is a process.
 *
 * Only the process holding the card may ask, which is `SPAWN_NET`. A
 * process that can send a raw frame can claim any address on the network
 * and read every frame that reaches it, so this is a grant of the same
 * weight as the disk: exactly one process gets it, and everything else
 * reaches the network by asking that one.
 *
 * `SYS_NET_RECV` answers `SYS_NO_INPUT` when nothing is waiting, which is
 * not an error and is what it does most of the time - the same convention
 * `SYS_KEY_EVENT` uses.
 */
#define SYS_NET_INFO   39   /* (&info)                -> 0 or error         */
#define SYS_NET_SEND   40   /* (ptr, len)             -> 0 or error         */
#define SYS_NET_RECV   41   /* (ptr, max)             -> bytes, or none     */

#define SYS_PTR_SPEED  42   /* (units)                -> the speed now      */

/* Whether a capability still names something, asked without using it. */
#define SYS_CAP_CHECK  43   /* (cap)                  -> 0 or error         */
/*
 * **The console owner's input wait, which a caller on a watched endpoint
 * also ends.**
 *
 * The window manager sleeps inside the console server's `CON_OP_WAIT`, and
 * that sleep is this process's `SYS_WAIT_INPUT`: until a key, the pointer or
 * the deadline. A request to the window manager meanwhile waited for the
 * deadline - 11.5 ms a round trip on an idle desktop, and the reason the
 * Super Nintendo managed 43 frames a second of 60 on the ThinkPad with the
 * machine idle. The console server now watches the window manager's
 * endpoint while it sleeps, and a caller arriving there ends the sleep too.
 *
 * `cap` is the watched endpoint, in the caller's table. Nothing is received:
 * the window manager collects its messages itself, afterwards, as it always
 * did. Refused to anybody but the console owner, like `SYS_WAIT_INPUT`.
 * Returns 0, or an IPC error - a stale capability once the watched process
 * has gone.
 */
#define SYS_WAIT_INPUT_OR_CALL 50   /* (ticks, cap)     -> 0 or error         */

/*
 * **A pointing device's movement, from the process that drives it.**
 *
 * A USB mouse's driver is a process, and what the mouse reports - how far,
 * and which buttons are down - has to reach the one pointer the window
 * manager asks `SYS_POINTER` about. This is that path: `dx` right and `dy`
 * down, in the device's own counts, and `buttons` its whole state, bit 0
 * left and bit 1 right. The board adds the movement to the position it keeps
 * for its own relative devices, at their speed (`hal_pointer_move`), so the
 * desktop cannot tell a USB mouse from a TrackPoint.
 *
 * **Three numbers in registers, one call a report - and `CLAUDE.md`'s rule
 * about streams was asked about it rather than assumed away.** A report does
 * recur because the hardware says so. What the rule protects against is a
 * payload built, copied and collected on every recurrence, with a server in
 * the path; here nothing is allocated or copied, no server is involved, and
 * the kernel folds three integers into a position and keeps none of them -
 * the same work the i8042's interrupt does for every packet. A ring shared
 * with the kernel would still need this call, or an interrupt, to say it had
 * something in it.
 *
 * **Gated on device authority.** A process able to move the pointer and press
 * its buttons can click anything on the screen for the person at it. The one
 * process with a reason to is a driver, and the driver there is holds
 * `owns_devices` already; a grant of its own belongs with the devices server
 * that will hand drivers their capabilities (`process.h`).
 *
 * `SYS_ERR_NO_DEVICE` when the board's pointer is absolute - a tablet, which
 * says where it is and has no position for a movement to be added to.
 */
#define SYS_POINTER_MOVE 51 /* (dx, dy, buttons, wheel) -> 0 or error       */

/*
 * **`SYS_IRQ_WAIT` on several lines at once**, for a driver with more than
 * one device and one thread: the xHCI driver on the ThinkPad's two
 * controllers, where a mouse on the second would wait out a nap on the first.
 * `caps` is an array of `count` interrupt capabilities, no more than
 * `IRQ_WAIT_ANY_MAX`. The answer is the place in it of a line that had an
 * interrupt - taken, as `SYS_IRQ_WAIT` takes one - or `SYS_NO_INTERRUPT` when
 * `ticks` ran out first; zero waits for ever.
 *
 * **And two endpoints, each when it is not negative** (USB step 5c, and
 * storage at full speed): a caller waiting on the first answers
 * `IRQ_WAIT_CALLER`, and on the second `IRQ_WAIT_CALLER + 1`, to be collected
 * with a receive that does not block - so a driver with clients of its own
 * waits for them and its devices on one wait. The xHCI driver's are the disk
 * server's write endpoint and `/dev/blocks`, whose reads waited out the
 * driver's 50 ms deadline while the wait could watch only one. A line with an
 * interrupt is answered first, and one endpoint named twice is refused.
 * `kernel/irq.c` has the rest.
 */
#define SYS_IRQ_WAIT_ANY 52 /* (&caps, count, ticks, ep, ep2) -> which, caller, none */

#define IRQ_WAIT_ANY_MAX       8u
#define IRQ_WAIT_ENDPOINTS_MAX 4u
/* Never a line's place; plus one, the second endpoint's caller. */
#define IRQ_WAIT_CALLER  ((long)IRQ_WAIT_ANY_MAX)

/*
 * **The firmware's AML, as bytes**: table `index` from zero, `max` bytes of
 * it from `offset` into `buf`. The answer is how many were copied - zero at
 * or past its end - or `SYS_ERR_NO_DEVICE` past the last table, which is
 * every index on a board with no ACPI. `hal_firmware_table` says which tables
 * these are, and why only AML.
 *
 * For `acpi`, which saves them so the ThinkPad's DSDT can be read on the Mac
 * with `iasl` - where its brightness is set, and its battery read.
 */
#define SYS_FIRMWARE   53   /* (index, offset, buf, max) -> bytes or error */

/*
 * **A key a driver presses**, as `SYS_POINTER_MOVE` is a mouse a driver
 * moves: evdev's code and whether it went down, from a process holding
 * device authority - the USB driver, for a game controller's buttons. They
 * join the board's own keys (`hal_key_push`) and reach the focused window as
 * any key does; a code past `KEY_PUSH_MOST` is refused. What the process
 * still holds down when it ends is let go by the kernel.
 */
#define SYS_KEY_PUSH   54   /* (code, down)           -> 0 or error         */

/*
 * **Where this thread's own data is**, which the hardware then hands back at
 * `%fs:0` on x86 and in `TPIDR_EL0` on AArch64 (`threads.md` step 2).
 *
 * The first thing in it is `errno`, which was one static int in a process
 * "because there is one thread". The kernel neither reads the block nor
 * knows what is in it: it keeps the number with the thread and puts it in
 * the register on every switch, which is the whole of what a thread pointer
 * is. A process that never asks keeps zero, and so does every kernel thread.
 *
 * The address is not checked, and deliberately: a process can only hurt
 * itself with it, exactly as it can with a bad pointer in its own code, and
 * the kernel never follows it.
 */
#define SYS_SET_TLS    55   /* (address)              -> 0                  */

/*
 * **Threads in a process** (`threads.md` step 3).
 *
 * `SYS_THREAD_CREATE` takes where to begin and one word to begin with, and
 * answers with the thread's *index in this process* - never a number that
 * means anything outside it, which is the rule capabilities follow and for
 * the same reason. The kernel makes its stack, a megabyte of address space
 * apiece with the stack at the top and the rest unmapped, so an overflow
 * faults instead of reaching a neighbour.
 *
 * `SYS_THREAD_EXIT` ends the caller with a code; `SYS_THREAD_WAIT` waits for
 * the thread with that index and answers with its code. A thread that has
 * already ended is answered at once - its slot is kept until somebody asks,
 * exactly as a process's is.
 *
 * The process ends when its *first* thread returns, as C's `main` does, and
 * that thread is also the one that tears the process down: it waits for its
 * siblings to leave first, because freeing the address space under a running
 * thread is the bug this design exists to avoid.
 */
#define SYS_THREAD_CREATE 56 /* (entry, arg)          -> index or error     */
#define SYS_THREAD_EXIT   57 /* (code)                -> does not return    */
#define SYS_THREAD_WAIT   58 /* (index)               -> its code or error  */

/*
 * **Wake whoever holds the network card, because a frame arrived.**
 *
 * The kernel's own virtio driver calls `process_wake_net` from its interrupt
 * handler; a network adapter on the USB bus is driven by a *process*, and
 * the kernel must not learn what USB is. So the one thing the kernel has
 * that a userland driver cannot do for itself - cutting short the stack's
 * timed receive - is reachable here (`usb.md` 7d, `roadmap.md` 5m-d).
 *
 * **For a process that drives devices**, `owns_devices`, and refused to
 * anything else. Not because waking the stack early is dangerous - the worst
 * it does is make it look at a ring - but because a process that could do it
 * in a loop would spend the stack's time, and the only processes with frames
 * to report are the ones holding hardware.
 *
 * Without it the stack has to come back and ask, which is a poll wearing a
 * different hat: it would look at the ring at a rate somebody picked, and
 * the round-trip time it reported would be that rate rather than the
 * network's. That sentence is `process_wake_net`'s own, and it is why this
 * exists rather than a shorter deadline.
 */
#define SYS_NET_WAKE   59   /* ()                     -> 0 or error         */

#define SYS_MAX         60

/*
 * What a spawn may hand its child beyond capabilities.
 *
 * A flag rather than a capability, for the same reason `owns_console` is a
 * boolean: a device should be named by a capability the process holds, and
 * that needs a capability that names a device. Until then, whoever spawns
 * decides, which is at least the right shape - authority flows from parent
 * to child and never sideways.
 */
#define SPAWN_CONSOLE   1u
#define SPAWN_SCREEN    2u

/*
 * The right to make a noise.
 *
 * Its own flag rather than riding on the screen's, because they are
 * different powers: a program that draws is not thereby allowed to play
 * sound over whatever else is playing, and a program that plays a sound
 * has no business drawing. One device, one owner, one grant - the same
 * shape the screen already has, which is what makes this a line of code
 * rather than a design.
 */
#define SPAWN_AUDIO    16u

/*
 * The disk, handed on the same way and for the same reason.
 *
 * A process that can read raw sectors can read every file on the machine
 * whatever any namespace says, so this is the most powerful grant there is -
 * more than the screen, which can only draw. Exactly one process gets it:
 * the filesystem server. Everything else reaches the disk by asking that
 * server, which is what makes a namespace mean anything.
 */
#define SPAWN_DISK      4u

/*
 * Authority over every process, not only your own children.
 *
 * `SYS_KILL` is otherwise parent-only, and that rule is right: holding a
 * capability to somebody is not permission to end them. But a task manager
 * is exactly the program that needs to end things it did not start, and the
 * answer to "this program needs a power it should not have by default" in
 * this system is a grant rather than a relaxed rule.
 *
 * So: init holds it, init hands it to one process, and that process can end
 * anything. Every other process keeps the parent-only rule. What makes this
 * safe is not that killing is hard - it is that being *able* to is visible,
 * granted once, and listed by `ps` next to the console and the screen.
 */
#define SPAWN_PROCCTL   8u

/*
 * The network card, handed on the same way as the disk and for a reason of
 * the same weight.
 *
 * A process that can put a raw frame on the wire can claim any address on
 * the network, answer for anybody, and read every frame that reaches the
 * machine - whatever any namespace says about who owns what. So exactly one
 * process gets it: the stack. Everything else reaches the network by asking
 * that stack, which is what makes a connection a capability rather than a
 * number anybody can name.
 */
#define SPAWN_NET      32u

/*
 * **Hardware itself: every device on the machine, and every physical
 * address.**
 *
 * The strongest grant there is, above the disk - because the disk is every
 * file and this is every *byte*, including the pages the kernel and every
 * other process are running out of.
 *
 * It exists so that drivers can stop being kernel code. `docs/drivers.md`
 * has the argument and `make size` has the number: a USB stack does not fit
 * in what is left of the kernel, and a driver at EL0 needs somewhere to get
 * its registers from. Somebody has to be able to mint the first capability
 * and in this system that is init.
 *
 * **A driver never holds this.** It is handed a capability naming one
 * register range, cannot express any other, and that is the entire point -
 * the USB driver and the sound driver are each unable to say the other's
 * name. This flag belongs to init and to whichever server it gives the job
 * of enumerating hardware, and `ps` lists it beside the console and the
 * screen for the same reason: being able to should be visible.
 */
#define SPAWN_DEVICES  64u



/*
 * What SYS_SCHED_INFO reports: which policy is running, how long a turn is,
 * and what else this machine could be using instead.
 *
 * `tick_hz` is here so a caller can turn a quantum in ticks into
 * milliseconds without knowing what the timer was configured to. A settings
 * app that hardcoded 100 would be lying the day the tick rate changes, and
 * changing the tick rate is the only way to get a quantum below 10 ms.
 */
#define SCHED_NAME_MAX   16
#define SCHED_POLICY_MAX  4

/* `struct proc_info.cpu` when the process has no thread to have a home. */
#define PROC_CPU_NONE  0xFFFFFFFFu

/* What SYS_SCHED_SET changes. */
#define SCHED_SET_QUANTUM  0
#define SCHED_SET_POLICY   1

/*
 * Lower *this* process's own band. Never raise it.
 *
 * The handler's comment explains at length why setting a priority is not a
 * syscall: bands are handed out by capability so that nothing can promote
 * itself. **This does not promote.** It gives up a band the process was
 * handed, which is the one direction that grants nothing - the same shape
 * as closing a capability you were given.
 *
 * It exists because `process_grant_screen` promotes to DISPLAY and the
 * screen is handed to every program, so a compute worker like `/bin/spin.lua`
 * runs in the compositor's own band and starves the desktop it is supposed
 * to be a workload for. The real fix is to stop granting the screen to
 * everything, and `user/init/init.lua` says why that is a larger change than
 * it looks. This lets the one program that is *deliberately* a hog say so.
 */
#define SCHED_SET_MY_BAND  2

/*
 * What SYS_SCREEN reports.
 *
 * The address is in the *caller's* address space, because the framebuffer is
 * mapped into a process that holds the screen and nowhere else. A process
 * without it never learns the number, which is the point: this is the whole
 * of what the kernel says about pixels, and it says it only to the one
 * process that was handed the device.
 *
 * The layout is written twice - here and in user/include/kosmos.h - for the
 * same reason `struct message` is, and is checked the same way.
 */
#ifndef __ASSEMBLER__
#include <stdint.h>

/*
 * Inside the guard, with every other struct in this header.
 *
 * This file is included from assembly - `user/hello.S` does, for the
 * syscall numbers - and a struct definition there is a syntax error per
 * line. The constants above may live outside because a `#define` is
 * meaningful to both; a type is not.
 */
struct schedinfo {
    uint32_t policy;        /* index of the one installed */
    uint32_t policies;      /* how many there are */
    uint32_t quantum;       /* a turn, in ticks */
    uint32_t tick_hz;       /* ticks per second */
    uint32_t priorities;    /* how many bands */
    char     name[SCHED_POLICY_MAX][SCHED_NAME_MAX];
};

struct screen_info {
    uint64_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;             /* bytes per row; never width * 4 */
    uint32_t reserved;
};

/*
 * What SYS_SYSINFO reports: the machine, and how much of it is in use.
 *
 * **The raw ID registers travel with the decoded numbers**, deliberately.
 * Decoding them is a table lookup and tables belong in Lua, not in the
 * kernel - `design.md` §1's whole argument is that policy goes up and
 * mechanism stays down. So the kernel reads registers and counts pools, and
 * userland decides what any of it means and how to say it.
 *
 * Not gated. A hardware inventory is not authority: it says nothing a
 * process could not learn by other means and grants nothing. It is the same
 * category as SYS_TICKS.
 *
 * The intended way to read this is `/dev`, through the namespace, the way
 * everything else is reached. This syscall is the door the device server
 * goes through, exactly as SYS_WRITE is the door the console server goes
 * through - and, unlike that one, nothing is lost by another process using
 * it directly.
 */
/*
 * One process, as SYS_PROCTABLE reports it.
 *
 * `design.md` §9.5 wants this reached through `/proc` in the namespace
 * eventually, like everything else. It is a syscall today for the same
 * reason SYS_SYSINFO is: something has to be the door a server goes
 * through, and there is not yet a server to be the one.
 *
 * The kernel names nothing here and interprets nothing. A process says what
 * it is with SYS_SETNAME; what a name *means* - which layer it belongs to,
 * whether it is a server or an app - is decided in Lua, where the tables
 * that decide such things belong.
 */
struct proc_info {
    uint32_t id;
    uint32_t state;             /* the thread's: ready, running, blocked */
    uint32_t exited;
    int32_t  exit_code;
    uint64_t ticks;             /* timer ticks charged to it, only rising */
    uint32_t pages;             /* pages it holds through SYS_MAP */
    uint32_t held;              /* and everything else: image, heap, stacks */
    uint32_t caps;              /* capabilities in its table */
    uint32_t owns;              /* 1 the console, 2 the screen, 4 the disk,
                                   8 process control, 16 device authority */

    /*
     * The band it is scheduled in - the *effective* one, so a server
     * carrying a caller's priority reports what it is actually running at
     * rather than what it was given.
     *
     * Reported because a scheduler with bands nobody can see is a scheduler
     * nobody can reason about. `scheduler` lets you change the policy and
     * the quantum while the machine runs, and until now there was no way to
     * look at what that did to any particular process.
     */
    uint32_t priority;

    /*
     * Which processor it runs on, and it is worth saying why this is a
     * fixed number rather than a sample.
     *
     * **Kosmos has strict affinity.** `t->sched.cpu` is assigned once in
     * `thread_create_suspended` and never changes: there is no migration,
     * no balancing and no work stealing, and `thread_block_and_release`
     * depends on that for its correctness - it releases the caller's lock
     * before the context switch, which is only safe because the thread can
     * be enqueued on one core's queue and no other.
     *
     * So this is not the "which core did it happen to be on when you
     * looked" that a migrating scheduler would report and that nothing
     * could usefully display. It is where the process lives for its whole
     * life, and it is the same answer every time it is asked.
     *
     * `PROC_CPU_NONE` when there is no thread, which reads as "nowhere"
     * rather than as core zero - the two are different and zero is a real
     * core. A sentinel rather than `NR_CPUS`, so that userland does not
     * have to know how many processor slots the kernel was built with in
     * order to recognise the answer "none".
     */
    uint32_t cpu;

    /*
     * The id of the process that spawned it, 0 for init. Reported so that a
     * process can be told apart from another of the same name by who
     * started it: the drives *server* is init's and the Drives *app* is a
     * runner's, and Processes called the server an app (`roadmap.md` 6g).
     */
    uint32_t parent;

    char     name[16];
};

/*
 * Where the pointer is, as the device reports it.
 *
 * Undecoded, like `sysinfo`: the range travels with the position so that
 * whoever knows the size of the screen does the scaling. A kernel that
 * scaled would have to know which screen, and it does not.
 */
/*
 * How many button transitions one `SYS_POINTER` can carry.
 *
 * Sixteen is eight clicks in whatever interval the caller polls at, and a
 * caller that polls once a frame is asking about 16 ms. Nobody clicks
 * eight times in a frame. The board keeps twice this and `dropped` says
 * when even that was not enough, so a loss is visible rather than silent.
 */
#define POINTER_EDGES_MAX 16u

/*
 * One button transition, and where the pointer was when it happened.
 *
 * **A position is a state and a button is an event.** `buttons`, `x` and
 * `y` below answer "where is it and what is held *now*", which is the
 * right and only answer for a position. It is the wrong answer for a
 * click: a press and a release that both happen between two calls leave
 * `buttons` exactly as they found it, and the click never existed as far
 * as the caller is concerned. That was a real bug on real hardware
 * (`hal/pointer_edges.c`), and these are the fix.
 *
 * Declared here rather than shared with `hal.h`'s `struct pointer_edge`,
 * exactly as `pointer_info` is declared rather than shared with
 * `pointer_state`: the kernel copies field by field, so the board's shape
 * and the system call's ABI can move independently.
 */
struct pointer_edge_info {
    uint32_t x, y;
    uint32_t buttons;           /* the merged state *after* this transition */
};

struct pointer_info {
    uint32_t x, y;
    uint32_t min_x, max_x;
    uint32_t min_y, max_y;
    uint32_t buttons;
    uint32_t moved;

    /* Wheel notches since the last call, positive away from the person -
     * `pointer_state`'s count, taken by reading. */
    int32_t  wheel;

    /* What the buttons did since the last call, in order. Reading takes
     * them, as reading a key does. */
    uint32_t nedges;
    uint32_t dropped;
    struct pointer_edge_info edges[POINTER_EDGES_MAX];
};

/*
 * Which architecture the raw words below belong to.
 *
 * Without this they cannot be read at all. The kernel deliberately decodes
 * nothing about the processor - the ID registers go out as they were read
 * and what they mean is userland's problem - and that division only works
 * while there is one architecture. Two of them, and "raw" is not enough
 * information: the same eight words are MIDR_EL1 and friends on one machine
 * and CPUID leaves on another.
 *
 * Numbers rather than a string because this is an ABI, and a string is a
 * thing to get subtly wrong on both sides.
 */
#define CPU_ARCH_UNKNOWN  0
#define CPU_ARCH_AARCH64  1
#define CPU_ARCH_X86_64   2

/*
 * How many raw words `sysinfo` carries. AArch64 fills six, x86-64 nine.
 *
 * Sized to the larger rather than to a round number, because the rule both
 * architectures follow is that **every word something decodes from is one
 * of these**. A decoded cache line whose source register was not exported
 * is a number a reader has to take on trust, which is the thing handing
 * them over raw exists to avoid.
 */
#define CPU_RAW_WORDS     10

/*
 * One device as a bus reported it, decoded by nobody.
 *
 * `id` is vendor in the high half and device in the low, which is how PCI
 * hands it over and close enough to how a virtio-mmio window does. `class`
 * is PCI's class/subclass/interface, and zero where the bus has no such
 * idea. Turning either into a name is a table, and a table belongs in
 * userland - the same division `cpu_raw` draws.
 */
struct bus_device {
    uint32_t id;        /* vendor << 16 | device */
    uint32_t class;     /* PCI class/subclass/prog-if, or 0 */
    uint16_t where;     /* bus << 8 | slot << 3 | function, or the window */
    uint8_t  claimed;   /* a driver in this system took it */
    uint8_t  reserved;
};

/*
 * Room for sixty-four, and a machine with more says how many more.
 *
 * It was thirty-two - QEMU's q35 with every device this system knows
 * attached, and the thirty-two virtio-mmio windows the ARM board lays out -
 * and the ThinkPad has twenty-two on bus 0 alone, before its bridges are
 * followed to the drive behind one. `bus_found` in `sysinfo` is how many the
 * board found, so a list this cuts short can say so.
 */
#define BUS_DEVICES_MAX 64

/*
 * What one processor has been doing, since boot.
 *
 * A declared shape rather than two parallel arrays, because the two numbers
 * are one fact about one core and splitting them would be an invitation to
 * read `idle[i]` against `busy[j]`.
 */
struct cpuload {
    uint64_t idle_ticks;
    uint64_t busy_ticks;

    /*
     * Busy split by whose it was - a thread's own code, or the kernel's
     * work for it - in the **counter's** units, `counter_hz` a second, and
     * named for them: these are measured at each crossing, not counted in
     * scheduler ticks like the two above (`roadmap.md` 5zx).
     */
    uint64_t user_counter;
    uint64_t kernel_counter;
};

/*
 * How many processors this interface can describe.
 *
 * Not how many there are - that is `cpu_count`, and `NR_CPUS` in
 * `kernel/percpu.h` is what the kernel was built for. This is the size of
 * the room, and it is bigger than either because a struct that crosses to
 * userland cannot grow without everything on both sides being rebuilt.
 */
#define CPUS_MAX        32

struct sysinfo {
    /*
     * The processor, raw, and what to make of it.
     *
     * `arch/<name>/cpu.c` decodes the same words for the boot log and
     * userland decodes them again for /dev/cpu, because the two want
     * different amounts of detail and neither should constrain the other.
     * What each word *is* depends on `cpu_arch`, and each architecture's
     * cpu.h names the indices.
     */
    uint32_t cpu_arch;
    uint32_t cpu_words;                 /* how many of the raw ones are set */
    uint64_t cpu_raw[CPU_RAW_WORDS];
    uint64_t counter_hz;

    /* Memory, in pages of PAGE_SIZE. */
    uint64_t ram_base;
    uint64_t ram_size;
    uint32_t pages_total;
    uint32_t pages_free;

    /* The fixed pools, and how full they are. Both halves matter: "3
     * processes" says nothing without "of 8". */
    uint32_t threads_used;
    uint32_t threads_total;
    uint32_t processes_used;    /* running */
    uint32_t processes_held;    /* slots occupied, including unreaped exits */
    uint32_t processes_total;
    uint32_t endpoints_used;
    uint32_t endpoints_total;
    uint32_t spaces_used;
    uint32_t spaces_total;

    /*
     * Shared regions. `memobj_in_use` and `memobj_total` have existed since
     * regions did and nothing ever called them, so the pool's depth was
     * invisible - and "a region could not be allocated" is the same message
     * whether the machine is out of memory or out of *descriptors*, which
     * are very different problems. Two rounds of debugging went to the wrong
     * layer for want of this number.
     */
    uint32_t regions_used;
    uint32_t regions_total;

    /*
     * What the board's bus enumeration found, and which of it is driven.
     *
     * Carried here rather than behind a syscall of its own because this is
     * the kernel describing the machine, which is what this struct is, and
     * because it is read once by a window rather than in a loop.
     *
     * `claimed` is the whole point. Presence alone cannot distinguish a
     * machine with no sound card from one whose card nothing drives, and
     * every other field here answers only presence.
     */
    uint32_t bus_count;
    struct bus_device bus[BUS_DEVICES_MAX];

    /* Devices. Zero width means there is no display. */
    uint32_t screen_width;
    uint32_t screen_height;
    uint32_t screen_pitch;
    uint32_t has_keyboard;

    /* Ticks charged to the idle thread and to everything else, since boot,
     * **summed over every processor**. Both only rise; a percentage is the
     * difference between two readings, which is the only kind that can mean
     * "recently" rather than "ever".
     *
     * Kept as the machine's total because four programs read them and a
     * total is what those four want. The per-processor split is below. */
    uint64_t idle_ticks;
    uint64_t busy_ticks;

    /*
     * And the same, per processor.
     *
     * **A total cannot answer the question SMP raises.** One core pinned
     * and three asleep is 25% busy by the sum, which is true and useless:
     * it is indistinguishable from four cores at a quarter each, and those
     * are opposite situations - the first is a machine that cannot use
     * itself and the second is one that is.
     *
     * `cpus` above says how many entries are filled - it is the same
     * number and there is no second one. `CPUS_MAX` is the *interface*
     * limit and is larger on purpose: this struct is an ABI, so the room
     * has to exist before the cores do, and the largest machine
     * `docs/targets.md` names has twenty hardware threads.
     */
    struct cpuload cpu[CPUS_MAX];

    /*
     * Seconds since 1970, from the board's clock, or 0 when it has none.
     *
     * Not a tick count, and it is the only number in here that is not.
     * Everything else says how long this machine has been doing something;
     * this says what time it is, which no counter since boot can answer -
     * a file written before the last reboot has an mtime of `sys.ticks()`
     * and that number means nothing at all across a restart.
     *
     * Read fresh on every call rather than latched at boot and added to,
     * because a load from an MMIO register is cheaper than being wrong when
     * the two drift.
     */
    uint64_t epoch;

    /*
     * The sound device's format, or zeroes when there is none.
     *
     * Reported rather than defined in a header both sides include, because
     * it is the *board's* fact: `hal.h` fixes it and a different board
     * would fix it differently. A caller sizes its buffer from what the
     * machine says it is rather than from a constant that might be stale.
     */
    uint32_t audio_rate;
    uint32_t audio_channels;
    uint32_t audio_period;      /* bytes in one period */
    uint32_t audio_periods;     /* how many the device will hold */

    /*
     * The card's MTU, or 0 when there is none.
     *
     * Here rather than only in `SYS_NET_INFO` because "is there a card" and
     * "give me the card" are different questions with different answers.
     * `SYS_NET_INFO` is owner-only and has to be - a MAC is an identity, and
     * a process that can read frames can read everybody's. Whether the
     * machine has a network at all is a fact about the machine, which is
     * what this whole struct is for, and init has to know it before it can
     * decide who to hand the card to.
     *
     * The same arrangement `audio_period` has, and it exists for the same
     * reason: `may_pass_audio` needed to ask without holding the device.
     */
    uint32_t net_mtu;
    uint32_t audio_dry;         /* periods that arrived at an empty device */
    uint32_t audio_floor;       /* smallest depth ever seen, in periods */
    uint32_t audio_wakes;       /* times the device raised its interrupt */

    /*
     * Three counts of processors, because there are three questions.
     *
     * **They are not interchangeable and collapsing any two of them has
     * already been a bug twice.** On this machine today they read 1, 4 and
     * 4, and each gap says something different about how far
     * `docs/smp.md` has got.
     *
     *   `cpus`          how many run *threads*. One: a secondary has an
     *                   idle thread and no runqueue to take work from, so
     *                   nothing can be scheduled onto it.
     *   `cpus_online`   how many are running kernel code and taking their
     *                   own timer interrupt. Four. This is the bound on
     *                   `cpu[]` below - every one of them charges its own
     *                   idle or busy tick, so every one has a real reading.
     *   `cpus_present`  how many the firmware says the machine has. Four.
     *                   The gap above this one is what is left to build.
     *
     * `cpus` was `NR_CPUS` once and reported four cores scheduling with
     * three of them parked in `wfi`; `cpus_online` did not exist, so a core
     * that had started and died was indistinguishable from one that was
     * simply not being asked to do anything. Both were the same mistake -
     * one number standing in for two facts.
     */
    uint32_t cpus;
    uint32_t cpus_online;
    uint32_t cpus_present;
    uint32_t tick_hz;
    uint32_t current_el;
    uint32_t page_size;

    /*
     * What the firmware says the machine is, from `hal_machine_ident`: three
     * names, empty when it did not say, and where they were read - or, when
     * they are empty, why. Each is terminated inside its field. What to make
     * of them is the caller's: this is the manufacturer's own spelling.
     *
     * Here because neofetch printed "QEMU q35 x86-64" as the Host of a
     * ThinkPad, from a string compiled into the image.
     */
    char machine_vendor[64];
    char machine_product[64];
    char machine_version[64];
    char machine_source[64];

    /*
     * How many devices the bus enumeration found. The same number as
     * `bus_count` until there are more than `bus` holds; then this is all of
     * them, and `bus_count` is how many are in the array.
     */
    uint32_t bus_found;

    /*
     * Where the screen's pixels come from, in the board's own words - the
     * line `hal_fb_describe` gives the boot log - and empty when there is no
     * screen. Terminated inside its field.
     *
     * Here because This Machine said "ramfb" on a ThinkPad, whose screen is
     * the loader's: the word was written into the program.
     */
    char screen_source[128];

    /*
     * The counter's reading at the boot log's zero: `sys.ticks()`'s counter,
     * which runs from power-on, when the kernel's first stamped line was
     * written. A moment taken with `sys.ticks()` is `(t - log_origin) /
     * counter_hz` seconds in the log's own time, which is how `diskinfo` puts
     * the disk server's wait for a stick beside the driver's lines. Zero if
     * the log was never stamped from the counter.
     */
    uint64_t log_origin;

    /*
     * The battery, as the board last read it (`hal_battery_read`): all
     * zeroes when `battery_known` is 0, which is a board that reads none.
     * A copy of a cached reading, so asking costs nothing at the hardware.
     */
    uint32_t battery_known;
    uint32_t battery_present;
    uint32_t battery_charging;
    uint32_t battery_discharging;
    uint32_t battery_on_ac;
    uint32_t battery_critical;
    uint32_t battery_percent;
};

/*
 * What SYS_NET_INFO answers.
 *
 * The MAC is six bytes and a byte array rather than a number, because it is
 * an address rather than a quantity: there is no arithmetic anybody should
 * do on it, and the order it goes on the wire is the order it is written.
 */
struct netinfo {
    uint8_t  mac[6];
    uint16_t present;               /* 0 when there is no card */
    uint32_t mtu;
};

/*
 * The largest frame any board here carries, so userland can size a buffer
 * without including the HAL. The same arrangement as
 * `HAL_SND_PERIOD_BYTES_MAX`: a number the ABI repeats, where `hal.h` holds
 * the board's real one and `sys.net()` reports the MTU the card actually
 * has. They agree today because there is one board.
 */
#define NET_FRAME_MAX  1514u

/*
 * What SYS_DISK_INFO answers.
 *
 * `sectors` is zero when there is no disk, which is a supported way to run:
 * the machine boots, and the filesystem server says it has nothing to mount
 * rather than the kernel refusing to start.
 */
struct diskinfo {
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t most;          /* the most bytes one SYS_DISK_READ or _WRITE moves */
};

#endif

/* Errors are negative so `if (result < 0)` reads correctly on both sides. */
#define SYS_ERR_BADCALL   (-100)    /* no such syscall number */
#define SYS_ERR_FAULT     (-101)    /* a pointer the process may not touch */
#define SYS_ERR_DENIED    (-102)    /* this process does not hold the device */
#define SYS_NO_INPUT      (-103)    /* nothing waiting; not an error */
#define SYS_ERR_NO_CHILD  (-104)    /* nothing to wait for */
#define SYS_NO_CHILD_READY (-106)   /* children, but none has exited yet */
#define SYS_NO_MESSAGE    (-107)    /* nothing to receive, and not blocking */
#define SYS_ERR_NO_ROOM   (-105)    /* out of processes, or out of memory */
#define SYS_ERR_NO_CAPS   (-109)    /* this thread's capability table is full */
#define SYS_ERR_NO_DEVICE (-108)    /* this machine has nothing of that kind */
#define SYS_NO_INTERRUPT  (-110)    /* a timed interrupt wait ran out; not an error */

/*
 * Everything above is plain preprocessor because user programs written in
 * assembly include this header for the numbers. Anything below would be a
 * stream of unknown mnemonics to the assembler.
 */
#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * **No two result codes share a value, and the compiler is what says so.**
 *
 * `SYS_NO_CHILD_READY` and `SYS_ERR_NO_CAPS` were both -106 for ten days.
 * The second was added to a list that is not in numeric order, and a caller
 * got one number for "none of your children has finished" and "your
 * capability table is full". Nothing noticed, because no single syscall
 * returns both - which was luck rather than design.
 *
 * Two case labels with one value are a constraint violation in C11, so this
 * switch stops the build the moment two codes agree, and the message names
 * both. It is never called and generates nothing. **A new code goes in here
 * as well as above**, and that is the one part no compiler can check: the
 * assembler reads the `#define`s, so they cannot be an enum that lists
 * itself.
 */
static inline void sys_result_codes_are_distinct(long result)
{
    switch (result) {
    case SYS_ERR_BADCALL:
    case SYS_ERR_FAULT:
    case SYS_ERR_DENIED:
    case SYS_NO_INPUT:
    case SYS_ERR_NO_CHILD:
    case SYS_ERR_NO_ROOM:
    case SYS_NO_CHILD_READY:
    case SYS_NO_MESSAGE:
    case SYS_ERR_NO_CAPS:
    case SYS_ERR_NO_DEVICE:
    case SYS_NO_INTERRUPT:
    default:
        break;
    }
}

/*
 * What a syscall carries, in the one shape every architecture can fill.
 *
 * This used to be `struct trapframe *` and the dispatcher read `tf->x[0]`
 * through `tf->x[4]` for the arguments and `tf->x[8]` for the number - which
 * meant the largest file in the kernel named AArch64 registers ninety-six
 * times. Nothing else in `kernel/` did anything of the kind: there is no
 * assembly here and never was, and those ninety-six were most of the
 * distance between that and being portable.
 *
 * Five arguments because that is what fits, not because anything counted:
 * AArch64 passes them in x0-x4 with the number in x8, and System V on
 * AMD64 in rdi, rsi, rdx, r10, r8 with the number in rax. The trap handler
 * fills this from whichever those are and copies `result` back out, so the
 * register file stays a fact about the machine and the protocol stays a
 * fact about the system.
 *
 * `unsigned long` rather than `uint64_t`: it is a machine word, and that is
 * the type that says so.
 */
struct syscall_frame {
    unsigned long number;
    unsigned long arg[5];
    unsigned long result;
};

/* Called from the trap handler on a syscall from user level. Writes into
 * `result`, which the handler puts back wherever that machine returns a
 * value. */
void syscall_dispatch(struct syscall_frame *sc);

/*
 * What `SYS_DEV_FIND` answers.
 *
 * A kind names a programming model *and* a job, because the driver needs
 * both: `DEV_PL061_POWER_KEY` is "an ARM PL061 GPIO controller, and `line`
 * is its input wired to the power key". Fixed-width and padded to a whole
 * number of words, since it crosses from the kernel to a process byte for
 * byte.
 */
#define DEV_PL061_POWER_KEY  1u

/*
 * An xHCI USB host controller: `base` and `size` are its first BAR, `intid`
 * the interrupt it raises, and `where` its address on PCI.
 */
#define DEV_XHCI             2u

/*
 * The Intel display engine's backlight: `base` is the page holding its two
 * PWM controllers' registers, inside the graphics device's first BAR, and
 * `where` is that device on PCI. Arrived with the ThinkPad's brightness,
 * which the firmware leaves to a graphics driver (`docs/thinkpad.md` 8b).
 */
#define DEV_INTEL_BACKLIGHT  3u

/*
 * An Intel Ethernet controller: `base` and `size` are its first BAR, `intid`
 * the interrupt it raises, `where` its address on PCI, and `line` its device
 * identifier - which the driver does not need to work and does need to say,
 * since one register set covers an I219 in a ThinkCentre and the 82540EM and
 * 82574L an emulator offers (`roadmap.md` 5zd-f).
 */
#define DEV_INTEL_ETHERNET   4u

struct dev_info {
    uint32_t kind;
    uint32_t intid;         /* for SYS_IRQ_CLAIM */
    uint32_t line;          /* which input on the device, where that matters */
    uint32_t where;         /* PCI bus << 8 | slot << 3 | function, or 0 */
    uint64_t base;          /* for SYS_DEV_MAP */
    uint64_t size;
    uint32_t id;            /* PCI vendor << 16 | device, or 0 off PCI */
    uint32_t reserved;
};

#endif /* !__ASSEMBLER__ */

#endif /* KERNEL_SYSCALL_H */
