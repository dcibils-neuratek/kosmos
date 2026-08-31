#ifndef ARCH_AARCH64_CPU_H
#define ARCH_AARCH64_CPU_H

#include <stdint.h>

/*
 * Which processor this is, asked of the processor.
 *
 * `arch/` is "which CPU are you" and this is the most literal possible
 * reading of that. Every value here comes out of a system register the core
 * implements because the architecture requires it, so this file works on any
 * AArch64 part and needs no board knowledge at all.
 *
 * The bit positions are from Linux's `arch/arm64/tools/sysreg`, which is the
 * machine-readable description the kernel generates its own accessors from,
 * and the part numbers from `arch/arm64/include/asm/cputype.h`. Neither was
 * written from memory: a decode that is subtly wrong prints a plausible
 * processor that is not the one you have, which is worse than printing
 * nothing.
 *
 * **The raw registers travel with the decode**, everywhere they are shown.
 * A table of part numbers goes stale the moment a part ships that is not in
 * it, and a reader who can see MIDR_EL1 can look it up; a reader who can
 * only see "unknown" cannot.
 */

struct cpu_info {
    /* Raw, exactly as read. */
    uint64_t midr;              /* who this core is */
    uint64_t mpidr;             /* where it is in the topology */
    uint64_t ctr;               /* cache geometry */
    uint64_t pfr0;              /* processor features: FP, SIMD, GIC */
    uint64_t isar0;             /* instruction set: AES, SHA, CRC32, atomics */
    uint64_t mmfr0;             /* memory model: physical address range */
    uint64_t counter_hz;        /* CNTFRQ_EL0 */

    /* Decoded from MIDR. */
    unsigned implementer;
    unsigned variant;
    unsigned architecture;
    unsigned part;
    unsigned revision;

    const char *implementer_name;   /* never NULL; "unknown" if not in the table */
    const char *part_name;          /* never NULL */
};

void cpu_identify(struct cpu_info *out);

/* Cache line sizes in bytes, from CTR_EL0. Both are logged as log2 of the
 * number of *words*, which is the encoding people get wrong. */
unsigned cpu_icache_line(const struct cpu_info *cpu);
unsigned cpu_dcache_line(const struct cpu_info *cpu);

/* The physical address range the core can drive, in bits. */
unsigned cpu_pa_bits(const struct cpu_info *cpu);

#endif /* ARCH_AARCH64_CPU_H */
