/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_QEMU_VIRT_H
#define HAL_QEMU_VIRT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared between this board's own files. Not part of the HAL interface: no
 * other board implements these and nothing outside hal/qemu-virt/ may call
 * them.
 */

/* The interrupt this board raises for the EL1 physical timer. PPI 14, so
 * INTID 16 + 14. Read out of QEMU's device tree rather than remembered. */
#define TIMER_INTID     30

/* Routes an interrupt to this CPU at the highest priority and enables it. */
void gic_enable_ppi(unsigned intid);

/* A shared interrupt, routed to this core. */
void gic_enable_spi(unsigned intid);

/*
 * Where the virtio-mmio interrupts land.
 *
 * `virt` maps mmio slot i to SPI 16 + i (hw/arm/virt.c, a15irqmap), and a
 * GIC interrupt ID for an SPI is 32 + the SPI number. So slot i is INTID
 * 48 + i, and the driver that owns a slot knows which one it took.
 */
#define VIRTIO_INTID_BASE   48

/* How many mmio windows `virt` lays out, and so how many INTIDs follow the
 * base. Every driver that scans them uses the same number.
 *
 * The windows themselves, from the device tree. Here rather than in a
 * driver because there is more than one driver now - input and block - and
 * a hardware address written down twice is a hardware address that can
 * disagree with itself. */
#define VIRTIO_MMIO_COUNT   32
#define VIRTIO_MMIO_BASE    0x0a000000UL
#define VIRTIO_MMIO_STRIDE  0x200UL

/*
 * The PL061 GPIO controller, and the power key wired to it.
 *
 * All four read out of the device tree QEMU 11.1.1 builds for
 * `-M virt,gic-version=3` - the harness's own options - rather than
 * remembered: `pl061@9030000` with `reg = <0 0x9030000 0 0x1000>` and
 * `interrupts = <0 7 4>`, which is SPI 7 and therefore INTID 32 + 7, level
 * high; and `gpio-keys` with `gpios = <&pl061 3 0>`, so the power key is
 * input 3.
 *
 * QEMU chooses this path only when the machine has no ACPI device, which a
 * `-kernel` boot without firmware does not - `virt_powerdown_req` in
 * hw/arm/virt.c. On a boot through UEFI the same button goes through ACPI
 * instead and this controller is not in the tree at all.
 */
#define PL061_BASE              0x09030000UL
#define PL061_SIZE              0x1000UL
#define PL061_INTID             39u
#define PL061_POWER_KEY_LINE    3u

/* The interrupt to service, or 1023 when there is none. */
unsigned gic_acknowledge(void);
void     gic_end_of_interrupt(unsigned intid);

#define GIC_SPURIOUS    1023u

/* Called by hal_irq_handle when the timer's interrupt arrives. Counts the
 * tick and rearms. */
void timer_interrupt(void);

#endif /* HAL_QEMU_VIRT_H */
