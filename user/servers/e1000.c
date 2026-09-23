/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An Intel Ethernet controller, driven from a process.
 *
 *   /net  <--- ethring.h ---  this  --- registers and rings --->  the card
 *
 * **Why it is here and not in `hal/`.** `docs/drivers.md` decided it on 12
 * September: a microkernel can do what a monolithic kernel cannot, which is
 * run a driver at EL0 where its bug is a crash and a restart. The three
 * primitives that needs - map a device's registers, get memory the device
 * can reach, receive an interrupt - all exist, and the xHCI driver is the
 * proof. The kernel's own virtio-net predates them.
 *
 * **Why it exists at all.** Diego's ThinkCentre M700 says
 * `Network not driven: Intel 8086:15b8 at 00:1f.6` - an I219-V behind a real
 * RJ-45 (`roadmap.md` 5zd-f). The USB Ethernet adapter already puts that
 * machine on a network, and this is the difference between a machine that
 * can be put on one and a machine that is on one.
 *
 * **One family, several chips.** The I219's MAC is in the chipset and QEMU's
 * are an 82540EM and an 82574L on a card; what all of them share is this
 * register set, these descriptors and this bring-up, which is why the
 * driver is named for the family rather than for any of them. What differs
 * is the PHY, and none of that is touched here: the firmware has already
 * brought the PHY up for its own network boot, and `CTRL.SLU` with
 * auto-speed detection is the whole of what this asks of it.
 *
 * **It answers the same protocol the USB adapter does** (`ethproto.h`), on
 * an endpoint of its own, so `net.c` attaches to whichever of them has a
 * card and nothing above knows the difference. That is what the ring in
 * `ethring.h` was for, and this is the second thing to use it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "mmio.h"
#include "say.h"
#include "ethproto.h"
#include "ethring.h"
#include "e1000_decode.h"

/*------------------------------------------------------------------ registers
 *
 * Offsets into the first BAR, from Intel's own manuals for this family
 * (82540EM 13.4, 82574L 8.2, I219 in the PCH datasheets). Only the ones this
 * driver writes are here; a register nobody touches is a register nobody has
 * to have got right.
 */
#define REG_CTRL        0x0000u
#define REG_STATUS      0x0008u
#define REG_ICR         0x00C0u     /* read to clear */
#define REG_IMS         0x00D0u
#define REG_IMC         0x00D8u
#define REG_RCTL        0x0100u
#define REG_TCTL        0x0400u
#define REG_TIPG        0x0410u
#define REG_RDBAL       0x2800u
#define REG_RDBAH       0x2804u
#define REG_RDLEN       0x2808u
#define REG_RDH         0x2810u
#define REG_RDT         0x2818u
#define REG_TDBAL       0x3800u
#define REG_TDBAH       0x3804u
#define REG_TDLEN       0x3808u
#define REG_TDH         0x3810u
#define REG_TDT         0x3818u
#define REG_MTA         0x5200u     /* 128 entries of the multicast table */
#define REG_RAL0        0x5400u
#define REG_RAH0        0x5404u

#define CTRL_SLU        (1u << 6)   /* set link up */
#define CTRL_ASDE       (1u << 5)   /* auto-speed detection */
#define CTRL_RST        (1u << 26)

#define RCTL_EN         (1u << 1)
#define RCTL_BAM        (1u << 15)  /* broadcast accept */
#define RCTL_BSIZE_2048 0u          /* bits 17:16 = 00 with BSEX clear */
#define RCTL_SECRC      (1u << 26)  /* strip the CRC the card checked */

#define TCTL_EN         (1u << 1)
#define TCTL_PSP        (1u << 3)   /* pad short packets to 64 */
#define TCTL_CT         (0x10u << 4)    /* collision threshold, 16 */
#define TCTL_COLD       (0x40u << 12)   /* collision distance, full duplex */

/* IEEE 802.3's inter-packet gaps for copper: 10, 8 and 6 (13.4.35). */
#define TIPG_IEEE       (10u | (8u << 10) | (6u << 20))

#define ICR_RXT0        (1u << 7)   /* a frame arrived */
#define ICR_LSC         (1u << 2)   /* the link changed */
#define ICR_RXDMT0      (1u << 4)   /* the receive ring is running low */

#define TX_CMD_EOP      0x01u
#define TX_CMD_IFCS     0x02u       /* the card appends the CRC */
#define TX_CMD_RS       0x08u       /* report status: write the descriptor back */

#define RESET_MS        20u
#define LINK_MS         2000u       /* how long a link is waited for, once */

/*
 * **Thirty-two descriptors each way.** The card wants a multiple of eight
 * and the ring's bytes a multiple of 128; 32 of 16 bytes is 512, which is
 * both. At a gigabit and full-size frames that is about four tenths of a
 * millisecond of buffering, which is the same figure `ethring.h` arrived at
 * for the same reason and is not a coincidence: both are "enough to cover
 * one pass of something being busy".
 */
#define RING_SLOTS      32u
#define FRAME_SLOT      2048u       /* what RCTL's BSIZE says a buffer is */

/*
 * The one region, laid out: the two descriptor rings in the first page, so
 * they share no cache line with a frame, and the frames after them.
 */
#define RX_DESC_AT      0u
#define TX_DESC_AT      (RING_SLOTS * E1000_DESC_BYTES)
#define RX_FRAMES_AT    4096u
#define TX_FRAMES_AT    (RX_FRAMES_AT + RING_SLOTS * FRAME_SLOT)
#define REGION_BYTES    (TX_FRAMES_AT + RING_SLOTS * FRAME_SLOT)
#define REGION_PAGES    ((REGION_BYTES + 4095u) / 4096u)

static long console = -1;
static long frames_endpoint = -1;

static struct {
    bool          present;
    uintptr_t     regs;             /* the card's registers, mapped here */
    unsigned      where;            /* its address on PCI */
    unsigned      device;           /* its PCI device identifier */
    long          irq;              /* a capability, or negative */
    unsigned      intid;

    uintptr_t     mem;              /* the rings and frames, mapped here */
    uint64_t      bus;              /* ...and where the card finds them */

    uint8_t       mac[6];
    bool          have_mac;
    struct e1000_link link;
    bool          link_said;

    uint32_t      rx_next;          /* the descriptor this end looks at next */
    uint32_t      tx_next;
    unsigned long sent, received, dropped;

    struct eth_ring *ring;          /* the stack's, when it has attached */
    long          ring_cap;
} card;

static uint32_t reg_read(unsigned at)
{
    return mmio_read32(card.regs + at);
}

static void reg_write(unsigned at, uint32_t value)
{
    mmio_write32(card.regs + at, value);
}

static uint8_t *rx_desc(unsigned i)
{
    return (uint8_t *)(card.mem + RX_DESC_AT + i * E1000_DESC_BYTES);
}

static uint8_t *tx_desc(unsigned i)
{
    return (uint8_t *)(card.mem + TX_DESC_AT + i * E1000_DESC_BYTES);
}

static void put64(uint8_t *at, uint64_t v)
{
    unsigned i;

    for (i = 0; i < 8u; i++) {
        at[i] = (uint8_t)(v >> (8u * i));
    }
}

static void put16(uint8_t *at, uint16_t v)
{
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
}

/*
 * **The card and this end must agree about memory that is not registers.**
 *
 * The descriptors are ordinary cached memory - they are read and written
 * constantly, which is why `MEM_CONTIGUOUS` maps them normally rather than
 * as device memory - so a store that publishes a descriptor has to be
 * visible to the card before the doorbell that tells it to look. That is the
 * same ordering `ethring.h` needs for another reader, and the same barrier.
 */
#if defined(__x86_64__)
#define RING_BARRIER()  __asm__ volatile("" ::: "memory")
#else
#define RING_BARRIER()  __asm__ volatile("dmb ish" ::: "memory")
#endif

/*--------------------------------------------------------------- bring-up */

static bool wait_for_reset(void)
{
    unsigned long ticks = kosmos_ticks(), hz = 62500000ul;
    struct sysinfo info;
    unsigned long span;

    if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) {
        hz = (unsigned long)info.counter_hz;
    }

    span = hz / 1000ul * RESET_MS;

    while (kosmos_ticks() - ticks < span) {
        if ((reg_read(REG_CTRL) & CTRL_RST) == 0) {
            return true;
        }

        kosmos_sleep(1);
    }

    return (reg_read(REG_CTRL) & CTRL_RST) == 0;
}

/*
 * The rings, and the buffers each descriptor points at.
 *
 * The receive ring is handed to the card full - every descriptor pointing at
 * an empty buffer - and its tail is the *last* of them: the card fills from
 * the head towards the tail and stops when they meet, so a tail at the last
 * descriptor is a ring with every slot available. A tail equal to the head
 * would be a ring the card believes is full of frames nobody has read.
 */
static void rings_start(void)
{
    unsigned i;

    memset((void *)card.mem, 0, REGION_BYTES);

    for (i = 0; i < RING_SLOTS; i++) {
        put64(rx_desc(i), card.bus + RX_FRAMES_AT + i * FRAME_SLOT);
    }

    RING_BARRIER();

    reg_write(REG_RDBAL, (uint32_t)(card.bus + RX_DESC_AT));
    reg_write(REG_RDBAH, (uint32_t)((card.bus + RX_DESC_AT) >> 32));
    reg_write(REG_RDLEN, RING_SLOTS * E1000_DESC_BYTES);
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RING_SLOTS - 1u);

    reg_write(REG_TDBAL, (uint32_t)(card.bus + TX_DESC_AT));
    reg_write(REG_TDBAH, (uint32_t)((card.bus + TX_DESC_AT) >> 32));
    reg_write(REG_TDLEN, RING_SLOTS * E1000_DESC_BYTES);
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);

    card.rx_next = 0;
    card.tx_next = 0;

    reg_write(REG_TIPG, TIPG_IEEE);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    /*
     * **Frames addressed to this card and broadcast, and nothing else.**
     * Not promiscuous, for the reason the USB adapter's filter is not: a
     * stack handed every frame on the wire throws most of them away. The
     * multicast table is cleared above with the rest of the region, so a
     * multicast address reaches nothing until something asks for one -
     * which is a thing to build when there is something that wants it.
     */
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
}

static bool bring_up(struct say_line *line)
{
    struct dev_info dev;
    long region, mapped, bus, regs;
    unsigned i;

    if (kosmos_dev_find(DEV_INTEL_ETHERNET, 0, &dev) != 0) {
        return false;               /* no such card; not a fault */
    }

    regs = kosmos_dev_map(dev.base, (dev.size + 4095u) / 4096u);

    if (regs < 0) {
        say_begin(line);
        say_text(line, "e1000: the card's registers could not be mapped");
        say_send(console, line);
        return false;
    }

    card.regs = (uintptr_t)regs;
    card.where = dev.where;
    card.device = dev.line;
    card.intid = dev.intid;

    /*
     * **Quiet first, then reset.** An interrupt from a card mid-reset is one
     * nobody can answer, and the mask survives the reset on this family only
     * by being set again afterwards - so it is written twice on purpose.
     */
    reg_write(REG_IMC, 0xFFFFFFFFu);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);

    if (!wait_for_reset()) {
        say_begin(line);
        say_text(line, "e1000: the card did not come out of its reset");
        say_send(console, line);
        return false;
    }

    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);

    card.have_mac = e1000_decode_mac(reg_read(REG_RAL0), reg_read(REG_RAH0),
                                     card.mac);

    /* The multicast table, every entry, before the receiver is enabled. */
    for (i = 0; i < 128u; i++) {
        reg_write(REG_MTA + i * 4u, 0);
    }

    /* Link up, and let the PHY work out the speed with the far end. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    region = kosmos_mem_create_flags(REGION_PAGES, MEM_CONTIGUOUS);
    mapped = region < 0 ? region : kosmos_mem_map(region);
    bus = mapped < 0 ? mapped : kosmos_mem_phys(region);

    if (region < 0 || mapped < 0 || bus <= 0) {
        say_begin(line);
        say_text(line, "e1000: no memory the card can reach for its rings");
        say_send(console, line);
        return false;
    }

    card.mem = (uintptr_t)mapped;
    card.bus = (uint64_t)bus;
    rings_start();

    card.irq = kosmos_irq_claim(dev.intid);
    reg_write(REG_IMS, ICR_RXT0 | ICR_LSC | ICR_RXDMT0);

    card.present = true;
    return true;
}

/*----------------------------------------------------------------- frames */

/*
 * A frame out: the next transmit descriptor, the doorbell, and the card's
 * write-back waited for.
 *
 * Waiting is right here and would not be on a faster link than this system
 * can saturate: the caller has the frame in hand and a descriptor that was
 * refused is a frame that did not go, which is something it has to know. A
 * ring of thirty-two that nobody waits on is a ring that wraps onto
 * descriptors the card has not finished with.
 */
static bool send_frame(const uint8_t *frame, unsigned length)
{
    uint8_t *desc = tx_desc(card.tx_next);
    unsigned tries;

    if (!card.present || length < 14u || length > FRAME_SLOT) {
        return false;
    }

    memcpy((uint8_t *)(card.mem + TX_FRAMES_AT + card.tx_next * FRAME_SLOT),
           frame, length);

    memset(desc, 0, E1000_DESC_BYTES);
    put64(desc, card.bus + TX_FRAMES_AT + card.tx_next * FRAME_SLOT);
    put16(desc + 8, (uint16_t)length);
    desc[11] = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;

    RING_BARRIER();
    card.tx_next = (card.tx_next + 1u) % RING_SLOTS;
    reg_write(REG_TDT, card.tx_next);

    for (tries = 0; tries < 1000u; tries++) {
        RING_BARRIER();

        if (e1000_decode_tx_done(desc)) {
            card.sent++;
            return true;
        }

        kosmos_sleep(1);
    }

    return false;
}

/*
 * Every frame the card has written back, into the stack's ring.
 *
 * The descriptors are walked from where this end left off rather than from
 * the card's head, because the head is where the card is *writing* and the
 * two are only the same when the ring is empty. A descriptor is given back
 * as soon as its frame is copied out, and the tail follows one behind.
 */
static void take_frames(void)
{
    unsigned taken = 0;

    for (;;) {
        uint8_t *desc = rx_desc(card.rx_next);
        struct e1000_rx got;

        RING_BARRIER();
        e1000_decode_rx(desc, &got);

        if (!got.done) {
            break;
        }

        if (got.end && !got.error && got.length >= 14u
            && got.length <= ETH_RING_SLOT && card.ring != NULL) {
            uint32_t write = card.ring->in_write;
            uint32_t read = eth_ring_acquire(&card.ring->in_read);

            if (eth_ring_space(write, read) > 0) {
                memcpy(eth_ring_in(card.ring, write),
                       (const uint8_t *)(card.mem + RX_FRAMES_AT
                                         + card.rx_next * FRAME_SLOT),
                       got.length);
                card.ring->in_length[write % ETH_RING_SLOTS] = got.length;
                eth_ring_publish(&card.ring->in_write, write + 1u);
                card.received++;
                taken++;
            } else {
                card.dropped++;
            }
        } else if (got.end) {
            card.dropped++;
        }

        /* Given back to the card, empty, and the tail follows. */
        memset(desc, 0, E1000_DESC_BYTES);
        put64(desc, card.bus + RX_FRAMES_AT + card.rx_next * FRAME_SLOT);
        RING_BARRIER();
        reg_write(REG_RDT, card.rx_next);
        card.rx_next = (card.rx_next + 1u) % RING_SLOTS;
    }

    if (taken > 0) {
        (void)kosmos_net_wake();
    }
}

/* Whatever the stack has put in the ring, onto the wire. */
static void drain_out(void)
{
    uint32_t read, write;

    if (card.ring == NULL) {
        return;
    }

    read = card.ring->out_read;
    write = eth_ring_acquire(&card.ring->out_write);

    while (eth_ring_ready(write, read) > 0) {
        uint32_t length = card.ring->out_length[read % ETH_RING_SLOTS];

        if (length >= 14u && length <= ETH_RING_SLOT) {
            (void)send_frame(eth_ring_out(card.ring, read), length);
        }

        read++;
        eth_ring_publish(&card.ring->out_read, read);
    }
}

/* The link, said when it changes and not on every look. */
static void say_link(struct say_line *line)
{
    struct e1000_link now;

    e1000_decode_link(reg_read(REG_STATUS), &now);

    if (card.link_said && now.up == card.link.up
        && now.mbps == card.link.mbps) {
        return;
    }

    card.link = now;
    card.link_said = true;

    say_begin(line);
    say_text(line, "e1000: its link is ");

    if (!now.up) {
        say_text(line, "down");
    } else {
        say_text(line, "up at ");

        if (now.mbps == 0) {
            say_text(line, "a speed this does not know");
        } else {
            say_dec(line, now.mbps);
            say_text(line, " Mb/s");
        }

        say_text(line, now.full_duplex ? ", full duplex" : ", half duplex");
    }

    say_send(console, line);
}

/*--------------------------------------------------------------- the stack */

static void about(struct eth_reply *rep)
{
    rep->present = 1;
    rep->mtu = 1514;
    rep->link = card.link.up ? 1u : 0u;
    rep->sent = (uint32_t)card.sent;
    rep->received = (uint32_t)card.received;
    memcpy(rep->mac, card.mac, sizeof(rep->mac));
}

static void answer(const struct message *msg, uint64_t sender, long cap,
                   struct say_line *line)
{
    struct eth_request req;
    struct eth_reply rep;
    struct message out;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));

    if (msg->length >= sizeof(req)) {
        memcpy(&req, msg->data, sizeof(req));
    } else {
        rep.error = ETH_ERR_BAD_OP;
    }

    if (rep.error == ETH_OK && (!card.present || !card.have_mac)) {
        rep.error = ETH_ERR_NO_ADAPTER;
    }

    if (rep.error == ETH_OK) {
        switch (req.op) {
        case ETH_OP_ATTACH: {
            long at;

            if (card.ring != NULL) {
                rep.error = ETH_ERR_TAKEN;
                break;
            }

            at = cap < 0 ? -1 : kosmos_mem_map(cap);

            if (at < 0 || !eth_ring_valid((struct eth_ring *)(uintptr_t)at)) {
                rep.error = ETH_ERR_NO_RING;
                break;
            }

            card.ring = (struct eth_ring *)(uintptr_t)at;
            card.ring_cap = cap;
            about(&rep);

            say_begin(line);
            say_text(line, "e1000: the network stack has it; frames go "
                           "through a ring of ");
            say_dec(line, ETH_RING_SLOTS);
            say_text(line, " each way");
            say_send(console, line);
            break;
        }

        case ETH_OP_SEND:
            if (card.ring == NULL) {
                rep.error = ETH_ERR_NO_RING;
                break;
            }

            drain_out();
            about(&rep);
            break;

        case ETH_OP_INFO:
            about(&rep);
            break;

        default:
            rep.error = ETH_ERR_BAD_OP;
            break;
        }
    }

    memset(&out, 0, sizeof(out));
    out.tag = msg->tag;
    out.length = sizeof(rep);
    memcpy(out.data, &rep, sizeof(rep));
    (void)kosmos_reply(sender, &out);
}

static void serve(struct say_line *line)
{
    struct message msg;
    uint64_t sender = 0;

    while (frames_endpoint >= 0
           && kosmos_receive(frames_endpoint, &msg, &sender, 1, 0) == 0) {
        answer(&msg, sender,
               msg.cap_plus_one > 0 ? (long)msg.cap_plus_one - 1 : -1, line);
    }
}

/*
 * **A driver with no card still answers**, because the stack asks every
 * driver it has an endpoint for whether there is one, and a question nobody
 * answers is a stack that waits for ever. The same arrangement the USB
 * driver has on a machine with no controller.
 */
void e1000_server(long console_cap, long frames_cap)
{
    struct say_line line;
    const long ends[1] = { frames_cap };

    console = console_cap;
    frames_endpoint = frames_cap;
    memset(&card, 0, sizeof(card));
    card.irq = -1;
    card.ring_cap = -1;

    if (bring_up(&line) && card.have_mac) {
        unsigned i;

        say_begin(&line);
        say_text(&line, "e1000: 8086:");
        say_hex(&line, card.device, 4);
        say_text(&line, " at ");
        say_hex(&line, (card.where >> 8) & 0xFFu, 2);
        say_text(&line, ":");
        say_hex(&line, (card.where >> 3) & 0x1Fu, 2);
        say_text(&line, ".");
        say_dec(&line, card.where & 0x7u);
        say_text(&line, ", MAC ");

        for (i = 0; i < 6u; i++) {
            if (i != 0) {
                say_text(&line, ":");
            }

            say_hex(&line, card.mac[i], 2);
        }

        say_text(&line, ", ");
        say_dec(&line, RING_SLOTS);
        say_text(&line, " descriptors each way, interrupt ");
        say_dec(&line, card.intid);

        if (card.irq < 0) {
            say_text(&line, " not claimed, so polled");
        }

        say_send(console, &line);
        say_link(&line);
    } else if (card.present) {
        say_begin(&line);
        say_text(&line, "e1000: the card has no address of its own; "
                        "not driven");
        say_send(console, &line);
        card.present = false;
    }

    for (;;) {
        long woke = SYS_NO_INTERRUPT;
        long lines[1];

        if (card.present && card.irq >= 0) {
            lines[0] = card.irq;
            woke = kosmos_irq_wait_any(lines, 1, 25ul, ends, 1u);
        } else {
            /*
             * No card, or a line that could not be claimed: the endpoint
             * alone, and a deadline that is the stack's own - a card that
             * is polled is looked at at that rate rather than at one this
             * file picked.
             */
            woke = kosmos_irq_wait_any(NULL, 0, card.present ? 4ul : 0ul,
                                       ends, 1u);
        }

        if (woke < 0 && woke != SYS_NO_INTERRUPT) {
            kosmos_exit(0);
        }

        if (card.present) {
            (void)reg_read(REG_ICR);     /* read to clear */
            take_frames();
            say_link(&line);

            if (card.irq >= 0) {
                (void)kosmos_irq_ack(card.irq);
            }
        }

        serve(&line);
    }
}
