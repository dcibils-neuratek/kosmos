/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_NET_TG3_DECODE_H
#define KOSMOS_DRIVERS_NET_TG3_DECODE_H

/*
 * What a Broadcom Tigon3 Ethernet controller of the 57765 class says, and
 * how it is spoken to, for the driver in `tg3.c`.
 *
 * Its own file, with no hardware and no system calls in it, for the reason
 * `e1000_decode.c` has one: a descriptor is bytes the card wrote, a status
 * block is bytes it wrote by DMA when it chose to, and reading either wrongly
 * is a fault that looks like a network problem. QEMU has no model of this
 * chip, so the Mac Pro is the only other place any of this can be checked;
 * `tools/test_tg3decode.c` checks it here first.
 *
 * **Where every number comes from.** Linux's `drivers/net/ethernet/broadcom/
 * tg3.c` and `tg3.h` (master at aacb7797, 14 November 2026), cited by macro
 * name, and Broadcom's *BCM57785 Programmer's Reference Guide*
 * (57785-PG105-R, March 2012), cited as "PRG" with its page - the 57785
 * family's guide is the 57765's: its Table 2 lists the BCM57762 and the
 * BCM57766, and Linux's `ASIC_REV_57765` is 0x57785. Where the two disagree,
 * the comment says so, and what is written follows Linux, which is what has
 * driven these chips for a decade.
 *
 * **Byte order.** Descriptors and the status block are read and written as
 * little-endian 32-bit words in the order `tg3.h` declares their fields.
 * That is true only because the driver sets GRC_MODE's WSWAP_DATA,
 * BSWAP_DATA and WSWAP_NONFRM_DATA and leaves BSWAP_NONFRM_DATA clear, as
 * Linux does on a little-endian host (`tg3_init_one`); both hosts Kosmos
 * runs on are little-endian.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG3_VENDOR_BROADCOM     0x14E4u     /* pci_ids.h PCI_VENDOR_ID_BROADCOM */

/* Descriptor and status block sizes: `struct tg3_tx_buffer_desc` (TXD_SIZE),
 * `struct tg3_rx_buffer_desc`, TG3_HW_STATUS_SIZE. */
#define TG3_TX_DESC_BYTES       16u
#define TG3_RX_DESC_BYTES       32u
#define TG3_STATUS_BYTES        0x50u

/*----------------------------------------------------------------- the chip */

/*
 * Which chip a PCI identifier names, from `tg3_pci_tbl`. Only the 57765
 * class - the ones `tg3_detect_asic_rev` reads their revision from the
 * GEN15 product-identifier register for - is known here, since that is the
 * only class this driver brings up. `fast_only` is TG3_DRV_DATA_FLAG_10_100_
 * ONLY in the table: a chip with no gigabit.
 */
struct tg3_chip {
    unsigned number;            /* 57762, 57766, ... as Broadcom names it */
    bool     fast_only;
    unsigned prodid_at;         /* config offset of its revision register */
};

bool tg3_decode_id(uint16_t vendor, uint16_t device, struct tg3_chip *out);

/*
 * The chip's revision. MISC_HOST_CTRL's top half says 0xF000xxxx on every
 * member of this family (PRG Table 2, p. 47; ASIC_REV_USE_PROD_ID_REG), and
 * then the real revision is the product-identifier register's whole word.
 * `asic` is the revision over twelve bits (`tg3_asic_rev`): 0x57785 for a
 * 57762 or 57765, 0x57766 for a 57766. `class_57765` is TG3_FLAG_57765_CLASS.
 */
struct tg3_rev {
    uint32_t id;                /* pci_chip_rev_id */
    uint32_t asic;              /* id >> 12 */
    bool     class_57765;
    bool     is_57766;          /* the one with the EEE patch firmware */
    bool     a0;                /* CHIPREV_ID_57765_A0, with its extra waits */
    bool     ax;                /* CHIPREV_57765_AX: no transmit-hang fix */
};

/* True when MISC_HOST_CTRL says the product register is to be read. */
bool tg3_rev_needs_prodid(uint32_t misc_host_ctrl);

void tg3_decode_rev(uint32_t misc_host_ctrl, uint32_t prodid,
                    struct tg3_rev *out);

/*
 * Whether the boot code on the chip's own processor has finished after a
 * reset: it answers the driver's KevT (NIC_SRAM_FIRMWARE_MBOX_MAGIC1) in
 * SRAM 0xB50 with its complement (`tg3_poll_fw`; PRG p. 146 step 15).
 */
bool tg3_fw_ready(uint32_t firmware_mbox);

/*------------------------------------------------------------------- the MAC */

/*
 * The address, in the order Linux looks (`tg3_get_device_address`), less
 * NVRAM, which this driver does not read: first the SRAM mailbox the boot
 * code fills - 0x484B ("HK") in the high word's top half says it is there -
 * and failing that the MAC_ADDR_0 registers. Both refuse an address that is
 * not one a station may have: zero, or a multicast one.
 */
bool tg3_decode_mac_mbox(uint32_t hi, uint32_t lo, uint8_t mac[6]);
bool tg3_decode_mac_regs(uint32_t hi, uint32_t lo, uint8_t mac[6]);
bool tg3_mac_valid(const uint8_t mac[6]);

/* 00:10:18:00:00:00 - the address a chip nobody programmed carries, which
 * Linux replaces with the platform's (`tg3_is_default_mac_address`). */
bool tg3_mac_is_default(const uint8_t mac[6]);

/* MAC_ADDR_n_HIGH and _LOW for an address, and the transmit backoff seed
 * Linux and the PRG (p. 147 step 39) both derive from it. */
void tg3_encode_mac(const uint8_t mac[6], uint32_t *hi, uint32_t *lo);
uint32_t tg3_backoff_seed(const uint8_t mac[6]);

/*------------------------------------------------------------ descriptors */

/* A send descriptor, the end of its packet: `tg3_tx_set_bd` with
 * TXD_FLAG_END. False, and nothing written, for a buffer the chip cannot
 * take - see `tg3_tx_buffer_ok`. */
bool tg3_put_tx(uint8_t *desc, uint64_t bus, unsigned length);

/*
 * Whether one buffer may be given to the chip whole: longer than eight bytes
 * (SHORT_DMA_BUG, every 5755-and-later chip), not crossing a 4 GB line
 * (`tg3_4g_overflow_test`), and no longer than 2 KB, which is the 57766's
 * DMA limit (TG3_TX_BD_DMA_MAX_2K) - Linux splits a longer one, and no
 * Ethernet frame this driver sends is.
 */
bool tg3_tx_buffer_ok(uint64_t bus, unsigned length);

/* A standard receive ring entry, as `tg3_rx_prodring_alloc` and
 * `tg3_alloc_rx_data` leave it: the buffer, its size, RXD_FLAG_END, and the
 * opaque cookie naming the standard ring and the entry. */
void tg3_put_rx_std(uint8_t *desc, uint64_t bus, unsigned index,
                    unsigned buffer_bytes);

/*
 * A receive return ring entry the chip wrote. `std` is whether its cookie
 * names the standard ring - anything else is not a buffer this driver gave,
 * and `tg3_rx` skips it too. `length` is the frame without its FCS
 * (`tg3_rx` takes ETH_FCS_LEN off), and `error` is RXD_ERR_MASK, which
 * leaves out RXD_ERR_ODD_NIBBLE_RCVD_MII as Linux does.
 */
struct tg3_rx {
    bool     std;
    bool     error;
    unsigned index;             /* the standard ring entry it came from */
    unsigned length;
};

void tg3_decode_rx(const uint8_t *desc, struct tg3_rx *out);

/*
 * The status block, as `struct tg3_hw_status` lays it out for one vector
 * (PRG Table 14, p. 81). `tag` is the status tag; `return_producer` is where
 * the chip has written receive return entries up to, `tx_consumer` how far
 * it has sent, and `std_consumer` how far it has taken standard buffers.
 *
 * The PRG puts the jumbo ring's consumer at 0x14 and Linux's struct puts it
 * in the low half of 0x08; this driver has no jumbo ring and reads neither.
 */
struct tg3_status {
    bool     updated;           /* SD_STATUS_UPDATED */
    bool     link_changed;      /* SD_STATUS_LINK_CHG */
    bool     error;             /* SD_STATUS_ERROR */
    uint32_t tag;
    unsigned return_producer;
    unsigned tx_consumer;
    unsigned std_consumer;
};

void tg3_decode_status(const uint8_t *block, struct tg3_status *out);

/* What the interrupt mailbox is written to re-arm the line once the work up
 * to `tag` is done: the tag in the top byte (`tg3_int_reenable`; PRG p. 234
 * step 9, "the saved Status Tag to the upper 8 bits of Interrupt Mailbox 0"). */
uint32_t tg3_int_mbox_rearm(uint32_t tag);

/*
 * A ring control block's MAXLEN_FLAGS (TG3_BDINFO_MAXLEN_FLAGS): the number
 * of entries in the top half. The standard receive ring of a 57765_PLUS chip
 * also carries the largest frame it may hold in bits 15:2 (`tg3_reset_hw`,
 * TG3_RX_STD_DMA_SZ << 2; PRG p. 147 step 29).
 */
uint32_t tg3_bdinfo_ring(unsigned entries);
uint32_t tg3_bdinfo_std(unsigned entries, unsigned buffer_bytes);

/*--------------------------------------------------------------- the rings */

/*
 * **The send ring, as this end keeps account of it.** The chip's ring has
 * 512 entries and that is fixed (TG3_TX_RING_SIZE, "hard coded in the NIC
 * firmware"), but this driver has fewer frame buffers than that - so
 * `limit` caps how many may be out at once, and a buffer is the entry's
 * index modulo the buffer count. `next` and `clean` run modulo `slots`; the
 * chip says how far it has gone with the status block's `tx_consumer`.
 */
struct tg3_tx_ring {
    unsigned slots;             /* entries in the ring, a power of two */
    unsigned limit;             /* most out at once, less than slots */
    unsigned next;              /* where the next frame goes */
    unsigned clean;             /* the oldest the chip has not passed */
};

bool tg3_tx_room(const struct tg3_tx_ring *r);
unsigned tg3_tx_out(const struct tg3_tx_ring *r);

/*
 * Everything up to the chip's consumer index, reclaimed: the count. A
 * consumer that claims more than is out is the chip and this end
 * disagreeing, and nothing is reclaimed - `tg3_tx`'s `tx_bug` path, which
 * Linux answers with a reset.
 */
unsigned tg3_tx_reclaim(struct tg3_tx_ring *r, unsigned hw_consumer);

/* How many entries lie between a consumer and a producer on a ring of
 * `slots` (a power of two) - the receive return ring's waiting frames. */
unsigned tg3_ring_ready(unsigned producer, unsigned consumer, unsigned slots);

/*---------------------------------------------------------------- the PHY */

/* MAC_MI_COM frames for a read and a write, and what one says back
 * (`__tg3_readphy`, `__tg3_writephy`). */
uint32_t tg3_mi_read_frame(unsigned phy, unsigned reg);
uint32_t tg3_mi_write_frame(unsigned phy, unsigned reg, uint16_t value);
bool tg3_mi_busy(uint32_t frame);
uint16_t tg3_mi_data(uint32_t frame);

/* The PHY's identifier in Linux's packing of PHYSID1 and PHYSID2
 * (`tg3_phy_probe`), and whether it is the 57765's own (TG3_PHY_ID_BCM57765
 * under TG3_PHY_ID_MASK). */
uint32_t tg3_phy_id(uint16_t id1, uint16_t id2);
bool tg3_phy_is_57765(uint32_t id);

/*
 * The link, from the PHY: BMSR's link bit (latched low, so read twice by the
 * caller, as `tg3_setup_copper_phy` does) and the speed and duplex from the
 * Broadcom auxiliary status register (`tg3_aux_stat_to_speed_duplex`).
 * `mbps` is 0 for a code that table does not name.
 */
struct tg3_link {
    bool     up;
    bool     full_duplex;
    unsigned mbps;
};

void tg3_decode_link(uint16_t bmsr, uint16_t aux_stat, struct tg3_link *out);

/* MAC_MODE with its port mode and duplex made to match the link: MII at 10
 * and 100, GMII at a gigabit and when down, half duplex when the link says
 * so (`tg3_setup_copper_phy`, the end of it). */
uint32_t tg3_mac_mode_for(uint32_t mac_mode, const struct tg3_link *link);

#endif /* KOSMOS_DRIVERS_NET_TG3_DECODE_H */
