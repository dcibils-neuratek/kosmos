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

static void psci(uint32_t function)
{
    register unsigned long x0 __asm__("x0") = function;

    __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
}

void hal_power_off(void)
{
    psci(PSCI_SYSTEM_OFF);
}

void hal_restart(void)
{
    psci(PSCI_SYSTEM_RESET);
}
