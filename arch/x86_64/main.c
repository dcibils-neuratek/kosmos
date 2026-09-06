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

#include "hal.h"

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

    say("\r\nIt speaks. That is the whole of this step.\r\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
