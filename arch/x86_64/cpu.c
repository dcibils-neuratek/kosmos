/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Asking an x86-64 who it is.
 *
 * The counterpart of `arch/aarch64/cpu.c`, and the two answer the same
 * question through opposite mechanisms. ARM has a system register per
 * subject - MIDR_EL1 for identity, CTR_EL0 for caches, six ID_AA64* for
 * features - and reads them with `mrs`. x86 has *one* instruction and a
 * leaf number in eax, and which of the four output registers means what
 * changes per leaf.
 *
 * **Ask leaf 0 first, always.** It returns the highest leaf the processor
 * implements, and asking for a higher one does not fail: it returns the
 * highest one it does have, silently. So a read of leaf 7 on a processor
 * that stops at 1 is not an error, it is leaf 1's contents wearing leaf 7's
 * name - and every bit you then test is a feature you have invented. The
 * extended leaves at 0x80000000 have their own maximum and the same trap.
 *
 * Intel SDM volume 2A, the CPUID instruction. AMD APM volume 3, appendix E.
 */

#include <stdint.h>

#include "cpu.h"

/* For CPU_ARCH_X86_64, which is an ABI number rather than a kernel fact. */
#include "syscall.h"

struct regs {
    uint32_t eax, ebx, ecx, edx;
};

/*
 * `ebx` is the awkward one and the constraint list says so.
 *
 * It is the PIC base register in 32-bit position-independent code, and GCC
 * refuses to let inline assembly clobber it there. This kernel is neither
 * 32-bit nor position independent, so `=b` is accepted - but writing it out
 * with an explicit save is what portable code does and the reason is worth
 * knowing before somebody moves this file.
 */
static struct regs cpuid(uint32_t leaf, uint32_t sub)
{
    struct regs r;

    __asm__ volatile("cpuid"
                     : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx)
                     : "a"(leaf), "c"(sub));

    return r;
}

void cpu_identify(struct cpu_info *out)
{
    struct regs r0 = cpuid(0, 0);
    struct regs r1 = cpuid(1, 0);
    uint32_t highest = r0.eax;
    uint32_t extended;
    unsigned i;

    /*
     * The vendor string, twelve characters in EBX, EDX, ECX - in that
     * order, which is not the order the registers are named in.
     */
    {
        uint32_t words[3] = { r0.ebx, r0.edx, r0.ecx };

        for (i = 0; i < 12; i++) {
            out->vendor_name[i] = (char)((words[i / 4] >> ((i % 4) * 8)) & 0xff);
        }

        out->vendor_name[12] = '\0';
    }

    out->vendor0   = ((uint64_t)r0.edx << 32) | r0.ebx;
    out->vendor1   = ((uint64_t)r0.eax << 32) | r0.ecx;
    out->signature = r1.eax;
    out->brand     = r1.ebx;
    out->feat1_ecx = r1.ecx;
    out->feat1_edx = r1.edx;

    /*
     * Leaf 7 only if it exists. See the note at the top: asking for it on a
     * processor that stops at 1 returns leaf 1, and every feature bit
     * tested afterwards is imaginary.
     */
    if (highest >= 7) {
        struct regs r7 = cpuid(7, 0);

        out->feat7_ebx = r7.ebx;
        out->feat7_ecx = r7.ecx;
    } else {
        out->feat7_ebx = 0;
        out->feat7_ecx = 0;
    }

    /* The extended leaves have their own maximum, in their own leaf 0. */
    extended = cpuid(0x80000000u, 0).eax;

    out->address = (extended >= 0x80000008u)
                 ? cpuid(0x80000008u, 0).eax
                 : 0;

    /*
     * Family and model, folded the way the manuals specify.
     *
     * The base fields ran out - family is four bits and Intel reached 15 -
     * so both grew extension fields that are added rather than replacing,
     * and the rule for *when* to add them differs between the two:
     * the extended model applies when the base family is 6 or 15, and the
     * extended family only when it is 15. Applying either unconditionally
     * gives a plausible wrong answer on most processors made this century.
     */
    {
        unsigned base_family = (unsigned)((r1.eax >> 8) & 0xf);
        unsigned base_model  = (unsigned)((r1.eax >> 4) & 0xf);

        out->family   = base_family;
        out->model    = base_model;
        out->stepping = (unsigned)(r1.eax & 0xf);

        if (base_family == 6 || base_family == 15) {
            out->model |= (unsigned)((r1.eax >> 16) & 0xf) << 4;
        }

        if (base_family == 15) {
            out->family += (unsigned)((r1.eax >> 20) & 0xff);
        }
    }

    /*
     * The counter's frequency, and on this architecture there is usually no
     * honest answer.
     *
     * AArch64 has CNTFRQ_EL0, which firmware is required to program, so the
     * ARM side simply reads it. The x86 equivalent is the TSC, whose rate
     * is only *stated* by CPUID leaf 0x15 on recent parts and otherwise has
     * to be calibrated against something that already knows the time.
     * Zero here means "not stated", which is the truth; it is not a
     * measurement waiting to be taken, it is one the processor declines to
     * report.
     */
    if (highest >= 0x15) {
        struct regs r15 = cpuid(0x15, 0);

        /* ECX is the core crystal in Hz, and EBX/EAX its ratio to the TSC.
         * Any of the three being zero means the leaf is present and has
         * nothing to say, which is common under emulation. */
        out->counter_hz = (r15.ecx != 0 && r15.eax != 0)
                        ? ((uint64_t)r15.ecx * r15.ebx) / r15.eax
                        : 0;
    } else {
        out->counter_hz = 0;
    }

    /* And the same processor in the words the boot log uses. */
    out->model_name = "x86-64";
    out->id_name    = "CPUID.1:EAX";
    out->id         = out->signature;

    /*
     * "f6m94s3" - family, model, stepping, in the only three numbers this
     * architecture has to say which part it is. ARM's "r0p3" is two digits
     * because a MIDR revision is two nibbles; these are up to three digits
     * each and are built the same way, a character at a time, because the
     * kernel has no printf and does not want one for this.
     */
    {
        char *p = out->revision_text;
        unsigned fields[3] = { out->family, out->model, out->stepping };
        const char letters[3] = { 'f', 'm', 's' };
        unsigned f;

        for (f = 0; f < 3; f++) {
            unsigned v = fields[f];
            unsigned scale = 100;

            *p++ = letters[f];

            if (v == 0) {
                *p++ = '0';
                continue;
            }

            while (scale > 0) {
                unsigned digit = (v / scale) % 10;

                if (digit != 0 || scale == 1 || p[-1] != letters[f]) {
                    *p++ = (char)('0' + digit);
                }

                scale /= 10;
            }
        }

        *p = '\0';
    }
}

unsigned cpu_arch(void)
{
    return CPU_ARCH_X86_64;
}

unsigned cpu_raw(const struct cpu_info *cpu, uint64_t *out, unsigned max)
{
    const uint64_t words[] = {
        cpu->vendor0, cpu->vendor1, cpu->signature, cpu->brand,
        cpu->feat1_ecx, cpu->feat1_edx,
        cpu->feat7_ebx, cpu->feat7_ecx,
        cpu->address,
    };
    unsigned n = (unsigned)(sizeof(words) / sizeof(words[0]));
    unsigned i;

    if (n > max) {
        n = max;
    }

    for (i = 0; i < n; i++) {
        out[i] = words[i];
    }

    return n;
}

/*
 * CLFLUSH's line size, in eight-byte units, from CPUID.1 EBX[15:8].
 *
 * ARM reports log2 of the number of *words* and this reports a count of
 * eight-byte chunks - two different encodings of the same fact, and both
 * of them are what the respective manuals say rather than what a reader
 * would guess. Valid only when CLFLUSH is present (EDX bit 19); 64 is the
 * answer on everything that has ever run long mode, and is the fallback.
 */
unsigned cpu_dcache_line(const struct cpu_info *cpu)
{
    unsigned units;

    /* Only meaningful when CLFLUSH exists, which EDX bit 19 says. 64 is the
     * answer on everything that has ever run long mode, and is what to
     * report when the processor declines to state one. */
    if ((cpu->feat1_edx & (1u << 19)) == 0) {
        return 64;
    }

    units = (unsigned)((cpu->brand >> 8) & 0xff);

    return (units != 0) ? units * 8 : 64;
}

/*
 * The physical address width, from CPUID.80000008 EAX[7:0].
 *
 * 36 is the fallback rather than 32: every processor that can enter long
 * mode implements at least PAE's 36 bits, and a machine that does not
 * report the leaf is old enough that guessing high would be the wrong way
 * to be wrong.
 */
unsigned cpu_pa_bits(const struct cpu_info *cpu)
{
    unsigned bits = (unsigned)(cpu->address & 0xff);

    return (bits != 0) ? bits : 36;
}
