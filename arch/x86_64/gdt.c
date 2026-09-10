/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "gdt.h"
#include "percpu.h"
#include "smp.h"

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

extern char __exception_stack_top[];

/*
 * A core's own block: what GS points at while that core is in the kernel.
 *
 * The pointer to the kernel's `struct percpu` first, because `cpu_self` is a
 * single load of the first word. Then the one word `syscall_entry` needs
 * before it has a stack - a process's rsp, parked for two instructions - and
 * then the core's TSS, whose rsp0 `switch.S` writes and `syscall_entry`
 * reads.
 *
 * **One of each per core, where there was one of each for the machine.**
 * That was correct while one processor ran the kernel. A TSS is the stack an
 * entry from ring 3 lands on, so two cores entering through one TSS would
 * land on the same stack; and the scratch word is the same shape one level
 * down, a cell two cores entering at once would both write.
 */
struct cpu_block {
    void      *self;
    uint64_t   user_rsp;
    struct tss tss;
};

_Static_assert(offsetof(struct cpu_block, self) == PCPU_SELF, "PCPU_SELF");
_Static_assert(offsetof(struct cpu_block, user_rsp) == PCPU_USER_RSP,
               "PCPU_USER_RSP");
_Static_assert(offsetof(struct cpu_block, tss) == PCPU_TSS, "PCPU_TSS");

static struct cpu_block blocks[NR_CPUS];

/*
 * Six eight-byte slots - five descriptors and a hole - and then a TSS
 * descriptor per core, each taking two slots because a system descriptor in
 * long mode carries a 64-bit base. Core n's is at `SEL_TSS + 16n`.
 */
#define GDT_SLOTS   (SEL_TSS / 8 + 2 * NR_CPUS)

static uint64_t gdt[GDT_SLOTS];

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
 *
 * **FS and GS get the null selector, and they used to get the kernel's data
 * segment.** Loading a selector into GS replaces the GS base with the
 * descriptor's, which is zero - and the GS base is now where this core keeps
 * its block. So nothing loads GS after `cpu_set_self`, and it holds null
 * from here on, which is also what a return to ring 3 would otherwise have
 * replaced a DPL 0 selector with.
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
        "xorl   %%eax, %%eax     \n"
        "movw   %%ax, %%fs       \n"
        "movw   %%ax, %%gs       \n"
        :
        : "r"(pointer), "i"(SEL_KERNEL_CODE), "i"(SEL_KERNEL_DATA)
        : "rax", "memory");
}

/*
 * The TSS descriptor for one core, which is a different shape from the
 * others: type 0x89 - present, DPL 0, "available 64-bit TSS" - a real limit,
 * since `iomap_base` is checked against it, and a base spread over four
 * fields and two slots.
 */
static void tss_descriptor(unsigned index)
{
    uintptr_t at = (uintptr_t)&blocks[index].tss;
    unsigned slot = SEL_TSS / 8 + 2 * index;

    gdt[slot] = (uint64_t)(sizeof(struct tss) - 1)
              | ((uint64_t)(at & 0xFFFFFF) << 16)
              | (0x89UL << 40)
              | ((uint64_t)((at >> 24) & 0xFF) << 56);
    gdt[slot + 1] = (uint64_t)(at >> 32);
}

void gdt_init(void)
{
    struct gdtr pointer;
    unsigned i;

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

    for (i = 0; i < NR_CPUS; i++) {
        blocks[i].tss.rsp0 = 0;
        blocks[i].tss.iomap_base = sizeof(struct tss);
        tss_descriptor(i);
    }

    /*
     * The stack a fault lands on, in the first interrupt stack table slot.
     *
     * **This is what makes a kernel stack overflow survivable here**, and
     * it is the one thing AArch64 gets from the architecture and x86 has to
     * be told. That kernel runs on SP_EL0, so every exception switches to
     * SP_EL1 and the handler stands on a stack of its own. Here a fault
     * from ring 0 stays on the stack that faulted unless the gate names an
     * IST entry - so an overflow pushed the fault frame into the guard
     * page, faulted again, could not deliver that either, and the machine
     * triple-faulted and reset.
     *
     * `trap.c` points #PF and #DF at this slot and nothing else at it. Not
     * every vector, because **an IST stack is not reentrant**: the
     * processor loads the same address every time, so a fault taken while
     * one is being handled writes over the frame that was being handled.
     * #PF is where a stack overflow arrives, and #DF is the backstop for a
     * #PF that could not be delivered - which is now only possible if this
     * stack is itself bad, and its own guard page is what catches that.
     *
     * The slots are numbered from 1 in a gate descriptor and from 0 here,
     * which is the kind of off-by-one that is worth writing down once.
     */
    blocks[0].tss.ist[0] = (uint64_t)(uintptr_t)__exception_stack_top;

    pointer.limit = (uint16_t)(sizeof(gdt) - 1);
    pointer.base  = (uint64_t)(uintptr_t)gdt;

    reload(&pointer);

    /* And the task register, which is what makes rsp0 mean anything - core
     * zero's descriptor here, and every other core loads its own. */
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)SEL_TSS));
}

/* `kernel/smp.c`'s, a stack per secondary for exceptions. */
extern uint8_t secondary_exception_stacks[NR_CPUS][SECONDARY_STACK_BYTES];

void gdt_init_here(unsigned index)
{
    struct gdtr pointer;

    if (index == 0 || index >= NR_CPUS) {
        return;
    }

    pointer.limit = (uint16_t)(sizeof(gdt) - 1);
    pointer.base  = (uint64_t)(uintptr_t)gdt;

    reload(&pointer);

    /* The stack #PF and #DF land on, this core's own - the note above
     * `gdt_init` says why it has to be one nobody else is using. */
    blocks[index].tss.ist[0] = (uint64_t)(uintptr_t)
        (secondary_exception_stacks[index] + SECONDARY_STACK_BYTES);

    __asm__ volatile("ltr %w0"
                     :: "r"((uint16_t)(SEL_TSS + 16u * index)));
}

/* IA32_GS_BASE and IA32_KERNEL_GS_BASE: Intel SDM volume 4, table 2-2. */
#define MSR_GS_BASE         0xC0000101u
#define MSR_KERNEL_GS_BASE  0xC0000102u

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value),
                                "d"((uint32_t)(value >> 32)));
}

void cpu_set_self(unsigned index, void *self)
{
    blocks[index].self = self;

    /*
     * The kernel's GS base is this core's block, and the other one - the GS
     * a process runs with after `swapgs` - is zero: nothing in ring 3 uses
     * GS, and a process must not be handed a kernel address in a register.
     */
    write_msr(MSR_GS_BASE, (uint64_t)(uintptr_t)&blocks[index]);
    write_msr(MSR_KERNEL_GS_BASE, 0);
}
