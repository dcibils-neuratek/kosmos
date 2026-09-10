/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
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
/*
 * Memory map, the a.out kludge, and a *video mode*.
 *
 * Bit 2 asks the loader to set a graphics mode and report where it put the
 * framebuffer. On a real PC that is the only way to get a screen: there is
 * no ramfb to point anywhere, and the firmware has already chosen a mode by
 * the time anything of ours runs. A loader that ignores the request simply
 * leaves bit 12 of the info flags clear, and `hal/pc/fb.c` falls back - so
 * asking costs nothing on a machine that will not answer.
 */
#define MB_FLAGS     0x00010006          /* memory map | video | a.out kludge */
#define MB_CHECKSUM  -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECKSUM
    .long __multiboot_header          /* where this header is */
    .long __load_start                /* where the image begins */
    .long 0                           /* 0: load to the end of the file */
    .long __image_end                 /* reserve up to here */
    .long _start                      /* and begin here */

    /*
     * The mode being asked for. Zero for width and height means "whatever
     * you think is best", which is the right answer on a laptop whose panel
     * the firmware already knows about; 32 bits per pixel is not a
     * preference but a requirement, because everything above `struct fb`
     * treats a pixel as one 32-bit word.
     */
    .long 0                           /* 0: a linear framebuffer */
    .long 0                           /* width:  no preference */
    .long 0                           /* height: no preference */
    .long 32                          /* depth */

/*
 * And a Multiboot 2 header beside it, because ACPI is invisible without
 * one.
 *
 * **Not for booting.** Multiboot 1 boots perfectly well under UEFI - that
 * was measured, through GRUB, under OVMF, all the way to a desktop. What it
 * cannot do is carry an *RSDP*, and without one this kernel cannot find
 * ACPI on a UEFI machine at all: `hal/pc/acpi.c` looks in the two places a
 * BIOS leaves the pointer, and UEFI passes it in the EFI Configuration
 * Table and leaves nothing behind. The same image on the same four-
 * processor machine reports four processors and drives the APIC under
 * `-kernel`, and reports one and falls back to the 8259 pair under GRUB.
 *
 * So both headers are here and the loader picks. A Multiboot 1 loader finds
 * the first and nothing changes; a Multiboot 2 loader finds this one and
 * hands over an RSDP, a memory map and a framebuffer in one structure.
 * `start.S` tells them apart by the magic the loader leaves in `eax`.
 *
 * Multiboot specification 2.0, section 3.1. Eight-byte alignment is
 * required rather than conventional, and the header must be inside the
 * first 32 KB - which is why it sits beside the other one at the very
 * front of the image.
 */
#define MB2_MAGIC       0xE85250D6
#define MB2_ARCH_I386   0

.align 8
__multiboot2_header:
    .long MB2_MAGIC
    .long MB2_ARCH_I386
    .long __multiboot2_end - __multiboot2_header
    .long -(MB2_MAGIC + MB2_ARCH_I386 + (__multiboot2_end - __multiboot2_header))

    /*
     * The address tag, which is this image's a.out kludge under another
     * name and needed for the same reason: a multiboot loader parses ELF32,
     * this kernel is ELF64, and the build hands over a flat binary. Five
     * addresses and the format stops being the loader's business.
     */
.align 8
    .short 2                          /* address */
    .short 0
    .long 24
    .long __multiboot2_header         /* where this header is */
    .long __load_start                /* where the image begins */
    .long 0                           /* 0: load to the end of the file */
    .long __image_end                 /* reserve up to here */

    /* Where to begin, which the address tag does not say. */
.align 8
    .short 3                          /* entry address */
    .short 0
    .long 12
    .long _start

    /*
     * The same video request the other header makes, and the same
     * reasoning: zero for width and height means "whatever suits the
     * panel", and 32 bits per pixel is a requirement rather than a
     * preference because everything above `struct fb` treats a pixel as one
     * 32-bit word.
     */
.align 8
    .short 5                          /* framebuffer */
    .short 0
    .long 20
    .long 0                           /* width:  no preference */
    .long 0                           /* height: no preference */
    .long 32                          /* depth */

.align 8
    .short 0                          /* end of tags */
    .short 0
    .long 8
__multiboot2_end:

/*
 * The page tables that get us into long mode.
 *
 * **Four gigabytes, identity mapped, with 2 MB pages**, and the size is the
 * whole point of this comment. It was one gigabyte, which is enough for
 * every device QEMU puts below that line and is *not* enough for a machine
 * with real memory in it: a multiboot loader may leave its information
 * structure anywhere below 4 GB, and `pc_capture_memory` reads it at boot
 * stage three.
 *
 * Measured rather than reasoned about. With `-m 4G` QEMU put the structure
 * at 0x7ffe2349 and the first read of it was a page fault at stage three -
 * before the framebuffer exists, on a machine whose only other output is a
 * serial port a laptop does not have. A black screen and nothing to ask.
 *
 * Four gigabytes is the ceiling because a multiboot 1 loader hands over a
 * *32-bit* pointer in `ebx`, so nothing it describes can be above that
 * line. Four PDs of 512 entries each, 16 KB of `.bss`, and no CPUID check -
 * 1 GB pages would do this in four entries and are an optional feature,
 * where 2 MB pages have been mandatory since PAE.
 *
 * `mmu.c` builds the real address space later, from C, with four levels.
 *
 * 4096-byte alignment is architectural: the low twelve bits of a table
 * entry are flags, so a table that is not page-aligned has its address
 * silently truncated into them.
 */
#define BOOT_GIGABYTES  4
#define BOOT_PD_ENTRIES (BOOT_GIGABYTES * 512)

.section .bss, "aw", @nobits
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096 * BOOT_GIGABYTES

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
    movl    $__stack_top, %esp

    /*
     * **Which loader this is, from the magic in eax.**
     *
     * Multiboot 1 leaves 0x2BADB002 and Multiboot 2 leaves 0x36D76289, and
     * the information structures they point `ebx` at have nothing in common
     * beyond both starting with a length. Parked in edi alongside the
     * pointer so that one branch in C decides, rather than two paths of
     * assembly.
     */

    /* Multiboot leaves the information structure in ebx. Parked in esi,
     * which nothing below touches: edi is the destination register for
     * both the table fill and `rep stos`, and ecx is their counter. */
    movl    %ebx, %esi
    movl    %eax, %ebx           /* ...and only now, since ebx held it */

    /*
     * .bss, zeroed - and it happens *here*, before the page tables are
     * built, because the page tables are in .bss.
     *
     * Zeroing it afterwards was the first version and it is a machine that
     * dies a few instructions into long mode: `rep stos` walks over pml4,
     * pdpt and pd while the processor is translating through them, and what
     * kills it is not the write but the first TLB miss after it.
     *
     * The stack is deliberately *not* in here - it lives in its own section
     * past __bss_end, behind a guard page - so `esp` set above survives
     * this and does not need setting again.
     *
     * The loader does not do this. A multiboot image is a flat file and
     * .bss is the part of it that was never in the file.
     */
    movl    $__bss_start, %edi
    movl    $__bss_end, %ecx
    subl    %edi, %ecx
    xorl    %eax, %eax
    rep stosb


    /*
     * Identity mapping the first four gigabytes.
     *
     * PML4[0] -> PDPT; PDPT[0..3] -> one PD each; and every PD entry maps
     * 2 MB with PS=1 (a large page, so there is no fourth level to fill
     * in). Present | writable throughout.
     */
    movl    $pdpt, %eax
    orl     $0x03, %eax
    movl    %eax, pml4

    movl    $pdpt, %edi
    movl    $pd, %eax
    orl     $0x03, %eax
    movl    $BOOT_GIGABYTES, %ecx

fill_pdpt:
    movl    %eax, (%edi)
    movl    $0, 4(%edi)
    addl    $4096, %eax                  /* the next page directory */
    addl    $8, %edi
    loop    fill_pdpt

    movl    $pd, %edi
    movl    $0, %eax                     /* physical address being mapped */
    movl    $BOOT_PD_ENTRIES, %ecx

    /*
     * The address stays in 32 bits throughout: the last entry written maps
     * 0xFFE00000, and the add that would carry past four gigabytes is the
     * one the counter stops before.
     */
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

    movq    $__stack_top, %rsp
    xorq    %rbp, %rbp

    /*
     * The multiboot pointer, parked in esi at entry, handed to the board
     * rather than to the kernel.
     *
     * `hal/pc/pc.h` says why it is a variable and not an argument: what the
     * firmware left is the board's business, and `arch/x86_64/` has no more
     * need to know that multiboot exists than `arch/aarch64/` has to know
     * about a device tree. Writing a 32-bit register zeroes the top half,
     * which is what is wanted - the pointer came from a 32-bit loader.
     *
     * After the .bss zeroing above, not before: `pc_multiboot` is in there.
     */
    movl    %esi, pc_multiboot(%rip)
    movl    %ebx, pc_multiboot_magic(%rip)

    call    kmain_x86

hang:
    cli
    hlt
    jmp     hang
