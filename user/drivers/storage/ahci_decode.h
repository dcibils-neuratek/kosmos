/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_STORAGE_AHCI_DECODE_H
#define KOSMOS_DRIVERS_STORAGE_AHCI_DECODE_H

/*
 * What a SATA controller and the disks behind it say, for the AHCI driver
 * in `ahci.c` (`docs/roadmap.md`, *SATA (AHCI)*): the command FIS it builds
 * and the IDENTIFY DEVICE page a disk answers with.
 *
 * Its own file, with no hardware and no system calls in it, as
 * `e1000_decode.c` is: a FIS built a byte wrong is a disk that reads the
 * wrong sector, and the page a disk returns is bytes it chose. Neither can
 * be made to go wrong on demand on a machine; `tools/test_ahcidecode.c`
 * holds both.
 *
 * **The references**: Serial ATA AHCI 1.3.1 for the command header, the
 * command table and its PRDT; SATA 3.x §10.5.6 for the Register Host to
 * Device FIS; ATA/ATAPI Command Set (ACS-3) §7 for the commands and
 * IDENTIFY DEVICE's words. Checked against QEMU's `hw/ide/ahci-internal.h`,
 * whose controller the suite drives.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AHCI_FIS_H2D            0x27u   /* Register FIS, host to device */
#define AHCI_FIS_BYTES          20u     /* its five words */

#define ATA_IDENTIFY            0xECu
#define ATA_READ_DMA_EXT        0x25u
#define ATA_WRITE_DMA_EXT       0x35u
#define ATA_FLUSH_CACHE_EXT     0xEAu

#define AHCI_SIG_DISK           0x00000101u   /* PxSIG for an ATA disk */

/*
 * A Register FIS that issues `command` for `count` sectors from `lba` -
 * LBA 48, so `lba` is up to 2^48 and `count` up to 65536 (0 meaning that,
 * as the standard has it). `out` is the FIS's 20 bytes.
 */
void ahci_fis_command(uint8_t out[AHCI_FIS_BYTES], uint8_t command,
                      uint64_t lba, uint32_t count);

/*
 * A command header's first word (AHCI 1.3.1 §4.2.2): the FIS's length in
 * words, whether the data goes to the device, and how many PRDT entries.
 */
uint32_t ahci_header_word(bool write, unsigned prdt_entries);

/* A PRDT entry's last word: `bytes` to move (even, at most 4 MB), and no
 * interrupt of its own. */
uint32_t ahci_prdt_count(uint32_t bytes);

/* What IDENTIFY DEVICE says about a disk. */
struct ahci_identity {
    char     model[41];         /* words 27-46, its bytes swapped back, spaces trimmed */
    char     serial[21];        /* words 10-19, the same */
    uint64_t sectors;           /* LBA 48's count (words 100-103), else LBA 28's */
    uint32_t sector_bytes;      /* logical sector size: 512 unless word 106 says */
    bool     lba48;
};

/* The 512-byte page read into `out`; false for one that is not a disk's. */
bool ahci_identity(const uint8_t page[512], struct ahci_identity *out);

/* Whether a 512-byte sector is a GPT header ("EFI PART" at its start). */
bool ahci_is_gpt_header(const uint8_t sector[512]);

#endif
