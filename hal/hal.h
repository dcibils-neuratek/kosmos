/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_H
#define HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * What the board provides. One implementation per board, under hal/<board>/.
 *
 * `arch/` is "which CPU are you" and is reimplemented per architecture.
 * `hal/` is "which peripheral do you have" and is one interface with several
 * implementations, because pushing out a character is pushing out a
 * character on any board.
 *
 * Deliberately only what has been needed. Each of these arrived with the
 * milestone that needed it - the interrupt controller and the timer at M1,
 * the display at M6 - and none of it was written ahead of a caller.
 *
 * It still takes its real shape at M2's second half, once there is a second
 * target to compare against. An interface written against a single target is
 * that target's shape wearing generic names, and everything below is written
 * knowing that.
 */

/* The minimum required to have output. Called before anything else. */
void hal_early_init(void);

/*
 * What this board is, in the words the boot log prints.
 *
 * **These exist because the log was lying.** It said "PL011 UART at
 * 0x09000000, polled" and "250 Hz off the generic timer through a GICv3" on
 * a PC, which has none of those - the sentences were written when there was
 * one board and were as much a part of the machine as the addresses in
 * them. `kernel/main.c` printing a string literal about hardware is the
 * same mistake as `kernel/process.c` decoding a page descriptor, one layer
 * up and in prose.
 *
 * The division is between the *why* and the *fact*. Why a stage exists is
 * about the design and stays in the kernel; what this particular machine
 * turned out to be is the board's to say, and the architecture's for the
 * two in `arch/`.
 *
 * Every one of these returns a sentence fragment with no full stop and no
 * newline, because the log decides its own punctuation.
 */
const char *hal_console_describe(void);     /* "PL011 UART at 0x09000000, polled" */
const char *hal_timer_describe(void);       /* "the generic timer through a GICv3" */
const char *hal_input_describe(void);       /* why the input scan looks as it does */

/* One character out the serial port. Blocks until there is room. */
void hal_putchar(char c);

/* One character in, or HAL_NO_INPUT when none is waiting. Never blocks:
 * a REPL has to stay responsive to a timer tick while it waits, and from M6
 * input has to keep flowing while an application is busy drawing. */
#define HAL_NO_INPUT    (-1)

int hal_getchar(void);

/* Where usable RAM is. One contiguous range is enough for every target so
 * far; a board with holes in its map would need a list, and that is the
 * moment to change this, not before. */
struct memrange {
    unsigned long base;
    unsigned long size;
};

void hal_ram_range(struct memrange *out);

/*
 * **Every usable range the board found, not merely the best one.**
 *
 * `hal_ram_range` answers with the range the kernel was loaded into,
 * because that is the one the page allocator can put its bitmap in. This
 * answers with all of them, so the allocator can *manage* the rest.
 *
 * A PC with four gigabytes or more does not have one block of memory: the
 * PCI hole splits it, a piece below and a larger piece above four
 * gigabytes. Until this existed the far piece was counted and unused, and
 * Diego's ThinkCentre ran on about three of its eight (`roadmap.md`
 * 5zd-d).
 *
 * Ranges come back in address order, none below one megabyte - the first
 * megabyte is the firmware's, the real-mode trampoline's and the BIOS
 * area's, and `hal_low_region` is how anything asks for that. Returns how
 * many were written, which is never more than `max`; a board that has one
 * range answers one, and the answer is the same range `hal_ram_range`
 * gives.
 */
unsigned hal_ram_ranges(struct memrange *out, unsigned max);

/*
 * Whether the board is reporting less RAM than the machine has, and how
 * much there really is.
 *
 * **A separate question because the answer is not a failure.** A kernel
 * that identity maps RAM below the region it gives processes cannot
 * describe a machine with more memory than that region starts at - which
 * is every laptop built this decade - and the choice is between refusing
 * to boot and running on the part it can reach. It runs, and this is how
 * it says so: `hal_ram_range` stays the truth about what may be used, and
 * this is the truth about what was there.
 *
 * A total rather than a range, because what was given up is usually
 * several regions and no single one of them is the answer.
 *
 * False on a board that is reporting everything, which is the ordinary
 * case and the one where there is nothing to say.
 */
bool hal_ram_capped(unsigned long *whole_bytes);

/*
 * A disk the loader left in memory beside the kernel, if it left one:
 * where it is and how long. The kernel prints it among the memory facts,
 * because on a machine with no serial port the boot screen is the only
 * place to learn whether the loader handed one over. False on a board
 * whose loader has no way to.
 */
bool hal_loader_disk(unsigned long *base, unsigned long *bytes);

/*
 * **What the loader said about memory, whole, and not only the part this
 * kernel adopted.**
 *
 * `hal_ram_range` answers "which region do I manage" and is the only thing
 * any decision is made from. This answers a different question - "what does
 * the firmware say is at this address" - and it exists because there was no
 * way to ask it. The userland image sits below every region the allocator
 * manages, no process can write a byte of it, and on the ThinkPad it is
 * corrupted anyway; the first thing anyone would want is the rest of the
 * map, and a laptop with no serial port has only the boot screen to print
 * it on.
 *
 * `type` is the loader's own number, passed through rather than translated:
 * 1 is usable and everything else is the firmware's business, and inventing
 * names for entries this kernel does not act on would be inventing meaning.
 *
 * False past the last entry, and false on the first for a board whose loader
 * hands over no map at all - which is `virt`, started with `-kernel`.
 */
bool hal_memory_entry(unsigned i, unsigned long *base, unsigned long *length,
                      unsigned *type);

/*
 * How many entries can be read, and through `seen` how many there were.
 *
 * The two differ when the board kept fewer than the loader listed, and the
 * difference is the whole reason this is separate: the first cap chosen was
 * twenty-four, OVMF's map is *exactly* twenty-four, and a full array and a
 * map that happens to end are indistinguishable without this. Zero on a
 * board with no map.
 */
unsigned hal_memory_entries(unsigned *seen);

/*
 * How many processors this machine has - not how many are being used.
 *
 * `sysinfo.cpus` is the second number and is `NR_CPUS`, which is 1. This is
 * the first, and the gap between them is the honest measure of how far
 * `docs/smp.md` has got.
 *
 * Each board answers however it can. AArch64 asks PSCI about processor 0,
 * 1, 2 until the firmware says there is no such thing, which needs no
 * device tree. x86-64 reads the ACPI MADT, one entry per local APIC -
 * `hal/pc/acpi.c` - and falls back to one on a machine whose firmware left
 * no tables.
 *
 * **Counting is not starting on either board.** The PC can say twelve and
 * still schedule on one, because `cpu_on.c` wants a local APIC and
 * `cpu_secondary_entry` wants a trampoline below 1 MB, and neither exists.
 * That gap is the honest measure of how far the port has got, which is
 * exactly what the paragraph above says this number is for.
 *
 * **A count is the right question on these two machines and the wrong one
 * on the next.** Alder Lake and everything after it are *hybrid*: the
 * Alienware in `docs/targets.md` has six performance cores, eight
 * efficiency cores and twenty hardware threads, and a scheduler told only
 * "twenty" will put the compositor on an efficiency core. This stays a
 * count until there is a board that can answer better, and `docs/smp.md`
 * records what the shape has to become.
 */
unsigned hal_cpu_count(void);

/*
 * Start a processor at `entry`, with `context` waiting for it in the first
 * argument register.
 *
 * **The address is physical.** The core this starts has no translation on
 * yet and will turn it on itself, with the tables this one already built -
 * so it is handed where the code *is* rather than where this core sees it.
 *
 * False when the firmware refused, which on a board that cannot do it at
 * all is the honest answer rather than a hang.
 */
bool hal_cpu_on(unsigned cpu, uintptr_t entry, unsigned long context);

/* The interrupt controller. hal_irq_handle() is called from the IRQ vector:
 * it acknowledges, services and signals end-of-interrupt. */
void hal_irq_init(void);

/*
 * The half of the two above that belongs to *this* processor.
 *
 * **A machine has one interrupt controller and one clock; a processor has
 * its own interface to both.** On AArch64 that is this core's GIC
 * redistributor and its four `ICC_*` system registers, and its own generic
 * timer - CNTP_CVAL_EL0 and CNTP_CTL_EL0 are banked per core by
 * architecture, so every core arms its own comparator and receives its own
 * PPI. None of that can be done for a core by another core.
 *
 * `hal_irq_init` and `hal_timer_init` still do the machine's part and then
 * call these for the processor that runs them, so core zero's boot is
 * unchanged and a board with one processor never sees the difference.
 * `kernel/smp.c` calls them on each secondary as it arrives.
 *
 * Separate from the two above rather than folded into them because a board
 * can honestly implement one and not the other, which is the same split
 * `hal_cpu_count` and `hal_cpu_on` already make - and `hal/pc/` implements
 * neither, because a PC's per-core interrupt controller is the local APIC
 * and there is no driver for it.
 */
void hal_irq_init_here(void);
void hal_timer_init_here(void);

/*
 * Interrupt another processor, so that it looks at its runqueue now rather
 * than at its next tick.
 *
 * **There is no message.** The caller has already put a thread where the
 * target will find it; this is only the poke that makes it look. A core in
 * `wfi` wakes on any interrupt and runs its exception epilogue, which is
 * where the scheduler decides - so the handler for this is deliberately
 * empty and the interruption is the entire content.
 *
 * Without it a cross-core wake is not lost but *late*, by up to one tick.
 * That is nothing for a background thread and everything for IPC, where a
 * single shell command is dozens of round trips: four milliseconds each
 * would make four processors slower than one.
 *
 * A board that cannot do this does nothing, and the system still works -
 * more slowly, and only for threads that live on another core. `hal/pc/`
 * is such a board, because a PC's answer is the local APIC.
 */
void hal_cpu_wake(unsigned cpu);
/*
 * Serves whatever interrupt arrived, and answers **whether that was the
 * tick**.
 *
 * The board is the only thing that knows: `arch/` sees one vector for every
 * hardware interrupt, on purpose, so that it does not have to know what a
 * PIC or a GIC is.
 *
 * **Both trap handlers assumed the answer was always yes**, and both said
 * so in a comment that named the day it would stop being true - "the timer
 * is the only interrupt source, so every IRQ is a tick; when there is a
 * second, hal_irq_handle has to say which one fired rather than this
 * assuming." There is a second, and a third: a keyboard, a sound controller
 * asking for a period 172 times a second, a network card. Every one of them
 * was charging the scheduler a tick it had not spent - shortening quanta,
 * inflating the busy and idle counts every processor meter reads, and
 * running a full scan of the thread table for sleepers whose deadlines had
 * not moved.
 */
bool hal_irq_handle(void);

/*
 * **The board's half of an interrupt a driver owns.**
 *
 * `kernel/irq.c` keeps the claims and does the waking; what it cannot know
 * is which numbers this machine spends on itself and how this controller
 * masks one. Both are facts about the board.
 *
 * `hal_irq_available` is the safety half: the timer and the inter-processor
 * interrupt are the kernel's, and a process that could claim the timer could
 * stop the machine scheduling. Asked rather than kept as a list in the
 * kernel, because only the board knows which numbers it uses.
 *
 * `hal_irq_set_masked` is what makes a level-triggered source survivable. It
 * is still asserted when the handler returns, so it must be masked there and
 * unmasked once the driver has quietened the device - otherwise the
 * controller delivers it again immediately and the driver, which is a
 * process, never gets a turn.
 *
 * **On a source that is not a line - an MSI - both directions do nothing,
 * and that is correct rather than missing**: the device wrote to the local
 * APIC, there is no redirection entry, and nothing is asserted afterwards.
 */
bool hal_irq_available(unsigned intid);
void hal_irq_set_masked(unsigned intid, bool masked);

/*
 * **Where a device is, for a driver that is not in the kernel.**
 *
 * A driver in a process needs three facts about its hardware - where its
 * registers are, how big the window is, and which interrupt it raises - and
 * `CLAUDE.md` is exact about where facts like that live: *no hardware
 * addresses outside `hal/`. Not one.* The init process is userland too, so it
 * cannot be the one to know them either. The board answers, the kernel
 * passes the answer to a process holding device authority, and the address
 * appears in userland only as a value a driver was handed.
 *
 * A `kind` names a programming model *and* the job, because both matter to
 * the driver: "a PL061 GPIO controller, whose `line` is the one wired to the
 * power key" is what the power-button driver needs to be told. `index`
 * counts the devices of one kind from zero, and the answer is false past the
 * last - or at once, when this board has nothing of that kind.
 *
 * Not speculative, which is the test `CLAUDE.md` sets for the HAL: it
 * arrived with the driver that calls it.
 */
#define HAL_DEV_PL061_POWER_KEY  1u
#define HAL_DEV_XHCI             2u     /* a USB host controller, on PCI */
#define HAL_DEV_INTEL_BACKLIGHT  3u     /* the page of Intel's backlight PWMs */
#define HAL_DEV_INTEL_ETHERNET   4u     /* an Intel Ethernet controller, on PCI */

struct hal_device {
    unsigned long base;
    unsigned long size;
    unsigned      intid;
    unsigned      line;         /* which input on it, where that matters */
    unsigned      where;        /* PCI bus << 8 | slot << 3 | function */
};

bool hal_device_find(unsigned kind, unsigned index, struct hal_device *out);

/*
 * **What the firmware says this machine is.**
 *
 * The manufacturer, the product and the version as the firmware wrote them,
 * each terminated and cut to fit - SMBIOS's System Information on a PC.
 * `neofetch`, About and `machine` print them where they used to print the
 * Makefile's platform string, which named QEMU on every PC the image booted,
 * a ThinkPad among them. That string is what the image was built *for*; this
 * is what it is running *on*.
 *
 * `source` says where the names were read and, when this returns false, why
 * there are none: no table found, or a board that does not look. False leaves
 * the names empty. Nothing past the strings is decoded, which is
 * `hal_bus_scan`'s rule - what a name means is userland's business.
 *
 * Captured once, early in boot while firmware memory is still mapped, so this
 * answers from a copy and may be asked from anywhere.
 *
 * Not speculative: it arrived with four callers and both boards answer it.
 */
#define HAL_MACHINE_TEXT 64

struct hal_machine {
    char vendor[HAL_MACHINE_TEXT];
    char product[HAL_MACHINE_TEXT];
    char version[HAL_MACHINE_TEXT];
    char source[HAL_MACHINE_TEXT];
};

bool hal_machine_ident(struct hal_machine *out);

/*
 * **The firmware's own account of the machine's hardware**: ACPI's DSDT and
 * SSDTs, as bytes. They are AML - the code a firmware writes about its own
 * devices, and where a laptop says how its backlight is set and where its
 * battery is read. This kernel runs none of it (`hal/pc/acpi.h`: no AML); it
 * hands the bytes up, so `acpi` can save them for `iasl` to read on another
 * machine. The ThinkPad's brightness keys start there.
 *
 * `hal_firmware_init` maps them, once, at boot and after the MMU is on: they
 * were found by the walk that counted the processors, in memory the firmware
 * kept and nothing here allocates from. It answers how many there are - zero
 * on a board with no ACPI, which is `virt`. `hal_firmware_table` gives the
 * n-th from zero, `bytes` in the kernel's own mapping; false past the last.
 *
 * **Only AML, deliberately.** Other tables can carry what is nobody's
 * business - MSDM holds a Windows licence key - and nothing needs them; the
 * tables kept are the two kinds that describe hardware.
 */
struct hal_firmware_table {
    char           signature[4];
    uint32_t       length;
    const uint8_t *bytes;
};

unsigned hal_firmware_init(void);
bool     hal_firmware_table(unsigned index, struct hal_firmware_table *out);

/*
 * Which interrupt controller this machine turned out to have.
 *
 * A board with one answer returns a constant; a PC has two and chooses at
 * boot - see `hal/pc/irq_bind.c`. In the boot log for `hal_fb_describe`'s
 * reason: "no tick" looks the same whether the controller is missing, the
 * line is routed to an input nothing is on, or the timer never counted.
 */
const char *hal_irq_describe(void);

/* The tick source. hal_timer_init needs hal_irq_init first. */
void hal_timer_init(unsigned hz);
unsigned long hal_ticks(void);

/*
 * How many deadlines have been missed: the timer came due again before the
 * previous interrupt was serviced, so the tick for it never happened.
 *
 * It means the system fell behind, and it is the difference between a tick
 * count and a clock. Under load the two diverge, and anything measuring
 * elapsed time from ticks is measuring something else. The counter is the
 * clock; ticks are a heartbeat.
 */
unsigned long hal_ticks_missed(void);

/*
 * Another processor's tick count, or 0 for one that does not exist.
 *
 * `hal_ticks` answers for the core that asks, which is what everything above
 * wants. This is the one question only a machine with more than one
 * processor can ask, and it exists because nothing else can tell a secondary
 * that is alive and taking interrupts from one that started, parked, and
 * quietly died.
 */
unsigned long hal_ticks_on(unsigned cpu);

/*
 * The display.
 *
 * A linear framebuffer and nothing else. What is above this line is the app
 * server's problem, and `gfx.md` is emphatic that it stays that way: the
 * kernel does not know what a window is, and the board does not know what a
 * pixel means beyond its format.
 *
 * `pitch` is bytes per row and is **not** width * 4. Under QEMU this board
 * chooses the stride itself and deliberately pads it, because a display that
 * hands back exactly width * 4 lets every address calculation in the system
 * be written wrong and still work - right up until the first real board,
 * where the pitch is whatever the firmware felt like. `gfx.md` §19.3 puts
 * this at the top of its list of traps.
 *
 * One format, XRGB8888: a uint32_t per pixel, 0x00RRGGBB. Not negotiable
 * from up here. When a board arrives that cannot do it, that is the moment
 * the interface grows a format field, and not before.
 */
struct fb {
    volatile uint32_t *pixels;

    /*
     * The same pixels, as the address the *hardware* knows them by.
     *
     * **Not the same number, and assuming it was cost an afternoon.** For
     * as long as the only framebuffer was ramfb the two were identical:
     * the guest allocates those pixels out of its own RAM and the kernel
     * is identity mapped, so a pointer to them *is* their physical
     * address. A firmware framebuffer is not in RAM at all - it is behind
     * a graphics aperture - so it has to be mapped, and from that moment
     * `pixels` is a kernel virtual address in the device window.
     *
     * `process_grant_screen` hands the compositor the pages the display
     * controller is scanning out, and a page table wants physical
     * addresses. It read `pixels`, which had been right for two years, and
     * mapped the process onto whatever RAM happened to live at 0x3xxxxxxx:
     * the window manager came up, drew a whole desktop into memory nobody
     * was looking at, and the console stayed on the screen underneath it.
     *
     * A board sets both. Where the pixels are in RAM they are the same
     * value, which is what made the bug invisible.
     */
    uintptr_t phys;

    unsigned width;
    unsigned height;
    unsigned pitch;             /* bytes per row; never assume width * 4 */
};

/*
 * Brings the display up and describes it. False when there is none, which
 * is not an error: a serial-only boot is a legitimate way to run, and the
 * system has to keep working without a screen.
 *
 * The board decides where the pixels live, because that is the one thing the
 * two targets disagree about. Under QEMU the guest points ramfb at memory it
 * chose; on the Pi the firmware answers the mailbox with an address it chose.
 * A caller that supplied the memory would be right on one board and wrong on
 * the other.
 */
bool hal_fb_init(struct fb *out);

/*
 * The same screen, asked for before there is a page allocator.
 *
 * **This exists because a laptop has no serial port.** The boot log reaches
 * the screen from stage six, and the three faults that stood between this
 * kernel and its first real machine were at stages three, four and five -
 * each of them, on that machine, a black panel with nothing to read. A
 * board that can answer this turns every one of them into a log you can
 * watch stop.
 *
 * It can only be answered where the pixels are reachable *before* the
 * kernel builds its own address space: a framebuffer the firmware set up,
 * which the boot page tables cover or the board can add to them - on a PC
 * that includes a screen above four gigabytes, where the ThinkPad's firmware
 * puts it, and the early screen was dark there until that was said. ramfb cannot - the guest
 * allocates those pixels and there is nothing to allocate from yet - so
 * `qemu-virt` says no and loses nothing, having a serial port and a cable
 * already attached to it.
 *
 * **The address it gives is not the address `hal_fb_init` will give**, and
 * that is the whole subtlety. The identity map that makes this possible is
 * replaced by `mmu_init`, after which the same pixels answer at a different
 * virtual address - so a board that says yes here expects
 * `console_rebase_screen` afterwards, and the console must not repaint,
 * because what is on the screen is in the memory both addresses name.
 */
bool hal_fb_early(struct fb *out);

/*
 * The same framebuffer, at the address it answers on once the kernel has
 * built its own map. Only meaningful where `hal_fb_early` said yes.
 *
 * **It must be asked the instant `mmu_init` returns, before anything is
 * printed.** The identity map that the early screen used is gone from that
 * instruction onward, and the console is still pointing into it: the next
 * line of the boot log faults inside a console write, the fault handler
 * blocks on the lock that write is holding, and the machine says `spinlock:
 * console held by 0, wanted by 0` for ever. That is not a hypothetical - it
 * is what the first version of the early screen did.
 *
 * It does not clear. The pixels are the ones already on the screen.
 */
bool hal_fb_remap(struct fb *out);

/* Which framebuffer answered, for the boot log. See `hal/pc/fb.c`. */
const char *hal_fb_describe(void);

/*
 * Brings up a keyboard, if the board has one. False is not an error: input
 * arriving over the serial line is how this system ran until M6 and is how
 * it runs on a board with a cable and no keyboard.
 *
 * There is deliberately no hal_keyboard_getchar. A keyboard is a source of
 * characters and `hal_getchar` is where characters come from, so the board
 * answers from whichever of its sources has one. Nothing above the HAL
 * changes because a keyboard exists.
 */
bool hal_keyboard_init(void);

/*
 * What the keyboard and the pointer turned out to be, in the words the boot
 * log uses.
 *
 * **The kernel had these as string literals and they went stale the moment a
 * second driver existed.** `kernel/main.c` said "virtio-input, negotiated
 * and polled like the serial line" on a machine whose keyboard is an i8042 -
 * a boot log that names the wrong driver is worse than one that says
 * nothing, because it is the first thing anybody reads when a board does not
 * work.
 *
 * A prefix rather than a whole sentence: the kernel appends the range the
 * pointer actually reported, which is a fact it gets by asking rather than
 * from a board.
 */
const char *hal_keyboard_describe(void);
const char *hal_pointer_describe(void);

/*
 * Where the pointer is, in the device's own units.
 *
 * Undecoded on purpose. An absolute pointing device reports in a range of
 * its own choosing - QEMU's tablet is 0 to 32767 on both axes whatever the
 * display happens to be - and the range travels with the position so that
 * whoever knows how big the screen is can do the scaling. The same division
 * `hal_ram_range` and `sysinfo` draw: this layer says what the hardware
 * said, and what it means belongs further up.
 */
struct pointer_state {
    uint32_t x, y;
    uint32_t min_x, max_x;
    uint32_t min_y, max_y;
    uint32_t buttons;               /* bit 0 left, bit 1 right */
    uint32_t moved;                 /* something happened since the last look */

    /*
     * **The wheel: notches turned since the last look**, positive away
     * from the person - which is up, and scrolls a list back towards its
     * start - and negative towards them. Counted rather than held, because
     * a wheel has no position: two notches between looks are two notches
     * however quickly they came (`roadmap.md` 5zv). Reading resets it.
     */
    int32_t  wheel;
};

/*
 * One button transition, and where the pointer was when it happened.
 *
 * **A position is a state and a button is an event**, which is the whole
 * of why this exists beside `pointer_state` rather than inside it. Asking
 * where the pointer is has one right answer - now - and asking what the
 * buttons did has as many as happened since the last time anyone asked.
 * A reader that only ever sampled the state lost every click that began
 * and ended between two samples, which is a bug a real machine found and
 * QEMU never did (`hal/pointer_edges.c`).
 *
 * `buttons` is the merged state *after* this transition, so a reader
 * replays them in order and ends where `hal_pointer_poll` would have put
 * it. The position is the one at the time of the edge rather than the one
 * now, because a click is at a place: a press in a menu and a release
 * somewhere else are two facts, and reporting both at wherever the
 * pointer ended up would lose the one that matters.
 */
struct pointer_edge {
    uint32_t x, y;
    uint32_t buttons;
};

/*
 * A board records one when the buttons it reports change; whoever owns the
 * console takes them. `hal/keys.c` is the same arrangement for keys, and
 * these are deliberately the same shape.
 */
void     hal_pointer_edge(uint32_t x, uint32_t y, uint32_t buttons);
unsigned hal_pointer_edges(struct pointer_edge *out, unsigned max);

/* How many did not fit since the last time this was asked, so that losing
 * one is something a reader can notice rather than a silent cap. */
unsigned hal_pointer_edges_dropped(void);

/*
 * Brings up a pointing device, if the board has one. False is not an error,
 * exactly as with the keyboard: a machine with a serial cable and no mouse
 * is a legitimate way to run this system and always will be.
 *
 * **Relative devices are merged, and an absolute one is not.** This said
 * there was no merging at all, because a position has only one place it can
 * come from and a second pointing device would be a second thing to choose
 * between - a choice that would not exist until there was a board with two.
 * The ThinkPad is that board, and it showed which half of that was right. A
 * tablet says where it is, and two of those would have to be chosen between.
 * A TrackPoint and a USB mouse say how far, and there is nothing to choose:
 * the board adds both into the one position it reports, and holds the
 * buttons of each (`hal/pc/pointer.c`).
 */
bool hal_pointer_init(void);

/*
 * The current position and buttons. False when there is no pointer at all;
 * `moved` distinguishes "nothing has happened" from "it is still there".
 */
bool hal_pointer_poll(struct pointer_state *out);

/*
 * How far the pointer travels per count of movement, read or set.
 *
 * Zero asks without changing anything. Meaningful only for a *relative*
 * device - a TrackPoint, a mouse - which is why it is a number of device
 * units rather than a screen distance: this layer does not know how big the
 * screen is, the same division `hal_pointer_poll` draws.
 *
 * A board with an absolute device answers with zero and does nothing: a
 * tablet reports where it is, and there is no gain to apply to that.
 */
unsigned hal_pointer_speed(unsigned units_per_count);

/*
 * Movement, the wheel and buttons from a pointing device the kernel does not
 * drive - a USB mouse, whose driver is a process (`SYS_POINTER_MOVE`).
 * `wheel` is notches, as `pointer_state` counts them.
 *
 * **Relative, in the device's own counts, right and down positive**, and
 * `buttons` is that device's whole state: bit 0 left, bit 1 right. The board
 * adds the movement to the position `hal_pointer_poll` reports, scaled by the
 * speed its own relative devices move at, and holds the buttons beside
 * theirs rather than in place of them.
 *
 * False when the board's pointer is absolute: a tablet says where it is, and
 * there is no position of the board's own for a movement to be added to.
 *
 * The kernel calls it too, with no movement and no buttons, when a process
 * that reported buttons ends (`process_exit`): nothing else would ever let
 * go of a button a dead driver held.
 */
bool hal_pointer_move(int dx, int dy, int wheel, uint32_t buttons);

/*
 * **A key a process presses**, as `hal_pointer_move` is a mouse a process
 * moves: the USB driver's game controller, whose buttons are evdev codes
 * past the typing block - `BTN_SOUTH` is 0x130. Queued beside the board's
 * own keys and handed out by `hal_key_event` after them, so they reach the
 * focused window like any other. False when the queue is full.
 *
 * `hal_key_release_all` lets go of every one still down, for the kernel
 * when the process that pressed them ends; true if there was one.
 *
 * Shared by every board (`hal/keys.c`): a queue of numbers is not a
 * peripheral, and each board only merges it with what it has.
 */
#define KEY_PUSH_MOST 0x3FFu        /* ten bits: past every code keys.h has */

bool hal_key_push(unsigned code, bool down);
bool hal_key_release_all(void);

/*
 * Has an input device raised an interrupt since this was last asked?
 *
 * Not "what happened" - the events are in the device's own queue and are
 * read by whoever wants them, in a thread, in its own time. This is only the
 * fact that there is something, which is what a sleeper needs in order to
 * stop sleeping. Reading it clears it.
 */
bool hal_input_pending(void);

/* The same question without clearing it, for the interrupt path - which
 * wakes the sleeper but must leave the fact for the sleeper to read. */
bool hal_input_pending_peek(void);

/*
 * A block device.
 *
 * The one piece of hardware the filesystem needs. Sectors are 512 bytes
 * because that is the unit virtio counts in, whatever the underlying device
 * reports - the filesystem's own block size is a separate and larger
 * number, and conflating the two is how a driver ends up reading the wrong
 * place on a disk that calls its blocks 4096.
 *
 * Synchronous, and the byte count must be a whole number of sectors. They
 * return false for a device that is not there, for a request past the end of
 * it, and for an error from it. The caller cannot tell those apart and does
 * not need to: all three mean the bytes are not there.
 */
#define HAL_BLK_SECTOR  512u

struct blkdev {
    uint64_t sectors;               /* how many, of HAL_BLK_SECTOR each */
    uint32_t sector_size;
};

bool hal_blk_init(struct blkdev *out);      /* M8; false when there is none */
bool hal_blk_read(uint64_t sector, void *buf, uint32_t bytes);
bool hal_blk_write(uint64_t sector, const void *buf, uint32_t bytes);

/*
 * The network, at the only level this layer has any business at: frames in
 * and frames out.
 *
 * **No addresses, no protocols, no checksums.** What crosses here is an
 * Ethernet frame, header included, exactly as it goes on the wire - the same
 * division `hal_pointer_poll` draws by reporting device units and leaving
 * the scaling to whoever knows how big the screen is. What an IP address
 * means is not a driver's business, and a HAL that grew one would be a HAL
 * with an opinion about the internet.
 *
 * The MAC comes from the card because the card has one. It is *asked for*
 * rather than assumed: `virtio_features` reports what was actually agreed,
 * and a device that does not offer VIRTIO_NET_F_MAC leaves `mac` zeroed and
 * whoever is above this has to invent one.
 *
 * `hal_net_recv` returning 0 means nothing was waiting, which is not an
 * error and is what it does most of the time. Negative is a frame too big
 * for the buffer offered, which is a caller with a buffer smaller than the
 * MTU rather than a broken card.
 */
#define HAL_NET_MTU     1500u
#define HAL_NET_FRAME   1514u       /* MTU plus the 14-byte Ethernet header */

struct netdev {
    uint8_t  mac[6];
    uint32_t mtu;
};

bool hal_net_init(struct netdev *out);      /* false when there is no card */
bool hal_net_send(const void *frame, unsigned bytes);
int  hal_net_recv(void *frame, unsigned max);   /* 0 when nothing waiting */

/* Has a frame arrived since this was last asked? Read-and-clear, the same
 * shape as `hal_snd_dry`: the question is "is there anything", and the
 * frames themselves are in the ring until somebody takes them. */
bool hal_net_arrived(void);

/* Whether the card came up at boot. Asked rather than re-initialising,
 * because bringing a running device up again is a reset with frames in
 * flight. */
bool hal_net_present(void);

/* What the card is, after it came up. The same struct `hal_net_init` filled,
 * asked for again by whoever did not do the asking. */
bool hal_net_info(struct netdev *out);

/*
 * Is this key down right now?
 *
 * `code` is the keycode the board's keyboard uses, which on this one is
 * Linux's `input-event-codes.h` numbering because that is what virtio-input
 * speaks. Undecoded, like `hal_pointer_poll`'s device units and `sysinfo`'s
 * raw ID registers: this layer says what the hardware said.
 *
 * **This is a departure from what `CLAUDE.md` says about keyboards**, and
 * worth stating rather than sliding past. The rule was that a keyboard is a
 * source of characters and `hal_getchar` is where characters come from, so
 * there was deliberately no second keyboard entry point. That reasoning is
 * still right for characters and it cannot answer this question: "W is
 * still held" is not a character, and no stream of characters expresses it.
 * A key that repeats is not the same as a key that is down - the repeat
 * rate is a setting, and a game walks at whatever rate the frame runs at.
 *
 * The cost is one entry point and a bitmap the driver already had the
 * events for. What it buys is holding a key, which is the whole of moving
 * in a game and half of a modifier in a shortcut.
 */
bool hal_key_held(unsigned code);

/*
 * The next key transition, oldest first, or false when there are none.
 *
 * The companion to `hal_key_held`, and both are needed. The bitmap answers
 * "is it down now", which is what a game asks once a frame; it cannot
 * answer "it was pressed", because a press and its release inside one frame
 * leave the bitmap as they found it. A key you tap would never appear to
 * have been held.
 *
 * `code` is the board's own numbering, undecoded - Linux's
 * `input-event-codes.h` here, because that is what virtio-input speaks.
 * Turning it into a character is `hal_getchar`'s job and turning it into
 * something an application means is the window manager's.
 */
bool hal_key_event(unsigned *code, bool *down);

/*
 * Seconds since 1970, or 0 when this board has no clock.
 *
 * Read-only, and deliberately the whole of it. Setting the time is a
 * different operation with a different question behind it - what is
 * authoritative, this machine or the network - and there is no network.
 *
 * A number, not a date. Decoding it into a year and a month is arithmetic
 * with no hardware in it, so it happens above this layer, in Lua, for the
 * same reason `hal_pointer_poll` reports the device's own units and lets
 * the window manager scale them: this layer says what the hardware said.
 */
unsigned long hal_rtc_seconds(void);

/*
 * Sound: PCM out, and the deadline that comes with it.
 *
 * The format is fixed here rather than negotiated per caller, and that is
 * the honest simplification: one output stream, 44100 Hz, stereo, signed
 * sixteen-bit little-endian, which is what every source in this system will
 * be resampled to anyway. A device that cannot do it is a device this board
 * does not have.
 *
 * A *period* is the unit: the amount the device consumes before it needs
 * more. 256 frames is 1024 bytes and 5.8 milliseconds at this rate, and
 * four of them in flight is 23 milliseconds of sound in hand. That is the
 * deadline - `roadmap.md` M11a promises a measurement rather than a bound,
 * and `hal_snd_queued` is what makes the measurement possible.
 */
#define HAL_SND_RATE          44100u
#define HAL_SND_CHANNELS      2u
#define HAL_SND_FRAME_BYTES   4u        /* stereo, sixteen bits */
#define HAL_SND_PERIOD_FRAMES 256u
#define HAL_SND_PERIOD_BYTES  (HAL_SND_PERIOD_FRAMES * HAL_SND_FRAME_BYTES)
#define HAL_SND_PERIODS       4u

/*
 * **What is on the bus, and what of it this system drives.**
 *
 * Every other entry here answers "have you got one of these", which is the
 * question a *driver* asks. This is the question a *person* asks, and it is
 * not the same one: a machine with a device nothing claims looks identical,
 * from every other function in this file, to a machine without the device.
 * `machine` printed "no card" next to a boot log that had found one, and
 * that is the shape of the failure - absence and silence read alike.
 *
 * So the board reports what its bus enumeration found, whether or not a
 * driver wanted it. `id` and `class` go out exactly as the bus reported
 * them and this layer decodes nothing, which is `sysinfo`'s rule about ID
 * registers applied to a different set of numbers: naming a vendor is a
 * table, and a table belongs in userland.
 *
 * `where` is the board's own address for it - bus/slot/function packed on a
 * PC, the window index on a machine with a device tree - and is only ever
 * compared or shown, never followed.
 *
 * A board with no enumerable bus returns 0 and that is a complete answer.
 *
 * **It answers how many it found, and writes the first `max`.** A PC's
 * enumeration follows its bridges to the buses behind them, which is where a
 * laptop keeps its drive, so the count can pass the room a caller made; the
 * caller can then say so, rather than the list stopping without a word.
 */
/* Defined by `kernel/syscall.h`, because it is part of what `sysinfo`
 * hands to userland. Only ever a pointer here, so the declaration is all
 * this layer needs - and including the kernel's ABI header from `hal/`
 * would be the wrong direction. */
struct bus_device;

bool          hal_blk_present(void);
   /* a disk was found and claimed */

/* Which disk answered, or why none did - for the boot log, the way
 * `hal_snd_describe` names the sound device. Both boards had one and
 * nothing declared it, so nothing could call it. */
const char   *hal_blk_describe(void);

unsigned      hal_bus_scan(struct bus_device *out, unsigned max);

bool          hal_snd_init(void);       /* false when there is no device */
bool          hal_snd_present(void);    /* asked after init, by the kernel */
bool          hal_snd_write(const void *pcm, unsigned bytes);
unsigned      hal_snd_queued(void);

/*
 * How many periods arrived at a device that had already run out, and the
 * smallest depth ever seen. The first is deadlines missed; the second is the
 * margin that is left.
 *
 * Here rather than computed above, because the only exact moment to read the
 * depth is as a period is handed over, and only the driver is there.
 */
/*
 * Has the sound device asked for a period since this was last asked?
 *
 * Read-and-clear. The interrupt is what makes an audio deadline a deadline
 * rather than a poll that is usually often enough - the device knows when it
 * consumed a period and nothing above can do better than guess.
 */
bool          hal_snd_wants(void);
unsigned      hal_snd_wakes(void);

unsigned      hal_snd_dry(void);
unsigned      hal_snd_floor(void);

/*
 * Which device answered, in the words the boot log uses.
 *
 * The same argument `hal_fb_describe` makes and the same shape. A PC has
 * two possible sources of sound - an HDA controller soldered to it, and a
 * virtio device when QEMU was told to add one - and "no sound" covers a
 * controller that is not there, a controller with no codec on its link, and
 * a codec whose output pin is wired to nothing. Those are three different
 * faults with three different fixes and they all sound identical.
 */
const char   *hal_snd_describe(void);

/*
 * Stop, or start again.
 *
 * Neither returns when it works. There is no `bool` here for the same
 * reason there is no error path: a machine that could not be turned off is
 * a machine still running, and the caller finds that out by still running.
 *
 * Firmware rather than a peripheral - PSCI on this board - so unlike every
 * other entry here it names no device and reads no register.
 */
void          hal_power_off(void);
void          hal_restart(void);

/*
 * The battery, as the board last read it, or false when the board reads
 * none - no battery, or none this board knows how to ask.
 *
 * **A copy of a reading, never a read.** The ThinkPad's is its embedded
 * controller's, asked every thirty seconds on processor zero's tick
 * (`hal/pc/ec.c`) and cached, so a caller that asks sixty times a second -
 * `sysinfo`, for the top bar - costs a copy under a lock and touches no
 * port. `percent` is remaining over full, rounded, at most 100; the rest is
 * what the controller said, and a battery the controller has not finished
 * measuring - `ready` false - keeps the reading before it.
 */
struct hal_battery {
    bool     present;           /* a battery is in the machine */
    bool     charging;
    bool     discharging;
    bool     on_ac;             /* the charger is plugged in */
    bool     critical;          /* the controller says it is nearly empty */
    unsigned percent;           /* 0 to 100 */
};

bool          hal_battery_read(struct hal_battery *out);

/*
 * A string the firmware was asked to carry, or false when there is none.
 *
 * QEMU takes `-fw_cfg name=opt/kosmos/boot,string=wm`, which is how a
 * machine is told what to do without rebuilding it. `opt/` is the namespace
 * QEMU reserves for exactly this, so nothing here collides with a name QEMU
 * defines itself.
 *
 * Not a kernel command line. There is a device tree with `/chosen/bootargs`
 * in it and parsing one is real work for a facility fw_cfg already
 * provides - and fw_cfg is here because `hal_fb_init` needed it.
 */
bool hal_boot_option(const char *name, char *out, unsigned long max);

#endif /* HAL_H */
