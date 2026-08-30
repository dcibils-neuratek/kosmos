/*
 * Kosmos linker script — QEMU virt, aarch64.
 *
 * QEMU virt puts RAM at 0x40000000 and parks the device tree in the first
 * pages. 0x40080000 is the conventional load address for an aarch64 kernel
 * on this machine, and it leaves that region alone.
 *
 * Sections are page aligned and bracketed by symbols so the MMU can give
 * each one the permissions it should have: text executable and read-only,
 * rodata read-only and never executable, everything else writable and never
 * executable. Without the alignment those permissions cannot be expressed,
 * because the smallest thing a page table can describe is a page.
 *
 * Two alignments carry real weight:
 *
 *   - __bss_start and __bss_end are 16-byte aligned, so start.S can zero the
 *     section 16 bytes at a time with `stp` and no tail to handle.
 *   - The boot stack sits above .bss behind an unmapped guard page, so
 *     zeroing .bss cannot walk into a live stack and a stack overflow faults
 *     instead of quietly eating whatever is underneath.
 */

ENTRY(_start)

PAGE_SIZE = 0x1000;

SECTIONS
{
    . = 0x40080000;

    __image_start = .;

    .text : ALIGN(PAGE_SIZE) {
        __text_start = .;
        /* The reset vector has to be the first thing in the image. */
        KEEP(*(.text.boot))
        *(.text .text.*)
        . = ALIGN(PAGE_SIZE);
        __text_end = .;
    }

    /*
     * Code meant to run at EL0, kept apart from everything else.
     *
     * It is never mapped from here: a process gets a copy in pages of its
     * own, so it can be given EL0 permissions without the kernel's image
     * being given them too. This section exists so the blob is contiguous,
     * page aligned, and has symbols marking its ends.
     */
    .user : ALIGN(PAGE_SIZE) {
        __user_start = .;
        KEEP(*(.user.text))
        . = ALIGN(PAGE_SIZE);
        __user_end = .;
    }

    .rodata : ALIGN(PAGE_SIZE) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(PAGE_SIZE);
        __rodata_end = .;
    }

    .data : ALIGN(PAGE_SIZE) {
        *(.data .data.*)
    }

    /*
     * NOLOAD: .bss takes no space in the file. It is zeroed by start.S,
     * which is why it needs the two symbols below.
     */
    .bss (NOLOAD) : ALIGN(PAGE_SIZE) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;

        /*
         * The stack guard. One page, deliberately left out of the page
         * tables, so a stack overflow is a data abort naming the address
         * instead of silent corruption of whatever sits below.
         */
        . = ALIGN(PAGE_SIZE);
        __stack_guard = .;
        . = . + PAGE_SIZE;

        /* The boot stack, on SP_EL0. 16 KB. */
        __stack_bottom = .;
        . = . + 0x4000;
        __stack_top = .;

        /*
         * The exception stack, on SP_EL1, with its own guard page.
         *
         * It has to be a different stack from the one being interrupted. The
         * handler builds a 288-byte frame the moment it is entered, so if it
         * ran on the thread's stack, an exception caused by that stack being
         * exhausted would fault again inside the vector. That is a double
         * fault: the kernel hangs with nothing printed, which is the exact
         * failure the whole exception dump exists to eliminate.
         */
        . = ALIGN(PAGE_SIZE);
        __exception_guard = .;
        . = . + PAGE_SIZE;

        __exception_stack_bottom = .;
        . = . + 0x4000;
        __exception_stack_top = .;
    }

    /*
     * The framebuffer, and why it is here rather than in .bss.
     *
     * It is three megabytes. Put in .bss it would sit before the stacks and
     * push both guard pages past the first 2 MB of RAM - and the first 2 MB
     * is the only part mapped a page at a time. A guard page inside a 2 MB
     * block cannot be punched out, so `mmu_init` would panic. Putting it
     * last means adding or resizing it moves nothing that matters.
     *
     * NOLOAD, so it costs the image file nothing. It is inside __image_end,
     * so the page allocator counts it as the kernel's and never hands it
     * out. And it is not between __bss_start and __bss_end, so start.S does
     * not zero it; hal_fb_init does, once, and only if there is a display.
     */
    .framebuffer (NOLOAD) : ALIGN(PAGE_SIZE) {
        __framebuffer_start = .;
        *(.framebuffer)
        . = ALIGN(PAGE_SIZE);
        __framebuffer_end = .;
    }

    . = ALIGN(PAGE_SIZE);
    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
