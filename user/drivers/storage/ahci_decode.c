/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The FIS, the command header and IDENTIFY DEVICE's page: `ahci_decode.h`
 * says what and from which standard.
 */

#include <string.h>

#include "ahci_decode.h"

void ahci_fis_command(uint8_t out[AHCI_FIS_BYTES], uint8_t command,
                      uint64_t lba, uint32_t count)
{
    memset(out, 0, AHCI_FIS_BYTES);

    out[0] = AHCI_FIS_H2D;
    out[1] = 0x80u;                     /* C: this updates the command register */
    out[2] = command;
    out[3] = 0;                         /* features, 7:0 */

    out[4] = (uint8_t)lba;
    out[5] = (uint8_t)(lba >> 8);
    out[6] = (uint8_t)(lba >> 16);
    out[7] = 0x40u;                     /* device: LBA addressing */
    out[8] = (uint8_t)(lba >> 24);
    out[9] = (uint8_t)(lba >> 32);
    out[10] = (uint8_t)(lba >> 40);
    out[11] = 0;                        /* features, 15:8 */

    out[12] = (uint8_t)count;           /* 65536 is 0, which this gives */
    out[13] = (uint8_t)(count >> 8);
}

uint32_t ahci_header_word(bool write, unsigned prdt_entries)
{
    return (AHCI_FIS_BYTES / 4u)                /* CFL, 4:0 */
           | (write ? (1u << 6) : 0u)           /* W */
           | ((uint32_t)prdt_entries << 16);    /* PRDTL, 31:16 */
}

uint32_t ahci_prdt_count(uint32_t bytes)
{
    return (bytes - 1u) & 0x3FFFFFu;            /* DBC, 21:0, the count less one */
}

/* IDENTIFY's words are little-endian; its strings hold two characters a
 * word with the first in the high byte (ACS-3 §3.4.9). */
static uint16_t word(const uint8_t *page, unsigned n)
{
    return (uint16_t)(page[2u * n] | page[2u * n + 1u] << 8);
}

static void ata_string(const uint8_t *page, unsigned first, unsigned words,
                       char *out)
{
    unsigned i, n = 0;

    for (i = 0; i < words; i++) {
        uint16_t w = word(page, first + i);

        out[n++] = (char)(w >> 8);
        out[n++] = (char)(w & 0xFFu);
    }

    out[n] = '\0';

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) {
        out[--n] = '\0';
    }

    for (i = 0; i < n; i++) {
        if ((unsigned char)out[i] < 0x20u || (unsigned char)out[i] > 0x7Eu) {
            out[i] = '?';
        }
    }
}

bool ahci_identity(const uint8_t page[512], struct ahci_identity *out)
{
    uint16_t general = word(page, 0);
    uint16_t sizes = word(page, 106);
    uint64_t lba28;

    memset(out, 0, sizeof(*out));

    /* Word 0 bit 15 set is an ATAPI device's page, not a disk's. */
    if ((general & 0x8000u) != 0) {
        return false;
    }

    ata_string(page, 27, 20, out->model);
    ata_string(page, 10, 10, out->serial);

    out->lba48 = (word(page, 83) & (1u << 10)) != 0;
    lba28 = (uint64_t)word(page, 60) | (uint64_t)word(page, 61) << 16;

    if (out->lba48) {
        out->sectors = (uint64_t)word(page, 100) | (uint64_t)word(page, 101) << 16
                     | (uint64_t)word(page, 102) << 32 | (uint64_t)word(page, 103) << 48;
    }

    if (out->sectors == 0) {
        out->sectors = lba28;
    }

    /*
     * Word 106 is valid when its bits 15:14 are 01; then bit 12 says the
     * logical sector is longer than 256 words, and words 117-118 say how
     * many words it is.
     */
    out->sector_bytes = 512u;

    if ((sizes & 0xC000u) == 0x4000u && (sizes & (1u << 12)) != 0) {
        uint32_t words = (uint32_t)word(page, 117) | (uint32_t)word(page, 118) << 16;

        if (words >= 256u && words <= 32768u) {
            out->sector_bytes = words * 2u;
        }
    }

    return out->sectors != 0;
}

bool ahci_is_gpt_header(const uint8_t sector[512])
{
    return memcmp(sector, "EFI PART", 8) == 0;
}
