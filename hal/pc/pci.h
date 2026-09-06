/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_PC_PCI_H
#define HAL_PC_PCI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Finding devices on a PC, which is the whole of what differs from the
 * other board.
 *
 * `virt` describes its devices in a device tree: thirty-two virtio windows
 * at a fixed address and a fixed stride, and a driver walks them by
 * arithmetic. A PC has no such list. Devices answer on a *bus*, and finding
 * one means asking every possible address whether anything is there - which
 * is what `pci_find` does, and why `hal/virtio/`'s drivers had to stop
 * naming slots before either board could share them.
 *
 * Configuration space is reached through two I/O ports, which is the oldest
 * mechanism and the one that needs nothing found first. There is a
 * memory-mapped alternative - ECAM, whose base is in an ACPI table - and it
 * is faster and reaches configuration space past the first 256 bytes.
 * Nothing here needs either, and finding ACPI is a table parser for a
 * device this can already talk to.
 */

struct pci_device {
    uint8_t  bus, slot, function;

    uint16_t vendor;
    uint16_t device;
    uint8_t  irq;               /* the legacy interrupt line, 0-15 */

    /*
     * The six base address registers, already decoded: the low bits that
     * say what kind of window it is are stripped, and a 64-bit memory BAR
     * has been folded into one entry with the next left zero. Zero means
     * "no window", which is what a BAR reads as when the device has fewer
     * than six.
     */
    uint64_t bar[6];
};

/* One 32-bit word of configuration space. `offset` is rounded down to four,
 * because that is the only width the port pair can address. */
uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t offset);
void     pci_config_write(uint8_t bus, uint8_t slot, uint8_t fn,
                          uint8_t offset, uint32_t value);

/*
 * The nth device with this vendor and device id, or false when there is no
 * such thing.
 *
 * `from` is where to start, so a caller wanting the second of something
 * passes the index after the first - which is how `virtio_open` finds a
 * keyboard and then a tablet behind one device id, exactly as the ARM board
 * scans on from a slot.
 *
 * A device id of 0xffff matches any, which is how a caller asks for "the
 * next virtio device of any kind".
 */
bool pci_find(uint16_t vendor, uint16_t device, unsigned from,
              struct pci_device *out, unsigned *found_at);

/* Turns on the two bits a driver needs before a BAR means anything: memory
 * space decoding, and bus mastering so the device may fetch its own
 * descriptors. Off out of reset, and a device with them off is one that
 * answers nothing and blames nobody. */
void pci_enable(const struct pci_device *dev);

#endif /* HAL_PC_PCI_H */
