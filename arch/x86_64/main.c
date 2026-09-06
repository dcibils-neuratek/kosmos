/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The first C that runs on x86-64, and for now the only C.
 *
 * `kernel/main.c` is the real one and this is not it. Bringing a second
 * architecture up means proving one thing at a time, and the first thing -
 * the thing every other thing needs before it can report a failure - is
 * that the machine got into long mode and can speak.
 *
 * The order this project used on ARM, and the reason it is worth repeating:
 * `docs/roadmap.md` records the first milestone as "boot under QEMU and
 * print", because a kernel that cannot print is a kernel debugged by
 * bisecting a hang.
 */

#include <stdint.h>

#include "cpu.h"
#include "hal.h"
#include "mmu.h"
#include "page.h"
#include "panic.h"
#include "pmm.h"
#include "trap.h"

void hal_ram_from_multiboot(uint32_t at);

static void say(const char *s);

/*
 * `panic`, on loan until `kernel/` builds here.
 *
 * The real one is `kernel/panic.c`, and it is four lines longer for a
 * reason that does not exist yet: it takes the screen back from whatever
 * compositor had it, so a machine with a desktop on it does not stop dead
 * with the explanation going only to a serial line nobody attached. There
 * is no screen and no compositor on this architecture, so there is nothing
 * to take back.
 *
 * **This disappears the moment `kernel/console.c` compiles here**, and it
 * is in this file rather than a plausible-looking one so that it cannot be
 * mistaken for a second implementation that was meant to stay.
 */
void panic(const char *msg)
{
    say("\r\nPANIC: ");
    say(msg);
    say("\r\n");

    for (;;) {
        cpu_irq_disable();
        cpu_wait_for_interrupt();
    }
}

static void say(const char *s)
{
    while (*s != '\0') {
        hal_putchar(*s++);
    }
}

static void say_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift;

    say("0x");

    for (shift = 60; shift >= 0; shift -= 4) {
        hal_putchar(digits[(v >> shift) & 0xf]);
    }
}

/*
 * What long mode looks like from the inside, reported rather than assumed.
 *
 * CR0.PG and EFER.LMA together are the only honest answer to "did the mode
 * switch work": LME is what was *asked* for and LMA is what the processor
 * says it *did*. They are set in different instructions and a boot that got
 * one and not the other is exactly the failure worth naming.
 */
extern char __text_start[], __rodata_start[], __stack_guard[];

/* What the map should say about the three kinds of page it distinguishes. */
static void check_map(void)
{
    struct { const char *what; uintptr_t at; uint64_t want; uint64_t mask; }
    cases[] = {
        { "text   ", (uintptr_t)__text_start,   MAP_TEXT, PTE_P | PTE_RW | PTE_NX },
        { "rodata ", (uintptr_t)__rodata_start, MAP_RO,   PTE_P | PTE_RW | PTE_NX },
    };
    unsigned i;
    uint64_t *guard;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t *e = mmu_page_entry(cases[i].at);

        say("  ");
        say(cases[i].what);
        if (e == NULL) {
            say("  NO PAGE ENTRY\r\n");
        } else if ((*e & cases[i].mask) != (cases[i].want & cases[i].mask)) {
            say("  WRONG: "); say_hex(*e); say("\r\n");
        } else {
            say("  "); say_hex(*e); say("\r\n");
        }
    }

    guard = mmu_page_entry((uintptr_t)__stack_guard);
    say("  guard  ");
    say((guard != NULL && (*guard & PTE_P) == 0) ? "  unmapped\r\n"
                                                 : "  STILL MAPPED\r\n");
}

/*
 * An address space, used and given back.
 *
 * The count either side is the part that matters. A space allocates its
 * PML4, its PDPT and a table per level it touches; `as_destroy` has to hand
 * back exactly those and none of the kernel's, and a leak here is the shape
 * of bug that only shows up on the fiftieth process.
 */
static void check_spaces(void)
{
    size_t before = pmm_free_pages();
    struct addrspace *as = as_create();
    uint64_t *entry;

    if (as == NULL) {
        say("  as       COULD NOT CREATE\r\n");
        return;
    }

    if (as_map(as, USER_VA_BASE, (uintptr_t)__rodata_start, 1,
               MAP_USER_RW) != AS_OK) {
        say("  as       MAP REFUSED\r\n");
        return;
    }

    /* Below the user region the tables are the kernel's, and writing there
     * would edit every space at once. It has to be refused. */
    if (as_map(as, PAGE_SIZE, PAGE_SIZE, 1, MAP_USER_RW) != AS_ERR_RANGE) {
        say("  as       THE KERNEL REGION WAS WRITABLE\r\n");
        return;
    }

    entry = as_page_entry(as, USER_VA_BASE);
    say("  as       ");
    if (entry == NULL || (*entry & PTE_US) == 0) {
        say("NO USER PAGE\r\n");
    } else {
        say_hex(*entry);
        say("\r\n");
    }

    /* And the kernel is still in it, sharing rather than copied. */
    say("  as text  ");
    entry = as_page_entry(as, (uintptr_t)__text_start);
    say((entry != NULL && *entry == *mmu_page_entry((uintptr_t)__text_start))
        ? "shared with the kernel\r\n" : "MISSING OR DIFFERENT\r\n");

    as_destroy(as);

    say("  as pages ");
    if (pmm_free_pages() == before) {
        say("all returned\r\n");
    } else {
        say_hex(before - pmm_free_pages());
        say(" LEAKED\r\n");
    }
}

void kmain_x86(uint32_t multiboot)
{
    uint64_t cr0, cr3, efer;

    hal_early_init();

    say("\r\nKosmos on x86-64.\r\n");

    __asm__ volatile ("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("movq %%cr3, %0" : "=r"(cr3));
    /*
     * `rdmsr` answers in EDX:EAX - two 32-bit halves, always, even in long
     * mode. The `"=A"` constraint means that pair on 32-bit x86 and means
     * "rax or rdx, whichever" on x86-64, so asking for it here reads half
     * the register and looks like it worked.
     */
    {
        uint32_t lo, hi;

        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        efer = ((uint64_t)hi << 32) | lo;
    }

    say("  cr0      "); say_hex(cr0);  say("\r\n");
    say("  cr3      "); say_hex(cr3);  say("\r\n");
    say("  efer     "); say_hex(efer); say("\r\n");
    say("  multiboot "); say_hex(multiboot); say("\r\n");

    say(((cr0 >> 31) & 1) ? "  paging on\r\n" : "  PAGING OFF\r\n");
    say(((efer >> 10) & 1) ? "  long mode active\r\n"
                           : "  LONG MODE NOT ACTIVE\r\n");

    /* Where the RAM is, before anything asks for a page of it. */
    hal_ram_from_multiboot(multiboot);

    {
        struct memrange ram;

        hal_ram_range(&ram);
        say("  ram at   "); say_hex(ram.base);
        say(" for ");       say_hex(ram.size);
        say(" ("); 
        say_hex(ram.size >> 20);
        say(" MB)\r\n");
    }

    trap_init();
    say("  idt      installed, 48 vectors\r\n");

    /*
     * The interrupt path, end to end, before anything depends on it.
     *
     * The controller has to be remapped before `sti` or the first tick
     * arrives as a double fault, so the order here is not a style: PIC,
     * then timer, then interrupts on, then wait and see whether the count
     * moved. `hlt` rather than a spin, because a spin cannot tell the
     * difference between a tick arriving and the loop being slow.
     */
    hal_irq_init();
    hal_timer_init(100);
    say("  pic      remapped to 32, timer at 100 Hz\r\n");

    cpu_irq_enable();

    {
        unsigned long before = hal_ticks();
        int waits;

        for (waits = 0; waits < 20; waits++) {
            __asm__ volatile ("hlt");
        }

        say("  ticks    ");
        say_hex(hal_ticks());
        say(hal_ticks() > before ? "  (rising)\r\n"
                                 : "  NO TICK ARRIVED\r\n");
    }

    /*
     * The page allocator, and then the real address space.
     *
     * This order is `kernel/main.c`'s and it is forced: the page tables
     * come out of `pmm_alloc_page`, so there has to be an allocator before
     * there can be a map.
     */
    pmm_init();
    say("  pmm      ");
    say_hex(pmm_free_pages());
    say(" of ");
    say_hex(pmm_total_pages());
    say(" pages free\r\n");

    mmu_init();
    say("  mmu      four levels, loaded");
    say(mmu_is_enabled() ? "\r\n" : "  BUT PAGING IS OFF\r\n");

    /*
     * Printing this sentence is most of the proof, and it is worth saying
     * why: it went through the UART driver in .text, a string in .rodata
     * and a stack frame in the guarded stack, all three of which are now
     * described by tables this file built rather than by the three static
     * ones in `start.S`.
     */
    check_map();
    check_spaces();

    /*
     * And a fault on purpose, because an exception handler that has never
     * run is an exception handler that does not work - and because a
     * permission nothing has tested is a permission that might not be
     * there.
     *
     * Writing to .text rather than to unmapped memory: the boot map made
     * every page writable, so this fault can only come from the narrowing
     * pass in `mmu_init`. The error code should say `protection, write,
     * kernel` rather than `not present`, and that difference is the whole
     * of what is being shown.
     */
    say("\r\nWriting to __text_start, which is mapped read-only:\r\n");

    *(volatile uint64_t *)(uintptr_t)__text_start = 1;

    say("*** the write succeeded, so .text is not read-only\r\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
