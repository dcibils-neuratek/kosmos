/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_SERVERS_VMMNET_H
#define USER_SERVERS_VMMNET_H

/*
 * A sandbox's network: a virtio-net device and the gateway behind it
 * (`vmmnet.c`, `docs/sandboxes.md` S4). `vmm.c` starts it when the sandbox
 * was given the network, sends its register accesses here, asks whether
 * its interrupt is wanted, and pumps what Kosmos's side has for the guest.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * Started with `/Network`: `outgoing` whether the guest may open
 * connections of its own and ask names, and `n` ports Kosmos answers on -
 * `from[i]`, carried to the guest's `to[i]`, a `from` of 0 none. What it
 * answers: a bit for each port Kosmos would not answer on.
 */
unsigned vmmnet_start(long net_cap, uint32_t tick_hz, bool outgoing,
                  const uint16_t *from, const uint16_t *to, unsigned n);
bool vmmnet_mmio(uint64_t off, unsigned size, bool write, uint64_t *value);
bool vmmnet_raised(void);

/* What arrived on Kosmos's side, to the guest: whether anything moved. */
bool vmmnet_pump(void);

/* Whether any connection is open - the monitor then waits less. */
bool vmmnet_busy(void);

/* Bytes in to the guest and out from it, for a window's graph. */
void vmmnet_counted(uint64_t *in, uint64_t *out);

#endif /* USER_SERVERS_VMMNET_H */
