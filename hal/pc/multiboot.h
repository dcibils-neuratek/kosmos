/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a multiboot loader leaves behind, and where.
 *
 * **A format rather than a port**, which is why it is here and not in
 * `pc.h`: that file carries this board's inline assembly, and a host test
 * that wants to check the layout cannot compile a port instruction. The
 * layout is the same on any machine that can read it.
 *
 * Multiboot specification 0.6.96, section 3.3.
 */
#ifndef KOSMOS_HAL_PC_MULTIBOOT_H
#define KOSMOS_HAL_PC_MULTIBOOT_H

#include <stdbool.h>
#include <stdint.h>

#define MB_FLAG_CMDLINE      (1u << 2)
#define MB_FLAG_MMAP         (1u << 6)

/*
 * The loader put a linear framebuffer somewhere and this says where.
 *
 * **This is how a real PC gets a screen.** `ramfb` is QEMU's - the guest
 * points it at memory the guest chose - and a laptop has nothing like it.
 * What a laptop has is firmware that already set a mode before anything of
 * ours ran, and a loader that passes the address on. Multiboot's video
 * request is how you ask for that: bit 2 of the *header* flags asks, and
 * bit 12 of the *info* flags says it was answered.
 */
#define MB_FLAG_FRAMEBUFFER  (1u << 12)

/* `framebuffer_type`: the only one worth having. 0 is a palette and 2 is
 * EGA text, and neither is a thing this system can draw into. */
#define MB_FB_RGB            1u

struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;

    /*
     * Everything from here down was not read before, and the fields are
     * listed rather than skipped for the reason the memory map's `size`
     * comment gives: a layout you step over by arithmetic is a layout that
     * can be wrong by four bytes and still look plausible.
     */
    uint32_t drives_length, drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;

    uint32_t vbe_control_info, vbe_mode_info;
    uint16_t vbe_mode, vbe_interface_seg;
    uint16_t vbe_interface_off, vbe_interface_len;

    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width, framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
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

/* A busy wait on the 8253's channel two, for whoever needs a known interval
 * before there is a tick. `timer.c` calibrates the TSC with it and
 * `apic.c` measures the local APIC's timer against it. */
void pc_timer_wait_ms(unsigned ms);

/* Masks every line on the 8259 pair. For a machine driving the I/O APIC
 * instead - see `apic.c`, which explains why silence is not the same as
 * being ignored. */
void pic_silence(void);

/* The 8259 pair under its own names, for `irq_bind.c` to choose between. */
void pic_init(void);
bool pic_handle(void);
void pic_unmask(unsigned irq);

/* Whether this machine took the APIC path. `timer.c` asks, because the
 * tick comes from a different chip on each. */
bool pc_irq_on_apic(void);

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

/*
 * The framebuffer the loader set up, if it set one up.
 *
 * Captured by the call above rather than read on demand, for the reason
 * that comment gives. False means there was none - a QEMU boot with ramfb
 * and no loader video mode, which is every boot this project has ever done
 * until now - and `hal/pc/fb.c` falls back.
 */
bool pc_loader_framebuffer(uint64_t *addr, uint32_t *pitch,
                           uint32_t *width, uint32_t *height);

/* What a loader handed over, once it has been believed. */
struct pc_loader_fb {
    uint64_t addr;
    uint32_t pitch;
    uint32_t width, height;
};

/*
 * Whether that structure describes a framebuffer worth using. Pure, and
 * separated from the reading so `tools/test_loaderfb.c` can check it
 * without a machine - which matters because QEMU's `-kernel` never sets
 * the flag, so this path is exercised by nothing else until hardware.
 */
bool pc_framebuffer_from(const struct multiboot_info *info,
                         struct pc_loader_fb *out);

/* Where the loader said ACPI's root pointer is, or NULL. Only a
 * Multiboot 2 loader can answer; see `multiboot2.h` for why that matters
 * more than it sounds. */
const void *pc_loader_rsdp(void);

/* Whether the page `hal/pc/trampoline.S` is copied to was usable RAM in the
 * loader's map - answered from the walk at boot, because the map is gone by
 * the time a processor is started. */
bool pc_trampoline_page_free(void);

/* The usable regions the loader listed below 1 MB, kept to say why that page
 * was refused and where one might go instead. False past the last one. */
bool pc_low_region(unsigned i, unsigned long *base, unsigned long *length);

#endif
