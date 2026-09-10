/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Reading the firmware's tables, without an interpreter.
 *
 * Three facts come out of here and each replaces a guess this board was
 * making: how many processors there are, where the interrupt controllers
 * live, and where PCIe keeps its configuration space. `hal/pc/cpus.c`
 * answered "one, because nothing here has asked" and this is the asking.
 *
 * **Everything is checksummed before it is believed.** A table is a
 * structure at an address the firmware chose, in memory this kernel does
 * not own and did not write; the one-byte sum is what separates a table
 * from whatever happened to be lying there. A table that fails is skipped
 * rather than used, and a missing table is a fact rather than a failure.
 *
 * **No AML.** See `acpi.h`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi.h"
#include "pc.h"
#include "string.h"

/*
 * Where the pointer to everything else hides.
 *
 * Two places, both of them the PC's own history. The word at 0x40E is the
 * segment of the Extended BIOS Data Area, which is a scrap of memory the
 * firmware kept for itself at the top of the first 640 KB; and the read-only
 * BIOS area from 0xE0000 is where it goes if there is no EBDA. Sixteen-byte
 * alignment in both, which is the specification's own rule and is what makes
 * the scan cheap.
 *
 * Both are inside the megabyte `arch/x86_64/mmu.c` maps as the low region -
 * mapped uncached and never executable, for exactly this kind of reading.
 */
#define EBDA_POINTER    0x40eu
#define BIOS_START      0xe0000u
#define BIOS_END        0x100000u

struct rsdp {
    char     signature[8];          /* "RSD PTR " */
    uint8_t  checksum;              /* over the first 20 bytes */
    char     oem[6];
    uint8_t  revision;              /* 0 is ACPI 1.0, 2 is 2.0 and later */
    uint32_t rsdt;

    /* Only present when revision >= 2. */
    uint32_t length;
    uint64_t xsdt;
    uint8_t  extended_checksum;     /* over `length` bytes */
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator;
    uint32_t creator_revision;
} __attribute__((packed));

/* MADT, which the specification calls APIC. */
struct madt {
    struct sdt header;
    uint32_t   lapic_address;
    uint32_t   flags;
    /* Entries follow, each a type byte and a length byte. */
} __attribute__((packed));

#define MADT_LAPIC      0u
#define MADT_OVERRIDE   2u
#define MADT_IOAPIC     1u
#define MADT_LAPIC_X2   9u

#define LAPIC_ENABLED   0x1u
#define LAPIC_ONLINE    0x2u        /* can be brought up, if not already */

struct mcfg_entry {
    uint64_t base;
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
    uint32_t reserved;
} __attribute__((packed));

static bool     found;
static unsigned cpus;
static uint64_t lapic;
static uint64_t ioapic;
static uint64_t ecam;

/*
 * Sixteen is every ISA line there is, so a table that size cannot overflow
 * on a well-formed machine - and a malformed one stops at the edge rather
 * than writing past it.
 */
#define OVERRIDE_MAX 16

static struct acpi_override overrides[OVERRIDE_MAX];
static unsigned override_count;

/*
 * A table is what it says it is when its bytes sum to zero.
 *
 * One byte wide on purpose: the checksum field is chosen so the whole
 * structure sums to zero in eight bits, which is the weakest useful check
 * there is and is exactly the one the specification defines.
 */
static bool sums_to_zero(const void *at, uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)at;
    uint8_t total = 0;
    uint32_t i;

    for (i = 0; i < length; i++) {
        total = (uint8_t)(total + bytes[i]);
    }

    return total == 0;
}

static bool signature_is(const char *field, const char *want, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (field[i] != want[i]) {
            return false;
        }
    }

    return true;
}

/*
 * Sixteen bytes at a time through a range, looking for the anchor.
 */
static const struct rsdp *scan(uintptr_t from, uintptr_t to)
{
    uintptr_t at;

    for (at = from; at + sizeof(struct rsdp) <= to; at += 16) {
        const struct rsdp *maybe = (const struct rsdp *)at;

        if (!signature_is(maybe->signature, "RSD PTR ", 8)) {
            continue;
        }

        /*
         * The first twenty bytes are the 1.0 structure and are checksummed
         * on their own, which is what lets a 2.0 table stay readable by
         * something that only knows the old one. Both are checked when both
         * are there.
         */
        if (!sums_to_zero(maybe, 20)) {
            continue;
        }

        if (maybe->revision >= 2 && !sums_to_zero(maybe, maybe->length)) {
            continue;
        }

        return maybe;
    }

    return NULL;
}

static const struct rsdp *find_rsdp(void)
{
    /*
     * **The loader's answer first, because on a UEFI machine it is the only
     * one there is.**
     *
     * Everything below this looks in the two places a BIOS leaves the
     * pointer. UEFI does not: it hands the RSDP to whatever it launched, in
     * the EFI Configuration Table, and is not obliged to leave a copy where
     * a scan of the first megabyte would find one. OVMF leaves none.
     *
     * What that cost was measured before it was fixed. The same image on
     * the same four-processor machine reported four processors and drove the
     * local APIC under QEMU's `-kernel`, and reported one and fell back to a
     * pair of 8259s through GRUB - so on the machine the whole x86 target
     * exists for, every line of ACPI, APIC and MSI code was dead.
     *
     * `multiboot2.h` is why `start.S` carries a second header, and this is
     * the one thing it buys.
     */
    {
        const struct rsdp *given = (const struct rsdp *)pc_loader_rsdp();

        if (given != NULL && signature_is(given->signature, "RSD PTR ", 8)
            && sums_to_zero(given, 20)) {
            if (given->revision < 2 || sums_to_zero(given, given->length)) {
                return given;
            }
        }
    }

    /*
     * `volatile`, and not for the usual reason. Nothing else writes this
     * word; what it stops is the compiler reasoning about a constant
     * address as though it were an object it could see the bounds of -
     * without it, `-Werror=array-bounds` refuses the dereference, and it is
     * right to by its own rules. The address is real because the firmware
     * put it there, which is a fact no C compiler has access to.
     */
    volatile uintptr_t at = EBDA_POINTER;
    uint16_t segment;
    uintptr_t ebda;
    const struct rsdp *r;

    /*
     * Read through a `volatile` address rather than from a constant one.
     *
     * `-Werror=array-bounds` refuses `*(uint16_t *)0x40E`, and it is right
     * to by its own rules: it can see the constant, it cannot see an object
     * there, so it concludes the dereference is out of bounds. The address
     * is real because the firmware put a word there, which is a fact no C
     * compiler has access to - so the way to say it is to stop the constant
     * being folded, and this is that.
     */
    memcpy(&segment, (const void *)at, sizeof(segment));
    ebda = (uintptr_t)segment << 4;

    /*
     * The EBDA first, and only when the pointer is sane. A machine without
     * one leaves whatever it likes at 0x40E, and following that into a
     * region nothing maps would be a page fault during the boot log.
     */
    if (ebda >= 0x1000u && ebda < 0xa0000u) {
        r = scan(ebda, ebda + 1024u);

        if (r != NULL) {
            return r;
        }
    }

    return scan(BIOS_START, BIOS_END);
}

/*
 * One processor entry at a time.
 *
 * Entries are a type and a length and then whatever that type is, packed
 * end to end - so the walk is driven by the lengths in the data rather than
 * by a table of sizes here. **A zero length would loop for ever**, which is
 * the one thing a malformed table could do to this function, so it stops.
 */
static void read_madt(const struct madt *m)
{
    const uint8_t *at = (const uint8_t *)m + sizeof(*m);
    const uint8_t *end = (const uint8_t *)m + m->header.length;

    lapic = m->lapic_address;

    while (at + 2 <= end) {
        uint8_t type = at[0];
        uint8_t length = at[1];

        if (length < 2 || at + length > end) {
            return;
        }

        if (type == MADT_LAPIC && length >= 8) {
            uint32_t flags;

            memcpy(&flags, at + 4, sizeof(flags));

            /*
             * Enabled, or capable of being enabled. A firmware may list a
             * core it has parked, and both bits mean "this is a processor
             * that exists" - where neither means an empty socket.
             */
            if ((flags & (LAPIC_ENABLED | LAPIC_ONLINE)) != 0) {
                cpus++;
            }
        } else if (type == MADT_LAPIC_X2 && length >= 16) {
            uint32_t flags;

            memcpy(&flags, at + 8, sizeof(flags));

            if ((flags & (LAPIC_ENABLED | LAPIC_ONLINE)) != 0) {
                cpus++;
            }
        } else if (type == MADT_OVERRIDE && length >= 10
                   && override_count < OVERRIDE_MAX) {
            struct acpi_override *o = &overrides[override_count];
            uint32_t gsi;
            uint16_t flags;

            memcpy(&gsi, at + 4, sizeof(gsi));
            memcpy(&flags, at + 8, sizeof(flags));

            o->source = at[3];
            o->gsi = gsi;
            o->flags = flags;
            override_count++;
        } else if (type == MADT_IOAPIC && length >= 12 && ioapic == 0) {
            uint32_t address;

            memcpy(&address, at + 4, sizeof(address));
            ioapic = address;
        }

        at += length;
    }
}

static void read_mcfg(const struct sdt *table)
{
    const uint8_t *at = (const uint8_t *)table + sizeof(*table) + 8;
    const uint8_t *end = (const uint8_t *)table + table->length;

    /* The first segment group is the one this kernel can use; a machine
     * with more than one is a machine this does not have. */
    if (at + sizeof(struct mcfg_entry) <= end) {
        struct mcfg_entry entry;

        memcpy(&entry, at, sizeof(entry));
        ecam = entry.base;
    }
}

/*
 * The pointers in the XSDT are 64 bit and the array starts at an offset of
 * 36, which is four-byte aligned and not eight - so every one of them is
 * unaligned by construction. `memcpy` rather than a cast, because a cast
 * would be undefined behaviour that happens to work on this architecture
 * and would stop working on the other one.
 */
static void walk(uintptr_t address, bool wide)
{
    const struct sdt *root = (const struct sdt *)address;
    uint32_t entries;
    uint32_t i;

    if (root->length < sizeof(*root) || !sums_to_zero(root, root->length)) {
        return;
    }

    entries = (root->length - (uint32_t)sizeof(*root)) / (wide ? 8u : 4u);

    for (i = 0; i < entries; i++) {
        const uint8_t *slot = (const uint8_t *)root + sizeof(*root)
                            + i * (wide ? 8u : 4u);
        uint64_t where = 0;
        const struct sdt *table;

        if (wide) {
            memcpy(&where, slot, 8);
        } else {
            uint32_t narrow;

            memcpy(&narrow, slot, 4);
            where = narrow;
        }

        if (where == 0) {
            continue;
        }

        table = (const struct sdt *)(uintptr_t)where;

        if (table->length < sizeof(*table)
            || !sums_to_zero(table, table->length)) {
            continue;
        }

        if (signature_is(table->signature, "APIC", 4)) {
            read_madt((const struct madt *)table);
        } else if (signature_is(table->signature, "MCFG", 4)) {
            read_mcfg(table);
        }
    }
}

bool acpi_init(void)
{
    const struct rsdp *r;

    /*
     * Idempotent, because there are two callers now and neither can know
     * whether it is first: `cpus.c` wants the processor count and `apic.c`
     * wants the controller addresses, and which runs first depends on the
     * board's boot order rather than on anything either of them decides.
     *
     * Without this the second call walks the MADT again and counts every
     * processor twice, which is a machine that reports eight cores and has
     * four.
     */
    if (found) {
        return true;
    }

    r = find_rsdp();

    if (r == NULL) {
        return false;
    }

    /*
     * The XSDT when there is one, and it is not merely the newer of two
     * equivalents: the RSDT's pointers are 32 bits, so a table above 4 GB
     * cannot be named in it at all. A machine that has both is telling you
     * which one it means by its revision.
     */
    if (r->revision >= 2 && r->xsdt != 0) {
        walk((uintptr_t)r->xsdt, true);
    } else if (r->rsdt != 0) {
        walk((uintptr_t)r->rsdt, false);
    } else {
        return false;
    }

    found = true;
    return true;
}

unsigned acpi_cpu_count(void)
{
    return found ? cpus : 0u;
}

uint64_t acpi_lapic_base(void)
{
    return lapic;
}

uint64_t acpi_ioapic_base(void)
{
    return ioapic;
}

unsigned acpi_overrides(struct acpi_override *out, unsigned max)
{
    unsigned i;

    if (!found) {
        return 0;
    }

    for (i = 0; i < override_count && i < max; i++) {
        out[i] = overrides[i];
    }

    return i;
}

uint64_t acpi_ecam_base(void)
{
    return ecam;
}
