/*
 * Kosmos entry point, x86-64.
 *
 * The first instructions that run on a PC, and the shape of the problem is
 * different from AArch64's in one way that decides everything else here:
 * **the machine does not start in the mode the kernel runs in.** ARM hands
 * you 64-bit execution and asks which privilege level you would like.
 * An x86 boots in 16-bit real mode, a multiboot loader has already put it
 * into 32-bit protected mode, and long mode has to be built by hand from
 * there - page tables, a GDT, three control-register bits and a far jump,
 * in that order, before a single 64-bit instruction may be executed.
 *
 * Nothing here may touch the stack before `esp` is set, for the same reason
 * `boot/start.S` says so on ARM: a prologue that spills a register writes
 * to an address nothing maps, and the fault has no handler yet.
 *
 * QEMU loads this with `-kernel`, which on x86-64 means it looks for a
 * multiboot header in the first 8 KB. That is why the header is first and
 * why it is aligned.
 */

/*
 * The multiboot header, with the a.out kludge - and the kludge is not
 * optional here.
 *
 * A multiboot loader parses an **ELF32** image. This kernel is ELF64, and
 * QEMU says so plainly: "Cannot load x86-64 image, give a 32bit one." Bit
 * 16 of the flags is the way out: it tells the loader to stop parsing the
 * format and use the five addresses below instead, at which point the file
 * can be a flat binary and its bitness stops being the loader's business.
 *
 * That is why the build runs `objcopy -O binary` - and why `.multiboot`
 * must be the first thing in the image, since `header_addr` says where the
 * header is and everything else is measured from the load address.
 */
#define MB_MAGIC     0x1BADB002
#define MB_FLAGS     0x00010002          /* memory map | a.out kludge */
#define MB_CHECKSUM  -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM
    .long __multiboot_header          /* where this header is */
    .long __load_start                /* where the image begins */
    .long 0                           /* 0: load to the end of the file */
    .long __bss_end                   /* zero up to here */
    .long _start                      /* and begin here */

/*
 * The page tables that get us into long mode.
 *
 * One gigabyte, identity mapped, with 2 MB pages: enough to run the kernel
 * and reach every device QEMU puts below 1 GB, and small enough to be three
 * static tables rather than an allocator that does not exist yet. `mmu.c`
 * builds the real address space later, from C, with four levels.
 *
 * 4096-byte alignment is architectural: the low twelve bits of a table
 * entry are flags, so a table that is not page-aligned has its address
 * silently truncated into them.
 */
.section .bss, "aw", @nobits
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096

.align 16
stack_bottom:
    .skip 16384
stack_top:

.section .rodata
.align 16
/*
 * The GDT, which long mode still needs even though it no longer segments.
 *
 * Three entries: the null one the architecture requires, a 64-bit code
 * segment, and a data segment. The base and limit fields are ignored in
 * long mode - what matters is bit 53 (L, "this is 64-bit code") in the code
 * descriptor, and setting it is what makes the far jump below land in
 * 64-bit mode rather than in 32-bit compatibility mode.
 */
gdt:
    .quad 0                                   /* null */
    .quad 0x00AF9A000000FFFF                  /* code: L=1, DPL=0 */
    .quad 0x00CF92000000FFFF                  /* data */
gdt_end:

gdt_pointer:
    .word gdt_end - gdt - 1
    .quad gdt

.section .text.boot, "ax"
.code32
.global _start

_start:
    cli
    movl    $stack_top, %esp

    /* Multiboot leaves the information structure in ebx. Parked in esi,
     * which nothing below touches: edi is the destination register for
     * both the table fill and `rep stos`, and ecx is their counter. */
    movl    %ebx, %esi

    /*
     * .bss, zeroed - and it happens *here*, before the page tables are
     * built, because the page tables are in .bss.
     *
     * Zeroing it afterwards was the first version and it is a machine that
     * dies a few instructions into long mode: `rep stos` walks over pml4,
     * pdpt and pd while the processor is translating through them, and what
     * kills it is not the write but the first TLB miss after it.
     *
     * The loader does not do this. A multiboot image is a flat file and
     * .bss is the part of it that was never in the file.
     */
    movl    $__bss_start, %edi
    movl    $__bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    rep stosb

    movl    $stack_top, %esp        /* the stack was in what we just zeroed */

    /*
     * Three tables, identity mapping the first gigabyte.
     *
     * PML4[0] -> PDPT, PDPT[0] -> PD, and PD's 512 entries each map 2 MB
     * with PS=1 (a large page, so there is no fourth level to fill in).
     * Present | writable throughout.
     */
    movl    $pdpt, %eax
    orl     $0x03, %eax
    movl    %eax, pml4

    movl    $pd, %eax
    orl     $0x03, %eax
    movl    %eax, pdpt

    movl    $pd, %edi
    movl    $0, %eax                     /* physical address being mapped */
    movl    $512, %ecx

fill_pd:
    movl    %eax, %edx
    orl     $0x83, %edx                  /* present | write | 2 MB page */
    movl    %edx, (%edi)
    movl    $0, 4(%edi)
    addl    $0x200000, %eax
    addl    $8, %edi
    loop    fill_pd

    /* CR4.PAE. Long mode is only reachable through PAE paging; without
     * this the EFER write below is accepted and the paging enable faults. */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    movl    $pml4, %eax
    movl    %eax, %cr3

    /* EFER.LME - "long mode enabled", which only means *allowed*. The CPU
     * enters it when paging is switched on below. */
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $(1 << 8), %eax
    wrmsr

    /* CR0.PG, and from this instruction the machine is in long mode -
     * running 32-bit compatibility code until something reloads CS. */
    movl    %cr0, %eax
    orl     $(1 << 31), %eax
    movl    %eax, %cr0

    lgdt    gdt_pointer
    ljmp    $0x08, $long_mode

.code64
long_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs

    movq    $stack_top, %rsp
    xorq    %rbp, %rbp

    /* The multiboot pointer, parked in esi at entry, as the first
     * argument. Writing to a 32-bit register zeroes the top half, which is
     * what is wanted: the pointer came from a 32-bit loader. */
    movl    %esi, %edi
    call    kmain_x86

hang:
    cli
    hlt
    jmp     hang
