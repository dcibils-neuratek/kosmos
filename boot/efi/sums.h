/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Whether a file read off a stick is the file the build wrote.
 *
 * The loader reads the kernel and the disk once and fingerprints what it
 * read, which says whether memory kept those bytes - and nothing about
 * whether the stick handed over the build's. A stick returning other bytes
 * passed every check the loader had, and nothing the ThinkPad has shown
 * rules that out (`docs/boot.md`). So `tools/mkusb_image.py` writes a sums
 * file beside each file it puts on a stick, and this compares a read with
 * it, page by page.
 *
 * A sums file, all of it little-endian:
 *
 *     0   "KOSMSUMS"
 *     8   the file's size in bytes, as the build wrote it
 *     16  the page size, 4096
 *     24  FNV-1a, 64 bits, over each page in turn - the last one short
 *
 * Here, with no firmware in reach, so the host test can hold it to account
 * the way it holds `mbi.c` (`tools/test_efiboot.c`).
 */
#ifndef KOSMOS_BOOT_EFI_SUMS_H
#define KOSMOS_BOOT_EFI_SUMS_H

#include <stddef.h>
#include <stdint.h>

/* "KOSMSUMS", as the little-endian word its eight bytes make. */
#define SUMS_MAGIC      0x534d55534d534f4bull
#define SUMS_HEADER     24u
#define SUMS_PAGE       4096u

enum sums_verdict {
    SUMS_SAME,          /* every page is the build's */
    SUMS_DIFFER,        /* `wrong` pages are not, the first of them `first` */
    SUMS_SIZE,          /* the file is not the size the build wrote */
    SUMS_MALFORMED      /* this is not a sums file for pages of 4096 */
};

struct sums_result {
    enum sums_verdict verdict;
    uint64_t pages;         /* in the file that was read */
    uint64_t wrong;
    uint64_t first;
    uint64_t built;         /* the size the sums file says the build wrote */
};

/* FNV-1a, 64 bits: the loader's fingerprint, and `mkusb_image.py`'s. */
uint64_t sums_fnv(const uint8_t *bytes, uint64_t size);

void sums_check(const uint8_t *data, uint64_t size,
                const uint8_t *sums, uint64_t sums_size,
                struct sums_result *out);

#endif
