/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * From UEFI's long mode to the machine state a Multiboot 2 kernel is
 * promised: 32-bit protected mode, paging off, a flat code and data segment,
 * interrupts off, `eax` the boot magic and `ebx` the information structure
 * - the state `boot/x86_64/start.S` expects. Kosmos's `_start` begins from that,
 * which is why the kernel does not change.
 *
 * **Copied, not called in place.** `loader.c` copies these bytes to the start
 * of a page it allocated below 4 GB as loader code, and calls the copy after
 * ExitBootServices. Below 4 GB because the last instructions run with paging
 * off, where an address is physical and 32 bits wide; loader code rather than
 * data because a firmware may map data non-executable. Everything here is
 * relative to the copy, so the copy works wherever the page is.
 *
 *     void enter(uint32_t mbi, uint32_t entry);     System V: edi, esi
 */

    .section .rodata, "a"
    .globl efi_trampoline_start
    .globl efi_trampoline_end

    .code64
efi_trampoline_start:
here:
    cli

    /*
     * A stack at the top of the copy's own page. The firmware's stack was
     * boot services memory, nobody's since ExitBootServices, and `lretq`
     * below needs two slots. Through a local label, so the assembler
     * resolves the distance itself and leaves no relocation to the linker.
     */
    leaq    here + 4096(%rip), %rsp

    /*
     * PCIDE off, because a processor refuses to turn paging off while it is
     * on, and CET off, because a shadow stack would refuse the `lretq`.
     * Neither is expected from a firmware; both cost one write to be sure.
     */
    movq    %cr4, %rax
    andq    $~((1 << 17) | (1 << 23)), %rax
    movq    %rax, %cr4

    /* A GDT with a 32-bit code segment, which the firmware's may not have.
     * Its base is written at run time because only the copy knows where it
     * is, and it must be below 4 GB: in protected mode GDTR has 32 bits. */
    leaq    gdt(%rip), %rax
    movq    %rax, gdt_base(%rip)
    lgdt    gdt_pointer(%rip)

    /* Into the 32-bit segment: a far return to `protected`, in selector 8. */
    leaq    protected(%rip), %rax
    pushq   $0x08
    pushq   %rax
    lretq

    .code32
protected:
    movl    $0x10, %eax
    movl    %eax, %ds
    movl    %eax, %es
    movl    %eax, %fs
    movl    %eax, %gs
    movl    %eax, %ss

    /* Paging off, which leaves long mode: from here an address is physical,
     * and the copy is below 4 GB and identity mapped, so the next
     * instruction is where it was. */
    movl    %cr0, %eax
    andl    $0x7FFFFFFF, %eax
    movl    %eax, %cr0

    /* EFER.LME off, which is only allowed with paging off. */
    movl    $0xC0000080, %ecx
    rdmsr
    andl    $~0x00000100, %eax
    wrmsr

    movl    %edi, %ebx              /* the information structure */
    movl    $0x36d76289, %eax       /* MB2_BOOT_MAGIC */
    jmp     *%esi

    .align 8
gdt:
    .quad   0
    .quad   0x00CF9A000000FFFF      /* 0x08: code, 32-bit, flat, ring 0 */
    .quad   0x00CF92000000FFFF      /* 0x10: data, flat, ring 0 */
gdt_pointer:
    .word   3 * 8 - 1
gdt_base:
    .quad   0
efi_trampoline_end:
