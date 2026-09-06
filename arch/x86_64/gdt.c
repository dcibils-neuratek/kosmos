/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stddef.h>
#include <stdint.h>

#include "gdt.h"

/*
 * A code or data descriptor: eight bytes, and the fields are scattered
 * because the 64-bit form was grown from the 16-bit one twice over. In long
 * mode the base and limit are ignored entirely, so what is actually being
 * said here is "ring 0 or ring 3, code or data, 64-bit or not" - four bits
 * of real information in a sixty-four bit word.
 *
 * The constants are written out rather than assembled from named fields,
 * because there are four of them and they are the same four every x86-64
 * kernel has. Intel SDM volume 3, figure 3-8.
 *
 *   0x00AF9A000000FFFF   kernel code: present, DPL 0, exec/read, L=1
 *   0x00CF92000000FFFF   kernel data: present, DPL 0, read/write
 *   0x00CFF2000000FFFF   user data:   the same, DPL 3
 *   0x00AFFA000000FFFF   user code:   the same, DPL 3
 *
 * The DPL is the nibble that goes 9 -> F: 0x9 is DPL 0 present, 0xF is
 * DPL 3 present. That single nibble is the whole of what separates the
 * kernel's code segment from a process's.
 */
#define GDT_KERNEL_CODE 0x00AF9A000000FFFFUL
#define GDT_KERNEL_DATA 0x00CF92000000FFFFUL
#define GDT_USER_DATA   0x00CFF2000000FFFFUL
#define GDT_USER_CODE   0x00AFFA000000FFFFUL

/*
 * The task state segment.
 *
 * Almost all of it is dead weight from 1985, when the processor switched
 * tasks by itself and this held a whole register file. Long mode kept the
 * structure and uses three fields of it: rsp0, which is the stack an entry
 * from ring 3 lands on, the interrupt stack table, and the I/O map base.
 *
 * `iomap_base` is set past the end of the segment on purpose. A ring 3
 * `in` or `out` consults the bitmap at that offset, and an offset beyond
 * the limit means every port is denied - which is the answer, since a
 * process that could talk to the PIC could mask the timer and keep the
 * machine.
 */
struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "the TSS is 104 bytes");
_Static_assert(offsetof(struct tss, rsp0) == TSS_RSP0, "TSS_RSP0 vs user.S");

struct tss tss;

/*
 * Seven eight-byte slots: five descriptors, a hole, and a TSS descriptor
 * that takes two of them because a system descriptor in long mode carries a
 * 64-bit base.
 */
static uint64_t gdt[8];

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

_Static_assert(SEL_USER_DATA == SEL_SYSRET_BASE + 8,  "sysretq loads SS from base+8");
_Static_assert(SEL_USER_CODE == SEL_SYSRET_BASE + 16, "sysretq loads CS from base+16");
_Static_assert(SEL_KERNEL_DATA == SEL_KERNEL_CODE + 8, "syscall loads SS from base+8");

/*
 * Loading the table is not enough.
 *
 * A selector caches its descriptor, so every segment register still
 * describes the table `start.S` built until it is written again. CS cannot
 * be written at all - it changes only through a far jump, a far return or
 * an interrupt - which is why this is a far return to the next instruction:
 * the address and the new selector are pushed, and `lretq` pops both.
 */
static void reload(const struct gdtr *pointer)
{
    __asm__ volatile(
        "lgdt   (%0)             \n"
        "pushq  %1               \n"
        "leaq   1f(%%rip), %%rax \n"
        "pushq  %%rax            \n"
        "lretq                   \n"
        "1:                      \n"
        "movl   %2, %%eax        \n"
        "movw   %%ax, %%ds       \n"
        "movw   %%ax, %%es       \n"
        "movw   %%ax, %%ss       \n"
        "movw   %%ax, %%fs       \n"
        "movw   %%ax, %%gs       \n"
        :
        : "r"(pointer), "i"(SEL_KERNEL_CODE), "i"(SEL_KERNEL_DATA)
        : "rax", "memory");
}

void gdt_init(void)
{
    struct gdtr pointer;
    uintptr_t at = (uintptr_t)&tss;

    gdt[SEL_NULL / 8]        = 0;
    gdt[SEL_KERNEL_CODE / 8] = GDT_KERNEL_CODE;
    gdt[SEL_KERNEL_DATA / 8] = GDT_KERNEL_DATA;

    /*
     * 0x18 stays zero, and it is a hole rather than an oversight. The only
     * thing that would ever load it is a 32-bit `sysretl` returning to
     * compatibility mode, which this kernel does not execute and will not:
     * Kosmos is 64-bit and only 64-bit. The slot exists because `sysretq`
     * counts from IA32_STAR[63:48] and the two selectors it wants are at
     * +8 and +16.
     */
    gdt[0x18 / 8]            = 0;

    gdt[SEL_USER_DATA / 8]   = GDT_USER_DATA;
    gdt[SEL_USER_CODE / 8]   = GDT_USER_CODE;

    tss.rsp0 = 0;
    tss.iomap_base = sizeof(struct tss);

    /*
     * The TSS descriptor, which is a different shape from the others: type
     * 0x89 - present, DPL 0, "available 64-bit TSS" - a real limit, since
     * `iomap_base` is checked against it, and a base spread over four
     * fields and two slots.
     */
    gdt[SEL_TSS / 8] = (uint64_t)(sizeof(struct tss) - 1)
                     | ((uint64_t)(at & 0xFFFFFF) << 16)
                     | (0x89UL << 40)
                     | ((uint64_t)((at >> 24) & 0xFF) << 56);
    gdt[SEL_TSS / 8 + 1] = (uint64_t)(at >> 32);

    pointer.limit = (uint16_t)(sizeof(gdt) - 1);
    pointer.base  = (uint64_t)(uintptr_t)gdt;

    reload(&pointer);

    /* And the task register, which is what makes rsp0 mean anything. */
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)SEL_TSS));
}

void gdt_set_kernel_stack(uintptr_t top)
{
    tss.rsp0 = (uint64_t)top;
}
