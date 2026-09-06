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

#include <stdint.h>

#include "hal.h"

#define COM1        0x3F8

#define UART_DATA   0        /* read: receive, write: transmit */
#define UART_IER    1        /* interrupt enable */
#define UART_FCR    2        /* FIFO control */
#define UART_LCR    3        /* line control */
#define UART_MCR    4        /* modem control */
#define UART_LSR    5        /* line status */

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

/* `hal_early_init` is the HAL's own name for this: whatever a board must do
 * before anything can be said. On QEMU virt it is the PL011; here it is
 * COM1. Same contract, and the kernel above never learns which. */
void hal_early_init(void)
{
    outb(COM1 + UART_IER, 0x00);            /* no interrupts: polled */

    /* 115200 baud: the divisor is 115200/115200 = 1, behind DLAB. */
    outb(COM1 + UART_LCR, LCR_DLAB);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);

    outb(COM1 + UART_LCR, LCR_8N1);         /* and DLAB back off */
    outb(COM1 + UART_FCR, 0xC7);            /* FIFOs on, cleared, 14-byte */
    outb(COM1 + UART_MCR, 0x0B);            /* DTR, RTS, OUT2 */
}

void hal_putchar(char c)
{
    while ((inb(COM1 + UART_LSR) & LSR_TX_EMPTY) == 0) {
    }

    outb(COM1 + UART_DATA, (uint8_t)c);
}

int hal_getchar(void)
{
    if ((inb(COM1 + UART_LSR) & LSR_RX_READY) == 0) {
        return HAL_NO_INPUT;
    }

    return inb(COM1 + UART_DATA);
}
