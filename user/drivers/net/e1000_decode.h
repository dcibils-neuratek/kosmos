/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_NET_E1000_DECODE_H
#define KOSMOS_DRIVERS_NET_E1000_DECODE_H

/*
 * What an Intel Ethernet controller's registers and descriptors say, for the
 * driver in `e1000.c`.
 *
 * Its own file, with no hardware and no system calls in it, for the reason
 * `usb_decode.c` and `smbios_decode.c` have theirs: a register is a number
 * the card chose and a descriptor is bytes it wrote, and reading either
 * wrongly is a fault that looks like a network problem. The card cannot be
 * asked to produce a malformed one on demand; `tools/test_e1000decode.c`
 * can.
 *
 * **One family, many chips.** The M700's is an I219 and QEMU's are an
 * 82540EM and an 82574L; what they share is this register set and these
 * descriptors, which is the whole reason one driver serves them
 * (`roadmap.md` 5zd-f).
 */

#include <stdbool.h>
#include <stdint.h>

/* A legacy descriptor, both directions, is sixteen bytes. */
#define E1000_DESC_BYTES    16u

/*
 * The link, out of the STATUS register.
 *
 * `mbps` is 0 when the two speed bits name a value this does not know -
 * which on a copper card they cannot, since 11b is reserved - and the link
 * is reported as up or down either way, because those are separate facts and
 * a card at an unknown speed is still a card that is connected.
 */
struct e1000_link {
    bool     up;
    bool     full_duplex;
    unsigned mbps;              /* 10, 100, 1000, or 0 for a reserved value */
};

void e1000_decode_link(uint32_t status, struct e1000_link *out);

/*
 * The MAC address out of the Receive Address registers the firmware leaves
 * programmed - low four bytes in RAL, the last two in RAH's low half.
 *
 * False, with `mac` untouched, when RAH's Address Valid bit is clear: a card
 * whose address nobody set is one this driver must not invent an identity
 * for. Reading the EEPROM instead is a different sequence on every chip in
 * the family, and the registers are what the firmware has already done it
 * into.
 */
bool e1000_decode_mac(uint32_t ral, uint32_t rah, uint8_t mac[6]);

/*
 * A receive descriptor the card has written back.
 *
 * `done` is the Descriptor Done bit, and nothing else in the descriptor
 * means anything until it is set. `end` is End Of Packet: a frame longer
 * than a buffer arrives as several descriptors and only the last carries it,
 * and this driver's buffers hold any frame an Ethernet carries, so a
 * descriptor that is done and not the end is a frame it will not reassemble
 * and says so.
 *
 * `error` is any of the error bits that mean the bytes are not what was sent
 * - a CRC or alignment error, a symbol error, a sequence error. `length` is
 * what the card wrote and is only meaningful when done.
 */
struct e1000_rx {
    bool     done;
    bool     end;
    bool     error;
    unsigned length;
};

void e1000_decode_rx(const uint8_t *desc, struct e1000_rx *out);

/*
 * **The same, written back in the extended layout** (`testing.md` 18.349),
 * which Linux's `e1000e` asks of every chip it drives (RFCTL.EXTEN) and the
 * I219's firmware may leave on: a 32-bit status and error word at byte 8 -
 * Descriptor Done, End Of Packet, and the frame errors in its top byte -
 * and the length at byte 12, where the legacy layout has its status. Read
 * as legacy, an extended descriptor is a status made of the length's low
 * byte. From `e1000e`'s `hw.h` (`union e1000_rx_desc_extended`) and
 * `defines.h` (`E1000_RXDEXT_ERR_FRAME_ERR_MASK`).
 */
void e1000_decode_rx_ext(const uint8_t *desc, struct e1000_rx *out);

/* Whether the card has finished with a transmit descriptor: its Descriptor
 * Done bit, which it writes back only for one that asked (Report Status). */
bool e1000_decode_tx_done(const uint8_t *desc);

/*
 * **The transmit ring, as this end keeps account of it** (`testing.md`
 * 18.345). A frame is written into the descriptor at `next` and the card is
 * told; the card's confirmation - Descriptor Done - is collected afterwards,
 * from `clean`, the oldest it has not confirmed. Nothing waits for it.
 *
 * Equal means nothing is out. One descriptor is always left unwritten, so a
 * full ring and an empty one never look alike - which is also what keeps
 * this end from ever writing a descriptor the card has not finished with.
 */
struct e1000_tx_ring {
    unsigned slots;             /* how many descriptors there are */
    unsigned next;              /* where the next frame goes */
    unsigned clean;             /* the oldest the card has not confirmed */
};

/* Whether a frame may be written now. */
bool e1000_tx_room(const struct e1000_tx_ring *r);

/* How many frames the card has been given and has not confirmed. */
unsigned e1000_tx_out(const struct e1000_tx_ring *r);

/*
 * Every confirmation the card has written, collected in order from `clean`:
 * the count of frames it has sent since the last call. `descs` is the ring's
 * first descriptor, `slots` of them in a row. It stops at the first the card
 * has not finished, because the card finishes them in order.
 */
unsigned e1000_tx_reclaim(struct e1000_tx_ring *r, const uint8_t *descs);

#endif /* KOSMOS_DRIVERS_NET_E1000_DECODE_H */
