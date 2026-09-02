/*
 * Which processor this is. See cpu.h for where every number comes from.
 */

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

/* MIDR_EL1, from cputype.h: implementer [31:24], variant [23:20],
 * architecture [19:16], part [15:4], revision [3:0]. */
#define MIDR_IMPLEMENTER(m) (((m) >> 24) & 0xff)
#define MIDR_VARIANT(m)     (((m) >> 20) & 0xf)
#define MIDR_ARCHITECTURE(m) (((m) >> 16) & 0xf)
#define MIDR_PART(m)        (((m) >> 4) & 0xfff)
#define MIDR_REVISION(m)    ((m) & 0xf)

static uint64_t read_midr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(v));
    return v;
}

struct named {
    unsigned    id;
    const char *name;
};

/* arch/arm64/include/asm/cputype.h */
static const struct named implementers[] = {
    { 0x41, "ARM" },
    { 0x42, "Broadcom" },
    { 0x43, "Cavium" },
    { 0x46, "Fujitsu" },
    { 0x48, "HiSilicon" },
    { 0x4e, "NVIDIA" },
    { 0x50, "APM" },
    { 0x51, "Qualcomm" },
    { 0x61, "Apple" },
    { 0x6d, "Microsoft" },
    { 0xc0, "Ampere" },
};

/*
 * Only the parts this project will plausibly meet: what QEMU emulates, and
 * what is in the boards on `hal.md`'s target list. A part that is not here
 * prints its number, which is the point of carrying the raw MIDR alongside.
 */
static const struct named arm_parts[] = {
    { 0xb76, "ARM1176JZF-S" },      /* Raspberry Pi 1 */
    { 0xc07, "Cortex-A7" },
    { 0xc08, "Cortex-A8" },
    { 0xc09, "Cortex-A9" },
    { 0xd03, "Cortex-A53" },        /* Raspberry Pi 3 */
    { 0xd05, "Cortex-A55" },
    { 0xd07, "Cortex-A57" },
    { 0xd08, "Cortex-A72" },        /* Raspberry Pi 4, and what QEMU is told to be */
    { 0xd09, "Cortex-A73" },
    { 0xd0a, "Cortex-A75" },
    { 0xd0b, "Cortex-A76" },        /* Raspberry Pi 5 */
    { 0xd0c, "Neoverse-N1" },
    { 0xd0d, "Cortex-A77" },
    { 0xd41, "Cortex-A78" },
};

static const char *lookup(const struct named *table, size_t count, unsigned id)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (table[i].id == id) {
            return table[i].name;
        }
    }

    return "unknown";
}

void cpu_identify(struct cpu_info *out)
{
    uint64_t midr = read_midr();

    out->midr = midr;

    __asm__ volatile("mrs %0, mpidr_el1"        : "=r"(out->mpidr));
    __asm__ volatile("mrs %0, ctr_el0"          : "=r"(out->ctr));
    __asm__ volatile("mrs %0, id_aa64pfr0_el1"  : "=r"(out->pfr0));
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(out->isar0));
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(out->mmfr0));
    __asm__ volatile("mrs %0, cntfrq_el0"       : "=r"(out->counter_hz));

    out->implementer  = (unsigned)MIDR_IMPLEMENTER(midr);
    out->variant      = (unsigned)MIDR_VARIANT(midr);
    out->architecture = (unsigned)MIDR_ARCHITECTURE(midr);
    out->part         = (unsigned)MIDR_PART(midr);
    out->revision     = (unsigned)MIDR_REVISION(midr);

    out->implementer_name = lookup(implementers,
                                   sizeof(implementers) / sizeof(implementers[0]),
                                   out->implementer);

    /* Part numbers are only meaningful within an implementer: 0xd08 is a
     * Cortex-A72 if ARM designed it and something else entirely otherwise. */
    out->part_name = (out->implementer == 0x41)
                   ? lookup(arm_parts, sizeof(arm_parts) / sizeof(arm_parts[0]),
                            out->part)
                   : "unknown";
}

/*
 * CTR_EL0: DminLine [19:16], log2 of the line size in *words*, not bytes.
 * Four bytes to a word, hence the extra shift - and forgetting it is how a
 * cache maintenance loop ends up striding four times too far and clearing a
 * quarter of what it should.
 *
 * IminLine [3:0] is the same thing for the instruction cache and had a
 * function of its own until nothing turned out to maintain that cache.
 */
unsigned cpu_dcache_line(const struct cpu_info *cpu)
{
    return 4u << ((cpu->ctr >> 16) & 0xf);
}

/* ID_AA64MMFR0_EL1 PARANGE [3:0]. The encoding is a table, not a formula. */
unsigned cpu_pa_bits(const struct cpu_info *cpu)
{
    static const unsigned char bits[] = { 32, 36, 40, 42, 44, 48, 52, 56 };
    unsigned code = (unsigned)(cpu->mmfr0 & 0xf);

    return (code < sizeof(bits)) ? bits[code] : 0;
}
