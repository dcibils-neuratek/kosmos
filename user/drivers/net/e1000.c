/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An Intel Ethernet controller, driven from a process.
 *
 *   /Network  <--- ethring.h ---  this  --- registers and rings --->  the card
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
#include "init/say.h"
#include "ethproto.h"
#include "ethring.h"
#include "e1000_decode.h"

/*------------------------------------------------------------------ registers
 *
 * Offsets into the first BAR, from Intel's own manuals for this family
 * (82540EM 13.4, 82574L 8.2, I219 in the PCH datasheets). Only the ones this
 * driver writes are here; a register nobody touches is a register nobody has
 * to have got right - and the two it only reads, so that a card which will
 * not send can be described in the log, are at the offsets Linux's e1000e
 * gives every chip it drives, the I219 included (`hw.h`).
 */
#define REG_CTRL        0x0000u
#define REG_STATUS      0x0008u
#define REG_CTRL_EXT    0x0018u     /* read only, for the log */
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
#define REG_TXDCTL      0x3828u     /* queue 0; read only, for the log */
#define REG_MTA         0x5200u     /* 128 entries of the multicast table */
#define REG_RAL0        0x5400u
#define REG_RAH0        0x5404u

/*
 * **The I219's own**, for what Linux's `e1000e` does to one that an 82574L
 * does not need (`pch_bring_up`, `testing.md` 18.346): every offset and bit
 * here is from v6.12's `regs.h`, `defines.h` and `ich8lan.h`, read on 2
 * October 2026, and none is from memory.
 */
#define REG_EXTCNF_CTRL 0x0F00u     /* the software flag, shared with firmware */
#define REG_IOSFPC      0x0F28u
#define REG_PBECCSTS    0x100Cu
#define REG_KABGTXD     0x3004u
#define REG_TXDCTL1     0x3928u
#define REG_TARC0       0x3840u
#define REG_TARC1       0x3940u
#define REG_RFCTL       0x5008u
#define REG_FWSM        0x5B54u
#define REG_PBA         0x1000u     /* packet buffer: receive's share, in KB */
#define REG_WUC         0x5800u     /* wake-up control */

#define CTRL_GIO_MASTER_DISABLE 0x00000004u
#define CTRL_MEHE               0x00080000u
#define STATUS_GIO_MASTER_ENABLE 0x00080000u
#define CTRL_EXT_BIT22          (1u << 22)
#define CTRL_EXT_PHYPDEN        0x00100000u
#define CTRL_EXT_DRV_LOAD       0x10000000u
#define TXDCTL_COUNT_DESC       (1u << 22)
#define TARC0_BITS              ((1u << 23) | (1u << 24) | (1u << 26) | (1u << 27))
#define TARC0_CB_MULTIQ_3_REQ   0x30000000u
#define TARC0_CB_MULTIQ_2_REQ   0x20000000u
#define TARC1_BITS              ((1u << 24) | (1u << 26) | (1u << 30))
#define TARC1_BIT28             (1u << 28)
#define TCTL_RTLC               0x01000000u
#define TCTL_MULR               0x10000000u
#define TCTL_CT_E1000E          (15u << 4)
#define TCTL_COLD_E1000E        (63u << 12)
#define RFCTL_NFS_DIS           0x000000C0u     /* NFSW_DIS | NFSR_DIS */
#define RFCTL_EXTEN             0x00008000u     /* extended receive descriptors */
#define PBECCSTS_ECC_ENABLE     0x00010000u
#define RDMTS_HEX               0x00010000u     /* E1000_RCTL_RDMTS_HEX */
#define KABGTXD_BGSQLBIAS       0x00050000u
#define EXTCNF_CTRL_SWFLAG      0x00000020u
#define RAH_AV                  0x80000000u
#define TCTL_CT_MASK            0x00000FF0u
#define TCTL_COLD_MASK          0x003FF000u
#define PBA_SPT_KB              26u     /* e1000_pch_spt_info's `.pba` */

/* In configuration space (`SYS_DEV_CONFIG`): `e1000.h`'s. */
#define PCI_COMMAND_WORD        0x04u
#define PCI_COMMAND_MASTER      0x0004u
#define PCICFG_DESC_RING_STATUS 0xE4u
#define FLUSH_DESC_REQUIRED     0x0100u

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
    struct e1000_tx_ring tx;        /* which transmit descriptors are whose */
    uint64_t      tx_posted[RING_SLOTS];    /* when each was handed over */
    unsigned long sent, received, dropped;
    unsigned long refused;          /* frames for a ring with no room */
    bool          tx_held;          /* the card has kept a frame a second */
    unsigned      tx_said;          /* how often that has been said */
    bool          tx_off;           /* `opt/kosmos/e1000fault`, for a test */
    uint64_t      hz;               /* the counter's, for "a second" */
    unsigned long tick_hz;          /* the scheduler's, for a sleep */
    bool          pch;              /* an I219, brought up as e1000e does */
    uint32_t      tctl_firmware;    /* TCTL as the firmware left it */
    bool          rx_ext;           /* receive descriptors in e1000e's layout */
    uint64_t      started_at;       /* when the rings were handed over */
    bool          rx_said;          /* the first five seconds, said */
    bool          reset_first;      /* `opt/kosmos/e1000path=pch-reset` */

    struct eth_ring *ring;          /* the stack's, when it has attached */
    long          ring_cap;
} card;

static bool send_frame(const uint8_t *frame, unsigned length);
static void tx_reclaim(void);

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
 * **What `e1000e` sets on an I219 that an 82574L does without**, in its
 * order: `e1000_initialize_hw_bits_ich8lan`, then `e1000_configure_tx` with
 * its errata for this generation (SPT) - and all of it before the
 * transmitter is enabled, which Linux's own comment insists on for TARC0
 * ("need to do this after setting TARC(0)"). Our driver set none of it, and
 * the I219 took a frame and never read its descriptor (`testing.md`
 * 18.346). Which of these that was, if any, is not known: this is the
 * whole of what the reference does, not a guess at the one that matters.
 */
static void pch_transmit_bits(void)
{
    uint32_t v;

    reg_write(REG_CTRL_EXT, reg_read(REG_CTRL_EXT) | CTRL_EXT_BIT22
                            | CTRL_EXT_PHYPDEN);

    reg_write(REG_TXDCTL, reg_read(REG_TXDCTL) | TXDCTL_COUNT_DESC);

    /* "erratum work around: set txdctl the same for both queues" */
    reg_write(REG_TXDCTL1, reg_read(REG_TXDCTL));

    v = reg_read(REG_TARC0) | TARC0_BITS;

    /* "SPT and KBL Si errata workaround to avoid Tx hang": two outstanding
     * requests rather than three. */
    v = (v & ~TARC0_CB_MULTIQ_3_REQ) | TARC0_CB_MULTIQ_2_REQ;
    reg_write(REG_TARC0, v);

    v = reg_read(REG_TARC1);
    v = (reg_read(REG_TCTL) & TCTL_MULR) ? (v & ~TARC1_BIT28) : (v | TARC1_BIT28);
    reg_write(REG_TARC1, v | TARC1_BITS);

    reg_write(REG_RFCTL, reg_read(REG_RFCTL) | RFCTL_NFS_DIS);
    reg_write(REG_PBECCSTS, reg_read(REG_PBECCSTS) | PBECCSTS_ECC_ENABLE);
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_MEHE);

    /* "SPT and KBL Si errata workaround to avoid data corruption" */
    reg_write(REG_IOSFPC, reg_read(REG_IOSFPC) | RDMTS_HEX);
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
    card.tx.slots = RING_SLOTS;
    card.tx.next = 0;
    card.tx.clean = 0;

    reg_write(REG_TIPG, TIPG_IEEE);

    if (card.pch) {
        pch_transmit_bits();
    }

    /*
     * **The transmitter left off, when a test asks** (`testing.md` 18.345):
     * the card then keeps every frame it is given and confirms none, which
     * is what the M700's I219 did on 2 October - and QEMU's 82574L does
     * exactly that with TCTL.EN clear. It is the one way to show, under
     * QEMU, that a card which never sends cannot stop the stack.
     */
    if (card.pch) {
        /*
         * **Read, changed and written, as `e1000_configure_tx` does it** -
         * the collision fields and the enable replaced, and every other bit
         * kept: on the M700 that is multiple requests, which a fixed value
         * here cleared (`testing.md` 18.347).
         */
        uint32_t tctl = reg_read(REG_TCTL)
                        & ~(TCTL_EN | TCTL_CT_MASK | TCTL_COLD_MASK);

        reg_write(REG_TCTL, tctl | (card.tx_off ? 0u : TCTL_EN) | TCTL_PSP
                            | TCTL_RTLC | TCTL_CT_E1000E | TCTL_COLD_E1000E);
    } else {
        reg_write(REG_TCTL, (card.tx_off ? 0u : TCTL_EN) | TCTL_PSP
                            | TCTL_CT | TCTL_COLD);
    }

    /*
     * **Frames addressed to this card and broadcast, and nothing else.**
     * Not promiscuous, for the reason the USB adapter's filter is not: a
     * stack handed every frame on the wire throws most of them away. The
     * multicast table is cleared above with the rest of the region, so a
     * multicast address reaches nothing until something asks for one -
     * which is a thing to build when there is something that wants it.
     */
    /*
     * **Received frames in `e1000e`'s layout on an I219** (`testing.md`
     * 18.349): `e1000_setup_rctl` sets RFCTL.EXTEN on every chip, and the
     * M700's firmware - whose own driver is Intel's - may leave it on, in
     * which case a frame read in the legacy layout is a length taken for a
     * status and never arrives. So the I219 is told to write the extended
     * layout and is read in it, whatever the firmware chose.
     */
    if (card.pch) {
        reg_write(REG_RFCTL, reg_read(REG_RFCTL) | RFCTL_EXTEN);
        card.rx_ext = true;
    }

    card.started_at = kosmos_ticks();

    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_BSIZE_2048 | RCTL_SECRC);
}

/*--------------------------------------------------------------- the I219 */

/* The I219s `e1000e` drives as `board_pch_spt` (`netdev.c`, `hw.h`). */
static bool is_pch_spt(unsigned device)
{
    switch (device) {
    case 0x156Fu: case 0x1570u: case 0x15B7u: case 0x15B8u: case 0x15B9u:
    case 0x15D6u: case 0x15D7u: case 0x15D8u: case 0x15E3u:
    case 0x0D53u: case 0x0D55u:
        return true;
    default:
        return false;
    }
}

/* At least `ms`: a sleep of n ticks ends at the nth tick from now, which is
 * between n - 1 and n of them away. */
static void sleep_ms(unsigned ms)
{
    unsigned long hz = card.tick_hz != 0 ? card.tick_hz : 250ul;

    kosmos_sleep((ms * hz + 999ul) / 1000ul + 1ul);
}

static unsigned long micros_since(uint64_t start)
{
    return (unsigned long)((kosmos_ticks() - start) * 1000000ull / card.hz);
}

/*
 * The transmitter, said: what the line after a failed attempt shows, so the
 * log says how far the card got.
 */
static void say_transmitter(struct say_line *line, const char *what)
{
    say_begin(line);
    say_text(line, what);
    say_text(line, ": TCTL ");
    say_hex(line, reg_read(REG_TCTL), 8);
    say_text(line, " TDH ");
    say_dec(line, reg_read(REG_TDH));
    say_text(line, " TDT ");
    say_dec(line, reg_read(REG_TDT));
    say_text(line, " TXDCTL ");
    say_hex(line, reg_read(REG_TXDCTL), 8);
    say_text(line, " TARC0 ");
    say_hex(line, reg_read(REG_TARC0), 8);
    say_text(line, " STATUS ");
    say_hex(line, reg_read(REG_STATUS), 8);
    say_text(line, " RDH ");
    say_dec(line, reg_read(REG_RDH));
    say_send(console, line);
}

/*
 * **What the firmware left**, before anything is touched: the M700's boots
 * its own network stack first, and what it did to the card is the first
 * thing to know when the card does not do what it is told.
 */
static void say_firmware(struct say_line *line)
{
    card.tctl_firmware = reg_read(REG_TCTL);

    say_begin(line);
    say_text(line, "e1000: as the firmware left it: CTRL ");
    say_hex(line, reg_read(REG_CTRL), 8);
    say_text(line, " STATUS ");
    say_hex(line, reg_read(REG_STATUS), 8);
    say_text(line, " CTRL_EXT ");
    say_hex(line, reg_read(REG_CTRL_EXT), 8);
    say_text(line, " RCTL ");
    say_hex(line, reg_read(REG_RCTL), 8);
    say_text(line, " TCTL ");
    say_hex(line, reg_read(REG_TCTL), 8);
    say_text(line, " FWSM ");
    say_hex(line, reg_read(REG_FWSM), 8);
    say_send(console, line);

    say_begin(line);
    say_text(line, "e1000: its rings as the firmware left them: TDLEN ");
    say_dec(line, reg_read(REG_TDLEN));
    say_text(line, " TDH ");
    say_dec(line, reg_read(REG_TDH));
    say_text(line, " TDT ");
    say_dec(line, reg_read(REG_TDT));
    say_text(line, " RDLEN ");
    say_dec(line, reg_read(REG_RDLEN));
    say_text(line, " RDH ");
    say_dec(line, reg_read(REG_RDH));
    say_text(line, " RDT ");
    say_dec(line, reg_read(REG_RDT));
    say_text(line, " TARC0 ");
    say_hex(line, reg_read(REG_TARC0), 8);
    say_text(line, " PBA ");
    say_hex(line, reg_read(REG_PBA), 8);
    say_text(line, " WUC ");
    say_hex(line, reg_read(REG_WUC), 8);
    say_text(line, " RFCTL ");
    say_hex(line, reg_read(REG_RFCTL), 8);
    say_send(console, line);
}

/*
 * **What the card is on the bus** (`testing.md` 18.347): its command
 * register, which says whether it may fetch from memory at all, and - for
 * an I219 - the descriptor-ring status Linux's `e1000e` reads before a
 * reset, whose bit 8 says the rings must be emptied first or the chip
 * "enter[s] a unit hang state which can only be released by PCI reset".
 */
static void say_pci(struct say_line *line)
{
    long command = kosmos_dev_config(DEV_INTEL_ETHERNET, 0, PCI_COMMAND_WORD);
    long rings = kosmos_dev_config(DEV_INTEL_ETHERNET, 0, PCICFG_DESC_RING_STATUS);

    say_begin(line);
    say_text(line, "e1000: on PCI: ");

    if (command < 0) {
        say_text(line, "its configuration could not be read");
        say_send(console, line);
        return;
    }

    say_text(line, "command ");
    say_hex(line, (unsigned long)command & 0xFFFFu, 4);
    say_text(line, ((unsigned long)command & PCI_COMMAND_MASTER) != 0
                   ? ", bus mastering on" : ", bus mastering OFF");
    say_text(line, "; status ");
    say_hex(line, ((unsigned long)command >> 16) & 0xFFFFu, 4);

    if (card.pch && rings >= 0) {
        say_text(line, "; descriptor rings ");
        say_hex(line, (unsigned long)rings & 0xFFFFu, 4);
        say_text(line, ((unsigned long)rings & FLUSH_DESC_REQUIRED) != 0
                       ? ", a flush asked for" : ", no flush asked for");
    }

    say_send(console, line);
}

/*
 * Both units stopped and a moment for whatever they were doing to finish -
 * `e1000_reset_hw_ich8lan`'s first steps, "to allow any pending
 * transactions to complete".
 */
static void pch_quiet(bool reset_follows)
{
    reg_write(REG_IMC, 0xFFFFFFFFu);
    reg_write(REG_RCTL, 0);

    /*
     * `e1000e` writes PSP alone, because a reset follows and puts TCTL back
     * to what the chip starts with - multiple requests among it (MULR, bit
     * 28, and bit 29 with it on the M700's). With no reset, that write
     * would be the last word, so the firmware's bits are kept and only the
     * enable taken away (`testing.md` 18.347).
     */
    reg_write(REG_TCTL, reset_follows ? TCTL_PSP
                                      : (card.tctl_firmware & ~TCTL_EN));
    (void)reg_read(REG_STATUS);
    sleep_ms(10);
}

/*
 * **The MAC reset, as `e1000e` does it** (`e1000_reset_hw_ich8lan`), for the
 * case where the first try sent nothing.
 *
 * The card's bus requests stopped first - "Prevent the PCI-E bus from
 * sticking if there is no TLP connection on the last TLP read/write
 * transaction when MAC is reset" - then the units, then the flag the
 * firmware shares, then the reset, and then **nothing read or written for
 * twenty milliseconds**: "cannot issue a flush here because it hangs the
 * hardware". The driver before this read CTRL in a loop straight after the
 * reset, which is exactly that.
 *
 * The MAC only. `e1000e` resets the PHY with it unless the firmware forbids
 * that, and then programs the PHY again over its own interface, which this
 * driver does not have; resetting the MAC alone is what `e1000e` does when
 * the PHY reset is forbidden, and the PHY keeps the link the firmware made.
 */
static void pch_reset(struct say_line *line)
{
    uint64_t start = kosmos_ticks();
    unsigned long stopped_us = 0;
    bool stopped = false, flag = false;
    uint32_t ctrl;
    unsigned i;

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_GIO_MASTER_DISABLE);

    /* MASTER_DISABLE_TIMEOUT, 800 looks a hundred microseconds apart. */
    while (micros_since(start) < 80000ul) {
        if ((reg_read(REG_STATUS) & STATUS_GIO_MASTER_ENABLE) == 0) {
            stopped = true;
            stopped_us = micros_since(start);
            break;
        }
    }

    pch_quiet(true);

    /* `e1000e_reset`'s first line: the packet buffer's split, which "require[s] a
     * configuration cycle of the hardware" - the reset below. */
    reg_write(REG_PBA, PBA_SPT_KB);

    /*
     * The software flag: free first (PHY_CFG_TIMEOUT, 100 ms), then taken
     * and read back as ours (SW_FLAG_TIMEOUT is a second; a tenth is
     * plenty to know). The reset goes ahead either way, as in `e1000e`.
     */
    for (i = 0; i < 25u && (reg_read(REG_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) != 0; i++) {
        sleep_ms(4);
    }

    ctrl = reg_read(REG_CTRL);
    reg_write(REG_EXTCNF_CTRL, reg_read(REG_EXTCNF_CTRL) | EXTCNF_CTRL_SWFLAG);

    for (i = 0; i < 25u; i++) {
        if ((reg_read(REG_EXTCNF_CTRL) & EXTCNF_CTRL_SWFLAG) != 0) {
            flag = true;
            break;
        }

        sleep_ms(4);
    }

    reg_write(REG_CTRL, ctrl | CTRL_RST);
    sleep_ms(20);

    reg_write(REG_IMC, 0xFFFFFFFFu);
    (void)reg_read(REG_ICR);
    reg_write(REG_KABGTXD, reg_read(REG_KABGTXD) | KABGTXD_BGSQLBIAS);

    say_begin(line);
    say_text(line, "e1000: its MAC reset as e1000e does it: bus requests ");

    if (stopped) {
        say_text(line, "stopped in ");
        say_dec(line, stopped_us);
        say_text(line, " us");
    } else {
        say_text(line, "still pending after 80 ms");
    }

    say_text(line, flag ? ", the flag taken" : ", the flag held by firmware");
    say_text(line, "; after it CTRL ");
    say_hex(line, reg_read(REG_CTRL), 8);
    say_text(line, " STATUS ");
    say_hex(line, reg_read(REG_STATUS), 8);
    say_send(console, line);
}

/*
 * **A frame to itself**: addressed to this card's own MAC, from it, of the
 * IEEE's local experimental type 0x88B5, so a switch learns the address is
 * on this port and drops the frame there - nothing else on the network sees
 * it. The card's confirmation within fifty milliseconds is the whole test;
 * on a link that is up it takes microseconds.
 */
static bool self_test(unsigned long *took_us)
{
    uint8_t frame[60];
    uint64_t start;

    memset(frame, 0, sizeof(frame));
    memcpy(frame, card.mac, 6);
    memcpy(frame + 6, card.mac, 6);
    frame[12] = 0x88u;
    frame[13] = 0xB5u;
    memcpy(frame + 14, "Kosmos e1000 self-test", 22);

    start = kosmos_ticks();

    if (!send_frame(frame, sizeof(frame))) {
        return false;
    }

    while (micros_since(start) < 50000ul) {
        tx_reclaim();

        if (e1000_tx_out(&card.tx) == 0) {
            *took_us = micros_since(start);
            return true;
        }

        kosmos_sleep(1);
    }

    return false;
}

/* The MAC address back in RAL0 and RAH0, if a reset left them empty. */
static void mac_restore(void)
{
    uint8_t now[6];

    if (e1000_decode_mac(reg_read(REG_RAL0), reg_read(REG_RAH0), now)) {
        return;
    }

    reg_write(REG_RAL0, (uint32_t)card.mac[0] | ((uint32_t)card.mac[1] << 8)
                        | ((uint32_t)card.mac[2] << 16)
                        | ((uint32_t)card.mac[3] << 24));
    reg_write(REG_RAH0, (uint32_t)card.mac[4] | ((uint32_t)card.mac[5] << 8)
                        | RAH_AV);
}

static void pch_start(void)
{
    unsigned i;

    mac_restore();

    for (i = 0; i < 128u; i++) {
        reg_write(REG_MTA + i * 4u, 0);
    }

    /* `e1000e_reset`'s order: "let the f/w know that the h/w is now under
     * the control of the driver", the wake-up control cleared, and then
     * the rest of the bring-up. */
    reg_write(REG_CTRL_EXT, reg_read(REG_CTRL_EXT) | CTRL_EXT_DRV_LOAD);
    reg_write(REG_WUC, 0);

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU);
    rings_start();
}

/*
 * **An I219, brought up as `e1000e` would** (`testing.md` 18.346).
 *
 * First as the firmware left it - the units stopped, the rings this
 * driver's, the bits `e1000e` sets - with no reset at all: a reset is where
 * `e1000e`'s warnings about this chip are, and the firmware's own network
 * boot has already brought it up. A frame to itself says whether that
 * sends. If it does not, the MAC is reset as `e1000e` resets it and the
 * test is made again. Either way the log says what happened and with what
 * the card said, and the driver goes on: a card that will not send is the
 * stack's to live with (18.345), not a reason to stop.
 */
static void pch_bring_up(struct say_line *line)
{
    unsigned long took = 0;

    say_firmware(line);

    card.have_mac = e1000_decode_mac(reg_read(REG_RAL0), reg_read(REG_RAH0),
                                     card.mac);
    card.present = true;

    if (!card.reset_first) {
        say_begin(line);
        say_text(line, "e1000: an I219, taken as Linux's e1000e takes one - "
                       "first as the firmware left it, without a reset");
        say_send(console, line);

        pch_quiet(false);
        pch_start();

        if (self_test(&took)) {
            say_begin(line);
            say_text(line, "e1000: a frame to itself went out in ");
            say_dec(line, took);
            say_text(line, " us - it sends");
            say_send(console, line);
            return;
        }

        say_transmitter(line, "e1000: a frame to itself did not go out in "
                              "50 ms");
    }

    pch_reset(line);
    pch_start();

    if (self_test(&took)) {
        say_begin(line);
        say_text(line, "e1000: after the reset, a frame to itself went out in ");
        say_dec(line, took);
        say_text(line, " us - it sends");
        say_send(console, line);
        return;
    }

    say_transmitter(line, "e1000: after the reset, a frame to itself did not "
                          "go out either");
}

static bool bring_up(struct say_line *line)
{
    struct dev_info dev;
    struct sysinfo info;
    long region, mapped, bus, regs;
    char option[16];
    long n;
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

    card.hz = 62500000u;
    card.tick_hz = 250u;

    if (kosmos_sysinfo(&info) == 0) {
        if (info.counter_hz != 0) {
            card.hz = info.counter_hz;
        }

        if (info.tick_hz != 0) {
            card.tick_hz = info.tick_hz;
        }
    }

    if (kosmos_boot_option("opt/kosmos/e1000fault", option, sizeof(option))
            == 11 && memcmp(option, "transmitter", 11u) == 0) {
        card.tx_off = true;
        say_begin(line);
        say_text(line, "e1000: its transmitter left off, as "
                       "opt/kosmos/e1000fault asks");
        say_send(console, line);
    }

    /*
     * The I219's way for an I219, and - when a test asks - for QEMU's
     * 82574L, so that the way is run in the gate on the one card QEMU has
     * (`opt/kosmos/e1000path`, `pch` or `pch-reset`).
     */
    card.pch = is_pch_spt(card.device);
    n = kosmos_boot_option("opt/kosmos/e1000path", option, sizeof(option));

    if (n == 3 && memcmp(option, "pch", 3u) == 0) {
        card.pch = true;
    } else if (n == 9 && memcmp(option, "pch-reset", 9u) == 0) {
        card.pch = true;
        card.reset_first = true;
    }

    say_pci(line);

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

    if (card.pch) {
        pch_bring_up(line);
    } else {
        /*
         * **Quiet first, then reset.** An interrupt from a card mid-reset is
         * one nobody can answer, and the mask survives the reset on this
         * family only by being set again afterwards - so it is written twice
         * on purpose.
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

        card.have_mac = e1000_decode_mac(reg_read(REG_RAL0),
                                         reg_read(REG_RAH0), card.mac);

        /* The multicast table, every entry, before the receiver is enabled. */
        for (i = 0; i < 128u; i++) {
            reg_write(REG_MTA + i * 4u, 0);
        }

        /* Link up, and let the PHY work out the speed with the far end. */
        reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

        rings_start();
    }

    card.irq = kosmos_irq_claim(dev.intid);
    reg_write(REG_IMS, ICR_RXT0 | ICR_LSC | ICR_RXDMT0);

    card.present = true;
    return true;
}

/*----------------------------------------------------------------- frames */

/*
 * Every confirmation the card has written since the last look: frames it
 * has sent, and descriptors this end may write again.
 */
static void tx_reclaim(void)
{
    RING_BARRIER();
    card.sent += e1000_tx_reclaim(&card.tx,
                                  (const uint8_t *)(card.mem + TX_DESC_AT));
}

/*
 * A frame out: the next transmit descriptor and the doorbell - **and
 * nothing waited for** (`testing.md` 18.345).
 *
 * It waited for the card's write-back, a tick's sleep at a time for up to a
 * thousand of them, on the grounds that a ring nobody waits on wraps onto
 * descriptors the card has not finished with. The M700's I219 never wrote
 * one back (2 October), so every frame held this driver four seconds and
 * more - and the network stack, which asks this driver to send in a call,
 * waited with it, and every program asking the stack anything waited behind
 * that: the Deskbar's first picture 23 seconds late, its menu deaf, and
 * `neofetch` still waiting when the machine was looked at. A driver is a
 * server, and a server does not wait on its hardware while somebody waits
 * on it.
 *
 * The wrap is answered by counting rather than waiting: confirmations are
 * collected as they arrive (`tx_reclaim`), before a descriptor is written
 * and every time this process wakes, and a ring with no room refuses the
 * frame and counts it. A frame the card keeps for a second is said in the
 * log with the registers that describe the transmitter (`tx_watch`).
 */
static bool send_frame(const uint8_t *frame, unsigned length)
{
    unsigned slot;
    uint8_t *desc;

    if (!card.present || length < 14u || length > FRAME_SLOT) {
        return false;
    }

    tx_reclaim();

    if (!e1000_tx_room(&card.tx)) {
        card.refused++;
        return false;
    }

    slot = card.tx.next;
    desc = tx_desc(slot);

    memcpy((uint8_t *)(card.mem + TX_FRAMES_AT + slot * FRAME_SLOT),
           frame, length);

    memset(desc, 0, E1000_DESC_BYTES);
    put64(desc, card.bus + TX_FRAMES_AT + slot * FRAME_SLOT);
    put16(desc + 8, (uint16_t)length);
    desc[11] = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;

    RING_BARRIER();
    card.tx_posted[slot] = kosmos_ticks();
    card.tx.next = (slot + 1u) % RING_SLOTS;
    reg_write(REG_TDT, card.tx.next);

    return true;
}

/*
 * **A card that keeps its frames, said once, with what it says about
 * itself.** The oldest frame not confirmed after a second is a transmitter
 * that is not sending - nothing on a link that is up takes that long - and
 * the registers are what would say why: whether it is enabled (TCTL), how
 * far it has read (TDH against TDT), its queue's control (TXDCTL) and the
 * device's (CTRL, CTRL_EXT, STATUS). Said again when it sends once more, so
 * the log has both ends of the gap, and a few times at most.
 */
static void tx_watch(struct say_line *line)
{
    uint64_t now;

    if (!card.present || card.mem == 0) {
        return;
    }

    tx_reclaim();

    if (e1000_tx_out(&card.tx) == 0) {
        if (card.tx_held) {
            card.tx_held = false;

            if (card.tx_said < 8u) {
                card.tx_said++;
                say_begin(line);
                say_text(line, "e1000: the card is sending again; ");
                say_dec(line, card.refused);
                say_text(line, " frames refused so far for want of room");
                say_send(console, line);
            }
        }

        return;
    }

    now = kosmos_ticks();

    if (card.tx_held || now - card.tx_posted[card.tx.clean] < card.hz) {
        return;
    }

    card.tx_held = true;

    if (card.tx_said >= 8u) {
        return;
    }

    card.tx_said++;
    say_begin(line);
    say_text(line, "e1000: the card has kept a frame a second without sending "
                   "it, ");
    say_dec(line, e1000_tx_out(&card.tx));
    say_text(line, " waiting");
    say_send(console, line);

    /* A line of its own: both would not fit in one (`SAY_LINE_MAX`). */
    say_begin(line);
    say_text(line, "e1000: its transmitter: TCTL ");
    say_hex(line, reg_read(REG_TCTL), 8);
    say_text(line, " TDH ");
    say_dec(line, reg_read(REG_TDH));
    say_text(line, " TDT ");
    say_dec(line, reg_read(REG_TDT));
    say_text(line, " TXDCTL ");
    say_hex(line, reg_read(REG_TXDCTL), 8);
    say_text(line, " CTRL ");
    say_hex(line, reg_read(REG_CTRL), 8);
    say_text(line, " CTRL_EXT ");
    say_hex(line, reg_read(REG_CTRL_EXT), 8);
    say_text(line, " STATUS ");
    say_hex(line, reg_read(REG_STATUS), 8);

    /* And whether it receives: frames written into memory are the card
     * using the bus, which a transmitter that reads nothing does not say. */
    say_text(line, " RDH ");
    say_dec(line, reg_read(REG_RDH));
    say_text(line, " got ");
    say_dec(line, card.received);
    say_send(console, line);
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

        if (card.rx_ext) {
            e1000_decode_rx_ext(desc, &got);
        } else {
            e1000_decode_rx(desc, &got);
        }

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

/*
 * **What arrived in the first five seconds**, said once (`testing.md`
 * 18.349): a card that sends and receives nothing looks, from above, like a
 * network with no router, and only this end can tell the two apart. On any
 * network something arrives in five seconds - an ARP, a router's
 * advertisement, the answer to the DHCP question this machine asked.
 */
static void rx_watch(struct say_line *line)
{
    if (card.rx_said || card.started_at == 0
        || kosmos_ticks() - card.started_at < 5u * card.hz) {
        return;
    }

    card.rx_said = true;
    say_begin(line);
    say_text(line, "e1000: in its first five seconds, ");
    say_dec(line, card.received);
    say_text(line, " frames received and ");
    say_dec(line, card.dropped);
    say_text(line, " dropped, ");
    say_dec(line, card.sent);
    say_text(line, card.rx_ext ? " sent; RDH " : " sent (legacy layout); RDH ");
    say_dec(line, reg_read(REG_RDH));
    say_text(line, " RDT ");
    say_dec(line, reg_read(REG_RDT));
    say_text(line, " RFCTL ");
    say_hex(line, reg_read(REG_RFCTL), 8);
    say_send(console, line);
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
            tx_watch(&line);
            rx_watch(&line);

            if (card.irq >= 0) {
                (void)kosmos_irq_ack(card.irq);
            }
        }

        serve(&line);
    }
}
