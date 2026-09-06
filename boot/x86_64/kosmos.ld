/*
 * Where an x86-64 Kosmos image goes.
 *
 * 1 MB, because that is where a multiboot loader puts a kernel and below it
 * is where the PC's own past lives: the BIOS data area, the VGA window, the
 * option ROMs. Nothing here is relocatable, and nothing needs to be while
 * there is one kernel in the machine.
 *
 * `.multiboot` is first and that is not tidiness: QEMU looks for the header
 * in the first 8 KB of the file and refuses to boot without it, with a
 * message about the format rather than about the header.
 */

ENTRY(_start)

SECTIONS
{
    . = 1M;

    __load_start = .;
    __multiboot_header = .;

    .multiboot : { KEEP(*(.multiboot)) }

    /*
     * Each region page aligned and bracketed, because `mmu.c` narrows the
     * permissions on them afterwards and a page is the smallest thing a
     * permission can be attached to. Text that shares a page with .data is
     * text that has to be writable.
     */
    .text : ALIGN(4096) {
        __text_start = .;
        *(.text.boot)
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    }

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    }

    .data   : ALIGN(4096) { *(.data .data.*) }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }
    . = ALIGN(4096);
    __bss_end = .;

    /*
     * The boot stack, and the unmapped page beneath it.
     *
     * It was 16 KB inside .bss, declared in `start.S`, which works right up
     * until it does not: an overflow there walks quietly backwards into the
     * page tables. Here it gets what `boot/kosmos.ld` gives the ARM side -
     * a guard page that `mmu.c` deliberately leaves out of the map, so an
     * overflow is a page fault naming the address rather than a machine
     * that starts behaving strangely several seconds later.
     *
     * NOLOAD: it costs the image file nothing, and nothing in it needs to
     * survive being uninitialised. It is still inside __image_end, so the
     * page allocator treats it as taken.
     */
    .stack (NOLOAD) : ALIGN(4096) {
        __stack_guard = .;
        . += 4096;

        __stack_bottom = .;
        . += 16384;
        __stack_top = .;
    }

    . = ALIGN(4096);
    __image_end = .;

    /* The old name for it, kept because `start.S` and the multiboot header
     * both measure from it. */
    __kernel_end = __image_end;
}
