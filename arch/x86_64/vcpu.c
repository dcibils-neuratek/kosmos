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

bool arch_vm_available(void) { return false; }
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
