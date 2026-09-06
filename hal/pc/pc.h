/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_PC_H
#define HAL_PC_H

#include <stdint.h>

/*
 * Shared between this board's own files. Not part of the HAL interface: no
 * other board implements these and nothing outside hal/pc/ may call them.
 *
 * `hal/qemu-virt/qemu-virt.h` is the same idea for the ARM board, and the
 * rule it states is the one that matters - a board's files may talk to each
 * other, and nothing above them may join in.
 */

/*
 * Port I/O, which is the whole of how a PC talks to its old peripherals.
 *
 * This is the x86 answer to `mmio_read32` / `mmio_write32`, and the reason
 * it exists in the same shape is the same: a device access should be one
 * named thing that carries whatever the architecture requires, rather than
 * a `volatile` and a hope. What it does *not* need is the barriers their
 * ARM counterparts carry - `in` and `out` are serialising on x86 by
 * definition, which is one of the few places this architecture asks for
 * less rather than more.
 *
 * The `"Nd"` constraint is "an 8-bit constant, or DX" - the two addressing
 * forms the instruction actually has.
 */
static inline void pc_out8(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t pc_in8(uint16_t port)
{
    uint8_t v;

    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));

    return v;
}

/*
 * What a multiboot loader leaves behind, and where.
 *
 * Here rather than in the one file that reads it, because two of them do
 * now - `memory.c` wants the memory map and `boot.c` the command line -
 * and `hal/qemu-virt/qemu-virt.h` states the rule this follows: a layout
 * written down twice is a layout that can disagree with itself.
 *
 * `flags` says which of the fields below were filled in, one bit each.
 * Multiboot specification 0.6.96, section 3.3.
 */
#define MB_FLAG_CMDLINE   (1u << 2)
#define MB_FLAG_MMAP      (1u << 6)

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed));

/*
 * One entry of the map, and the `size` field is the trap in it.
 *
 * `size` does not include itself. Walking the list by `entry + size` steps
 * four bytes short every time and lands in the middle of the next entry,
 * which produces a plausible list of regions that do not exist. The
 * specification says so in one sentence and it is the sentence everybody
 * misses.
 */
struct multiboot_mmap {
    uint32_t size;
    uint64_t base;
    uint64_t length;
    uint32_t type;              /* 1 is usable; everything else is not */
} __attribute__((packed));

/* Where IRQ 0 lands once the 8259s have been remapped, clear of the 32
 * vectors the architecture reserves for exceptions. `pic.c` says why. */
#define PC_IRQ_BASE     32

/* Lets one interrupt line through. Everything starts masked. */
void pc_irq_unmask(unsigned irq);

/* What `pic.c` calls when IRQ 0 arrives. In `timer.c`, which owns the count. */
void pc_timer_interrupt(void);

/*
 * Where the loader left its information structure, stored by `start.S` and
 * read by whichever file here needs a field out of it.
 *
 * **A variable rather than an argument, because this is the board's
 * business and not the processor's.** It was a pair of calls from
 * `kmain_x86` for a while, which meant `arch/x86_64/` had to know that
 * multiboot exists - and multiboot is a *firmware* protocol, in the same
 * category as the device tree the ARM board reads. `arch/` is "which CPU
 * are you"; a PC's boot handoff is not an answer to that question.
 *
 * Zero when there was none. Written once before there is a second thread
 * and read-only afterwards, which is what makes a file-scope variable
 * acceptable here - the same argument `kernel/screen.c` makes about the
 * display it found.
 */
extern uint32_t pc_multiboot;

/*
 * Read what the loader left, **before the page allocator can reuse the
 * memory it is in**.
 *
 * That is the whole reason these are not read on first ask. QEMU puts the
 * multiboot structure and the command line in RAM just past the kernel
 * image - 0x572000 on this machine, against an image ending near 0x55e000 -
 * and `pmm_init` quite correctly considers everything past `__image_end`
 * free. By the time the first process asks what it should do, the string is
 * whatever was allocated over it.
 *
 * What that looked like: `flags` with the command-line bit set, a plausible
 * pointer, and an empty string behind it - found while the command line was
 * still where `hal_boot_option` came from. It answers out of fw_cfg now,
 * the way the other board does, so the only thing still taken from here is
 * the memory map. The lesson kept its file anyway: it is the same trap for
 * the next field somebody wants.
 *
 * `hal_early_init` calls it, which is the first thing `kmain` does.
 */
void pc_capture_memory(void);

#endif /* HAL_PC_H */
