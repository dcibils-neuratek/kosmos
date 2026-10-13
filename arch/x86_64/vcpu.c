/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Guests on x86-64: not yet. `kernel/vm.h`'s architecture half, answering
 * that this machine cannot enter one - so all four syscalls refuse with
 * `SYS_ERR_NO_DEVICE`, as on an ARM machine started below EL2.
 *
 * VT-x is the second architecture's step (`docs/virtualization.md`, *Two ways
 * to begin*): VMXON, a VMCS a vCPU, EPT for the second stage. Diego chose ARM
 * in QEMU first, because QEMU emulates EL2 and not VT-x.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../kernel/vm.h"
#include "../../kernel/syscall.h"

bool arch_vm_available(void) { return false; }

/*
 * **The probe**, before VT-x is written (`docs/sandboxes.md`, S5): whether
 * the processor has VMX, and what its firmware decided. Intel SDM vol. 2,
 * CPUID leaf 1, ECX bit 5 is VMX; vol. 3 §24.7 and vol. 4,
 * `IA32_FEATURE_CONTROL` (MSR 0x3A): bit 0 locks it until reset, bit 2
 * allows VMXON outside SMX. The MSR is read only when CPUID says VMX
 * exists - on a processor without it, reading it faults.
 */
#define CPUID_VMX               (1u << 5)
#define MSR_FEATURE_CONTROL     0x3Au
#define FEATURE_LOCKED          (1u << 0)
#define FEATURE_VMX_OUTSIDE_SMX (1u << 2)

unsigned arch_vm_probe(void)
{
    uint32_t a, b, c, d, lo, hi;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));

    if ((c & CPUID_VMX) == 0) {
        return VIRT_NONE;
    }

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_FEATURE_CONTROL));
    (void)hi;

    if ((lo & FEATURE_LOCKED) == 0) {
        return VIRT_UNLOCKED;
    }

    return (lo & FEATURE_VMX_OUTSIDE_SMX) != 0 ? VIRT_ON : VIRT_OFF;
}
bool arch_vm_here(void) { return false; }
unsigned arch_vm_tags(void) { return 0; }
void *arch_guest_tables(void) { return NULL; }

bool arch_guest_map(void *tables, uint64_t ipa, uintptr_t pa)
{
    (void)tables, (void)ipa, (void)pa;
    return false;
}

void arch_guest_free(void *tables) { (void)tables; }

void *arch_vcpu_new(void *tables, unsigned tag)
{
    (void)tables, (void)tag;
    return NULL;
}

void arch_vcpu_free(void *state) { (void)state; }

long arch_vcpu_run(void *state, struct vcpu_regs *io)
{
    (void)state, (void)io;
    return -1;
}
