/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef HAL_FWCFG_H
#define HAL_FWCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * QEMU's firmware configuration device, which both boards have.
 *
 * It is how the machine is told things nobody wanted to rebuild an image
 * to change: which display to bring up, what the machine was asked to do at
 * boot, and a directory of files the host handed over. `hal/virtio/` says
 * why a device shared by two boards lives in its own directory rather than
 * being copied into each, and this is the same argument with a smaller
 * device.
 *
 * **The protocol is identical and the register is not.** On `virt` the
 * three registers are MMIO at 0x09020000, out of the device tree; on a PC
 * they are I/O ports at 0x510. Everything else - the DMA descriptor, its
 * big-endian fields, the directory walk, the bounded spin - is one copy in
 * `fwcfg.c`, and a board provides the two functions below and nothing else.
 */

/*
 * The 64-bit big-endian DMA address register.
 *
 * **The write to the low half is what starts the operation**, so a board
 * writing this as two halves must write the high one first. That is not a
 * style choice and it is the only ordering rule in the device.
 */
void     fwcfg_reg_write(uint64_t value);
uint64_t fwcfg_reg_read(void);

/* Whether the device is there at all, and whether the half of it this code
 * uses is. `fwcfg.c` decides from what `fwcfg_reg_read` answers. */
bool fwcfg_present(void);

/* An item by name, from the file directory. */
bool fwcfg_find(const char *name, uint16_t *select, uint32_t *size);

/* The nth item, by position rather than by name - which is what a file
 * server wants, since its whole point is serving files nobody compiled a
 * name for. */
bool fwcfg_entry(unsigned index, char *name, size_t name_len,
                 uint16_t *select, uint32_t *size);

/* An item's bytes, into memory the caller provides. */
bool fwcfg_read(uint16_t select, void *buffer, uint32_t length);

/* And the other direction, which is how ramfb is told where the pixels are:
 * a writable item takes bytes from the guest rather than giving them. */
bool fwcfg_write(uint16_t select, const void *data, uint32_t length);

#endif /* HAL_FWCFG_H */
