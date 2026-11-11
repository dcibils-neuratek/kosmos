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

/*
 * **Two segments, and the ELF says which is which** (`docs/elf.md`). The
 * code and what it only reads, read and execute; the data, read and write.
 * It was one segment with every permission, and the split existed only in
 * the header's `rx_bytes` below - which is all the kernel reads, so nothing
 * noticed until a program's image came off a disk and its reader wanted to
 * refuse a segment that was writable and executable at once. The flat
 * binary `objcopy` makes is the same bytes either way: this names the
 * segments and moves nothing.
 */
PHDRS
{
    code PT_LOAD FLAGS(5);              /* read and execute */
    data PT_LOAD FLAGS(6);              /* read and write */
    tls  PT_TLS  FLAGS(4);              /* the template a thread's copy is made from */
}

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
    } :code

    .rodata : {
        *(.rodata .rodata.*)
        /* The template's alignment as a word to read, not a symbol's value:
         * AArch64 reaches a symbol by its distance from the code, and a
         * small absolute number is four gigabytes and more away from an
         * image at 256 GB. */
        . = ALIGN(8);
        __tls_align = .;
        QUAD(ALIGNOF(.tdata) > ALIGNOF(.tbss) ? ALIGNOF(.tdata) : ALIGNOF(.tbss))
    } :code

    /*
     * **The unwinding tables, kept** (`docs/webkit.md` W1b). They were
     * discarded, because nothing here unwound a stack; C++ does, every time
     * it throws, and libunwind's bare-metal build finds them by these four
     * names rather than by asking a dynamic linker. Kept in every image,
     * not only C++'s: it is half a megabyte of read-only pages, shared by
     * every process, against a second linker script to keep in step.
     */
    .eh_frame_hdr : {
        __eh_frame_hdr_start = .;
        *(.eh_frame_hdr)
        __eh_frame_hdr_end = .;
    } :code
    .eh_frame : {
        __eh_frame_start = .;
        KEEP(*(.eh_frame))
        __eh_frame_end = .;
    } :code

    /* Everything above this is mapped read-only and executable, everything
     * below it writable and never executable. */
    . = ALIGN(4096);
    __rx_end = .;

    /*
     * **A thread's own variables' template** (`thread_local`, `_Thread_local`):
     * `.tdata` is what each copy starts as and `.tbss` how much more is
     * zeroes. The runtime makes a thread's copy beside its `struct
     * tls_block` (`runtime/libc/tls.c`); nothing reads these addresses but
     * that, and a thread's variables never live here.
     */
    .tdata : {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    } :data :tls
    .tbss : {
        *(.tbss .tbss.*)
        *(.tcommon)
        __tbss_end = .;
    } :data :tls

    /*
     * **Constructors, run before `main` asks anything of the image**
     * (`user/init/main.c`, after the heap is ready). C++ needs them for
     * every global with a constructor; C has them too, as
     * `__attribute__((constructor))`.
     */
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    } :data
    .fini_array : {
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array .dtors))
    } :data

    .data   : { *(.data .data.*) } :data

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
     * The heap is a gigabyte above `USER_BASE` (`USER_IMAGE_MAX`,
     * `kernel/process.h`): an image has that gigabyte to itself, the most
     * x86-64's small code model allows when images start at 1 GB. It was
     * 16, 32, 40 and 42 MB, moved each time an image ran into it, until
     * JetSki's WebKit made a fixed line no answer (`docs/webkit.md` W1).
     */
    ASSERT(__image_end <= USER_BASE + 0x40000000,
           "the userland image is larger than the gigabyte it has (USER_IMAGE_MAX, kernel/process.h)")

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
    }
}
