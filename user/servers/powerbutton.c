/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The power button: the first driver in this system that is not in the
 * kernel.
 *
 * It does almost nothing, and that is its job. It exists to prove the three
 * primitives `docs/drivers.md` says a driver outside the kernel needs, with a
 * device simple enough that a failure points at *them* rather than at the
 * device: xHCI's rings are a poor place to discover that an interrupt never
 * arrives. QEMU's `virt` machine has a PL061 GPIO controller with the power
 * key wired to one input, and the test harness can press that key on demand
 * with `system_powerdown` - so every step can be provoked and watched.
 *
 *   find     the board says where the controller is      SYS_DEV_FIND
 *   map      its registers, uncached, into this process  SYS_DEV_MAP
 *   claim    its interrupt, as a capability               SYS_IRQ_CLAIM
 *   wait     until the key is pressed                     SYS_IRQ_WAIT
 *   ack      once the controller has been quietened       SYS_IRQ_ACK
 *
 * --------------------------------------------------------------------
 * What it does not have, on purpose.
 *
 * **No console.** A process that owns the console may also read every key
 * and pointer event on the machine - `owns_console` gates those syscalls as
 * well as `SYS_WRITE` - and a power-button driver has no business with
 * either. So it reports the way any other process does: it is handed the
 * console server's endpoint and sends it a write.
 *
 * **No address of its own.** `0x09030000` is written in `hal/qemu-virt/`
 * and nowhere else; this file asks for "a PL061 carrying the power key" and
 * is told where one is. On a PC there is none - the button is an ACPI event
 * there - and this exits quietly having found nothing.
 *
 * --------------------------------------------------------------------
 * The controller, and why it is configured the way it is.
 *
 * Register offsets are from QEMU 11.1.1's `hw/gpio/pl061.c`, which is the
 * model this runs against, and they are the ARM PL061's. QEMU's `gpio-key`
 * holds its line high for 100 ms on the virtual clock and then drops it: a
 * pulse, not a level. So the input is configured **edge-sensitive, rising
 * edge only** - one interrupt per press, where level-sensitive would re-raise
 * for as long as the key stayed down and both-edges would count every press
 * twice.
 *
 * On that edge the controller latches a bit in its interrupt state and holds
 * its interrupt output high until the bit is cleared. That output is a
 * level-triggered GIC line, which is precisely the case `kernel/irq.c` masks
 * on delivery for: without the mask it would arrive again before this
 * process could run. So the order in the loop matters and is written down
 * where it happens.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "conproto.h"
#include "mmio.h"

#define PL061_DIR           0x400u      /* 1 = output */
#define PL061_IS            0x404u      /* 1 = level-sensitive */
#define PL061_IBE           0x408u      /* 1 = both edges */
#define PL061_IEV           0x40cu      /* 1 = rising edge, or high level */
#define PL061_IE            0x410u      /* 1 = unmasked */
#define PL061_MIS           0x418u      /* masked interrupt status, read */
#define PL061_IC            0x41cu      /* interrupt clear, write-one */
#define PL061_PERIPH_ID0    0xfe0u      /* 0x61 on a PL061 */

static long console = -1;

/*
 * A line to the console server, as its client.
 *
 * Built in a stack buffer exactly the way `con_kosmos.c` builds one, because
 * the request is a declared shape - `user/include/conproto.h` - and there is
 * one way to fill it in.
 */
static void say(const char *s)
{
    struct message msg, rep;
    struct con_request *req = (struct con_request *)msg.data;
    size_t n = strlen(s);

    if (console < 0) {
        return;
    }

    if (n > CON_TEXT_MAX) {
        n = CON_TEXT_MAX;
    }

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*req);
    req->op = CON_OP_WRITE;
    req->length = (uint32_t)n;
    memcpy(req->text, s, n);

    (void)kosmos_call(console, &msg, &rep);
}

/* A number in a sentence, without a formatted-print library in a server. */
static void say_number(const char *before, unsigned n, const char *after)
{
    char line[96];
    char digits[12];
    size_t at = 0, d = 0, i;

    for (i = 0; before[i] != '\0' && at < sizeof(line) - 1; i++) {
        line[at++] = before[i];
    }

    do {
        digits[d++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0 && d < sizeof(digits));

    while (d > 0 && at < sizeof(line) - 1) {
        line[at++] = digits[--d];
    }

    for (i = 0; after[i] != '\0' && at < sizeof(line) - 1; i++) {
        line[at++] = after[i];
    }

    line[at] = '\0';
    say(line);
}

void powerbutton_server(long console_cap)
{
    struct dev_info dev;
    uintptr_t base;
    uint32_t bit;
    long mapped, irq;

    console = console_cap;

    if (kosmos_dev_find(DEV_PL061_POWER_KEY, &dev) != 0) {
        kosmos_exit(0);                 /* this machine has none: not an error */
    }

    mapped = kosmos_dev_map((unsigned long)dev.base, 1);

    if (mapped < 0) {
        say("powerbutton: the controller's registers could not be mapped\n");
        kosmos_exit(1);
    }

    base = (uintptr_t)mapped;

    /*
     * Asked of the device before anything is written to it. A window mapped
     * at the wrong address reads back something, and writing an interrupt
     * configuration into a stranger is how a wrong address stops being
     * harmless.
     */
    if ((mmio_read32(base + PL061_PERIPH_ID0) & 0xffu) != 0x61u) {
        say("powerbutton: that window is not a PL061\n");
        kosmos_exit(1);
    }

    bit = 1u << dev.line;

    /*
     * An input, rising edge only, any stale latched state cleared - and only
     * then unmasked, so an edge from before this process existed is not
     * reported as a press.
     */
    mmio_write32(base + PL061_DIR, mmio_read32(base + PL061_DIR) & ~bit & 0xffu);
    mmio_write32(base + PL061_IS,  mmio_read32(base + PL061_IS)  & ~bit & 0xffu);
    mmio_write32(base + PL061_IBE, mmio_read32(base + PL061_IBE) & ~bit & 0xffu);
    mmio_write32(base + PL061_IEV, (mmio_read32(base + PL061_IEV) | bit) & 0xffu);
    mmio_write32(base + PL061_IC,  bit);
    mmio_write32(base + PL061_IE,  (mmio_read32(base + PL061_IE) | bit) & 0xffu);

    irq = kosmos_irq_claim(dev.intid);

    if (irq < 0) {
        say_number("powerbutton: interrupt ", dev.intid, " was refused\n");
        kosmos_exit(1);
    }

    say_number("powerbutton: waiting on line ", dev.line, "\n");

    for (;;) {
        if (kosmos_irq_wait(irq) != 0) {
            say("powerbutton: the interrupt line went away\n");
            kosmos_exit(1);
        }

        /*
         * **Clear the controller, then ack - never the other way round.**
         *
         * The PL061 holds its output high until its latched bit is cleared,
         * and the kernel masked the GIC line when it delivered. Unmasking
         * first would find the line still asserted and deliver the same
         * press again at once. Clearing first means the line has dropped by
         * the time it is unmasked, and the next interrupt is a new press.
         */
        if ((mmio_read32(base + PL061_MIS) & bit) != 0) {
            mmio_write32(base + PL061_IC, bit);
            say("powerbutton: pressed\n");
        }

        (void)kosmos_irq_ack(irq);
    }
}
