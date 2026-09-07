/*
 * Turning the machine off, and turning it over.
 *
 * PSCI - the ARM Power State Coordination Interface - which is firmware,
 * not a peripheral: there is no MMIO address here because there is no
 * device. A call goes to a higher exception level and something above the
 * kernel does the work.
 *
 * **The method comes from the device tree this QEMU publishes**, not from
 * memory: `-M virt,dumpdtb=` gives a `psci` node with
 * `compatible = "arm,psci-1.0"` and `method = "hvc"`. So it is `hvc`, and
 * that follows from where Kosmos runs: `boot/start.S` drops the kernel to
 * EL1, QEMU implements PSCI at EL2, and HVC from EL1 is the door to EL2.
 * A board whose firmware sits at EL3 says `smc` instead, and that is a
 * different board and a different file.
 *
 * The function numbers are the specification's rather than the board's -
 * PSCI fixes them, which is the point of having a specification for this -
 * and they are 32-bit calls, which is what the 0x84 prefix means.
 *
 * Neither of these returns. If one does, the call was refused, and saying
 * so is better than looking like a machine that ignored you.
 */

#include <stdint.h>

#include "hal.h"

#define PSCI_SYSTEM_OFF     0x84000008u
#define PSCI_SYSTEM_RESET   0x84000009u

/*
 * The 64-bit `AFFINITY_INFO`, which is how this board is asked how many
 * processors it has.
 *
 * **There is no "how many CPUs" call in PSCI**, and there is no device-tree
 * parser here to read `/cpus` from. What there is instead is a question you
 * can ask about a *specific* processor: is it on, off, or coming on. Ask it
 * about number 0, then 1, then 2, and the firmware answers
 * `INVALID_PARAMETERS` for the first one that does not exist. Counting up
 * to that is the count.
 *
 * A pure query - it starts nothing and changes nothing - which is why it
 * can be asked at boot, long before anything is ready to have a second core
 * running in it. `CPU_ON` is the call that would, and it is `smp.md` step
 * three.
 *
 * The argument is an MPIDR-shaped affinity value. QEMU's `virt` numbers its
 * cores in Aff0 up to eight and starts using Aff1 beyond that, so this is
 * right for the machines this runs on and would need the affinity built
 * properly for a bigger one. Said here rather than discovered there.
 *
 * ARM DEN 0022, PSCI: AFFINITY_INFO returns 0 for ON, 1 for OFF, 2 for
 * ON_PENDING, and a negative error for a target that is not a processor.
 */
#define PSCI_AFFINITY_INFO  0xC4000004u

/* Enough for anything this runs on, and a bound rather than a promise: the
 * loop below must stop even if a firmware answers "valid" for ever. */
#define CPUS_ASKED_MAX      64u

static long psci(uint32_t function, unsigned long a1, unsigned long a2)
{
    register unsigned long x0 __asm__("x0") = function;
    register unsigned long x1 __asm__("x1") = a1;
    register unsigned long x2 __asm__("x2") = a2;

    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2) : "memory");

    return (long)x0;
}

unsigned hal_cpu_count(void)
{
    unsigned n = 0;

    while (n < CPUS_ASKED_MAX && psci(PSCI_AFFINITY_INFO, n, 0) >= 0) {
        n++;
    }

    /* A firmware that refuses even processor zero is one this cannot ask.
     * There is certainly one core, because this code is running on it. */
    return (n == 0) ? 1u : n;
}

void hal_power_off(void)
{
    psci(PSCI_SYSTEM_OFF, 0, 0);
}

void hal_restart(void)
{
    psci(PSCI_SYSTEM_RESET, 0, 0);
}
