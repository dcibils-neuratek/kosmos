/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Starting another processor, the way a PC does it.
 *
 * AArch64 asks PSCI, which is firmware: one call, an address, and the core is
 * running. **x86 has no firmware to ask.** A processor is started by sending
 * it INIT and then two STARTUP inter-processor interrupts through the local
 * APIC, at a vector naming a page under 1 MB where `trampoline.S` waits to
 * climb out of real mode. So this copies that page into place, writes the
 * entry and the context into it, and sends the sequence to the local APIC id
 * the MADT listed for the processor.
 *
 * **The board knows how to start a processor and not where it lands**, the
 * split `hal/hal.h` describes: `entry` is `cpu_secondary_entry`, a flat
 * 32-bit address in `boot/x86_64/start.S`, and the context is the core's
 * index, which the trampoline hands over in edi.
 *
 * **Every processor asked about gets a line in the boot log, whatever
 * happened to it.** The first real machine this ran on counted eight
 * processors and started none, and the log said only `0 of the others in the
 * kernel too` - the fault the interrupt controller had a revision earlier, a
 * refusal nobody could see. So a processor not started says why; one started
 * says the last stage it reached, out of the word it writes into the
 * trampoline page as it climbs; and one that reaches the kernel says how
 * long that took.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "acpi.h"
#include "apic.h"
#include "console.h"
#include "hal.h"
#include "multiboot.h"
#include "pc.h"
#include "trampoline.h"

/*
 * Why a processor that was started never reached the kernel, by the last
 * value it wrote: `trampoline.h` has the stages.
 */
static const char *const stopped_after[REACHED_KERNEL] = {
    "never ran the trampoline",
    "stopped in real mode, inside the trampoline",
    "reached protected mode and never reached the kernel's 32-bit entry",
    "reached the kernel's 32-bit entry and never reached long mode",
    "reached long mode and never reached C",
    "reached C and stopped loading its own GDT and TSS",
    "loaded its GDT and TSS and stopped moving onto the kernel's page tables",
    "moved onto the kernel's page tables and stopped arming syscall and FP",
};

static void say(unsigned cpu, const char *what)
{
    kputs("cpu_on: processor ");
    kputu(cpu);
    kputc(' ');
    kputs(what);
}

/* An address in five hex digits below 1 MB, which is how a real-mode map is
 * read, and in eight above it. */
static void put_address(unsigned long v)
{
    kputs("0x");
    kputx(v, v > 0xfffffUL ? 8u : 5u);
}

/*
 * The local APIC id of the n-th processor started: the MADT's list, in the
 * firmware's order, with the running processor taken out. Core zero is
 * whichever one the firmware booted, not whichever it happens to list first.
 */
static bool apic_id_of(unsigned cpu, uint32_t *out)
{
    uint32_t self = (uint32_t)apic_id();
    uint32_t id;
    unsigned seen = 0;
    unsigned n;

    for (n = 0; acpi_cpu_apic_id(n, &id); n++) {
        if (id == self) {
            continue;
        }

        if (++seen == cpu) {
            *out = id;
            return true;
        }
    }

    return false;
}

static uint32_t page_word(unsigned offset)
{
    uint32_t word;

    memcpy(&word, (void *)(uintptr_t)(TRAMPOLINE_BASE + offset), sizeof(word));

    return word;
}

static void set_page_word(unsigned offset, uint32_t word)
{
    memcpy((void *)(uintptr_t)(TRAMPOLINE_BASE + offset), &word, sizeof(word));
}

bool hal_cpu_on(unsigned cpu, uintptr_t entry, unsigned long context)
{
    static bool copied;
    uint32_t id;
    uint32_t first;
    uint32_t reached;
    unsigned ms;

    if (cpu == 0) {
        return false;
    }

    if (!pc_irq_on_apic()) {
        say(cpu, "not started: no local APIC to send INIT and STARTUP "
                 "through - the interrupts line says why\n");
        return false;
    }

    if (!pc_trampoline_page_free()) {
        unsigned i;
        unsigned long base;
        unsigned long length;

        say(cpu, "not started: page 0x8000 is not usable RAM in the loader's "
                 "map. Usable below 1 MB:");

        for (i = 0; pc_low_region(i, &base, &length); i++) {
            kputc(' ');
            put_address(base);
            kputc('-');
            put_address(base + length);
        }

        if (i == 0) {
            kputs(" nothing");
        }

        kputc('\n');
        return false;
    }

    if (entry > 0xffffffffu) {
        say(cpu, "not started: its entry is above 4 GB, where the trampoline "
                 "cannot jump\n");
        return false;
    }

    if (!apic_id_of(cpu, &id)) {
        say(cpu, "not started: the MADT lists no such processor\n");
        return false;
    }

    /* Once: the code does not change between cores, only the words. */
    if (!copied) {
        memcpy((void *)(uintptr_t)TRAMPOLINE_BASE, trampoline_start,
               (size_t)(trampoline_end - trampoline_start));
        copied = true;
    }

    set_page_word(TRAMPOLINE_ENTRY, (uint32_t)entry);
    set_page_word(TRAMPOLINE_CONTEXT, (uint32_t)context);
    set_page_word(TRAMPOLINE_REACHED, REACHED_NOTHING);

    /*
     * Read back, because the map calling a page RAM is a claim rather than a
     * fact. A page that is not memory, or is memory something still guards,
     * reads back as something else - usually all ones - and the progress
     * word would then say a processor reached the kernel before it had been
     * sent anything.
     */
    memcpy(&first, trampoline_start, sizeof(first));

    if (page_word(0) != first
        || page_word(TRAMPOLINE_ENTRY) != (uint32_t)entry
        || page_word(TRAMPOLINE_CONTEXT) != (uint32_t)context
        || page_word(TRAMPOLINE_REACHED) != REACHED_NOTHING) {
        say(cpu, "not started: page 0x8000 did not keep what was written to "
                 "it, and reads 0x");
        kputx(page_word(0), 8);
        kputs(" where the trampoline begins\n");
        return false;
    }

    if (!apic_start_processor(id, TRAMPOLINE_VECTOR)) {
        say(cpu, "not started: APIC id 0x");
        kputx(id, 8);
        kputs(" is past what the memory-mapped local APIC can address\n");
        return false;
    }

    /*
     * Two seconds for it to reach the kernel, which a processor that works
     * does in a few milliseconds. Whether it then arrives is the kernel's
     * question, and `kernel/smp.c` says so if it never does; how far it got
     * on the way is the half only the board can read.
     */
    for (ms = 0; ms < 2000u; ms++) {
        reached = page_word(TRAMPOLINE_REACHED);

        if (reached >= REACHED_KERNEL) {
            break;
        }

        pc_timer_wait_ms(1);
    }

    reached = page_word(TRAMPOLINE_REACHED);

    say(cpu, "(APIC id 0x");
    kputx(id, 2);
    kputs(") ");

    if (reached == REACHED_KERNEL) {
        kputs("reached the kernel ");
        kputu(ms + 1u);
        kputs(" ms after its second STARTUP\n");
    } else if (reached < REACHED_KERNEL) {
        kputs(stopped_after[reached]);
        kputc('\n');
    } else {
        kputs("wrote 0x");
        kputx(reached, 8);
        kputs(" into its progress word, which no stage writes\n");
    }

    return true;
}
