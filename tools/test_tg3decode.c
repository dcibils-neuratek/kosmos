/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a Broadcom 57765-class Ethernet controller's registers, descriptors
 * and status block say, on the host - `user/drivers/net/tg3_decode.c`.
 *
 * QEMU has no model of this chip, so this is the one place short of the Mac
 * Pro where any of it is checked. Every expected value below is worked out
 * by hand from Linux's `tg3.h` (the struct layouts and macro values, named
 * beside each check) and from Broadcom's BCM57785 Programmer's Reference
 * Guide, never by calling the encoder to check the decoder.
 *
 * Usage: test_tg3decode
 */

#include <stdio.h>
#include <string.h>

#include "tg3_decode.h"

static int checks, fails;

static void check(int ok, const char *what)
{
    checks++;

    if (!ok) {
        fails++;
        printf("  %s\n", what);
    }
}

static void le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

/* A receive return entry as the chip writes one: `struct tg3_rx_buffer_desc`,
 * eight words - addr_hi, addr_lo, idx_len, type_flags, ip_tcp_csum,
 * err_vlan, reserved, opaque. */
static void ret_desc(uint8_t *d, uint32_t idx_len, uint32_t err_vlan,
                     uint32_t opaque)
{
    memset(d, 0, TG3_RX_DESC_BYTES);
    le32(d + 8, idx_len);
    le32(d + 12, 0x0004u);          /* RXD_FLAG_END */
    le32(d + 20, err_vlan);
    le32(d + 28, opaque);
}

int main(void)
{
    struct tg3_chip chip;
    struct tg3_rev rev;
    struct tg3_rx rx;
    struct tg3_status st;
    struct tg3_link link;
    struct tg3_tx_ring tx;
    uint8_t d[TG3_RX_DESC_BYTES];
    uint8_t sb[TG3_STATUS_BYTES];
    uint8_t mac[6];
    uint32_t hi, lo;

    /*
     * The identifiers: TG3PCI_DEVICE_TIGON3_57762 0x1682 and _57766 0x1686,
     * which the PRG's Table 2 (p. 47) gives too, under vendor 0x14E4.
     */
    check(tg3_decode_id(0x14E4u, 0x1682u, &chip) && chip.number == 57762
          && !chip.fast_only && chip.prodid_at == 0xFCu,
          "14e4:1682 is a BCM57762, gigabit, with its revision at 0xFC "
          "(TG3PCI_GEN15_PRODID_ASICREV)");
    check(tg3_decode_id(0x14E4u, 0x1686u, &chip) && chip.number == 57766,
          "14e4:1686 is a BCM57766");
    check(tg3_decode_id(0x14E4u, 0x16B2u, &chip) && chip.fast_only,
          "14e4:16b2, a BCM57791, is ten and a hundred only "
          "(TG3_DRV_DATA_FLAG_10_100_ONLY)");
    check(!tg3_decode_id(0x8086u, 0x1682u, &chip),
          "an Intel device with a Broadcom number was taken for one");
    check(!tg3_decode_id(0x14E4u, 0x1657u, &chip),
          "a 5719 (0x1657), not of the 57765 class, was taken for one");

    /*
     * The revision. MISC_HOST_CTRL 0xF0000000-ish says "read the product
     * register" (ASIC_REV_USE_PROD_ID_REG 0x0f in the top nibble of the
     * top half); a product register of 0x57766000 is a 57766 A0, of
     * 0x57785100 a 57765-class B0 - not AX, so it takes the hang fix.
     */
    check(tg3_rev_needs_prodid(0xF0000000u),
          "MISC_HOST_CTRL 0xf000xxxx did not ask for the product register");
    check(!tg3_rev_needs_prodid(0x40010000u),
          "a 5750's revision in MISC_HOST_CTRL asked for the product register");

    tg3_decode_rev(0xF0000000u, 0x57766000u, &rev);
    check(rev.asic == 0x57766u && rev.class_57765 && rev.is_57766 && !rev.a0,
          "a product register of 0x57766000 is not a 57766 of the 57765 class");

    tg3_decode_rev(0xF0000000u, 0x57785000u, &rev);
    /* Not AX: tg3_chip_rev of 0x57785000 is 0x577850, and CHIPREV_57765_AX
     * is 0x577650, so Linux's AX test never matches a 57765 (the comment in
     * tg3_decode_rev). */
    check(rev.asic == 0x57785u && rev.class_57765 && !rev.is_57766
          && rev.a0 && !rev.ax,
          "0x57785000 is CHIPREV_ID_57765_A0, and not CHIPREV_57765_AX");

    tg3_decode_rev(0xF0000000u, 0x57785100u, &rev);
    check(rev.class_57765 && !rev.a0 && !rev.ax,
          "0x57785100, a B0, read as A0 or AX");

    tg3_decode_rev(0x57190000u, 0x57785000u, &rev);
    check(!rev.class_57765,
          "the product register was believed when MISC_HOST_CTRL did not "
          "point at it");

    /* The boot code's answer: ~0x4B657654 (KevT) = 0xB49A89AB, which the PRG
     * prints at p. 146 step 15. */
    check(tg3_fw_ready(0xB49A89ABu), "the boot code's ~KevT was not seen");
    check(!tg3_fw_ready(0x4B657654u),
          "KevT itself, the driver's own word, was taken as the answer");

    /*
     * The MAC from the SRAM mailbox: 0x484B in the top half, then the first
     * two octets, then four more in the low word (`tg3_get_device_address`).
     * 00:25:00:fe:12:34, an address in Apple's block, as the boot code puts it.
     */
    check(tg3_decode_mac_mbox(0x484B0025u, 0x00FE1234u, mac)
          && mac[0] == 0x00 && mac[1] == 0x25 && mac[2] == 0x00
          && mac[3] == 0xFE && mac[4] == 0x12 && mac[5] == 0x34,
          "the mailbox's address did not come out in its order");

    memset(mac, 0x5A, sizeof(mac));
    check(!tg3_decode_mac_mbox(0x00000025u, 0x00FE1234u, mac) && mac[0] == 0x5A,
          "a mailbox without the 0x484b signature was taken, and the answer "
          "changed");
    check(!tg3_decode_mac_mbox(0x484B0000u, 0u, mac),
          "a zero address in the mailbox was taken; some old boot code "
          "leaves one, Linux says");
    check(!tg3_decode_mac_regs(0x00000125u, 0x00FE1234u, mac),
          "01:25:... is multicast and was taken for a station's address");
    check(tg3_decode_mac_regs(0x00000025u, 0x00FE1234u, mac) && mac[1] == 0x25
          && mac[5] == 0x34,
          "MAC_ADDR_0_HIGH and _LOW did not give 00:25:00:fe:12:34");

    {
        const uint8_t def[6] = { 0x00, 0x10, 0x18, 0x00, 0x00, 0x00 };
        check(tg3_mac_is_default(def),
              "00:10:18:00:00:00 was not known as the unprogrammed default");
    }

    /* Writing it back, and the backoff seed: 0x00+0x25+0x00+0xFE+0x12+0x34 =
     * 0x169, under 0x3FF (TX_BACKOFF_SEED_MASK). */
    {
        const uint8_t m[6] = { 0x00, 0x25, 0x00, 0xFE, 0x12, 0x34 };
        const uint8_t big[6] = { 0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

        tg3_encode_mac(m, &hi, &lo);
        check(hi == 0x00000025u && lo == 0x00FE1234u,
              "the address did not go into MAC_ADDR_n as "
              "`__tg3_set_one_mac_addr` writes it");
        check(tg3_backoff_seed(m) == 0x169u, "the backoff seed is not 0x169");
        /* 0xFE + 5*0xFF = 0x5F9, masked 0x1F9 */
        check(tg3_backoff_seed(big) == 0x1F9u,
              "the backoff seed was not masked to ten bits");
    }

    /*
     * A send descriptor: `struct tg3_tx_buffer_desc` is addr_hi, addr_lo,
     * len_flags (length << 16 | flags), vlan_tag. A 1514-byte frame at bus
     * address 0x1_2345_6800, the end of its packet (TXD_FLAG_END 0x0004).
     */
    memset(d, 0xEE, sizeof(d));
    check(tg3_put_tx(d, 0x123456800ull, 1514u)
          && rd32(d + 0) == 0x00000001u && rd32(d + 4) == 0x23456800u
          && rd32(d + 8) == ((1514u << 16) | 0x0004u) && rd32(d + 12) == 0,
          "a send descriptor was not addr_hi, addr_lo, length << 16 | END, 0");

    memset(d, 0xEE, sizeof(d));
    check(!tg3_put_tx(d, 0x1000u, 8u) && d[0] == 0xEE,
          "an eight-byte buffer was given (SHORT_DMA_BUG), or the "
          "descriptor written anyway");
    check(!tg3_tx_buffer_ok(0xFFFFFC00ull, 1514u),
          "a buffer crossing the 4 GB line was allowed "
          "(`tg3_4g_overflow_test`)");
    check(tg3_tx_buffer_ok(0xFFFFF000ull, 2048u - 16u),
          "a buffer ending well before the 4 GB line was refused");
    check(!tg3_tx_buffer_ok(0x1000u, 2049u),
          "a buffer over the 57766's 2 KB limit was allowed whole");

    /*
     * A standard receive entry: the buffer, idx_len = the size
     * (RXD_LEN_SHIFT 0), type_flags = RXD_FLAG_END, opaque =
     * RXD_OPAQUE_RING_STD | index. Entry 37, 1536 bytes (TG3_RX_STD_DMA_SZ).
     */
    tg3_put_rx_std(d, 0x80042000ull, 37u, 1536u);
    check(rd32(d + 0) == 0 && rd32(d + 4) == 0x80042000u
          && rd32(d + 8) == 1536u && rd32(d + 12) == 0x0004u
          && rd32(d + 20) == 0 && rd32(d + 28) == (0x00010000u | 37u),
          "a standard receive entry was not laid out as tg3_rx_prodring_alloc "
          "lays it");

    /* A return entry: a 64-byte frame with its FCS is idx_len 68, from
     * standard entry 37; the driver sees 64 (`tg3_rx`: - ETH_FCS_LEN). */
    ret_desc(d, 68u, 0u, 0x00010000u | 37u);
    tg3_decode_rx(d, &rx);
    check(rx.std && rx.index == 37 && rx.length == 64 && !rx.error,
          "a 68-byte return entry from standard entry 37 was not a 64-byte "
          "frame from 37");

    ret_desc(d, 1518u, 0x00010000u, 0x00010000u | 2u);    /* RXD_ERR_BAD_CRC */
    tg3_decode_rx(d, &rx);
    check(rx.error, "a bad CRC was not an error");

    ret_desc(d, 1518u, 0x00100000u, 0x00010000u | 2u);    /* ODD_NIBBLE_MII */
    tg3_decode_rx(d, &rx);
    check(!rx.error,
          "an odd nibble on MII was an error; RXD_ERR_MASK leaves it out");

    ret_desc(d, 1518u, 0x0000ABCDu, 0x00010000u | 2u);    /* a VLAN tag */
    tg3_decode_rx(d, &rx);
    check(!rx.error, "a VLAN tag in err_vlan's low half was read as an error");

    ret_desc(d, 1518u, 0u, 0x00020000u | 2u);             /* jumbo ring */
    tg3_decode_rx(d, &rx);
    check(!rx.std, "an entry from the jumbo ring was taken as standard");

    ret_desc(d, 2u, 0u, 0x00010000u);
    tg3_decode_rx(d, &rx);
    check(rx.error && rx.length == 0,
          "a length shorter than an FCS wrapped instead of being refused");

    /*
     * The status block, `struct tg3_hw_status` on a little-endian host:
     * status at 0, status_tag at 4, rx_consumer in the high half of the word
     * at 8, and idx[0] at 16 - rx_producer (the return ring's) low,
     * tx_consumer high. PRG Table 14 (p. 81) agrees for all of these.
     */
    memset(sb, 0, sizeof(sb));
    le32(sb + 0, 0x00000003u);              /* UPDATED | LINK_CHG */
    le32(sb + 4, 0x0000002Au);
    le32(sb + 8, 0x01230000u);              /* std consumer 0x123 */
    le32(sb + 16, 0x01F40045u);             /* tx consumer 500, return 69 */
    tg3_decode_status(sb, &st);
    check(st.updated && st.link_changed && !st.error && st.tag == 0x2A,
          "the status word and tag did not read as updated, link changed, 42");
    check(st.return_producer == 69 && st.tx_consumer == 500
          && st.std_consumer == 0x123,
          "the indices did not come from the halves tg3.h puts them in");

    le32(sb + 0, 0x00000004u);
    tg3_decode_status(sb, &st);
    check(st.error && !st.updated, "SD_STATUS_ERROR was not read alone");

    check(tg3_int_mbox_rearm(0x2Au) == 0x2A000000u,
          "the interrupt mailbox is not re-armed with the tag in its top byte");

    /* The ring control blocks: a 512-entry send ring is 512 << 16; the
     * standard ring is 512 << 16 | 1536 << 2 on a 57765_PLUS chip. */
    check(tg3_bdinfo_ring(512u) == 0x02000000u,
          "a 512-entry ring's MAXLEN_FLAGS is not 0x02000000");
    check(tg3_bdinfo_std(512u, 1536u) == (0x02000000u | 0x1800u),
          "the standard ring's MAXLEN_FLAGS is not 512 << 16 | 1536 << 2");

    /*
     * The send ring as this end keeps it: 512 entries, at most 63 out.
     */
    tx.slots = 512u;
    tx.limit = 63u;
    tx.next = 0;
    tx.clean = 0;
    check(tg3_tx_room(&tx) && tg3_tx_out(&tx) == 0, "an empty ring had no room");

    tx.next = 63u;
    check(!tg3_tx_room(&tx) && tg3_tx_out(&tx) == 63,
          "63 out of a limit of 63 still had room");

    check(tg3_tx_reclaim(&tx, 40u) == 40 && tx.clean == 40 && tg3_tx_room(&tx),
          "the chip's consumer at 40 did not give back 40");

    check(tg3_tx_reclaim(&tx, 64u) == 0 && tx.clean == 40,
          "a consumer past what was given was believed");

    /* Across the wrap: next at 10 after 510, clean at 500. */
    tx.next = 10u;
    tx.clean = 500u;
    check(tg3_tx_out(&tx) == 22u, "22 out across the wrap did not count as 22");
    check(tg3_tx_reclaim(&tx, 4u) == 16u && tx.clean == 4u,
          "reclaiming across the wrap to 4 did not give 16");

    check(tg3_ring_ready(3u, 510u, 512u) == 5u,
          "five return entries across the wrap did not count as five");
    check(tg3_ring_ready(7u, 7u, 512u) == 0u, "an empty return ring was not empty");

    /*
     * MAC_MI_COM: a read of register 1 on PHY 1 is
     * (1 << 21) | (1 << 16) | MI_COM_CMD_READ | MI_COM_START.
     */
    check(tg3_mi_read_frame(1u, 1u)
          == ((1u << 21) | (1u << 16) | 0x08000000u | 0x20000000u),
          "the MI read frame for PHY 1, register 1 is wrong");
    check(tg3_mi_write_frame(1u, 0u, 0x1200u)
          == ((1u << 21) | 0x1200u | 0x04000000u | 0x20000000u),
          "the MI write frame for BMCR = 0x1200 is wrong");
    check(tg3_mi_busy(0x20000000u) && !tg3_mi_busy(0x0000796Du)
          && tg3_mi_data(0x0000796Du) == 0x796Du,
          "MI_COM's busy bit or its data were misread");

    /*
     * The PHY identifier: Linux packs PHYSID1 << 10, PHYSID2's top six bits
     * << 16, its low ten bits as they are. 0x5c0d8a40 is TG3_PHY_ID_BCM57765,
     * and the registers that pack into it, worked back by hand:
     *   bits 31:26 of 0x5c0d8a40 = 0x17   -> PHYSID2 bits 15:10, 0x5C00
     *   bits 25:10               = 0x0362 -> PHYSID1
     *   bits  9:0                = 0x240  -> PHYSID2 bits 9:0
     * so PHYSID1 0x0362 and PHYSID2 0x5E40.
     */
    check(tg3_phy_id(0x0362u, 0x5E40u) == 0x5C0D8A40u,
          "PHYSID1 0x0362 and PHYSID2 0x5e40 did not pack into 0x5c0d8a40");
    check(tg3_phy_is_57765(0x5C0D8A47u) && !tg3_phy_is_57765(0x5C0D8A20u),
          "the revision nibble was not masked, or a 5719C passed for a 57765");

    /*
     * The link: BMSR bit 2 up, MII_TG3_AUX_STAT bits 10:8 - 0x0700 a
     * gigabit full, 0x0300 a hundred half, 0x0400 the 100BASE-T4 code the
     * table does not name.
     */
    tg3_decode_link(0x796Du, 0x0700u, &link);
    check(link.up && link.full_duplex && link.mbps == 1000,
          "a gigabit full-duplex link did not read as one");
    tg3_decode_link(0x7969u, 0x0700u, &link);
    check(!link.up && link.mbps == 0, "BMSR without LSTATUS read as up");
    tg3_decode_link(0x796Du, 0x0300u, &link);
    check(link.up && !link.full_duplex && link.mbps == 100,
          "a hundred-megabit half-duplex link did not read as one");
    tg3_decode_link(0x796Du, 0x0400u, &link);
    check(link.up && link.mbps == 0,
          "MII_TG3_AUX_STAT_100_4 was given a speed; Linux's table has none");

    /* MAC_MODE for it: port mode bits 3:2 (GMII 0x8, MII 0x4), half duplex
     * bit 1, everything else kept. */
    tg3_decode_link(0x796Du, 0x0700u, &link);
    check(tg3_mac_mode_for(0x00E0C806u, &link) == 0x00E0C808u,
          "a gigabit link did not leave MAC_MODE in GMII, full duplex, with "
          "its other bits kept");
    tg3_decode_link(0x796Du, 0x0300u, &link);
    check(tg3_mac_mode_for(0x00E0C808u, &link) == 0x00E0C806u,
          "a hundred half did not give MII and half duplex");
    tg3_decode_link(0x7969u, 0x0000u, &link);
    check(tg3_mac_mode_for(0u, &link) == 0x00000008u,
          "a link that is down did not leave the port in GMII");

    /*
     * NVRAM_CFG1, as `tg3_get_57780_nvram_info` reads it: the vendor in
     * bits 25:22 and 1:0 (NVRAM_CFG1_5752VENDOR_MASK 0x03c00003), the page
     * size in 30:28 (NVRAM_CFG1_5752PAGE_SIZE_MASK 0x70000000).
     *
     * An AT45DB021D (FLASH_57780VENDOR_ATMEL_AT45DB021D 0x00400002) with
     * 264-byte pages (FLASH_5752PAGE_SIZE_264 0x50000000): Atmel, buffered,
     * flash, and a page of 264 - so addresses are translated, page number
     * << ATMEL_AT45DB0X1B_PAGE_POS (9) plus the offset in the page. Offset
     * 0x7c is in page 0 and stays 0x7c; 512 is page 1, offset 248, so
     * (1 << 9) + 248 = 760 = 0x2f8. Bits outside both masks (0x0c000000)
     * change nothing.
     */
    {
        struct tg3_nvram nv;

        tg3_decode_nvram_cfg1(0x0C000000u | 0x50000000u | 0x00400002u, &nv);
        check(nv.present && nv.flash && nv.translate && nv.page_bytes == 264u,
              "an AT45DB021D with 264-byte pages did not read as translated "
              "Atmel flash");
        check(tg3_nvram_phys_addr(&nv, 0x7Cu) == 0x7Cu
              && tg3_nvram_phys_addr(&nv, 512u) == 0x2F8u,
              "Atmel page addressing did not put 0x7c at 0x7c and 512 at 0x2f8");

        /* The same part with 256-byte pages (code 0): NO_NVRAM_ADDR_TRANS. */
        tg3_decode_nvram_cfg1(0x00400002u, &nv);
        check(nv.present && !nv.translate && nv.page_bytes == 256u
              && tg3_nvram_phys_addr(&nv, 512u) == 512u,
              "an Atmel flash with 256-byte pages was translated");

        /* An ST M45PE20 (0x02400002), 512-byte pages (0x10000000): flash,
         * JEDEC_ST, never translated. */
        tg3_decode_nvram_cfg1(0x10000000u | 0x02400002u, &nv);
        check(nv.present && nv.flash && !nv.translate && nv.page_bytes == 512u,
              "an ST M45PE20 did not read as untranslated flash of 512-byte "
              "pages");

        /* FLASH_5787VENDOR_ATMEL_EEPROM_376KHZ (0x03000002) with
         * COMPAT_BYPASS (0x80000000) set: an EEPROM, not flash, and the
         * bypass bit is to be cleared on the way back. */
        tg3_decode_nvram_cfg1(0x80000000u | 0x03000002u, &nv);
        check(nv.present && !nv.flash && !nv.translate
              && nv.cfg1_write == 0x03000002u,
              "the 376 kHz EEPROM was not an EEPROM, or COMPAT_BYPASS was "
              "not cleared");
        tg3_decode_nvram_cfg1(0x10000000u | 0x02400002u, &nv);
        check(nv.cfg1_write == 0x12400002u,
              "NVRAM_CFG1 was to be rewritten for a part Linux leaves alone");

        /* A vendor field naming nothing: TG3_FLAG_NO_NVRAM. */
        tg3_decode_nvram_cfg1(0u, &nv);
        check(!nv.present, "a vendor field of 0 was taken for an NVRAM part");
        tg3_decode_nvram_cfg1(0x01000001u, &nv);
        check(!nv.present, "vendor 0x01000001, in no table, was taken for one");
    }

    /* NVRAM_CMD_RD 0 | GO 0x10 | FIRST 0x80 | LAST 0x100 | DONE 0x8. */
    check(tg3_nvram_read_cmd() == 0x198u,
          "a one-word NVRAM read is not RD | GO | FIRST | LAST | DONE, 0x198");
    check(tg3_nvram_done(0x00000008u) && !tg3_nvram_done(0x00000190u),
          "NVRAM_CMD_DONE was misread");

    /* The EEE patch: a 57766 with no NVRAM, and nothing else. */
    tg3_decode_rev(0xF0000000u, 0x57766000u, &rev);
    check(tg3_wants_eee_patch(&rev, false) && !tg3_wants_eee_patch(&rev, true),
          "a 57766 wanted its patch with NVRAM, or not without");
    tg3_decode_rev(0xF0000000u, 0x57785100u, &rev);
    check(!tg3_wants_eee_patch(&rev, false),
          "a 57762 (ASIC_REV_57765) was said to want tg357766.bin");

    /*
     * Configuration space, a word at a time: the capability pointer is the
     * low byte at 0x34 (PCI_CAPABILITY_LIST); each capability is its id,
     * then the next pointer. Power management at 0x48, MSI (0x05) at 0x50,
     * PCI Express (0x10, PCI_CAP_ID_EXP) at 0xac, which ends the list.
     */
    {
        uint32_t cfg[64];

        memset(cfg, 0, sizeof(cfg));
        cfg[0x34 / 4] = 0x00000048u;
        cfg[0x48 / 4] = 0xC8035001u;        /* PM, next 0x50 */
        cfg[0x50 / 4] = 0x0080AC05u;        /* MSI, next 0xac */
        cfg[0xAC / 4] = 0x00020010u;        /* PCIe, next 0 */
        check(tg3_pci_cap(cfg, 0x10u) == 0xACu && tg3_pci_cap(cfg, 0x05u) == 0x50u,
              "the capability list did not lead to PCIe at 0xac and MSI at 0x50");
        check(tg3_pci_cap(cfg, 0x11u) == 0,
              "an MSI-X capability that is not there was found");

        cfg[0xAC / 4] = 0x0002AC10u;        /* PCIe pointing at itself */
        check(tg3_pci_cap(cfg, 0x11u) == 0,
              "a capability list that loops did not end");
    }

    /*
     * Device Control after the reset: 0x2810 is a 512-byte read request
     * (bits 14:12 = 2), no-snoop (0x0800) and relaxed ordering (0x0010);
     * both cleared leave 0x2000. Device Status in the top half: CED | NFED
     * | FED | URD = 0xf, written to clear them.
     */
    check(tg3_pcie_devctl_after_reset(0x00092810u) == 0x000F2000u,
          "DEVCTL 0x2810 did not become 0x2000 with DEVSTA's errors cleared");

    /* PCI_COMMAND: memory 0x2, master 0x4; 0x0406 has both and INTx off. */
    check(tg3_pci_command_ok(0x00100406u) && !tg3_pci_command_ok(0x00100404u)
          && !tg3_pci_command_ok(0x00000002u),
          "the command register's memory and bus master bits were misread");

    /* `tg3_init_one`: HOSTCC_MODE_ENABLE and WDMAC_MODE_ENABLE are both
     * 0x2; anything else in those registers is not "running". */
    check(!tg3_chip_busy(0u, 0u) && tg3_chip_busy(0x2u, 0u)
          && tg3_chip_busy(0u, 0x3FEu) && !tg3_chip_busy(0x300u, 0x20000000u),
          "a chip left running, or one left idle, was misjudged");

    /*
     * GRC_LOCAL_CTRL: INT_ON_ATTN 0x8 | AUTO_SEEPROM 0x01000000 |
     * GPIO_UART_SEL 0x10, and the GPIO mask OE0-OE2 | OUTPUT0-OUTPUT2 =
     * 0x0001f800 kept from what was there.
     */
    check(tg3_grc_local_ctrl(0xFFFFFFFFu) == 0x0101F818u
          && tg3_grc_local_ctrl(0u) == 0x01000018u,
          "GRC_LOCAL_CTRL did not keep exactly the six GPIO bits");

    /* MII_ADVERTISE: CSMA 0x1 | 10HALF 0x20 | 10FULL 0x40 | 100HALF 0x80 |
     * 100FULL 0x100 = 0x1e1, no pause; MII_CTRL1000 1000HALF 0x100 |
     * 1000FULL 0x200. */
    check(tg3_mii_advertise() == 0x01E1u,
          "MII_ADVERTISE is not 10 and 100 at both duplexes, 0x1e1");
    check(tg3_mii_ctrl1000(false) == 0x0300u && tg3_mii_ctrl1000(true) == 0,
          "MII_CTRL1000 is not both gigabit duplexes, or not none on a "
          "10/100 chip");

    /* The return ring is 1024 on this class (TG3_RX_RET_MAX_SIZE_5700:
     * JUMBO_CAPABLE and not 5780_CLASS, `tg3_rx_ret_ring_size`). */
    check(tg3_ring_ready(5u, 1020u, 1024u) == 9u,
          "nine return entries across a 1024-entry wrap did not count as nine");

    if (fails == 0) {
        printf("test_tg3decode: %d checks, all passed\n", checks);
        return 0;
    }

    printf("test_tg3decode: %d of %d checks FAILED\n", fails, checks);
    return 1;
}
