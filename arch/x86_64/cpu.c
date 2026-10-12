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

/*
 * What the board measured, when CPUID would not say. Written once at boot,
 * before there is a second thread, and read-only after - the same argument
 * `kernel/screen.c` makes about the display it found.
 */
static uint64_t measured_hz;

void cpu_set_counter_hz(uint64_t hz)
{
    measured_hz = hz;
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
     * **The brand string**: forty-eight characters in leaves 0x80000002 to
     * 0x80000004, EAX, EBX, ECX, EDX of each in turn - the name the maker
     * gave it, which the signature only implies. Intel pads it with
     * spaces at the front on older parts and AMD at the back, so runs of
     * spaces are made one and the ends trimmed. Empty when the processor
     * has no such leaves, which a person is then told by what is left.
     */
    {
        char raw[49];
        unsigned at = 0, n = 0;

        raw[0] = '\0';

        if (extended >= 0x80000004u) {
            for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
                struct regs r = cpuid(leaf, 0);
                uint32_t words[4] = { r.eax, r.ebx, r.ecx, r.edx };

                for (i = 0; i < 16; i++) {
                    raw[at++] = (char)((words[i / 4] >> ((i % 4) * 8)) & 0xff);
                }
            }

            raw[48] = '\0';
        }

        for (i = 0; i < 48 && raw[i] != '\0'; i++) {
            char c = raw[i];

            if (c < 32 || c > 126) c = ' ';
            if (c == ' ' && (n == 0 || out->name[n - 1] == ' ')) continue;

            out->name[n++] = c;
        }

        while (n > 0 && out->name[n - 1] == ' ') n--;

        out->name[n] = '\0';
    }

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

    /*
     * And the measurement, when the processor declined to state a rate.
     *
     * **Zero here is not harmless**, which is how this was found. It
     * reaches userland through `/dev/cpu`, and the browser does
     *
     *     local HZ = (fs.read("/dev/cpu") or {}).counter_hz or 62500000
     *
     * where `or` catches a missing field and not a present zero - so HZ was
     * 0, the first `// HZ` raised, and the browser died before it drew
     * anything. The window opened, had a title bar, and stayed black. The
     * boot log had been saying "0 MHz counter" the whole time.
     */
    if (out->counter_hz == 0) {
        out->counter_hz = measured_hz;
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

/*
 * IA32_APERF (0xE8) counts at the speed the core actually ran and
 * IA32_MPERF (0xE7) at its fixed reference rate - the invariant TSC's -
 * while the core is in C0; they exist when CPUID.06H:ECX[0] says so, and
 * reading one where they do not is a #GP. (Intel SDM vol. 4, table 2-2;
 * the numbers checked against Linux's `msr-index.h`.)
 *
 * Whether they exist is asked once, by the first processor to want them:
 * every processor gives the same answer, so a second asking in the same
 * instant writes the same value, and a CPUID on every tick would be a VM
 * exit on every tick under a hypervisor.
 */
#define MSR_IA32_MPERF 0xE7u
#define MSR_IA32_APERF 0xE8u

static int clock_counts_kept;           /* 0 not asked, 1 kept, 2 not */

static uint64_t read_msr(uint32_t msr)
{
    uint32_t lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * HWP's three registers (Intel SDM vol. 3, 15.4; the numbers and fields
 * checked against Linux's `msr-index.h`): IA32_PM_ENABLE (0x770) bit 0 turns
 * it on, and once on it stays on until reset; IA32_HWP_CAPABILITIES (0x771)
 * says the highest performance in bits 7:0 and the lowest in 31:24; and
 * IA32_HWP_REQUEST (0x774) asks for a minimum in 7:0, a maximum in 15:8, a
 * desired level in 23:16 - 0, the processor's own choice - and the energy-
 * performance preference in 31:24. CPUID.06H:EAX bit 7 says HWP is there,
 * bit 10 that the preference is.
 */
#define MSR_IA32_PM_ENABLE        0x770u
#define MSR_IA32_HWP_CAPABILITIES 0x771u
#define MSR_IA32_HWP_REQUEST      0x774u

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value),
                                "d"((uint32_t)(value >> 32)));
}

bool cpu_speed_set(unsigned leaning)
{
    if (cpuid(0, 0).eax < 6) {
        return false;
    }

    uint32_t eax = cpuid(6, 0).eax;

    if ((eax & (1u << 7)) == 0) {
        return false;
    }

    uint64_t caps, request;

    write_msr(MSR_IA32_PM_ENABLE, 1);
    caps = read_msr(MSR_IA32_HWP_CAPABILITIES);

    request = ((caps >> 24) & 0xffu)            /* minimum: the lowest */
            | ((caps & 0xffu) << 8);            /* maximum: the highest */

    if (eax & (1u << 10)) {
        request |= (uint64_t)(leaning & 0xffu) << 24;
    }

    write_msr(MSR_IA32_HWP_REQUEST, request);
    return true;
}

bool cpu_clock_counts(uint64_t *actual, uint64_t *reference)
{
    int kept = __atomic_load_n(&clock_counts_kept, __ATOMIC_RELAXED);

    if (kept == 0) {
        kept = (cpuid(0, 0).eax >= 6 && (cpuid(6, 0).ecx & 1u)) ? 1 : 2;
        __atomic_store_n(&clock_counts_kept, kept, __ATOMIC_RELAXED);
    }

    if (kept != 1) {
        return false;
    }

    /* MPERF first: an interval read this way round can only make the ratio
     * a hair high, never divide by a reference that has not moved. */
    *reference = read_msr(MSR_IA32_MPERF);
    *actual = read_msr(MSR_IA32_APERF);
    return true;
}

/*
 * **How hot it is, and what it is allowed to draw** (`roadmap.md`, "The
 * processor's heat and power"). Intel only, and only on the models below:
 * reading an MSR a processor does not have is a #GP in the kernel, so the
 * numbers are asked of a processor known to keep them rather than probed.
 * The registers, checked against Linux's `msr-index.h` and the SDM vol. 4:
 *
 *   IA32_THERM_STATUS (0x19C)          this core: bits 22:16 how many degrees
 *                                      under the limit, bit 31 that it is valid
 *   MSR_TEMPERATURE_TARGET (0x1A2)     bits 23:16 the limit, TjMax, in C
 *   IA32_PACKAGE_THERM_STATUS (0x1B1)  the package: the same readout, bit 0
 *                                      hot now, bit 10 held by a power limit
 *   MSR_RAPL_POWER_UNIT (0x606)        units: power 1/2^(3:0) W, energy
 *                                      1/2^(12:8) J, time 1/2^(19:16) s
 *   MSR_PKG_POWER_LIMIT (0x610)        PL1 in 14:0, its window as 2^(21:17)
 *                                      times (1 + (23:22)/4) time units;
 *                                      PL2 in 46:32
 *   MSR_PKG_ENERGY_STATUS (0x611)      energy used, 32 bits, wrapping
 *
 * Never under a hypervisor (CPUID.1:ECX bit 31): a guest can be told it is
 * an Intel model and not be given the registers that model has.
 */
#define MSR_IA32_THERM_STATUS         0x19Cu
#define MSR_TEMPERATURE_TARGET        0x1A2u
#define MSR_IA32_PACKAGE_THERM_STATUS 0x1B1u
#define MSR_RAPL_POWER_UNIT           0x606u
#define MSR_PKG_POWER_LIMIT           0x610u
#define MSR_PKG_ENERGY_STATUS         0x611u

/* Family 6 models with a digital sensor, TjMax and package RAPL - Sandy
 * Bridge on - as Linux's `intel_rapl_common.c` lists them, cores only. */
static const uint8_t heat_models[] = {
    0x2A, 0x2D, 0x3A, 0x3E,                     /* Sandy Bridge, Ivy Bridge */
    0x3C, 0x3F, 0x45, 0x46,                     /* Haswell */
    0x3D, 0x47, 0x4F, 0x56,                     /* Broadwell */
    0x4E, 0x5E, 0x55, 0x8E, 0x9E, 0xA5, 0xA6,   /* Skylake to Comet Lake */
    0x66, 0x6A, 0x6C, 0x7D, 0x7E, 0xA7,         /* Cannon, Ice, Rocket Lake */
    0x8C, 0x8D, 0x8F, 0x97, 0x9A,               /* Tiger, Sapphire, Alder Lake */
    0xB7, 0xBA, 0xBF, 0xAA, 0xAC, 0xBD,         /* Raptor, Meteor, Lunar Lake */
    0xC5, 0xC6,                                 /* Arrow Lake */
};

static int heat_kept;                   /* 0 not asked, 1 kept, 2 not */
static int heat_limit_c;                /* TjMax, read once */
static unsigned heat_power_units;       /* bits 3:0, 12:8, 19:16 of 0x606 */
static bool heat_package_sensor;        /* CPUID.06H:EAX bit 6, PTM */

static bool heat_asked(void)
{
    int kept = __atomic_load_n(&heat_kept, __ATOMIC_ACQUIRE);

    if (kept == 0) {
        struct regs r0 = cpuid(0, 0);
        struct regs r1 = cpuid(1, 0);
        unsigned family = (r1.eax >> 8) & 0xfu;
        unsigned model = ((r1.eax >> 4) & 0xfu) | (((r1.eax >> 16) & 0xfu) << 4);
        bool known = false;

        kept = 2;

        /* "GenuineIntel", in EBX, EDX, ECX. */
        if (r0.ebx == 0x756e6547u && r0.edx == 0x49656e69u && r0.ecx == 0x6c65746eu
            && r0.eax >= 6 && family == 6 && (r1.ecx & (1u << 31)) == 0
            && (cpuid(6, 0).eax & 1u)) {
            for (unsigned i = 0; i < sizeof(heat_models); i++) {
                known = known || heat_models[i] == model;
            }
        }

        if (known) {
            heat_limit_c = (int)((read_msr(MSR_TEMPERATURE_TARGET) >> 16) & 0xffu);
            heat_power_units = (unsigned)read_msr(MSR_RAPL_POWER_UNIT);
            heat_package_sensor = (cpuid(6, 0).eax & (1u << 6)) != 0;
            kept = heat_limit_c > 0 ? 1 : 2;
        }

        __atomic_store_n(&heat_kept, kept, __ATOMIC_RELEASE);
    }

    return kept == 1;
}

bool cpu_core_temperature(int *celsius)
{
    if (!heat_asked()) {
        return false;
    }

    uint64_t status = read_msr(MSR_IA32_THERM_STATUS);

    if ((status & (1ull << 31)) == 0) {
        return false;
    }

    *celsius = heat_limit_c - (int)((status >> 16) & 0x7fu);
    return true;
}

bool cpu_package_heat(struct cpu_heat *out)
{
    if (!heat_asked()) {
        return false;
    }

    uint64_t status = read_msr(MSR_IA32_PACKAGE_THERM_STATUS);
    uint64_t limit = read_msr(MSR_PKG_POWER_LIMIT);
    unsigned power_shift = heat_power_units & 0xfu;
    unsigned time_shift = (heat_power_units >> 16) & 0xfu;
    unsigned y = (unsigned)(limit >> 17) & 0x1fu;
    unsigned z = (unsigned)(limit >> 22) & 0x3u;

    /* Bit 31 is "valid" in the core's register and reserved in the
     * package's, which is valid whenever the processor has one (PTM). */
    out->package_c = heat_package_sensor
                   ? heat_limit_c - (int)((status >> 16) & 0x7fu) : CPU_HEAT_NONE;
    out->limit_c = heat_limit_c;
    out->held = ((status & 1u) ? CPU_HELD_HOT : 0)
              | ((status & (1u << 10)) ? CPU_HELD_POWER : 0);
    out->pl1_mw = (uint32_t)(((limit & 0x7fffu) * 1000u) >> power_shift);
    out->pl2_mw = (uint32_t)((((limit >> 32) & 0x7fffu) * 1000u) >> power_shift);
    /* 2^Y (1 + Z/4) time units, in ms: (1000 << Y) (4 + Z) / 4 / 2^time. */
    out->pl1_window_ms = (uint32_t)((((uint64_t)1000u << y) * (4u + z) / 4u) >> time_shift);
    out->energy_raw = (uint32_t)read_msr(MSR_PKG_ENERGY_STATUS);
    out->energy_shift = (heat_power_units >> 8) & 0x1fu;
    return true;
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
