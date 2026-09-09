/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where the page bitmap goes, asked the questions firmware will ask.
 *
 * **The case that matters cannot be produced under QEMU**, and that is why
 * this file exists rather than a boot test. `pmm_init` put its bitmap
 * immediately after the kernel image and panicked if the memory the board
 * reported began above that - which is right for every machine whose
 * largest usable block is the one the kernel was loaded into, and that is
 * what `-kernel` gives.
 *
 * Firmware promises no such thing. A PC's low memory is carved up by
 * whatever the firmware kept for itself, and the biggest piece left can
 * begin above the image entirely. OVMF does not: measured at ten memory
 * sizes from 512 MB to 32 GB it reports 0x900000 every time, which is below
 * the image end at 0xf98000. So there is no QEMU configuration here that
 * reaches the bug, and a real machine that does would find it with a panic
 * on a boot with no serial port.
 *
 * Same split as `tools/test_loaderfb.c` and for the same reason: the
 * decision is arithmetic, so it is asked on the host where every awkward
 * case is one line.
 */

#include <stdio.h>
#include <string.h>

#include "../kernel/pmm_place.h"

#define PAGE  4096u
#define MB    (1024u * 1024u)

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

int main(void)
{
    struct pmm_layout at;

    /*
     * 1. The ordinary machine, and the one every boot until now has been:
     *    RAM starts at 1 MB, the kernel is at the bottom of it, and the
     *    bitmap goes immediately above the image.
     */
    check(pmm_place(0xf98000, 0x100000, 512u * MB, PAGE, &at),
          "the ordinary layout was refused");
    check(at.base == 0x100000, "the ordinary layout moved the base");
    check(at.bitmap == 0xf98000, "the bitmap is not just above the image");
    check(at.reserved > (0xf98000 - 0x100000) / PAGE,
          "the pages under the bitmap are not reserved");

    /*
     * 2. **The case that panicked.** RAM begins above the image, which is
     *    what firmware that kept the first 32 MB for itself leaves behind.
     *    The bitmap must move up into the range rather than sit outside it.
     */
    check(pmm_place(0xf98000, 32u * MB, 512u * MB, PAGE, &at),
          "a range beginning above the image was refused, which is the "
          "panic this file exists to remove");
    check(at.base == 32u * MB, "the base is not where the board said");
    check(at.bitmap == 32u * MB,
          "the bitmap is outside the memory the board reported");
    check(at.reserved >= 1, "the bitmap's own pages are not reserved");

    /*
     * 3. OVMF's real answer, which is the one every UEFI boot here takes:
     *    the range starts below the image and contains it.
     */
    check(pmm_place(0xf98000, 0x900000, 759u * MB, PAGE, &at),
          "OVMF's own layout was refused");
    check(at.base == 0x900000, "OVMF's base moved");
    check(at.bitmap == 0xf98000,
          "the bitmap is not above the image when the image is inside RAM");

    /*
     * 4. The image entirely above the range. Nothing can be placed: the
     *    kernel is not in this memory and the caller must say so.
     */
    check(!pmm_place(0x40000000, 0x100000, 16u * MB, PAGE, &at),
          "a range that does not reach the image was accepted");

    /*
     * 5. A range too small to hold the structure describing it.
     */
    check(!pmm_place(0x1000, 0x100000, 1u * PAGE, PAGE, &at),
          "a range of one page was accepted, where the bitmap describing it "
          "needs that page");
    check(!pmm_place(0x1000, 0x100000, 0, PAGE, &at),
          "an empty range was accepted");

    /* Two pages is the smallest that works: one for the bitmap, one to
     * hand out. Worth pinning, because the boundary moved once already
     * while this file was being written and the first guess was eight. */
    check(pmm_place(0x1000, 0x100000, 2u * PAGE, PAGE, &at),
          "two pages was refused, and one of them is free");
    check(at.pages == 2 && at.reserved == 1,
          "two pages did not come out as one reserved and one free");

    /*
     * 6. **A base that is not page aligned**, which the multiboot map is
     *    not obliged to give. Everything downstream counts pages from the
     *    base, so it moves up and the size comes down with it - rather than
     *    every index in the allocator being wrong by part of a page for the
     *    life of the machine.
     */
    check(pmm_place(0xf98000, 0x100800, 512u * MB, PAGE, &at),
          "an unaligned base was refused");
    check(at.base == 0x101000, "an unaligned base was not rounded up");
    check((at.pages * PAGE) <= (0x100800 + 512u * MB) - at.base,
          "rounding the base up did not take the size down with it");

    /*
     * 7. The bitmap landing exactly at the bottom of the range, which is
     *    case 2 with the image ending precisely where RAM begins.
     */
    check(pmm_place(32u * MB, 32u * MB, 64u * MB, PAGE, &at),
          "an image ending exactly at the base was refused");
    check(at.bitmap == 32u * MB, "the bitmap moved for no reason");

    /*
     * 8. A page size that is not a power of two, and zero. Neither can
     *    happen from `arch/`; both would produce silent nonsense.
     */
    check(!pmm_place(0x1000, 0x100000, 512u * MB, 0, &at),
          "a page size of zero was accepted");
    check(!pmm_place(0x1000, 0x100000, 512u * MB, 3000, &at),
          "a page size that is not a power of two was accepted");

    if (fails > 0) {
        printf("FAIL: %d of %d checks on where the page bitmap goes.\n",
               fails, fails + checks);
        return 1;
    }

    printf("PASS: %d checks on where the page bitmap goes, on this machine "
           "(the case that matters is one no QEMU configuration here "
           "produces).\n", checks);
    return 0;
}
