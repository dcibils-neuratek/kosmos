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

    .text : {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : {
        *(COMMON)
        *(.bss .bss.*)
    }
    . = ALIGN(4096);
    __bss_end = .;

    __kernel_end = .;
}
