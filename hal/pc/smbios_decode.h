/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What the firmware says the machine is, as arithmetic over bytes.
 *
 * SMBIOS is the table a PC's firmware keeps about itself: who made the
 * machine, what it is sold as, which board and which memory are in it. It is
 * where Linux's `/sys/class/dmi/id` comes from, and so it is where neofetch's
 * "Host" comes from on every PC it has run on. This half has no hardware in
 * it - `smbios.c` finds the table and hands the bytes here - for the reason
 * `apic_decode.c` gives: the tables that matter are the ones no emulator
 * here produces, so the awkward cases are asked on the host, in
 * `tools/test_smbiosdecode.c`.
 *
 * DMTF DSP0134, the System Management BIOS Reference Specification. The
 * offsets are its, and each is written beside the field it reads. They were
 * written from knowledge of the specification rather than from a copy of it,
 * and are held to what firmware actually produced rather than to memory:
 * SeaBIOS and OVMF, both of which put their tables where a real machine's
 * firmware does.
 */
#ifndef KOSMOS_HAL_PC_SMBIOS_DECODE_H
#define KOSMOS_HAL_PC_SMBIOS_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Where the structure table is, as an entry point says.
 *
 * `length` is exact for a 2.1 entry point and only a ceiling for a 3.0 one,
 * whose table ends at its end-of-table structure instead - which is why the
 * walk below stops at either.
 */
struct smbios_table {
    uint64_t address;
    uint32_t length;
    uint8_t  major;
    uint8_t  minor;
};

/*
 * An entry point, from the bytes at `at`, of which `avail` may be read.
 *
 * Either anchor: `_SM3_`, the 3.0 entry point, or `_SM_`, the 2.1 one. False
 * when neither is there, when there are fewer bytes than the structure needs,
 * or when a checksum does not hold - which is what separates an entry point
 * from four bytes that happen to spell one.
 */
bool smbios_entry(const uint8_t *at, size_t avail, struct smbios_table *out);

/*
 * Three strings from System Information, the type 1 structure.
 *
 * Copied as the firmware wrote them, with one change: a control character
 * becomes `?`, because these go into the boot log and a newline inside a
 * manufacturer's name is a boot log with a line nobody printed. Bytes above
 * 0x7F are kept, since a later specification allows UTF-8. Cut to fit, and
 * empty when the firmware gave no string.
 */
#define SMBIOS_TEXT 64

struct smbios_system {
    char manufacturer[SMBIOS_TEXT];
    char product[SMBIOS_TEXT];
    char version[SMBIOS_TEXT];
};

/*
 * System Information out of a structure table of `length` bytes.
 *
 * False when there is none before the end-of-table structure or the end of
 * the bytes, and false when a structure before it is malformed - a length
 * too short to be a header, or a string set with no end - because the walk
 * cannot find the next structure past one it cannot measure.
 */
bool smbios_system(const uint8_t *table, size_t length,
                   struct smbios_system *out);

#endif
