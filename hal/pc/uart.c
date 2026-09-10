/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * 16550 UART on a PC, at the address it has been at since 1981.
 *
 * The counterpart of `hal/qemu-virt/uart.c`, and the difference between them
 * is the whole reason `hal/` exists: that one reads and writes *memory*, and
 * this one uses `in` and `out` on a separate I/O address space that only
 * x86 has. Same interface above, entirely different instruction below.
 *
 * 0x3F8 is COM1. QEMU's `-serial` connects to it, which is what makes this
 * the first thing worth writing on a new architecture: it is the only way
 * the machine can say anything at all before there is a screen.
 *
 * Register offsets: PC16550D datasheet, table 2. They have not moved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hal.h"
#include "pc.h"
#include "virtio.h"
#include "spinlock.h"

#define COM1        0x3F8

#define UART_DATA   0        /* read: receive, write: transmit */
#define UART_IER    1        /* interrupt enable */
#define UART_FCR    2        /* FIFO control */
#define UART_LCR    3        /* line control */
#define UART_MCR    4        /* modem control */
#define UART_LSR    5        /* line status */
#define UART_SCR    7        /* scratch: read/write, controls nothing */

#define LSR_RX_READY  (1u << 0)
#define LSR_TX_EMPTY  (1u << 5)

#define LCR_8N1       0x03
#define LCR_DLAB      0x80   /* the divisor is behind this bit */

/*
 * Port I/O, which is the one thing on x86 with no memory-mapped equivalent.
 *
 * `mmio_read32`/`mmio_write32` carry their barriers because a memory access
 * can be reordered around another one. These cannot be: `in` and `out` are
 * serialising against I/O by definition, which is why there is no barrier
 * here and why that is not an oversight.
 */
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

/*
 * Whether anything answers at COM1 at all.
 *
 * **A PC with no serial port reads 0xFF from every register here, and 0xFF
 * in the line status register says a byte has arrived.** This file assumed
 * the port existed, because QEMU always gives it one - so on the first real
 * machine, a T14 with no COM1, `hal_getchar` answered every call with a
 * byte of 0xFF, for ever.
 *
 * Nothing crashed, which is why it took this long to see. The console
 * server's line editor loops until the input runs out, and it never did: at
 * a prompt it span discarding a non-printable byte while the keyboard,
 * asked first, still typed perfectly. At the desktop every `CON_OP_WAIT`
 * came back with sixty-four of them, so the window manager never slept and
 * posted sixty-four key events a pass to the focused window - whose queue
 * holds sixty-four and drops the oldest. A core at 100% with the console
 * taking 94% of it, and every button pressed and never released, because
 * the release was pushed out of the queue by keys nobody typed.
 *
 * Reproduced under QEMU by taking the port away, `-serial none`: an idle
 * desktop went from 20% of a host core to 100% every time, and in the first
 * run a click on the Deskbar's button left it down with no menu while the
 * same click with the port present opened it. Not in every run - whether
 * the release is lost depends on where it lands in a queue the flood is
 * filling - which is why `tools/run_x86.py` tests this by whether the
 * processor ever halts rather than by the click.
 *
 * The scratch register is the test because it is the one register that
 * does nothing - PC16550D: read/write, and it "does not control the UART in
 * any way". Two complementary patterns, because one could be 0xFF, which a
 * port that is not there reads back regardless.
 */
static bool present;

static bool answers(void)
{
    outb(COM1 + UART_SCR, 0x5a);

    if (inb(COM1 + UART_SCR) != 0x5a) {
        return false;
    }

    outb(COM1 + UART_SCR, 0xa5);

    return inb(COM1 + UART_SCR) == 0xa5;
}

/* `hal_early_init` is the HAL's own name for this: whatever a board must do
 * before anything can be said. On QEMU virt it is the PL011; here it is
 * COM1. Same contract, and the kernel above never learns which. */
void hal_early_init(void)
{
    present = answers();

    if (present) {
        outb(COM1 + UART_IER, 0x00);        /* no interrupts: polled */

        /* 115200 baud: the divisor is 115200/115200 = 1, behind DLAB. */
        outb(COM1 + UART_LCR, LCR_DLAB);
        outb(COM1 + 0, 0x01);
        outb(COM1 + 1, 0x00);

        outb(COM1 + UART_LCR, LCR_8N1);     /* and DLAB back off */
        outb(COM1 + UART_FCR, 0xC7);        /* FIFOs on, cleared, 14-byte */
        outb(COM1 + UART_MCR, 0x0B);        /* DTR, RTS, OUT2 */
    }

    /*
     * And what the loader left, while it is still there.
     *
     * `pc.h` explains the ordering: the multiboot structure sits in RAM
     * past the kernel image, which `pmm_init` will hand out. This is the
     * first thing `kmain` calls, so it is the last moment it is readable -
     * and reading it here means nothing later has to remember to.
     */
    pc_capture_memory();
}

void hal_putchar(char c)
{
    /* Nobody to tell. The log ring and the screen still get every byte -
     * this is only the wire - and waiting for a transmitter that is not
     * there is a loop whose exit is decided by a floating bus. */
    if (!present) {
        return;
    }

    while ((inb(COM1 + UART_LSR) & LSR_TX_EMPTY) == 0) {
    }

    outb(COM1 + UART_DATA, (uint8_t)c);
}

/*
 * Status, then data: one question, and two cores between the two reads could
 * both see a byte ready and one of them read the next one, or nothing.
 */
static struct spinlock uart_lock = SPINLOCK("uart");

int hal_getchar(void)
{
    /*
     * Two sources, one answer - and `hal/qemu-virt/uart.c` says the same
     * sentence for the same reason. A character is a character: the console
     * server, the shell and every process reading a line are unchanged by a
     * keyboard existing, which is why `hal.h` has no `hal_keyboard_getchar`
     * for them to have to know about.
     *
     * The keyboard first, because the person at the screen is more likely
     * to be the one typing; with both attached, either works.
     */
    int key = keyboard_getchar();

    if (key >= 0) {
        return key;
    }

    if (!present) {
        return HAL_NO_INPUT;
    }

    {
        unsigned long flags = spin_lock(&uart_lock);
        int c = HAL_NO_INPUT;

        if ((inb(COM1 + UART_LSR) & LSR_RX_READY) != 0) {
            c = inb(COM1 + UART_DATA);
        }

        spin_unlock(&uart_lock, flags);
        return c;
    }
}

const char *hal_console_describe(void)
{
    return present ? "16550 UART at 0x3f8, polled"
                   : "none - nothing answers at 0x3f8, so the screen and `log` only";
}
