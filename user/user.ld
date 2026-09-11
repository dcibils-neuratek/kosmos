/*
 * How a process is laid out.
 *
 * Linked at a fixed address rather than made position independent: a process
 * has its own address space, so there is nothing to collide with and nothing
 * to relocate. `hello.S` had to be PIC because it was a blob copied into the
 * kernel's own map; this is a real image with a real address.
 *
 * The gaps are deliberate. Code, then a hole, then the heap, then a hole,
 * then the stack growing down. A region that runs past its end lands in an
 * unmapped hole and faults, rather than in whatever was next.
 *
 * **Where it starts comes from the Makefile, not from here**, because the
 * two architectures put it in different places and this file has no way to
 * read `USER_VA_BASE` out of `arch/<name>/mmu.h`. `--defsym` is the seam;
 * the Makefile's comment beside it says which header it has to agree with,
 * and `user/init/main.c` would fault immediately if they ever did not.
 */

ENTRY(_start)

SECTIONS
{
    . = USER_BASE;

    .text : {
        __image_start = .;

        /*
         * A sixteen-byte header, and the smallest thing that deserves to be
         * called an image format.
         *
         * The kernel has to know where the read-only half of the image ends,
         * or it can only map the whole thing one way: writable and
         * executable, which throws away W^X inside a process for want of a
         * single number. Inventing two fields is cheaper than giving that up.
         *
         * It grows into something real at M8, when there is a filesystem to
         * load from and more than one thing to load. Until then two fields
         * are exactly what is needed and no more.
         */
        QUAD(0x534f4d534f4b)            /* "KOSMOS", as a magic number */
        QUAD(__rx_end - __image_start)  /* bytes that are read-only + exec */

        KEEP(*(.text.start))
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }

    /* Everything above this is mapped read-only and executable, everything
     * below it writable and never executable. */
    . = ALIGN(4096);
    __rx_end = .;

    .data   : { *(.data .data.*) }

    /*
     * .bss is NOT marked NOLOAD, on purpose.
     *
     * The image is turned into a flat binary with objcopy, which skips
     * anything with no file content. A NOLOAD .bss would simply vanish, and
     * the first global Lua touched would be somewhere past the end of what
     * was loaded. Emitting it as zeroes costs file size and removes a whole
     * class of bug that appears only at run time.
     */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /*
     * One word with contents, after .bss, so the flat binary actually
     * covers it.
     *
     * The comment above says .bss is emitted as zeroes rather than left
     * NOLOAD, and the intent was right, but a section made only of `*(.bss)`
     * is still NOBITS - so objcopy skipped it and the flat binary stopped
     * short of the last global. The kernel maps the image by its *file*
     * length, so those globals were simply not mapped.
     *
     * Nothing failed for a long time because a Lua process keeps almost
     * everything on its heap and had only a few hundred bytes of .bss, all
     * of which happened to fall inside the last mapped page. Adding an
     * eight-kilobyte buffer to sys_user.c pushed one past the end, and what
     * came out was a data abort in init at an address one byte beyond the
     * mapping - which says nothing at all about the cause.
     *
     * A word of real content forces the gap before it to be emitted too, so
     * the file is as long as the memory image and the two cannot drift.
     */
    .image_tail : {
        LONG(0x534f4d4b)            /* "KMOS", so it is recognisable */
    }

    . = ALIGN(4096);
    __image_end = .;

    /*
     * And the image has to end before the heap begins.
     *
     * `USER_HEAP` in `user/include/kosmos.h` is sixteen megabytes above
     * `USER_BASE`, fixed. An image that grew past it would overlap the heap,
     * and the link is the place to find that out rather than whatever the
     * first allocation lands on. The ordinary image is about six megabytes
     * and `MEGA=1`, which carries everything, is the one that gets closest.
     */
    ASSERT(__image_end <= USER_BASE + 0x01000000,
           "the userland image runs into its heap, which starts at USER_BASE + 16 MB (USER_HEAP, user/include/kosmos.h)")

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
    }
}
