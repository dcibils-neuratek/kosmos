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

        /*
         * And the stack an exception lands on, which has to be a different
         * one from the stack being interrupted.
         *
         * `boot/kosmos.ld` gives AArch64 the same thing for the same
         * reason, and gets it for free: that kernel runs on SP_EL0, so
         * every exception switches to SP_EL1 and lands here. **x86 has no
         * such split.** A fault from ring 0 stays on the stack that
         * faulted unless the gate names an entry in the interrupt stack
         * table, and every gate named none - so a kernel stack overflow
         * pushed the fault frame onto the guard page, faulted again,
         * failed to deliver *that*, and triple-faulted the machine.
         *
         * `trap.c` puts #PF and #DF on this. Its own guard page below it,
         * because a handler that overflows its own stack should fault
         * rather than walk into the stack it was called to report on.
         */
        . = ALIGN(4096);
        __exception_guard = .;
        . += 4096;

        __exception_stack_bottom = .;
        . += 16384;
        __exception_stack_top = .;
    }

    /*
     * The framebuffer, and why it is here rather than in .bss.
     *
     * It is several megabytes. Put in .bss it would sit before the stack
     * and push the guard page past the first 2 MB of RAM - and the low part
     * of RAM is the only part `mmu_init` maps a page at a time, so a guard
     * inside a 2 MB page cannot be punched out. `boot/kosmos.ld` says the
     * same thing about the same section for the same reason.
     *
     * NOLOAD, so it costs the image file nothing. It is inside
     * __image_end, so the page allocator treats it as taken.
     */
    .framebuffer (NOLOAD) : ALIGN(4096) {
        __framebuffer_start = .;
        *(.framebuffer)
        . = ALIGN(4096);
        __framebuffer_end = .;
    }

    . = ALIGN(4096);
    __image_end = .;

    /* The old name for it, kept because `start.S` and the multiboot header
     * both measure from it. */
    __kernel_end = __image_end;
}
