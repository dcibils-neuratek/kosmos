/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What this machine is called, asked of its firmware.
 *
 * `neofetch` on a ThinkPad T14 said `Host  QEMU q35 x86-64`, and About said
 * the same beside "Platform:". Neither had read anything: the string was the
 * Makefile's, compiled into every PC build, so one image said QEMU on every
 * machine it booted. A PC can say what it is. Its firmware writes the
 * manufacturer, the product and the version into SMBIOS's System Information,
 * and that is where Linux reads the same row from.
 *
 * **Two places the entry point can be, and the firmware decides which.**
 *
 *   - A BIOS leaves it in the read-only area from 0xF0000, on a sixteen-byte
 *     boundary. SeaBIOS does, so QEMU's `-kernel` finds it there.
 *   - UEFI hands it to whatever it launched, in the EFI Configuration Table,
 *     and need leave no copy below 1 MB - the trap `acpi.c` fell into with
 *     the RSDP. A Multiboot 2 loader passes on the EFI System Table's address,
 *     and a GUID in the table's configuration entries says which is SMBIOS's.
 *
 * The EFI side first: on a machine that has both, it is the one the firmware
 * handed over rather than one left behind for older software.
 *
 * **Read once, at the first moment, and copied.** `start.S`'s page tables map
 * the first four gigabytes and `mmu_init` replaces them with tables that map
 * RAM and the devices this kernel drives, so firmware memory can be read
 * during `hal_early_init` and not afterwards. Nothing at or above 4 GB is read
 * at all: it was never mapped, and a table there is reported as out of reach
 * rather than faulted on.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal.h"
#include "multiboot.h"
#include "smbios.h"
#include "smbios_decode.h"
#include "string.h"

#define BIOS_AREA_START 0xf0000u
#define BIOS_AREA_END   0x100000u

/* What `start.S` identity maps, and so the most this can read. */
#define BOOT_MAPPED     0x100000000ull

/*
 * A 3.0 entry point gives the table's maximum size rather than its length,
 * and the walk ends at the end-of-table structure. A megabyte is far past any
 * laptop's table, which is tens of kilobytes, and it bounds what a wrong
 * maximum can make this read.
 */
#define TABLE_MOST      (1024u * 1024u)

/* The larger entry point, 2.1's, and a byte: what is read at an address the
 * EFI table gives, where there is no area to bound it by. */
#define ENTRY_BYTES     0x20u

/*
 * The UEFI System Table on a 64-bit machine: a 24-byte header, whose first
 * eight bytes are the signature and whose HeaderSize is at 12, then
 * FirmwareVendor at 24, FirmwareRevision at 32 padded to eight, the three
 * console handle and protocol pairs, RuntimeServices and BootServices - which
 * puts NumberOfTableEntries at 104 and ConfigurationTable at 112. Each
 * configuration entry is a 16-byte GUID and a pointer.
 *
 * From the UEFI specification's "EFI System Table", written from knowledge of
 * it rather than a copy - and a wrong offset here finds no GUID, so OVMF
 * naming itself through this path is what says they are right.
 */
#define EFI_SYSTEM_SIGNATURE  0x5453595320494249ull     /* "IBI SYST" */
#define EFI_HEADER_SIZE_AT    12u
#define EFI_ENTRIES_AT        104u
#define EFI_TABLE_AT          112u
#define EFI_SYSTEM_BYTES      120u
#define EFI_ENTRY_BYTES       24u

/* A laptop's configuration table has a couple of dozen entries. A count past
 * this is a table that is not one. */
#define EFI_ENTRIES_MOST      256u

/* SMBIOS3_TABLE_GUID and SMBIOS_TABLE_GUID, as they lie in memory: the first
 * three fields little-endian, the last eight in order. */
static const uint8_t smbios3_guid[16] = {
    0x44, 0x15, 0xfd, 0xf2,  0x94, 0x97,  0x2c, 0x4a,
    0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94,
};

static const uint8_t smbios_guid[16] = {
    0x31, 0x2d, 0x9d, 0xeb,  0x88, 0x2d,  0xd3, 0x11,
    0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d,
};

static struct hal_machine machine;
static bool known;

static bool reachable(uint64_t address, uint64_t bytes)
{
    return address != 0 && bytes <= BOOT_MAPPED
        && address <= BOOT_MAPPED - bytes;
}

/* `text` onto the end of a string of `size` bytes, which stays terminated. */
static void append(char *out, size_t size, const char *text)
{
    size_t n = 0;

    while (n < size && out[n] != '\0') {
        n++;
    }

    while (*text != '\0' && n + 1 < size) {
        out[n++] = *text++;
    }

    if (n < size) {
        out[n] = '\0';
    }
}

/* A version number, which is a byte: three digits at most, written backwards
 * from the end of a buffer that holds exactly that. */
static void append_number(char *out, size_t size, unsigned v)
{
    char text[4];
    char *p = text + sizeof(text);

    *--p = '\0';

    do {
        *--p = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0 && p > text);

    append(out, size, p);
}

static void say(const char *why)
{
    machine.source[0] = '\0';
    append(machine.source, sizeof(machine.source), why);
}

/* The configuration entry for `guid`, as the address it points at, or 0. */
static uint64_t efi_entry(uint64_t system, const uint8_t *guid)
{
    const uint8_t *st;
    uint64_t signature, count, table, i;
    uint32_t header_size;

    if (!reachable(system, EFI_SYSTEM_BYTES)) {
        return 0;
    }

    st = (const uint8_t *)(uintptr_t)system;

    memcpy(&signature, st, sizeof(signature));
    memcpy(&header_size, st + EFI_HEADER_SIZE_AT, sizeof(header_size));

    if (signature != EFI_SYSTEM_SIGNATURE || header_size < EFI_SYSTEM_BYTES) {
        return 0;
    }

    memcpy(&count, st + EFI_ENTRIES_AT, sizeof(count));
    memcpy(&table, st + EFI_TABLE_AT, sizeof(table));

    if (count > EFI_ENTRIES_MOST
        || !reachable(table, count * EFI_ENTRY_BYTES)) {
        return 0;
    }

    for (i = 0; i < count; i++) {
        const uint8_t *entry =
            (const uint8_t *)(uintptr_t)(table + i * EFI_ENTRY_BYTES);
        uint64_t where;

        if (memcmp(entry, guid, 16) == 0) {
            memcpy(&where, entry + 16, sizeof(where));
            return where;
        }
    }

    return 0;
}

/*
 * Sixteen bytes at a time through the BIOS area. A 3.0 entry point is taken
 * over a 2.1 one wherever the two are, because only it can name a table above
 * 4 GB - and this still refuses to read one there, but says so rather than
 * reading the older table and calling that the answer.
 *
 * The range is passed rather than written inline for the reason `acpi.c`'s
 * scan gives: a dereference of a constant address is one GCC can see no
 * object behind, and `-Werror=array-bounds` refuses it.
 */
static bool scan(uintptr_t from, uintptr_t to, struct smbios_table *out)
{
    uintptr_t at;
    bool found = false;

    for (at = from; at + 0x18u <= to; at += 16u) {
        const uint8_t *maybe = (const uint8_t *)at;
        struct smbios_table t;

        if (!smbios_entry(maybe, to - at, &t)) {
            continue;
        }

        if (memcmp(maybe, "_SM3_", 5) == 0) {
            *out = t;
            return true;
        }

        if (!found) {
            *out = t;
            found = true;
        }
    }

    return found;
}

void pc_capture_machine(void)
{
    struct smbios_table where;
    struct smbios_system sys;
    const char *via = NULL;
    uint64_t system = pc_loader_efi_system_table();
    uint64_t length;

    memset(&machine, 0, sizeof(machine));
    known = false;

    if (system != 0) {
        uint64_t entry = efi_entry(system, smbios3_guid);

        if (entry == 0) {
            entry = efi_entry(system, smbios_guid);
        }

        if (entry != 0 && reachable(entry, ENTRY_BYTES)
            && smbios_entry((const uint8_t *)(uintptr_t)entry, ENTRY_BYTES,
                            &where)) {
            via = "the EFI system table";
        }
    }

    if (via == NULL && scan(BIOS_AREA_START, BIOS_AREA_END, &where)) {
        via = "the BIOS area";
    }

    if (via == NULL) {
        say(system != 0
            ? "no SMBIOS in the EFI system table or the BIOS area"
            : "no SMBIOS entry point in the BIOS area");
        return;
    }

    length = where.length < TABLE_MOST ? where.length : TABLE_MOST;

    if (!reachable(where.address, length)) {
        say("SMBIOS's table is above 4 GB, which is not mapped at boot");
        return;
    }

    if (!smbios_system((const uint8_t *)(uintptr_t)where.address,
                       (size_t)length, &sys)
        || (sys.manufacturer[0] == '\0' && sys.product[0] == '\0'
            && sys.version[0] == '\0')) {
        say("SMBIOS has no System Information to name it by");
        return;
    }

    _Static_assert(sizeof(machine.vendor) == sizeof(sys.manufacturer)
                   && sizeof(machine.product) == sizeof(sys.product)
                   && sizeof(machine.version) == sizeof(sys.version),
                   "the HAL's names are the decoder's size");

    memcpy(machine.vendor, sys.manufacturer, sizeof(machine.vendor));
    memcpy(machine.product, sys.product, sizeof(machine.product));
    memcpy(machine.version, sys.version, sizeof(machine.version));

    say("SMBIOS ");
    append_number(machine.source, sizeof(machine.source), where.major);
    append(machine.source, sizeof(machine.source), ".");
    append_number(machine.source, sizeof(machine.source), where.minor);
    append(machine.source, sizeof(machine.source), " in ");
    append(machine.source, sizeof(machine.source), via);

    known = true;
}

bool hal_machine_ident(struct hal_machine *out)
{
    if (out == NULL) {
        return false;
    }

    *out = machine;
    return known;
}
