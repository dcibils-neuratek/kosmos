/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a Broadcom 57765-class Ethernet controller says about itself, and how
 * it is told things. See the header, which says where every number is from.
 */

#include <string.h>

#include "tg3_decode.h"

/* `user/include/bytes.h`, by its path from here: the host test compiles this
 * file with no include path for it. */
#include "../../include/bytes.h"

/* tg3.h: the PCI identifiers of the 57765 class (TG3PCI_DEVICE_TIGON3_*). */
#define DEV_57761           0x16B0u
#define DEV_57765           0x16B4u
#define DEV_57781           0x16B1u
#define DEV_57785           0x16B5u
#define DEV_57791           0x16B2u
#define DEV_57795           0x16B6u
#define DEV_57762           0x1682u
#define DEV_57766           0x1686u
#define DEV_57782           0x16B7u
#define DEV_57786           0x16B3u

/* tg3.h: where the revision is, and what it says. */
#define TG3PCI_GEN15_PRODID_ASICREV     0xFCu
#define MISC_HOST_CTRL_CHIPREV_SHIFT    16u
#define ASIC_REV_USE_PROD_ID_REG        0x0Fu
#define ASIC_REV_57765                  0x57785u
#define ASIC_REV_57766                  0x57766u
#define CHIPREV_ID_57765_A0             0x57785000u
#define CHIPREV_57765_AX                0x577650u

#define NIC_SRAM_FIRMWARE_MBOX_MAGIC1   0x4B657654u

/* tg3.h, the descriptors. */
#define TXD_FLAG_END        0x0004u
#define TXD_LEN_SHIFT       16u
#define RXD_LEN_MASK        0x0000FFFFu
#define RXD_FLAG_END        0x0004u
#define RXD_FLAGS_SHIFT     0u
#define RXD_LEN_SHIFT       0u
#define RXD_OPAQUE_INDEX_MASK   0x0000FFFFu
#define RXD_OPAQUE_RING_STD     0x00010000u
#define RXD_OPAQUE_RING_MASK    0x00070000u
#define RXD_ERR_MASK        (0x00010000u | 0x00020000u | 0x00040000u \
                             | 0x00080000u | 0x00200000u | 0x00400000u \
                             | 0x00800000u | 0x01000000u)
#define ETH_FCS_LEN         4u

/* Byte offsets of the 32-bit words, in `tg3.h`'s field order. */
#define TXD_ADDR_HI         0u
#define TXD_ADDR_LO         4u
#define TXD_LEN_FLAGS       8u
#define TXD_VLAN_TAG        12u
#define RXD_ADDR_HI         0u
#define RXD_ADDR_LO         4u
#define RXD_IDX_LEN         8u
#define RXD_TYPE_FLAGS      12u
#define RXD_ERR_VLAN        20u
#define RXD_OPAQUE          28u

/* tg3.h, the status block. */
#define SD_STATUS_UPDATED   0x00000001u
#define SD_STATUS_LINK_CHG  0x00000002u
#define SD_STATUS_ERROR     0x00000004u
#define SB_STATUS           0x00u
#define SB_TAG              0x04u
#define SB_RX_CONSUMER      0x08u   /* rx_consumer in the high half */
#define SB_IDX0             0x10u   /* rx_producer low, tx_consumer high */

#define BDINFO_FLAGS_MAXLEN_SHIFT   16u

/* tg3.h, MAC_MI_COM. */
#define MI_COM_CMD_WRITE        0x04000000u
#define MI_COM_CMD_READ         0x08000000u
#define MI_COM_START            0x20000000u
#define MI_COM_BUSY             0x20000000u
#define MI_COM_PHY_ADDR_MASK    0x03E00000u
#define MI_COM_PHY_ADDR_SHIFT   21u
#define MI_COM_REG_ADDR_MASK    0x001F0000u
#define MI_COM_REG_ADDR_SHIFT   16u
#define MI_COM_DATA_MASK        0x0000FFFFu

#define TG3_PHY_ID_MASK         0xFFFFFFF0u
#define TG3_PHY_ID_BCM57765     0x5C0D8A40u

/* mii.h, and tg3.h's auxiliary status codes. */
#define BMSR_LSTATUS                0x0004u
#define MII_TG3_AUX_STAT_SPDMASK    0x0700u
#define MII_TG3_AUX_STAT_10HALF     0x0100u
#define MII_TG3_AUX_STAT_10FULL     0x0200u
#define MII_TG3_AUX_STAT_100HALF    0x0300u
#define MII_TG3_AUX_STAT_100FULL    0x0500u
#define MII_TG3_AUX_STAT_1000HALF   0x0600u
#define MII_TG3_AUX_STAT_1000FULL   0x0700u

#define MAC_MODE_HALF_DUPLEX        0x00000002u
#define MAC_MODE_PORT_MODE_MASK     0x0000000Cu
#define MAC_MODE_PORT_MODE_GMII     0x00000008u
#define MAC_MODE_PORT_MODE_MII      0x00000004u

/* tg3.h, NVRAM_CFG1 and NVRAM_CMD. */
#define NVRAM_CFG1_COMPAT_BYPASS            0x80000000u
#define NVRAM_CFG1_5752VENDOR_MASK          0x03C00003u
#define FLASH_5752VENDOR_ATMEL_FLASH_BUFFERED 0x02000003u
#define FLASH_5752VENDOR_ST_M45PE10         0x02400000u
#define FLASH_5752VENDOR_ST_M45PE20         0x02400002u
#define FLASH_5752VENDOR_ST_M45PE40         0x02400001u
#define FLASH_5787VENDOR_ATMEL_EEPROM_376KHZ 0x03000002u
#define FLASH_5787VENDOR_MICRO_EEPROM_376KHZ 0x02000000u
#define FLASH_57780VENDOR_ATMEL_AT45DB011D  0x00400000u
#define FLASH_57780VENDOR_ATMEL_AT45DB011B  0x03400000u
#define FLASH_57780VENDOR_ATMEL_AT45DB021D  0x00400002u
#define FLASH_57780VENDOR_ATMEL_AT45DB021B  0x03400002u
#define FLASH_57780VENDOR_ATMEL_AT45DB041D  0x00400001u
#define FLASH_57780VENDOR_ATMEL_AT45DB041B  0x03400001u
#define NVRAM_CFG1_5752PAGE_SIZE_MASK       0x70000000u
#define FLASH_5752PAGE_SIZE_256             0x00000000u
#define FLASH_5752PAGE_SIZE_512             0x10000000u
#define FLASH_5752PAGE_SIZE_1K              0x20000000u
#define FLASH_5752PAGE_SIZE_2K              0x30000000u
#define FLASH_5752PAGE_SIZE_4K              0x40000000u
#define FLASH_5752PAGE_SIZE_264             0x50000000u
#define FLASH_5752PAGE_SIZE_528             0x60000000u
#define ATMEL_AT45DB0X1B_PAGE_POS           9u
#define NVRAM_CMD_DONE                      0x00000008u
#define NVRAM_CMD_GO                        0x00000010u
#define NVRAM_CMD_RD                        0x00000000u
#define NVRAM_CMD_FIRST                     0x00000080u
#define NVRAM_CMD_LAST                      0x00000100u

/* pci_regs.h (include/uapi/linux, v6.12). */
#define PCI_CAPABILITY_LIST         0x34u
#define PCI_COMMAND_MEMORY          0x0002u
#define PCI_COMMAND_MASTER          0x0004u
#define PCI_EXP_DEVCTL_RELAX_EN     0x0010u
#define PCI_EXP_DEVCTL_NOSNOOP_EN   0x0800u
#define PCI_EXP_DEVSTA_CED          0x0001u
#define PCI_EXP_DEVSTA_NFED         0x0002u
#define PCI_EXP_DEVSTA_FED          0x0004u
#define PCI_EXP_DEVSTA_URD          0x0008u

/* tg3.h, the enables the busy test reads, and GRC_LOCAL_CTRL. */
#define HOSTCC_MODE_ENABLE          0x00000002u
#define WDMAC_MODE_ENABLE           0x00000002u
#define GRC_LCLCTRL_INT_ON_ATTN     0x00000008u
#define GRC_LCLCTRL_GPIO_UART_SEL   0x00000010u
#define GRC_LCLCTRL_GPIO_OE0        0x00000800u
#define GRC_LCLCTRL_GPIO_OE1        0x00001000u
#define GRC_LCLCTRL_GPIO_OE2        0x00002000u
#define GRC_LCLCTRL_GPIO_OUTPUT0    0x00004000u
#define GRC_LCLCTRL_GPIO_OUTPUT1    0x00008000u
#define GRC_LCLCTRL_GPIO_OUTPUT2    0x00010000u
#define GRC_LCLCTRL_AUTO_SEEPROM    0x01000000u

/* mii.h (include/uapi/linux, v6.12). */
#define ADVERTISE_CSMA              0x0001u
#define ADVERTISE_10HALF            0x0020u
#define ADVERTISE_10FULL            0x0040u
#define ADVERTISE_100HALF           0x0080u
#define ADVERTISE_100FULL           0x0100u
#define ADVERTISE_1000HALF          0x0100u     /* in MII_CTRL1000 */
#define ADVERTISE_1000FULL          0x0200u

/* The 57766's largest single buffer, and the short-DMA bound. */
#define TG3_TX_BD_DMA_MAX_2K        2048u
#define SHORT_DMA_LIMIT             8u

/*-------------------------------------------------------------------- chip */

bool tg3_decode_id(uint16_t vendor, uint16_t device, struct tg3_chip *out)
{
    static const struct { uint16_t device; unsigned number; bool fast; } known[] = {
        { DEV_57761, 57761u, false }, { DEV_57765, 57765u, false },
        { DEV_57781, 57781u, false }, { DEV_57785, 57785u, false },
        { DEV_57791, 57791u, true  }, { DEV_57795, 57795u, true  },
        { DEV_57762, 57762u, false }, { DEV_57766, 57766u, false },
        { DEV_57782, 57782u, false }, { DEV_57786, 57786u, false },
    };
    unsigned i;

    if (out == NULL || vendor != TG3_VENDOR_BROADCOM) {
        return false;
    }

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (known[i].device == device) {
            out->number = known[i].number;
            out->fast_only = known[i].fast;
            out->prodid_at = TG3PCI_GEN15_PRODID_ASICREV;
            return true;
        }
    }

    return false;
}

bool tg3_rev_needs_prodid(uint32_t misc_host_ctrl)
{
    return ((misc_host_ctrl >> MISC_HOST_CTRL_CHIPREV_SHIFT) >> 12)
           == ASIC_REV_USE_PROD_ID_REG;
}

void tg3_decode_rev(uint32_t misc_host_ctrl, uint32_t prodid,
                    struct tg3_rev *out)
{
    uint32_t id = misc_host_ctrl >> MISC_HOST_CTRL_CHIPREV_SHIFT;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if ((id >> 12) == ASIC_REV_USE_PROD_ID_REG) {
        id = prodid;
    }

    out->id = id;
    out->asic = id >> 12;
    out->class_57765 = out->asic == ASIC_REV_57765 || out->asic == ASIC_REV_57766;
    out->is_57766 = out->asic == ASIC_REV_57766;
    out->a0 = id == CHIPREV_ID_57765_A0;
    /*
     * Faithful to Linux, and suspicious: CHIPREV_57765_AX is 0x577650, but a
     * 57765's product register reads 0x57785xxx (ASIC_REV_57765 is 0x57785,
     * CHIPREV_ID_57765_A0 0x57785000), whose tg3_chip_rev is 0x577850. So
     * `tg3_chip_rev(tp) != CHIPREV_57765_AX` in tg3_reset_hw is true for
     * every chip of the class and the transmit-hang fix is applied to all of
     * them, A0 included. The host test holds this, and the driver follows
     * Linux in applying the fix everywhere.
     */
    out->ax = (id >> 8) == CHIPREV_57765_AX;
}

bool tg3_fw_ready(uint32_t firmware_mbox)
{
    return firmware_mbox == (uint32_t)~NIC_SRAM_FIRMWARE_MBOX_MAGIC1;
}

/*--------------------------------------------------------------------- MAC */

bool tg3_mac_valid(const uint8_t mac[6])
{
    /* `is_valid_ether_addr`: not multicast (the first octet's low bit), and
     * not all zeros. */
    return mac != NULL && (mac[0] & 0x01u) == 0
           && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) != 0;
}

bool tg3_mac_is_default(const uint8_t mac[6])
{
    static const uint8_t def[6] = { 0x00, 0x10, 0x18, 0x00, 0x00, 0x00 };

    return mac != NULL && memcmp(mac, def, sizeof(def)) == 0;
}

static bool take_if_valid(const uint8_t got[6], uint8_t mac[6])
{
    if (!tg3_mac_valid(got)) {
        return false;
    }

    memcpy(mac, got, 6);
    return true;
}

bool tg3_decode_mac_mbox(uint32_t hi, uint32_t lo, uint8_t mac[6])
{
    uint8_t got[6];

    if (mac == NULL || (hi >> 16) != 0x484Bu) {
        return false;
    }

    got[0] = (uint8_t)(hi >> 8);
    got[1] = (uint8_t)hi;
    got[2] = (uint8_t)(lo >> 24);
    got[3] = (uint8_t)(lo >> 16);
    got[4] = (uint8_t)(lo >> 8);
    got[5] = (uint8_t)lo;
    return take_if_valid(got, mac);
}

bool tg3_decode_mac_regs(uint32_t hi, uint32_t lo, uint8_t mac[6])
{
    uint8_t got[6];

    if (mac == NULL) {
        return false;
    }

    /* The same packing as the mailbox, less its signature: MAC_ADDR_0_HIGH
     * holds the first two octets in its low half, _LOW the other four. */
    got[0] = (uint8_t)(hi >> 8);
    got[1] = (uint8_t)hi;
    got[2] = (uint8_t)(lo >> 24);
    got[3] = (uint8_t)(lo >> 16);
    got[4] = (uint8_t)(lo >> 8);
    got[5] = (uint8_t)lo;
    return take_if_valid(got, mac);
}

void tg3_encode_mac(const uint8_t mac[6], uint32_t *hi, uint32_t *lo)
{
    if (mac == NULL || hi == NULL || lo == NULL) {
        return;
    }

    *hi = ((uint32_t)mac[0] << 8) | mac[1];
    *lo = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16)
          | ((uint32_t)mac[4] << 8) | mac[5];
}

uint32_t tg3_backoff_seed(const uint8_t mac[6])
{
    if (mac == NULL) {
        return 0;
    }

    /* TX_BACKOFF_SEED_MASK */
    return ((uint32_t)mac[0] + mac[1] + mac[2] + mac[3] + mac[4] + mac[5])
           & 0x3FFu;
}

/*------------------------------------------------------------------- NVRAM */

/* `tg3_nvram_get_pagesize`: a code the table does not name leaves the size
 * as it was, which for a fresh `tp` is 0. */
static unsigned nvram_page(uint32_t cfg1)
{
    switch (cfg1 & NVRAM_CFG1_5752PAGE_SIZE_MASK) {
    case FLASH_5752PAGE_SIZE_256: return 256u;
    case FLASH_5752PAGE_SIZE_512: return 512u;
    case FLASH_5752PAGE_SIZE_1K:  return 1024u;
    case FLASH_5752PAGE_SIZE_2K:  return 2048u;
    case FLASH_5752PAGE_SIZE_4K:  return 4096u;
    case FLASH_5752PAGE_SIZE_264: return 264u;
    case FLASH_5752PAGE_SIZE_528: return 528u;
    default:                      return 0u;
    }
}

void tg3_decode_nvram_cfg1(uint32_t cfg1, struct tg3_nvram *out)
{
    bool atmel = false;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->cfg1_write = cfg1;
    out->part = "nothing (no NVRAM)";

    switch (cfg1 & NVRAM_CFG1_5752VENDOR_MASK) {
    case FLASH_5787VENDOR_ATMEL_EEPROM_376KHZ:
    case FLASH_5787VENDOR_MICRO_EEPROM_376KHZ:
        /* A serial EEPROM: buffered, not flash, so never translated; Linux
         * clears COMPAT_BYPASS and returns before the page size. */
        out->present = true;
        out->part = "a serial EEPROM";
        out->cfg1_write = cfg1 & ~NVRAM_CFG1_COMPAT_BYPASS;
        return;

    case FLASH_5752VENDOR_ATMEL_FLASH_BUFFERED:
    case FLASH_57780VENDOR_ATMEL_AT45DB011D:
    case FLASH_57780VENDOR_ATMEL_AT45DB011B:
    case FLASH_57780VENDOR_ATMEL_AT45DB021D:
    case FLASH_57780VENDOR_ATMEL_AT45DB021B:
    case FLASH_57780VENDOR_ATMEL_AT45DB041D:
    case FLASH_57780VENDOR_ATMEL_AT45DB041B:
        atmel = true;
        out->part = "an Atmel AT45DB flash";
        break;

    case FLASH_5752VENDOR_ST_M45PE10:
    case FLASH_5752VENDOR_ST_M45PE20:
    case FLASH_5752VENDOR_ST_M45PE40:
        out->part = "an ST M45PE flash";
        break;

    default:
        return;                         /* TG3_FLAG_NO_NVRAM */
    }

    out->present = true;
    out->flash = true;
    out->page_bytes = nvram_page(cfg1);
    out->translate = atmel && (out->page_bytes == 264u || out->page_bytes == 528u);
}

uint32_t tg3_nvram_phys_addr(const struct tg3_nvram *nv, uint32_t addr)
{
    if (nv == NULL || !nv->translate || nv->page_bytes == 0) {
        return addr;
    }

    return ((addr / nv->page_bytes) << ATMEL_AT45DB0X1B_PAGE_POS)
           + (addr % nv->page_bytes);
}

uint32_t tg3_nvram_read_cmd(void)
{
    return NVRAM_CMD_RD | NVRAM_CMD_GO | NVRAM_CMD_FIRST | NVRAM_CMD_LAST
           | NVRAM_CMD_DONE;
}

bool tg3_nvram_done(uint32_t nvram_cmd)
{
    return (nvram_cmd & NVRAM_CMD_DONE) != 0;
}

bool tg3_wants_eee_patch(const struct tg3_rev *rev, bool has_nvram)
{
    return rev != NULL && rev->is_57766 && !has_nvram;
}

/*------------------------------------------------------------ PCI and PCIe */

unsigned tg3_pci_cap(const uint32_t cfg[64], unsigned id)
{
    unsigned at, guard;

    if (cfg == NULL) {
        return 0;
    }

    at = (unsigned)(cfg[PCI_CAPABILITY_LIST / 4u] & 0xFCu);

    /* 48 is how many four-byte capabilities fit above 0x40. */
    for (guard = 0; at >= 0x40u && at <= 0xFCu && guard < 48u; guard++) {
        uint32_t word = cfg[at / 4u];

        if ((word & 0xFFu) == id) {
            return at;
        }

        at = (unsigned)((word >> 8) & 0xFCu);
    }

    return 0;
}

uint32_t tg3_pcie_devctl_after_reset(uint32_t word_at_cap8)
{
    uint32_t devctl = word_at_cap8 & 0xFFFFu;

    devctl &= ~(PCI_EXP_DEVCTL_RELAX_EN | PCI_EXP_DEVCTL_NOSNOOP_EN);
    return devctl | ((uint32_t)(PCI_EXP_DEVSTA_CED | PCI_EXP_DEVSTA_NFED
                                | PCI_EXP_DEVSTA_FED | PCI_EXP_DEVSTA_URD) << 16);
}

bool tg3_pci_command_ok(uint32_t command_word)
{
    return (command_word & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
           == (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

/*----------------------------------------------------------- the bring-up */

bool tg3_chip_busy(uint32_t hostcc_mode, uint32_t wdmac_mode)
{
    return (hostcc_mode & HOSTCC_MODE_ENABLE) != 0
           || (wdmac_mode & WDMAC_MODE_ENABLE) != 0;
}

uint32_t tg3_grc_local_ctrl(uint32_t now)
{
    const uint32_t gpio = GRC_LCLCTRL_GPIO_OE0 | GRC_LCLCTRL_GPIO_OE1
                          | GRC_LCLCTRL_GPIO_OE2 | GRC_LCLCTRL_GPIO_OUTPUT0
                          | GRC_LCLCTRL_GPIO_OUTPUT1 | GRC_LCLCTRL_GPIO_OUTPUT2;

    return GRC_LCLCTRL_INT_ON_ATTN | GRC_LCLCTRL_AUTO_SEEPROM
           | GRC_LCLCTRL_GPIO_UART_SEL | (now & gpio);
}

uint16_t tg3_mii_advertise(void)
{
    return (uint16_t)(ADVERTISE_CSMA | ADVERTISE_10HALF | ADVERTISE_10FULL
                      | ADVERTISE_100HALF | ADVERTISE_100FULL);
}

uint16_t tg3_mii_ctrl1000(bool fast_only)
{
    return fast_only ? 0u : (uint16_t)(ADVERTISE_1000HALF | ADVERTISE_1000FULL);
}

/*------------------------------------------------------------- descriptors */

bool tg3_tx_buffer_ok(uint64_t bus, unsigned length)
{
    uint32_t base = (uint32_t)bus;

    if (length <= SHORT_DMA_LIMIT || length > TG3_TX_BD_DMA_MAX_2K) {
        return false;
    }

    /* `tg3_4g_overflow_test`: base + len + 8 wrapping the low word. */
    return (uint32_t)(base + length + 8u) >= base;
}

bool tg3_put_tx(uint8_t *desc, uint64_t bus, unsigned length)
{
    if (desc == NULL || !tg3_tx_buffer_ok(bus, length)) {
        return false;
    }

    put_le32(desc + TXD_ADDR_HI, (uint32_t)(bus >> 32));
    put_le32(desc + TXD_ADDR_LO, (uint32_t)bus);
    put_le32(desc + TXD_LEN_FLAGS, ((uint32_t)length << TXD_LEN_SHIFT)
                                   | TXD_FLAG_END);
    put_le32(desc + TXD_VLAN_TAG, 0);
    return true;
}

void tg3_put_rx_std(uint8_t *desc, uint64_t bus, unsigned index,
                    unsigned buffer_bytes)
{
    if (desc == NULL) {
        return;
    }

    memset(desc, 0, TG3_RX_DESC_BYTES);
    put_le32(desc + RXD_ADDR_HI, (uint32_t)(bus >> 32));
    put_le32(desc + RXD_ADDR_LO, (uint32_t)bus);

    /*
     * `rxd->idx_len = rx_pkt_dma_sz << RXD_LEN_SHIFT`, and RXD_LEN_SHIFT is
     * 0: the buffer's size is the low half, where the chip later writes the
     * received length in the return ring's copy.
     */
    put_le32(desc + RXD_IDX_LEN,
             ((uint32_t)buffer_bytes << RXD_LEN_SHIFT) & RXD_LEN_MASK);
    put_le32(desc + RXD_TYPE_FLAGS, RXD_FLAG_END << RXD_FLAGS_SHIFT);
    put_le32(desc + RXD_OPAQUE, RXD_OPAQUE_RING_STD
                                | (index & RXD_OPAQUE_INDEX_MASK));
}

void tg3_decode_rx(const uint8_t *desc, struct tg3_rx *out)
{
    uint32_t opaque, length;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (desc == NULL) {
        return;
    }

    opaque = get_le32(desc + RXD_OPAQUE);
    out->std = (opaque & RXD_OPAQUE_RING_MASK) == RXD_OPAQUE_RING_STD;
    out->index = opaque & RXD_OPAQUE_INDEX_MASK;
    out->error = (get_le32(desc + RXD_ERR_VLAN) & RXD_ERR_MASK) != 0;

    length = get_le32(desc + RXD_IDX_LEN) & RXD_LEN_MASK;

    /* Shorter than its own FCS is not a frame; call it an error rather than
     * let the subtraction wrap. */
    if (length < ETH_FCS_LEN) {
        out->error = true;
        out->length = 0;
    } else {
        out->length = length - ETH_FCS_LEN;
    }
}

void tg3_decode_status(const uint8_t *block, struct tg3_status *out)
{
    uint32_t status, idx0;

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (block == NULL) {
        return;
    }

    status = get_le32(block + SB_STATUS);
    idx0 = get_le32(block + SB_IDX0);

    out->updated = (status & SD_STATUS_UPDATED) != 0;
    out->link_changed = (status & SD_STATUS_LINK_CHG) != 0;
    out->error = (status & SD_STATUS_ERROR) != 0;
    out->tag = get_le32(block + SB_TAG);
    out->return_producer = idx0 & 0xFFFFu;
    out->tx_consumer = idx0 >> 16;
    out->std_consumer = get_le32(block + SB_RX_CONSUMER) >> 16;
}

uint32_t tg3_int_mbox_rearm(uint32_t tag)
{
    return tag << 24;
}

uint32_t tg3_bdinfo_ring(unsigned entries)
{
    return (uint32_t)entries << BDINFO_FLAGS_MAXLEN_SHIFT;
}

uint32_t tg3_bdinfo_std(unsigned entries, unsigned buffer_bytes)
{
    return ((uint32_t)entries << BDINFO_FLAGS_MAXLEN_SHIFT)
           | (((uint32_t)buffer_bytes << 2) & 0xFFFCu);
}

/*------------------------------------------------------------------- rings */

bool tg3_tx_room(const struct tg3_tx_ring *r)
{
    return r != NULL && r->slots != 0 && tg3_tx_out(r) < r->limit;
}

unsigned tg3_tx_out(const struct tg3_tx_ring *r)
{
    if (r == NULL || r->slots == 0) {
        return 0;
    }

    return (r->next - r->clean) & (r->slots - 1u);
}

unsigned tg3_tx_reclaim(struct tg3_tx_ring *r, unsigned hw_consumer)
{
    unsigned done;

    if (r == NULL || r->slots == 0) {
        return 0;
    }

    done = (hw_consumer - r->clean) & (r->slots - 1u);

    if (done > tg3_tx_out(r)) {
        return 0;
    }

    r->clean = hw_consumer & (r->slots - 1u);
    return done;
}

unsigned tg3_ring_ready(unsigned producer, unsigned consumer, unsigned slots)
{
    if (slots == 0) {
        return 0;
    }

    return (producer - consumer) & (slots - 1u);
}

/*--------------------------------------------------------------------- PHY */

uint32_t tg3_mi_read_frame(unsigned phy, unsigned reg)
{
    return (((uint32_t)phy << MI_COM_PHY_ADDR_SHIFT) & MI_COM_PHY_ADDR_MASK)
           | (((uint32_t)reg << MI_COM_REG_ADDR_SHIFT) & MI_COM_REG_ADDR_MASK)
           | MI_COM_CMD_READ | MI_COM_START;
}

uint32_t tg3_mi_write_frame(unsigned phy, unsigned reg, uint16_t value)
{
    return (((uint32_t)phy << MI_COM_PHY_ADDR_SHIFT) & MI_COM_PHY_ADDR_MASK)
           | (((uint32_t)reg << MI_COM_REG_ADDR_SHIFT) & MI_COM_REG_ADDR_MASK)
           | ((uint32_t)value & MI_COM_DATA_MASK)
           | MI_COM_CMD_WRITE | MI_COM_START;
}

bool tg3_mi_busy(uint32_t frame)
{
    return (frame & MI_COM_BUSY) != 0;
}

uint16_t tg3_mi_data(uint32_t frame)
{
    return (uint16_t)(frame & MI_COM_DATA_MASK);
}

uint32_t tg3_phy_id(uint16_t id1, uint16_t id2)
{
    return ((uint32_t)id1 << 10)
           | (((uint32_t)id2 & 0xFC00u) << 16)
           | ((uint32_t)id2 & 0x03FFu);
}

bool tg3_phy_is_57765(uint32_t id)
{
    return (id & TG3_PHY_ID_MASK) == TG3_PHY_ID_BCM57765;
}

void tg3_decode_link(uint16_t bmsr, uint16_t aux_stat, struct tg3_link *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->up = (bmsr & BMSR_LSTATUS) != 0;

    switch (aux_stat & MII_TG3_AUX_STAT_SPDMASK) {
    case MII_TG3_AUX_STAT_10HALF:   out->mbps = 10;   break;
    case MII_TG3_AUX_STAT_10FULL:   out->mbps = 10;   out->full_duplex = true; break;
    case MII_TG3_AUX_STAT_100HALF:  out->mbps = 100;  break;
    case MII_TG3_AUX_STAT_100FULL:  out->mbps = 100;  out->full_duplex = true; break;
    case MII_TG3_AUX_STAT_1000HALF: out->mbps = 1000; break;
    case MII_TG3_AUX_STAT_1000FULL: out->mbps = 1000; out->full_duplex = true; break;
    default:                        out->mbps = 0;    break;  /* incl. 100_4 */
    }

    if (!out->up) {
        out->mbps = 0;
        out->full_duplex = false;
    }
}

uint32_t tg3_mac_mode_for(uint32_t mac_mode, const struct tg3_link *link)
{
    mac_mode &= ~(MAC_MODE_PORT_MODE_MASK | MAC_MODE_HALF_DUPLEX);

    if (link != NULL && link->up && (link->mbps == 10 || link->mbps == 100)) {
        mac_mode |= MAC_MODE_PORT_MODE_MII;
    } else {
        mac_mode |= MAC_MODE_PORT_MODE_GMII;
    }

    if (link != NULL && link->up && link->mbps != 0 && !link->full_duplex) {
        mac_mode |= MAC_MODE_HALF_DUPLEX;
    }

    return mac_mode;
}
