/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * QEMU's firmware configuration device.
 *
 * The channel a guest uses to ask QEMU questions before it knows anything,
 * and the only way to reach ramfb. Everything here is out of
 * `docs/specs/fw_cfg.rst` as shipped with this QEMU (9.1.2), read from
 * /opt/local/share/doc/qemu/specs/fw_cfg.html rather than remembered - which
 * matters more than usual, because the register layout is different on Arm
 * from x86 and the whole interface is big-endian.
 *
 * On Arm, from that document:
 *
 *     Selector Register address: Base + 8   (2 bytes)
 *     Data Register address:     Base + 0   (8 bytes)
 *     DMA Address address:       Base + 16  (8 bytes)
 *
 * Only the DMA interface is used. It can select an item itself, so the
 * selector register is never touched: one mechanism instead of two, and the
 * one that can also write.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fwcfg.h"
#include "spinlock.h"

/*
 * The DMA descriptor. Every field big-endian, control at the lowest address.
 *
 * It lives on the caller's stack, which is identity-mapped RAM, so its
 * virtual address is its physical one. That equality is the only reason this
 * is allowed to hand a pointer straight to the device, and it stops being
 * true the day the kernel moves to TTBR1.
 */
struct dma_access {
    uint32_t control;
    uint32_t length;
    uint64_t address;
};

#define DMA_ERROR   (1u << 0)
#define DMA_READ    (1u << 1)
#define DMA_SKIP    (1u << 2)
#define DMA_SELECT  (1u << 3)   /* the upper 16 bits are the item index */
#define DMA_WRITE   (1u << 4)

/* Selector keys named in the spec. */
#define FWCFG_FILE_DIR      0x0019

/*
 * Runs one operation and waits for it.
 *
 * The spec says a transfer in progress "doesn't happen today due to
 * implementation not being async, but may in the future", so the loop is
 * bounded rather than absent: correct now, and correct later without
 * hanging if that changes.
 */
static bool fwcfg_dma(uint32_t control, void *buffer, uint32_t length)
{
    struct dma_access access;
    unsigned spins;

    access.control = __builtin_bswap32(control);
    access.length  = __builtin_bswap32(length);
    access.address = __builtin_bswap64((uint64_t)(uintptr_t)buffer);

    /* mmio_write32 carries a `dmb oshst` before its store, which is what
     * orders these three normal stores ahead of the register write that
     * tells the device to go and read them. */
    fwcfg_reg_write((uint64_t)(uintptr_t)&access);

    for (spins = 0; spins < 1000000u; spins++) {
        uint32_t now = __builtin_bswap32(access.control);

        if ((now & DMA_ERROR) != 0) {
            return false;
        }

        if (now == 0) {
            return true;        /* all bits clear: finished */
        }

        /* The device writes `access` through the emulator rather than
         * through this core, so the compiler must not keep the field in a
         * register across the loop. */
        __asm__ volatile("" ::: "memory");
    }

    return false;
}

bool fwcfg_present(void)
{
    /*
     * The spec: "If the DMA interface is available, then reading the DMA
     * Address Register returns 0x51454d5520434647 (QEMU CFG in big-endian
     * format)."
     *
     * This is a better probe than the signature item, because it proves the
     * half of the device that is actually used. Reading the traditional
     * signature would prove a path this file never takes.
     */
    return fwcfg_reg_read() == 0x51454d5520434647ULL;   /* "QEMU CFG" */
}

/* One entry of the file directory, 64 bytes, big-endian where it is not a
 * string. Laid out exactly as `struct FWCfgFile` in the spec. */
struct fwcfg_file {
    uint32_t size;
    uint16_t select;
    uint16_t reserved;
    char     name[56];
};

/*
 * **One conversation with the device at a time.** Selecting an item resets
 * a read position the device keeps, and every read advances it - so two
 * cores walking the directory at once would each skip the entries the other
 * read. Taken by the public functions below; the walks under them assume it.
 */
static struct spinlock fwcfg_lock = SPINLOCK("fw_cfg");

static bool find_unlocked(const char *name, uint16_t *select, uint32_t *size)
{
    struct fwcfg_file entry;
    uint32_t count;
    uint32_t i;

    /*
     * Select the directory and read its count in one operation, then walk
     * the entries with plain reads.
     *
     * Selecting resets the item's offset to zero and each read advances it,
     * so the entries arrive one at a time without a buffer big enough for
     * all of them. The directory on this machine is a couple of kilobytes,
     * and a kernel with no allocator has nowhere to put that.
     */
    if (!fwcfg_dma(DMA_SELECT | (FWCFG_FILE_DIR << 16) | DMA_READ,
                   &count, sizeof(count))) {
        return false;
    }

    count = __builtin_bswap32(count);

    for (i = 0; i < count; i++) {
        if (!fwcfg_dma(DMA_READ, &entry, sizeof(entry))) {
            return false;
        }

        /* The name is NUL-terminated ASCII inside 56 bytes. Compared with
         * a bounded compare, because a directory entry is data from outside
         * this kernel and a missing terminator must not run off the end. */
        entry.name[sizeof(entry.name) - 1] = '\0';

        if (strcmp(entry.name, name) == 0) {
            *select = __builtin_bswap16(entry.select);
            *size   = __builtin_bswap32(entry.size);
            return true;
        }
    }

    return false;
}

/*
 * The nth file the firmware is carrying, by position rather than by name.
 *
 * `fwcfg_find` answers "is there one called this", which is what a driver
 * wants: ramfb knows the name of the thing it needs. This answers "what is
 * there at all", which is what a *file server* wants, because the whole
 * point of it is to serve files nobody compiled a name for.
 *
 * The walk is the same one and for the same reason: selecting the directory
 * resets its offset and each read advances it, so entries arrive one at a
 * time and a kernel with no allocator never needs room for all of them.
 */
static bool entry_unlocked(unsigned index, char *name, size_t name_len,
                           uint16_t *select, uint32_t *size)
{
    struct fwcfg_file entry;
    uint32_t count;
    uint32_t i;

    if (!fwcfg_dma(DMA_SELECT | (FWCFG_FILE_DIR << 16) | DMA_READ,
                   &count, sizeof(count))) {
        return false;
    }

    count = __builtin_bswap32(count);

    if (index >= count) {
        return false;
    }

    for (i = 0; i <= index; i++) {
        if (!fwcfg_dma(DMA_READ, &entry, sizeof(entry))) {
            return false;
        }
    }

    entry.name[sizeof(entry.name) - 1] = '\0';

    if (name != NULL && name_len > 0) {
        size_t n = 0;

        while (n + 1 < name_len && entry.name[n] != '\0') {
            name[n] = entry.name[n];
            n++;
        }

        name[n] = '\0';
    }

    *select = __builtin_bswap16(entry.select);
    *size   = __builtin_bswap32(entry.size);
    return true;
}

/*
 * An item's bytes, into memory the caller provides.
 *
 * Selecting resets the offset, so this reads from the start every time and
 * is safe to call twice. `length` is what the caller has room for and is
 * not checked against the item's size here - the caller asked the directory
 * how big it was and is the one that can do something about the answer.
 */
static bool read_unlocked(uint16_t select, void *buffer, uint32_t length)
{
    if (length == 0) {
        return true;
    }

    return fwcfg_dma(DMA_SELECT | ((uint32_t)select << 16) | DMA_READ,
                     buffer, length);
}

static bool write_unlocked(uint16_t select, const void *data, uint32_t length)
{
    /*
     * Writes go through the DMA interface and nowhere else. Writes to the
     * data register were removed in QEMU 2.4 and reinstated in 2.9 for DMA
     * only, so this is not a shortcut - it is the only door.
     *
     * The cast drops const because the descriptor field is one address for
     * both directions. Nothing writes through it on this path: the control
     * word says DMA_WRITE, so the device reads.
     */
    return fwcfg_dma(DMA_SELECT | ((uint32_t)select << 16) | DMA_WRITE,
                     (void *)(uintptr_t)data, length);
}

/*
 * A string passed on the QEMU command line.
 *
 *   -fw_cfg name=opt/kosmos/boot,string=wm
 *
 * Returns false when there is no such entry, which is the ordinary case and
 * not an error: a machine started without one boots to the shell.
 */
bool fwcfg_boot_option(const char *name, char *out, unsigned long max)
{
    uint16_t select;
    uint32_t size;

    if (out == NULL || max == 0) {
        return false;
    }

    out[0] = '\0';

    if (!fwcfg_present() || !fwcfg_find(name, &select, &size)) {
        return false;
    }

    if (size == 0) {
        return false;
    }

    /* Room for the terminator, and a value longer than the buffer is a
     * value nobody meant to pass. */
    if (size >= max) {
        size = (uint32_t)(max - 1);
    }

    if (!fwcfg_read(select, out, size)) {
        return false;
    }

    out[size] = '\0';
    return true;
}

bool fwcfg_find(const char *name, uint16_t *select, uint32_t *size)
{
    unsigned long flags = spin_lock(&fwcfg_lock);
    bool ok = find_unlocked(name, select, size);

    spin_unlock(&fwcfg_lock, flags);
    return ok;
}

bool fwcfg_entry(unsigned index, char *name, size_t name_len,
                 uint16_t *select, uint32_t *size)
{
    unsigned long flags = spin_lock(&fwcfg_lock);
    bool ok = entry_unlocked(index, name, name_len, select, size);

    spin_unlock(&fwcfg_lock, flags);
    return ok;
}

bool fwcfg_read(uint16_t select, void *buffer, uint32_t length)
{
    unsigned long flags = spin_lock(&fwcfg_lock);
    bool ok = read_unlocked(select, buffer, length);

    spin_unlock(&fwcfg_lock, flags);
    return ok;
}

bool fwcfg_write(uint16_t select, const void *data, uint32_t length)
{
    unsigned long flags = spin_lock(&fwcfg_lock);
    bool ok = write_unlocked(select, data, length);

    spin_unlock(&fwcfg_lock, flags);
    return ok;
}
