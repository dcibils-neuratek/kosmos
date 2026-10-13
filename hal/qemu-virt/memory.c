/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The memory map of QEMU's virt machine.
 *
 * Source: QEMU hw/arm/virt.c, `base_memmap`. RAM starts at 0x40000000 and
 * everything below it is devices: flash at 0, the GIC at 0x08000000, the
 * PL011 at 0x09000000.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "fwcfg.h"

#define RAM_BASE    0x40000000UL

/*
 * **The size is asked of QEMU**, not assumed: the SMBIOS tables it publishes
 * through its firmware configuration device (`etc/smbios/smbios-tables`,
 * `hal/fwcfg/fwcfg.c`) carry a type 19 structure - Memory Array Mapped
 * Address - for the RAM `-m` gave, and their sizes add up to it. The DMTF's
 * SMBIOS specification, 7.20: the starting and ending address in kilobytes
 * at 4 and 8, and - when those read 0xFFFFFFFF - in bytes at 0x0F and 0x17.
 *
 * It was 512 MB, compiled in, matching every `-m` this project used - and a
 * constant is exactly what `CLAUDE.md` says the machine should be asked
 * instead. It bit on 13 October 2026: a sandbox test given 1 GB and then
 * 2 GB saw 512 MB, and a second guest's RAM could not be had
 * (`docs/sandboxes.md`). Neither of the other answers is there for a
 * kernel QEMU loads as an ELF: its `FW_CFG_RAM_SIZE` item is the PC's, and
 * reads zero on this board; and it leaves no device tree at the base of RAM
 * - both tried, and found empty, before this. The device tree is how a
 * Pi's memory will be found, on the day there is one.
 *
 * Read once, at the first ask - which is `pmm_init`, before any page is
 * handed out, so into a buffer of its own. 512 MB stays the answer for a
 * machine that says nothing, and a total below 64 MB is not believed.
 */
#define RAM_SIZE_ASSUMED    (512UL * 1024 * 1024)
#define SMBIOS_MAX          8192u

static uint8_t smbios[SMBIOS_MAX];

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = (v << 8) | p[n];
    }

    return v;
}

/* The bytes type 19 says there are, added up; 0 if it says nothing. */
static uint64_t smbios_ram(const uint8_t *t, uint32_t len)
{
    uint64_t total = 0;
    uint32_t at = 0;

    while (at + 4 <= len) {
        uint8_t type = t[at], flen = t[at + 1];
        uint32_t next;

        if (flen < 4 || at + flen > len || type == 127) {
            break;
        }

        if (type == 19 && flen >= 0x0f) {
            uint64_t from = le(t + at + 4, 4), to = le(t + at + 8, 4);

            if (from == 0xffffffffu && flen >= 0x1f) {
                from = le(t + at + 0x0f, 8);
                to = le(t + at + 0x17, 8);
                total += to >= from ? to - from + 1 : 0;
            } else {
                total += to >= from ? (to - from + 1) * 1024 : 0;
            }
        }

        /* Past the formatted part and its strings, which end in two NULs. */
        next = at + flen;

        while (next + 1 < len && !(t[next] == 0 && t[next + 1] == 0)) {
            next++;
        }

        at = next + 2;
    }

    return total;
}

static unsigned long ram_size(void)
{
    static unsigned long known;
    uint16_t select;
    uint32_t size;
    uint64_t bytes = 0;

    if (known != 0) {
        return known;
    }

    if (fwcfg_present() && fwcfg_find("etc/smbios/smbios-tables", &select, &size)) {
        if (size > SMBIOS_MAX) {
            size = SMBIOS_MAX;
        }

        if (fwcfg_read(select, smbios, size)) {
            bytes = smbios_ram(smbios, size);
        }
    }

    known = bytes >= 64UL * 1024 * 1024 ? (unsigned long)bytes : RAM_SIZE_ASSUMED;
    return known;
}

void hal_ram_range(struct memrange *out)
{
    out->base = RAM_BASE;
    out->size = ram_size();
}

/*
 * One range, which is what this board has.
 *
 * A device tree's `/memory` node can describe several and QEMU's `virt`
 * describes one, starting at 1 GB and running for however much `-m` asked
 * for. There is no PCI hole through it and nothing below it, so the answer
 * is `hal_ram_range`'s and the interface costs this board four lines.
 */
unsigned hal_ram_ranges(struct memrange *out, unsigned max)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    hal_ram_range(&out[0]);

    return out[0].size > 0 ? 1u : 0u;
}

/*
 * Never, on this board. `virt` is given its memory on the command line and
 * `make qemu` asks for 512 MB, which is comfortably inside what AArch64's
 * identity map describes - so there has never been anything to cap and the
 * question has one answer.
 */
bool hal_ram_capped(unsigned long *whole_bytes)
{
    (void)whole_bytes;

    return false;
}

/*
 * No loader hands this board a disk: `virt` is started with `-kernel`,
 * and the disk it has is the virtio-blk device QEMU was given.
 */
bool hal_loader_disk(unsigned long *base, unsigned long *bytes)
{
    (void)base;
    (void)bytes;

    return false;
}

/*
 * And no map either, for the same reason: `-kernel` hands over a device
 * tree rather than a list of ranges, and what this board knows about its
 * own memory is the one range `hal_ram_range` reports.
 */
unsigned hal_memory_entries(unsigned *seen)
{
    if (seen != NULL) {
        *seen = 0;
    }

    return 0;
}

bool hal_memory_entry(unsigned i, unsigned long *base, unsigned long *length,
                      unsigned *type)
{
    (void)i;
    (void)base;
    (void)length;
    (void)type;

    return false;
}
