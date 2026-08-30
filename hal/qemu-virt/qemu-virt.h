#ifndef HAL_QEMU_VIRT_H
#define HAL_QEMU_VIRT_H

#include <stdbool.h>

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

/* The interrupt to service, or 1023 when there is none. */
unsigned gic_acknowledge(void);
void     gic_end_of_interrupt(unsigned intid);

#define GIC_SPURIOUS    1023u

/* Called by hal_irq_handle when the timer's interrupt arrives. Counts the
 * tick and rearms. */
void timer_interrupt(void);

#endif /* HAL_QEMU_VIRT_H */
