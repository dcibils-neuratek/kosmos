/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The USB host controllers - the first step of the USB stack, and only that.
 *
 * `roadmap.md` builds USB in steps that each end in something visible:
 * controller up, enumeration, bulk transfers, mass storage, Ethernet. This is
 * the first. For every xHCI controller the board reports it maps the
 * registers, takes the controller from the firmware if the firmware held it,
 * halts and resets it, and says which ports have something plugged in and at
 * what speed. Then it exits. With no rings there is nothing to wait for, and
 * a process that lingered would be holding controllers it does nothing with.
 *
 * **No interrupt is claimed**, for the same reason. Every question here is a
 * register read, and the waits - for a halt, a reset, the firmware - are
 * bounded and polled. The event ring is where interrupts start to matter,
 * and they arrive with it.
 *
 * --------------------------------------------------------------------
 * Where the numbers come from.
 *
 * Every offset and bit below is from Intel's *eXtensible Host Controller
 * Interface for Universal Serial Bus* specification, revision 1.2, and the
 * table or section is named beside each. QEMU 11.1.1's `hw/usb/hcd-xhci.c`
 * is the model the tests run against and agrees with all of them; it was read
 * for how it behaves, and nothing here is taken from it.
 *
 * --------------------------------------------------------------------
 * The firmware, which QEMU does not have.
 *
 * On a real machine the firmware drives the controller before the operating
 * system does - that is how a USB keyboard works in a boot menu - and it has
 * to be asked for it back. The protocol is the USB Legacy Support capability
 * (7.1, 4.22.1): the OS sets its semaphore and waits, for no more than a
 * second, for the firmware to clear its own. The two semaphores sit in
 * adjacent bytes so each side can write its own without rewriting the
 * other's, which is why that write is a byte and not a word.
 *
 * QEMU's controller has no such capability, so under emulation this path
 * never runs and the log says so. The ThinkPad is where it runs first, and
 * the line it prints says which of the outcomes happened.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kosmos.h"
#include "mmio.h"
#include "say.h"

/* Capability registers, from the start of the BAR: Table 5-9. */
#define CAP_LENGTH_VERSION  0x00u   /* CAPLENGTH 7:0, HCIVERSION 31:16 (BCD) */
#define CAP_HCSPARAMS1      0x04u
#define CAP_HCCPARAMS1      0x10u

#define HCS1_SLOTS(v)       ((v) & 0xFFu)               /* Table 5-10 */
#define HCS1_PORTS(v)       (((v) >> 24) & 0xFFu)
#define HCC1_XECP(v)        (((v) >> 16) & 0xFFFFu)     /* 5.3.6, in Dwords */

/* Operational registers, from base + CAPLENGTH: Table 5-18. */
#define OP_USBCMD           0x00u
#define OP_USBSTS           0x04u
#define OP_PORTSC(n)        (0x400u + 0x10u * ((n) - 1u))   /* 5.4.8, n >= 1 */

#define USBCMD_RS           (1u << 0)                   /* 5.4.1 */
#define USBCMD_HCRST        (1u << 1)
#define USBSTS_HCH          (1u << 0)                   /* 5.4.2 */
#define USBSTS_CNR          (1u << 11)

#define PORTSC_CCS          (1u << 0)                   /* Table 5-27 */
#define PORTSC_SPEED(v)     (((v) >> 10) & 0xFu)

/* Extended capabilities: Tables 7-1 and 7-2. */
#define XCAP_ID(v)          ((v) & 0xFFu)
#define XCAP_NEXT(v)        (((v) >> 8) & 0xFFu)        /* Dwords, from this one */
#define XCAP_LEGACY         1u
#define XCAP_PROTOCOL       2u

/* USB Legacy Support, Table 7-4: bits 16 and 24, as the bytes they sit in. */
#define LEGSUP_BIOS_BYTE    2u
#define LEGSUP_OS_BYTE      3u
#define LEGSUP_OWNED        0x01u

/* USBLEGCTLSTS, Table 7-5: the five SMI enables, bits 0, 4, 13, 14, 15. */
#define LEGCTLSTS           4u
#define LEGCTLSTS_ENABLES   0x0000E011u

/* Supported Protocol, Tables 7-6 and 7-8. */
#define PROTO_MAJOR(v)      (((v) >> 24) & 0xFFu)       /* BCD: 02h, 03h */
#define PROTO_PORTS         8u
#define PROTO_FIRST(v)      ((v) & 0xFFu)
#define PROTO_COUNT(v)      (((v) >> 8) & 0xFFu)

#define PORTS_MAX           255u        /* MaxPorts is eight bits */
#define CAPS_MAX            64u         /* a list longer than this is a loop */

/* Scheduler ticks, a hundredth of a second each. */
#define HALT_TICKS          20u         /* 5.4.1 allows 16 ms */
#define FIRMWARE_TICKS      100u        /* 4.22.1: no more than a second */
#define RESET_TICKS         100u
#define SETTLE_TICKS        50u

struct controller {
    uintptr_t     base;                 /* the capability registers */
    uintptr_t     op;                   /* the operational registers */
    unsigned long size;                 /* of the window, in bytes */
    unsigned      where;                /* bus << 8 | slot << 3 | function */
    unsigned      ports;
    unsigned char usb[PORTS_MAX + 1];   /* the USB major version, 0 unknown */
};

static long console = -1;

/* A PCI address the way people write one: 00:0d.0. */
static void address(struct say_line *line, unsigned where)
{
    say_hex(line, (where >> 8) & 0xFFu, 2);
    say_text(line, ":");
    say_hex(line, (where >> 3) & 0x1Fu, 2);
    say_text(line, ".");
    say_dec(line, where & 7u);
}

/* "xhci: 00:0d.0" - the start of every line about one controller. */
static void about(struct say_line *line, const struct controller *c)
{
    say_begin(line);
    say_text(line, "xhci: ");
    address(line, c->where);
}

/* Whether (reg & mask) == want within `ticks`, polling once a tick. */
static bool settles(uintptr_t reg, uint32_t mask, uint32_t want,
                    unsigned ticks)
{
    unsigned waited = 0;

    for (;;) {
        if ((mmio_read32(reg) & mask) == want) {
            return true;
        }

        if (waited++ >= ticks) {
            return false;
        }

        kosmos_sleep(1);
    }
}

/*
 * The OS semaphore set, and a second for the firmware to let go (4.22.1).
 *
 * Used either way when the second is up, and said so: the specification
 * gives the firmware no way to refuse, only to be slow, and a machine whose
 * firmware never answers is better told than hung.
 */
static void take_from_firmware(const struct controller *c, uintptr_t legsup,
                               struct say_line *line)
{
    uint32_t enables;
    unsigned waited = 0;
    bool held = (mmio_read8(legsup + LEGSUP_BIOS_BYTE) & LEGSUP_OWNED) != 0;

    mmio_write8(legsup + LEGSUP_OS_BYTE,
                (uint8_t)(mmio_read8(legsup + LEGSUP_OS_BYTE) | LEGSUP_OWNED));

    while ((mmio_read8(legsup + LEGSUP_BIOS_BYTE) & LEGSUP_OWNED) != 0
           && waited < FIRMWARE_TICKS) {
        kosmos_sleep(1);
        waited++;
    }

    about(line, c);

    if (!held) {
        say_text(line, " was not the firmware's; claimed");
    } else if ((mmio_read8(legsup + LEGSUP_BIOS_BYTE) & LEGSUP_OWNED) != 0) {
        say_text(line, " is still the firmware's after a second of asking; "
                       "used anyway");
    } else {
        say_text(line, " taken from the firmware after ");
        say_dec(line, (unsigned long)waited * 10u);
        say_text(line, " ms");
    }

    enables = mmio_read32(legsup + LEGCTLSTS) & LEGCTLSTS_ENABLES;

    if (enables != 0) {
        say_text(line, ", with its SMI enables 0x");
        say_hex(line, enables, 4);
        say_text(line, " still set");
    }

    say_send(console, line);
}

/*
 * The extended capabilities: the firmware handoff, and which ports speak
 * which USB. Every read is checked against the window first, because the
 * pointers are the device's to set and a wrong one would fault this process.
 */
static void walk_capabilities(struct controller *c, struct say_line *line)
{
    unsigned long at = (unsigned long)HCC1_XECP(
        mmio_read32(c->base + CAP_HCCPARAMS1)) * 4u;
    unsigned guard;
    bool legacy = false;

    for (guard = 0; at != 0 && guard < CAPS_MAX; guard++) {
        uint32_t head;

        if (at + 12u > c->size) {
            break;
        }

        head = mmio_read32(c->base + at);

        if (XCAP_ID(head) == XCAP_LEGACY) {
            take_from_firmware(c, c->base + at, line);
            legacy = true;
        } else if (XCAP_ID(head) == XCAP_PROTOCOL) {
            uint32_t ports = mmio_read32(c->base + at + PROTO_PORTS);
            unsigned p;

            for (p = PROTO_FIRST(ports);
                 p < PROTO_FIRST(ports) + PROTO_COUNT(ports) && p <= PORTS_MAX;
                 p++) {
                c->usb[p] = (unsigned char)PROTO_MAJOR(head);
            }
        }

        if (XCAP_NEXT(head) == 0) {
            break;
        }

        at += (unsigned long)XCAP_NEXT(head) * 4u;
    }

    if (!legacy) {
        about(line, c);
        say_text(line, " has no firmware handoff to make");
        say_send(console, line);
    }
}

/*
 * Halted, then reset, then ready (4.2, 5.4.1, 5.4.2).
 *
 * Nothing operational is written until Controller Not Ready clears, a reset
 * is only asked of a halted controller, and the reset is over when the
 * controller clears HCRST itself and is ready again.
 */
static bool reset(const struct controller *c, struct say_line *line)
{
    const char *why = NULL;

    if (!settles(c->op + OP_USBSTS, USBSTS_CNR, 0, RESET_TICKS)) {
        why = " never became ready";
    } else {
        if ((mmio_read32(c->op + OP_USBSTS) & USBSTS_HCH) == 0) {
            mmio_write32(c->op + OP_USBCMD,
                         mmio_read32(c->op + OP_USBCMD) & ~USBCMD_RS);

            if (!settles(c->op + OP_USBSTS, USBSTS_HCH, USBSTS_HCH,
                         HALT_TICKS)) {
                why = " would not halt";
            }
        }

        if (why == NULL) {
            mmio_write32(c->op + OP_USBCMD,
                         mmio_read32(c->op + OP_USBCMD) | USBCMD_HCRST);

            if (!settles(c->op + OP_USBCMD, USBCMD_HCRST, 0, RESET_TICKS)
                || !settles(c->op + OP_USBSTS, USBSTS_CNR, 0, RESET_TICKS)) {
                why = " did not finish resetting";
            }
        }
    }

    if (why != NULL) {
        about(line, c);
        say_text(line, why);
        say_send(console, line);
        return false;
    }

    return true;
}

/*
 * Table 7-13's default speeds. A controller that defines its own Protocol
 * Speed IDs (PSIC nonzero) may number them differently; the number is
 * printed alongside for that reason.
 */
static const char *speed_name(unsigned id)
{
    switch (id) {
    case 1:  return "Full-speed";
    case 2:  return "Low-speed";
    case 3:  return "High-speed";
    case 4:  return "SuperSpeed";
    case 5:
    case 6:
    case 7:  return "SuperSpeedPlus";
    default: return "unnamed-speed";
    }
}

/* One controller, start to finish. Answers how many ports have a device. */
static unsigned bring_up(struct controller *c, const struct dev_info *dev,
                         struct say_line *line)
{
    uint32_t word, hcs1;
    unsigned port, plugged = 0;
    long mapped;

    for (port = 0; port <= PORTS_MAX; port++) {
        c->usb[port] = 0;
    }

    c->where = dev->where;
    c->size = (unsigned long)dev->size;

    mapped = kosmos_dev_map((unsigned long)dev->base,
                            (c->size + 4095u) / 4096u);

    if (mapped < 0) {
        about(line, c);
        say_text(line, " could not be mapped");
        say_send(console, line);
        return 0;
    }

    c->base = (uintptr_t)mapped;
    word = mmio_read32(c->base + CAP_LENGTH_VERSION);
    hcs1 = mmio_read32(c->base + CAP_HCSPARAMS1);
    c->op = c->base + (word & 0xFFu);
    c->ports = HCS1_PORTS(hcs1);

    about(line, c);
    say_text(line, ", version ");
    say_dec(line, (word >> 24) & 0xFFu);
    say_text(line, ".");
    say_dec(line, (word >> 20) & 0xFu);
    say_text(line, ", ");
    say_dec(line, c->ports);
    say_text(line, " ports, ");
    say_dec(line, HCS1_SLOTS(hcs1));
    say_text(line, " slots");
    say_send(console, line);

    if ((word & 0xFFu) + OP_PORTSC(c->ports) + 4u > c->size) {
        about(line, c);
        say_text(line, "'s ports lie outside its window");
        say_send(console, line);
        return 0;
    }

    walk_capabilities(c, line);

    if (!reset(c, line)) {
        return 0;
    }

    /*
     * A moment for the ports. A port reports a device while the controller
     * is halted (4.19.2), but a USB 3 link that the reset retrained needs
     * time to come back, and reading at once would miss it.
     */
    kosmos_sleep(SETTLE_TICKS);

    for (port = 1; port <= c->ports; port++) {
        uint32_t sc = mmio_read32(c->op + OP_PORTSC(port));

        if ((sc & PORTSC_CCS) == 0) {
            continue;
        }

        plugged++;

        about(line, c);
        say_text(line, " port ");
        say_dec(line, port);

        /*
         * **A speed only where the field holds one.** Table 5-27: the speed
         * "is invalid on a USB2 protocol port until after the port is
         * reset", because a USB 2 device says how fast it is during that
         * reset, and this step resets no port. The ThinkPad showed what
         * printing it anyway looks like: four USB 2 ports, every one of them
         * "Full-speed", which nothing had yet asked.
         */
        if (c->usb[port] >= 3) {
            say_text(line, ", USB ");
            say_dec(line, c->usb[port]);
            say_text(line, ": a ");
            say_text(line, speed_name(PORTSC_SPEED(sc)));
            say_text(line, " device (speed ID ");
            say_dec(line, PORTSC_SPEED(sc));
            say_text(line, ")");
        } else if (c->usb[port] == 2) {
            say_text(line, ", USB 2: a device, its speed unknown until the "
                           "port is reset");
        } else {
            say_text(line, ": a device, on a port no protocol capability "
                           "describes");
        }

        say_send(console, line);
    }

    return plugged;
}

/* As many controllers as the closing line names; the board keeps four. */
#define NAMED_MAX           8u

void xhci_server(long console_cap)
{
    static struct controller c;
    struct dev_info dev;
    struct say_line line;
    unsigned where[NAMED_MAX];
    unsigned index, i, plugged = 0;
    long asked;

    console = console_cap;

    for (index = 0; (asked = kosmos_dev_find(DEV_XHCI, index, &dev)) == 0;
         index++) {
        if (index < NAMED_MAX) {
            where[index] = dev.where;
        }

        plugged += bring_up(&c, &dev, &line);
    }

    if (asked != SYS_ERR_NO_DEVICE) {
        say_begin(&line);
        say_text(&line, "xhci: the board would not say where the "
                        "controllers are");
        say_send(console, &line);
        kosmos_exit(1);
    }

    if (index == 0) {
        kosmos_exit(0);                 /* no USB controller: not an error */
    }

    /*
     * **The addresses as well as the count.** The ThinkPad's photograph
     * said "2 controllers" beside the lines of only one: the other's were
     * printed before the shell's banner and had scrolled away. A closing
     * line that names them is one photograph instead of two.
     */
    say_begin(&line);
    say_text(&line, "xhci: ");
    say_dec(&line, index);
    say_text(&line, index == 1 ? " controller (" : " controllers (");

    for (i = 0; i < index && i < NAMED_MAX; i++) {
        if (i > 0) {
            say_text(&line, ", ");
        }

        address(&line, where[i]);
    }

    say_text(&line, "), ");
    say_dec(&line, plugged);
    say_text(&line, plugged == 1 ? " port with something plugged in"
                                 : " ports with something plugged in");
    say_send(console, &line);

    kosmos_exit(0);
}
