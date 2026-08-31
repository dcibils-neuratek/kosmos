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

/* One of the input devices has events waiting. `slot` is which window. */
void input_interrupt(unsigned slot);

/* The interrupt to service, or 1023 when there is none. */
unsigned gic_acknowledge(void);
void     gic_end_of_interrupt(unsigned intid);

#define GIC_SPURIOUS    1023u

/* Called by hal_irq_handle when the timer's interrupt arrives. Counts the
 * tick and rearms. */
void timer_interrupt(void);

/*
 * QEMU's firmware configuration device, which is how this board reaches
 * ramfb. See fwcfg.c: everything about it is big-endian and the register
 * layout on Arm is not the one x86 uses.
 */

/* Whether the device and its DMA interface are there at all. */
bool fwcfg_present(void);

/* Looks an item up by name in the file directory. Returns its selector key
 * and its length, both of which the caller needs before it can touch it. */
bool fwcfg_find(const char *name, uint16_t *select, uint32_t *size);

/* Writes an item whole. The DMA interface is the only one that can: writes
 * through the data register were removed in QEMU 2.4. */
bool fwcfg_write(uint16_t select, const void *data, uint32_t length);

/* The nth file the firmware carries, for a server that has to serve what it
 * was not told the name of. False when there is no nth. */
bool fwcfg_entry(unsigned index, char *name, size_t name_len,
                 uint16_t *select, uint32_t *size);

/* An item's bytes, into memory the caller provides. */
bool fwcfg_read(uint16_t select, void *buffer, uint32_t length);

/*
 * The keyboard: virtio-input over virtio-mmio. See keyboard.c, which also
 * holds the virtio transport - there is one virtio device on this board, and
 * splitting the transport out before there are two would be inventing an
 * interface against a single caller.
 */

/* One character, or -1 when nothing is waiting. Polled, like the UART. */
int keyboard_getchar(void);

bool keyboard_present(void);

#endif /* HAL_QEMU_VIRT_H */
