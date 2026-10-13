/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KERNEL_VM_H
#define KERNEL_VM_H

/*
 * **Guests and virtual CPUs, as Nebula's objects** - `docs/virtualization.md`
 * step V2.
 *
 * Two new kinds of thing a capability can name, and the kernel learns
 * nothing else: a *guest* is an address space seen through the processor's
 * second stage of translation, and a *vCPU* is a set of registers that runs
 * in it. Both are threads and address spaces in another form, which is why
 * they belong here, and everything a guest *sees* - a disk, a network, a
 * screen - is a monitor's, in user space.
 *
 * **A guest's memory is a region** (`memobj.h`), mapped into it at a guest
 * physical address. The monitor holds the same region and maps it into
 * itself, so it loads a kernel into the guest by writing to its own memory
 * and reads a virtio ring the same way: *control by message, data by shared
 * memory*, with the second stage as the window. A region mapped into a guest
 * is held by it until the guest ends.
 *
 * **Owned, as an interrupt line is**: by the process that made it, until
 * that process ends - a capability names one and a generation guards
 * against a slot reused, but the count of capabilities does not decide its
 * lifetime. A vCPU is run by whichever thread of its process asks, one at a
 * time, on a core that can enter a guest.
 *
 * The mechanism under it is the architecture's - `arch/aarch64/vcpu.c` and
 * `hyp.S` on ARM, and a refusal on x86-64 until VT-x arrives - through the
 * `arch_vm_*` functions below.
 */

#include <stdbool.h>
#include <stdint.h>

struct process;
struct thread;
struct vcpu_regs;

struct guest;
struct vcpu;

/* The pools, once at boot. */
void vm_init(void);

/* The four syscalls; each answers a capability index, 0, or SYS_ERR_*. */
long vm_guest_create(struct thread *t);
long vm_guest_map(struct thread *t, long guest, long region, uint64_t ipa);
long vm_vcpu_create(struct thread *t, long guest);
long vm_vcpu_run(struct thread *t, long vcpu, uintptr_t regs);

/* Everything a process made, ended with it: before its capabilities go. */
void vm_release_owned_by(struct process *p);

/* For the capability table: whether a slot still names its object. */
bool vm_guest_current(const struct guest *g, unsigned generation);
bool vm_vcpu_current(const struct vcpu *v, unsigned generation);

/* How many of each exist, for a test. */
unsigned vm_guests_in_use(void);
unsigned vm_vcpus_in_use(void);

/*
 * The architecture's half. `tables` is a guest's second stage, `tag` its
 * VMID (1 to `arch_vm_tags()`, 0 being the host's), and `state` a vCPU's
 * registers in whatever form the architecture keeps them.
 */
bool  arch_vm_available(void);          /* any core can enter a guest */
bool  arch_vm_here(void);               /* this one can */
unsigned arch_vm_tags(void);            /* VMIDs there are, beside the host's */
void *arch_guest_tables(void);
bool  arch_guest_map(void *tables, uint64_t ipa, uintptr_t pa);
void  arch_guest_free(void *tables);
void *arch_vcpu_new(void *tables, unsigned tag);
void  arch_vcpu_free(void *state);
long  arch_vcpu_run(void *state, struct vcpu_regs *io);

#endif /* KERNEL_VM_H */
