/*
 * Kosmos linker script — QEMU virt, aarch64.
 *
 * QEMU virt puts RAM at 0x40000000 and parks the device tree in the first
 * pages. 0x40080000 is the conventional load address for an aarch64 kernel
 * on this machine, and it leaves that region alone.
 *
 * The alignment here is not cosmetic. Two things depend on it:
 *
 *   - __bss_start and __bss_end are 16-byte aligned, so start.S can zero
 *     the section 16 bytes at a time with `stp` and no tail to handle.
 *   - The boot stack lives above __bss_end, so zeroing .bss cannot walk
 *     into the stack that has already been set up. That collision is the
 *     classic way this file goes wrong, and it fails silently.
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    __image_start = .;

    .text : ALIGN(16) {
        /* The reset vector has to be the first thing in the image. */
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(16) {
        *(.data .data.*)
    }

    /*
     * NOLOAD: .bss takes no space in the file. It is zeroed by start.S,
     * which is why it needs the two symbols below.
     */
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;

        /* The boot stack. 16 KB, above .bss and never zeroed by the loop. */
        __stack_bottom = .;
        . = . + 0x4000;
        __stack_top = .;
    }

    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
