/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VMM_H
#define USER_SERVERS_VMM_H

/*
 * **Between a sandbox's monitor and its machine** - `vmm.c` is what both
 * machines share, and `vmm_arm.c` or `vmm_x86.c` the machine, one of them
 * built for each architecture. Inside one process and nowhere else: this is
 * not a protocol, and nothing outside the monitor includes it.
 */

#include <stdbool.h>
#include <stdint.h>

#include "kosmos.h"
#include "vmmproto.h"

/* Where the guest's RAM starts, in its own physical addresses. */
#if defined(__aarch64__)
#define VMM_GUEST_RAM   VMM_RAM_BASE
#else
#define VMM_GUEST_RAM   0ul
#endif

/* The monitor's, for the machine to read. */
extern struct vmm_setup vmm_cfg;            /* as it was handed, checked */
extern uint8_t *vmm_ram;                    /* the guest's RAM, mapped here */
extern long vmm_vcpu;
extern uint64_t vmm_counter_hz, vmm_tick_hz;
extern volatile bool vmm_stop_asked;
extern volatile uint64_t vmm_guest_ticks, vmm_exits;

bool vmm_networked(void);                   /* a virtio-net device, outgoing or ports */
bool vmm_rx_empty(void);                    /* nothing typed for the guest */
int  vmm_rx_take(void);                     /* the next byte typed, or -1 */
void vmm_guest_byte(char c);                /* a byte of the guest's console */
void vmm_flush_out(void);                   /* what the guest said, into the ring now */
void vmm_note(const char *text);            /* a line of the monitor's own */
void vmm_tell(const char *what, unsigned long a, unsigned long b);
void vmm_finish(void) __attribute__((noreturn));    /* the guest has ended */

/* The machine's: each file answers all five. */
bool vmm_machine_start(void);               /* its own state, before anything */
bool vmm_machine_setup_ok(void);            /* the setup fits this machine */
bool vmm_machine_prepare(void);             /* its firmware's tables, in the guest's RAM */
void vmm_machine_run(unsigned long unused); /* the vCPU's thread */

#endif /* USER_SERVERS_VMM_H */
