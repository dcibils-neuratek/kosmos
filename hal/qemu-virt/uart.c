/*
 * PL011 UART on QEMU virt.
 *
 * This is the only file in the system that knows where a piece of hardware
 * lives. Not one address escapes hal/.
 *
 * Base address: QEMU's virt machine memory map, VIRT_UART (hw/arm/virt.c).
 * Register offsets and bit positions: ARM PrimeCell UART (PL011) Technical
 * Reference Manual, ARM DDI 0183G, chapter 3.
 */

#include <stdint.h>

#include "mmio.h"
#include "qemu-virt.h"
#include "hal.h"

#define UART0_BASE      0x09000000UL

#define UART_DR         (UART0_BASE + 0x00)   /* data */
#define UART_FR         (UART0_BASE + 0x18)   /* flags */
#define UART_IBRD       (UART0_BASE + 0x24)   /* integer baud divisor */
#define UART_FBRD       (UART0_BASE + 0x28)   /* fractional baud divisor */
#define UART_LCRH       (UART0_BASE + 0x2c)   /* line control */
#define UART_CR         (UART0_BASE + 0x30)   /* control */
#define UART_IMSC       (UART0_BASE + 0x38)   /* interrupt mask set/clear */
#define UART_ICR        (UART0_BASE + 0x44)   /* interrupt clear */

#define FR_RXFE         (1u << 4)             /* receive FIFO empty */
#define FR_TXFF         (1u << 5)             /* transmit FIFO full */

#define LCRH_FEN        (1u << 4)             /* enable FIFOs */
#define LCRH_WLEN_8     (3u << 5)             /* 8 data bits */

#define CR_UARTEN       (1u << 0)
#define CR_TXE          (1u << 8)
#define CR_RXE          (1u << 9)

#define IMSC_ALL        0x7ffu                /* every interrupt source */

void hal_early_init(void)
{
    /*
     * Disable the UART before touching the baud rate or the line control.
     * The TRM requires it: the divisors are latched by the write to LCRH,
     * and changing them while the UART is enabled corrupts whatever transfer
     * is in flight.
     */
    mmio_write32(UART_CR, 0);

    /*
     * 115200 baud from the 24 MHz UARTCLK that QEMU virt gives the PL011.
     *
     *   divisor = 24000000 / (16 * 115200) = 13.0208...
     *   IBRD    = 13
     *   FBRD    = round(0.0208... * 64) = 1
     *
     * QEMU ignores both registers. The Pi does not, and leaving them at
     * whatever the firmware left behind is how the first real target
     * produces line noise instead of text.
     */
    mmio_write32(UART_IBRD, 13);
    mmio_write32(UART_FBRD, 1);

    /* 8-N-1 with the FIFOs on. This write is also what latches IBRD/FBRD. */
    mmio_write32(UART_LCRH, LCRH_WLEN_8 | LCRH_FEN);

    /*
     * Mask every interrupt source and clear anything already pending.
     * There is no exception vector and no interrupt controller until M1,
     * so an unmasked source is a fault nobody handles: a silent hang before
     * the first character.
     */
    mmio_write32(UART_IMSC, 0);
    mmio_write32(UART_ICR, IMSC_ALL);

    mmio_write32(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

void hal_putchar(char c)
{
    /* Spin while the transmit FIFO is full. Polled on purpose: interrupts
     * do not exist yet, and output has to keep working when everything else
     * in the system has died. */
    while (mmio_read32(UART_FR) & FR_TXFF) {
        /* wait */
    }

    mmio_write32(UART_DR, (uint32_t)(unsigned char)c);
}

int hal_getchar(void)
{
    /*
     * Two sources, one answer.
     *
     * `hal_getchar` is "one character in, from wherever this board's input
     * comes from", and on this board that is two places: the serial line and
     * a virtio keyboard, when the machine was started with one. Nothing
     * above the HAL knows or cares which - the console server, the shell and
     * every process reading a line are unchanged by the keyboard existing.
     *
     * The keyboard first, because the person at the screen is more likely to
     * be the one typing; with both attached, either works.
     */
    int key = keyboard_getchar();

    if (key >= 0) {
        return key;
    }

    if (mmio_read32(UART_FR) & FR_RXFE) {
        return HAL_NO_INPUT;
    }

    /*
     * The low byte is the character; the bits above it are the receive error
     * flags (break, parity, framing, overrun). Masking them off rather than
     * checking them is deliberate for now: there is nothing useful to do
     * about a framing error on a console except carry on, and reporting one
     * would mean an error path with no handler.
     */
    return (int)(mmio_read32(UART_DR) & 0xff);
}
