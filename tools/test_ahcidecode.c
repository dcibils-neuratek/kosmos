/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What the AHCI driver sends a disk and reads from it, on the host -
 * `user/drivers/storage/ahci_decode.c`.
 *
 * The bytes below are worked out from the standards `ahci_decode.h` names,
 * not from the code: a FIS the code built wrongly and a test that copied
 * the code would agree with each other and with nothing else.
 *
 * Usage: test_ahcidecode
 */

#include <stdio.h>
#include <string.h>

#include "ahci_decode.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("  %s\n", what);
    }
}

/* An IDENTIFY page's word `n`, little-endian. */
static void put_word(uint8_t *page, unsigned n, uint16_t v)
{
    page[2u * n] = (uint8_t)v;
    page[2u * n + 1u] = (uint8_t)(v >> 8);
}

/* A string as IDENTIFY holds one: two characters a word, the first high. */
static void put_string(uint8_t *page, unsigned first, unsigned words, const char *s)
{
    unsigned i;
    size_t n = strlen(s);

    for (i = 0; i < words; i++) {
        uint8_t hi = (2u * i < n) ? (uint8_t)s[2u * i] : ' ';
        uint8_t lo = (2u * i + 1u < n) ? (uint8_t)s[2u * i + 1u] : ' ';

        put_word(page, first + i, (uint16_t)(hi << 8 | lo));
    }
}

int main(void)
{
    uint8_t fis[AHCI_FIS_BYTES], page[512], sector[512];
    struct ahci_identity id;

    /* READ DMA EXT of 8 sectors at 0x123456789A (SATA 3.x §10.5.6). */
    {
        static const uint8_t want[AHCI_FIS_BYTES] = {
            0x27, 0x80, 0x25, 0x00,  0x9A, 0x78, 0x56, 0x40,
            0x34, 0x12, 0x00, 0x00,  0x08, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };

        ahci_fis_command(fis, ATA_READ_DMA_EXT, 0x123456789Aull, 8);
        check(memcmp(fis, want, sizeof want) == 0, "READ DMA EXT's FIS is not the standard's bytes");
    }

    /* 65536 sectors is a count of 0, and the top of 48 bits is kept. */
    ahci_fis_command(fis, ATA_WRITE_DMA_EXT, 0xFFFFFFFFFFFFull, 65536);
    check(fis[2] == 0x35 && fis[12] == 0 && fis[13] == 0 && fis[10] == 0xFF && fis[9] == 0xFF,
          "WRITE DMA EXT of 65536 sectors at the last LBA 48 reaches is not count 0 at FFFF FFFF FFFF");

    /* The header: five words of FIS, W, one PRDT entry (AHCI 1.3.1 §4.2.2). */
    check(ahci_header_word(false, 1) == 0x00010005u, "a read's command header word is not 0x00010005");
    check(ahci_header_word(true, 1) == 0x00010045u, "a write's command header word is not 0x00010045");
    check(ahci_prdt_count(4096) == 4095u, "4096 bytes is not a DBC of 4095");
    check(ahci_prdt_count(4u * 1024u * 1024u) == 0x3FFFFFu, "4 MB is not the largest DBC, 0x3FFFFF");

    /* A disk's IDENTIFY: QEMU's model, LBA 48, 131072 sectors of 512. */
    memset(page, 0, sizeof page);
    put_string(page, 10, 10, "QM00001");
    put_string(page, 27, 20, "QEMU HARDDISK");
    put_word(page, 60, 0);
    put_word(page, 61, 0x0002);         /* LBA 28: 131072 */
    put_word(page, 83, 1u << 10);       /* LBA 48 supported */
    put_word(page, 100, 0);
    put_word(page, 101, 0x0002);        /* LBA 48: 131072 */
    check(ahci_identity(page, &id), "a disk's IDENTIFY page was refused");
    check(strcmp(id.model, "QEMU HARDDISK") == 0, "the model was not read back, swapped and trimmed");
    check(strcmp(id.serial, "QM00001") == 0, "the serial was not read back");
    check(id.lba48 && id.sectors == 131072u && id.sector_bytes == 512u,
          "131072 sectors of 512, by LBA 48, were not read");

    /* A 4K-sector disk: word 106 valid, bit 12, 2048 words a sector. */
    put_word(page, 106, 0x4000u | (1u << 12));
    put_word(page, 117, 2048);
    put_word(page, 118, 0);
    check(ahci_identity(page, &id) && id.sector_bytes == 4096u,
          "a logical sector of 2048 words was not 4096 bytes");

    /* Word 106 not valid (bits 15:14 not 01): 512, whatever else it says. */
    put_word(page, 106, 0xC000u | (1u << 12));
    check(ahci_identity(page, &id) && id.sector_bytes == 512u,
          "an invalid word 106 was believed");

    /* LBA 28 only. */
    put_word(page, 83, 0);
    put_word(page, 106, 0);
    check(ahci_identity(page, &id) && !id.lba48 && id.sectors == 131072u,
          "a disk without LBA 48 was not sized by words 60-61");

    /* An ATAPI page (word 0 bit 15) is not a disk's. */
    put_word(page, 0, 0x8580u);
    check(!ahci_identity(page, &id), "an ATAPI device's page was taken for a disk's");

    /* GPT's signature at the start of LBA 1 (UEFI 2.10 §5.3.2). */
    memset(sector, 0, sizeof sector);
    memcpy(sector, "EFI PART", 8);
    check(ahci_is_gpt_header(sector), "\"EFI PART\" was not a GPT header");
    sector[7] = 'X';
    check(!ahci_is_gpt_header(sector), "a sector not beginning \"EFI PART\" was a GPT header");

    if (fails != 0) {
        printf("FAIL: %d of %d checks on the AHCI driver's FIS and IDENTIFY\n", fails, checks);
        return 1;
    }

    printf("PASS: %d checks on the AHCI driver's FIS and IDENTIFY (READ and WRITE DMA EXT "
           "byte for byte, the command header and PRDT, a disk's model, serial and size by "
           "LBA 48 and 28, a 4K sector, an ATAPI page refused, GPT's signature)\n", checks);
    return 0;
}
