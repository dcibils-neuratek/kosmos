/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Broadcom 57765-class Ethernet controller, driven from a process.
 *
 *   /Network  <--- ethring.h ---  this  --- registers, SRAM, rings --->  the chip
 *
 * **UNPROVED. A DRAFT THAT HAS NEVER RUN.** QEMU has no model of this chip
 * and no machine has booted this file; it is written so the Mac Pro's first
 * boot has something to try, and every line of it should be read as a
 * hypothesis. What *is* checked is the layer beneath it,
 * `tg3_decode.c`, by `tools/test_tg3decode.c` on the host. Nothing calls
 * `tg3_server` yet, nothing links this file, and there is no device kind for
 * it to find (`DEV_BROADCOM_ETHERNET` below is a placeholder for the number
 * the main session gives it, with the HAL's vendor 0x14E4 match and a
 * configuration read for it).
 *
 * **Why it exists.** Two of Diego's Macs have this family on their board:
 * the Mac Pro 2013 a BCM57762 (14e4:1682) and, believed but not yet seen,
 * the iMac 2019 a BCM57766 (14e4:1686). `docs/drivers.md`, *Broadcom
 * Ethernet*, has the reading this is built from.
 *
 * **Where every number comes from.** Linux's `drivers/net/ethernet/
 * broadcom/tg3.c` and `tg3.h` (torvalds/linux master aacb7797, fetched 14
 * November 2026), and Broadcom's *BCM57785 Programmer's Reference Guide*,
 * 57785-PG105-R (March 2012), whose Table 2 lists the 57762 and the 57766
 * as members of the family it describes. Each register below is named as
 * tg3.h names it, so a grep in either finds the other. Where the two
 * sources disagree, the comment says so and this follows Linux, which has
 * run on these chips; the PRG's page is given.
 *
 * **It answers the same protocol the Intel driver does** (`ethproto.h`),
 * so `net.c` attaches to whichever has a card. The shape is `e1000.c`'s,
 * and what of that shape did not fit this chip is said where it bends.
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
#include "tg3_decode.h"
#include "bytes.h"

/*
 * The device kind. **A placeholder**: the kernel has DEV_INTEL_ETHERNET 4
 * and nothing for Broadcom; the main session adds HAL_DEV_ and DEV_
 * BROADCOM_ETHERNET with the next free number (the `_Static_assert` in
 * `syscall.c` holds the two in step), a vendor 0x14E4 class 02/00 match in
 * `hal/pc/devices.c`, and `hal_device_config` for it.
 */
#ifndef DEV_BROADCOM_ETHERNET
#define DEV_BROADCOM_ETHERNET   7u
#endif

/*------------------------------------------------------------- PCI config
 *
 * Read through SYS_DEV_CONFIG. The registers below 0x100 of the first BAR
 * mirror the configuration space on this family - Linux writes
 * TG3PCI_MISC_HOST_CTRL with `tw32` in `tg3_disable_ints` - which is how this
 * writes the two that `tg3_restore_pci_state` writes through configuration
 * space, since Kosmos has no configuration write (see `chip_reset`).
 */
#define PCI_COMMAND_WORD                0x04u   /* PCI_COMMAND, low half */
#define PCI_CAPABILITY_LIST             0x34u
#define PCI_CAP_ID_MSI                  0x05u
#define PCI_MSI_FLAGS_ENABLE            0x0001u /* in the word at cap + 2 */

/*------------------------------------------------------------- registers
 *
 * Offsets into the first BAR, as tg3.h names them.
 */
#define TG3PCI_MISC_HOST_CTRL           0x0068u
#define  MISC_HOST_CTRL_MASK_PCI_INT    0x00000002u
#define  MISC_HOST_CTRL_WORD_SWAP       0x00000008u
#define  MISC_HOST_CTRL_PCISTATE_RW     0x00000010u
#define  MISC_HOST_CTRL_INDIR_ACCESS    0x00000080u
#define  MISC_HOST_CTRL_TAGGED_STATUS   0x00000200u
#define TG3PCI_DMA_RW_CTRL              0x006Cu
#define  DMA_RWCTRL_DIS_CACHE_ALIGNMENT 0x00000001u
#define  DMA_RWCTRL_CRDRDR_RDMA_MRRS_MSK 0x00000380u
#define TG3PCI_PCISTATE                 0x0070u
#define  PCISTATE_ROM_ENABLE            0x00000020u
#define  PCISTATE_ROM_RETRY_ENABLE      0x00000040u
#define TG3PCI_MEM_WIN_BASE_ADDR        0x007Cu
#define TG3PCI_MEM_WIN_DATA             0x0084u

/* Mailboxes: each is a 64-bit slot and the 32-bit write goes to its low
 * word, TG3_64BIT_REG_LOW 0x4. */
#define TG3_64BIT_REG_LOW               0x4u
#define MAILBOX_INTERRUPT_0             0x0200u
#define MAILBOX_RCV_STD_PROD_IDX        0x0268u
#define MAILBOX_RCV_JUMBO_PROD_IDX      0x0270u
#define MAILBOX_RCVRET_CON_IDX_0        0x0280u
#define MAILBOX_SNDHOST_PROD_IDX_0      0x0300u

#define MAC_MODE                        0x0400u
#define  MAC_MODE_HALF_DUPLEX           0x00000002u
#define  MAC_MODE_PORT_MODE_MASK        0x0000000Cu
#define  MAC_MODE_PORT_MODE_GMII        0x00000008u
#define  MAC_MODE_RXSTAT_ENABLE         0x00000800u
#define  MAC_MODE_RXSTAT_CLEAR          0x00001000u
#define  MAC_MODE_TXSTAT_ENABLE         0x00004000u
#define  MAC_MODE_TXSTAT_CLEAR          0x00008000u
#define  MAC_MODE_TDE_ENABLE            0x00200000u
#define  MAC_MODE_RDE_ENABLE            0x00400000u
#define  MAC_MODE_FHDE_ENABLE           0x00800000u
#define MAC_STATUS                      0x0404u
#define  MAC_STATUS_SYNC_CHANGED        0x00000010u
#define  MAC_STATUS_CFG_CHANGED         0x00000008u
#define  MAC_STATUS_LNKSTATE_CHANGED    0x00001000u
#define  MAC_STATUS_MI_COMPLETION       0x00400000u
#define MAC_EVENT                       0x0408u
#define  MAC_EVENT_LNKSTATE_CHANGED     0x00001000u
#define MAC_LED_CTRL                    0x040Cu
#define MAC_ADDR_0_HIGH                 0x0410u
#define MAC_ADDR_0_LOW                  0x0414u
#define MAC_TX_BACKOFF_SEED             0x0438u
#define MAC_RX_MTU_SIZE                 0x043Cu
#define MAC_MI_COM                      0x044Cu
#define MAC_MI_STAT                     0x0450u
#define  MAC_MI_STAT_LNKSTAT_ATTN_ENAB  0x00000001u
#define MAC_MI_MODE                     0x0454u
#define  MAC_MI_MODE_500KHZ_CONST       0x00008000u
#define MAC_TX_MODE                     0x045Cu
#define  TX_MODE_ENABLE                 0x00000002u
#define  TX_MODE_MBUF_LOCKUP_FIX        0x00000100u
#define MAC_TX_LENGTHS                  0x0464u
#define MAC_RX_MODE                     0x0468u
#define  RX_MODE_ENABLE                 0x00000002u
#define  RX_MODE_IPV6_CSUM_ENABLE       0x01000000u
#define MAC_HASH_REG_0                  0x0470u
#define MAC_RCV_RULE_0                  0x0480u
#define MAC_RCV_VALUE_0                 0x0484u
#define MAC_RCV_RULE_CFG                0x0500u
#define  RCV_RULE_CFG_DEFAULT_CLASS     0x00000008u
#define MAC_LOW_WMARK_MAX_RX_FRAME      0x0504u

#define SNDDATAI_MODE                   0x0C00u
#define SNDDATAI_STATSCTRL              0x0C08u
#define SNDDATAI_STATSENAB              0x0C0Cu
#define SNDDATAC_MODE                   0x1000u
#define SNDBDS_MODE                     0x1400u
#define SNDBDI_MODE                     0x1800u
#define SNDBDC_MODE                     0x1C00u
#define RCVLPC_MODE                     0x2000u
#define RCVLPC_CONFIG                   0x2010u
#define RCVLPC_STATSCTRL                0x2014u
#define RCVLPC_STATS_ENABLE             0x2018u
#define  RCVLPC_STATSENAB_DACK_FIX      0x00040000u
#define RCVDBDI_MODE                    0x2400u
#define  RCVDBDI_MODE_ENABLE            0x00000002u
#define  RCVDBDI_MODE_INV_RING_SZ       0x00000010u
#define RCVDBDI_JUMBO_BD                0x2440u
#define RCVDBDI_STD_BD                  0x2450u
#define RCVDCC_MODE                     0x2800u
#define RCVBDI_MODE                     0x2C00u
#define RCVBDI_STD_THRESH               0x2C18u
#define RCVCC_MODE                      0x3000u
#define STD_REPLENISH_LWM               0x2D00u

#define TG3_CPMU_LSPD_10MB_CLK          0x3604u
#define  CPMU_LSPD_10MB_MACCLK_MASK     0x001F0000u
#define  CPMU_LSPD_10MB_MACCLK_6_25     0x00130000u
#define TG3_CPMU_PADRNG_CTL             0x3668u
#define  TG3_CPMU_PADRNG_CTL_RDIV2      0x00040000u

#define HOSTCC_MODE                     0x3C00u
#define  HOSTCC_MODE_ENABLE             0x00000002u
#define  HOSTCC_MODE_NOW                0x00000008u
#define  HOSTCC_MODE_32BYTE             0x00000100u
#define  HOSTCC_MODE_CLRTICK_RXBD       0x00000200u
#define  HOSTCC_MODE_CLRTICK_TXBD       0x00000400u
#define HOSTCC_RXCOL_TICKS              0x3C08u
#define HOSTCC_TXCOL_TICKS              0x3C0Cu
#define HOSTCC_RXMAX_FRAMES             0x3C10u
#define HOSTCC_TXMAX_FRAMES             0x3C14u
#define HOSTCC_RXCOAL_MAXF_INT          0x3C20u
#define HOSTCC_TXCOAL_MAXF_INT          0x3C24u
#define HOSTCC_STATUS_BLK_HOST_ADDR     0x3C38u
#define HOSTCC_FLOW_ATTN                0x3C48u

#define MEMARB_MODE                     0x4000u
#define  MEMARB_MODE_ENABLE             0x00000002u
#define BUFMGR_MODE                     0x4400u
#define  BUFMGR_MODE_ENABLE             0x00000002u
#define  BUFMGR_MODE_ATTN_ENABLE        0x00000004u
#define BUFMGR_MB_RDMA_LOW_WATER        0x4410u
#define BUFMGR_MB_MACRX_LOW_WATER       0x4414u
#define BUFMGR_MB_HIGH_WATER            0x4418u
#define BUFMGR_DMA_LOW_WATER            0x4434u
#define BUFMGR_DMA_HIGH_WATER           0x4438u
#define RDMAC_MODE                      0x4800u
#define TG3_RDMA_RSRVCTRL_REG           0x4900u
#define  TG3_RDMA_RSRVCTRL_FIFO_OFLW_FIX 0x00000004u
#define WDMAC_MODE                      0x4C00u
#define  WDMAC_MODE_STATUS_TAG_FIX      0x20000000u

/* RDMAC_MODE's and WDMAC_MODE's bits 1-9 are the same nine enables in both
 * (tg3.h: RDMAC_MODE_ENABLE .. RDMAC_MODE_LNGREAD_ENAB, WDMAC_MODE_ENABLE ..
 * WDMAC_MODE_LNGREAD_ENAB). */
#define DMAC_MODE_ENABLES               0x000003FEu
#define  RDMAC_MODE_FIFO_LONG_BURST     0x00030000u
#define  RDMAC_MODE_JMB_2K_MMRR         0x00800000u
#define  RDMAC_MODE_IPV4_LSO_EN         0x08000000u
#define  RDMAC_MODE_IPV6_LSO_EN         0x10000000u

#define GRC_MODE                        0x6800u
#define  GRC_MODE_WSWAP_NONFRM_DATA     0x00000004u
#define  GRC_MODE_BSWAP_DATA            0x00000010u
#define  GRC_MODE_WSWAP_DATA            0x00000020u
#define  GRC_MODE_HOST_STACKUP          0x00010000u
#define  GRC_MODE_HOST_SENDBDS          0x00020000u
#define  GRC_MODE_NO_TX_PHDR_CSUM       0x00100000u
#define  GRC_MODE_PCIE_PORT_MASK        0xA0400000u  /* TL 0 | PL | DL | HI_1K_EN */
#define  GRC_MODE_PCIE_DL_SEL           0x20000000u
#define  GRC_MODE_IRQ_ON_MAC_ATTN       0x04000000u
#define GRC_MISC_CFG                    0x6804u
#define  GRC_MISC_CFG_CORECLK_RESET     0x00000001u
#define  GRC_MISC_CFG_PRESCALAR_MASK    0x000000FEu
#define  GRC_MISC_CFG_PRESCALAR_SHIFT   1u
#define GRC_LOCAL_CTRL                  0x6808u
#define  GRC_LCLCTRL_INT_ON_ATTN        0x00000008u
#define  GRC_LCLCTRL_GPIO_UART_SEL      0x00000010u
#define  GRC_LCLCTRL_AUTO_SEEPROM       0x01000000u
#define GRC_FASTBOOT_PC                 0x6894u
#define MSGINT_MODE                     0x6000u
#define  MSGINT_MODE_ENABLE             0x00000002u

#define TG3_PCIE_TLDLPL_PORT            0x7C00u
#define TG3_PCIE_DL_LO_FTSMAX           0x000Cu
#define  TG3_PCIE_DL_LO_FTSMAX_MSK      0x000000FFu
#define  TG3_PCIE_DL_LO_FTSMAX_VAL      0x0000002Cu

#define NVRAM_SWARB                     0x7020u
#define  SWARB_REQ_SET1                 0x00000002u
#define  SWARB_REQ_CLR1                 0x00000020u
#define  SWARB_GNT1                     0x00000200u

/*-------------------------------------------------------------- NIC SRAM
 *
 * Reached through the memory window, never mapped.
 */
#define NIC_SRAM_SEND_RCB               0x0100u
#define NIC_SRAM_RCV_RET_RCB            0x0200u
#define NIC_SRAM_FIRMWARE_MBOX          0x0B50u
#define  NIC_SRAM_FIRMWARE_MBOX_MAGIC1  0x4B657654u
#define NIC_SRAM_DATA_SIG               0x0B54u
#define  NIC_SRAM_DATA_SIG_MAGIC        0x4B657654u
#define NIC_SRAM_DATA_CFG               0x0B58u
#define  NIC_SRAM_DATA_CFG_ASF_ENABLE   0x00000080u
#define NIC_SRAM_MAC_ADDR_HIGH_MBOX     0x0C14u
#define NIC_SRAM_MAC_ADDR_LOW_MBOX      0x0C18u
#define NIC_SRAM_RX_BUFFER_DESC         0x6000u

/* A ring control block, `TG3_BDINFO_*`: the host address in two words, then
 * MAXLEN_FLAGS, then NIC_ADDR - which `tg3_set_bdinfo` writes only on chips
 * before 5705_PLUS for the send and return rings. */
#define TG3_BDINFO_HOST_ADDR            0x0u
#define TG3_BDINFO_MAXLEN_FLAGS         0x8u
#define TG3_BDINFO_NIC_ADDR             0xCu
#define TG3_BDINFO_SIZE                 0x10u
#define BDINFO_FLAGS_DISABLED           0x00000002u

/* The PHY, through MAC_MI_COM (mii.h names). */
#define TG3_PHY_MII_ADDR                1u
#define MII_BMCR                        0x00u
#define  BMCR_ANRESTART                 0x0200u
#define  BMCR_ANENABLE                  0x1000u
#define MII_BMSR                        0x01u
#define MII_PHYSID1                     0x02u
#define MII_PHYSID2                     0x03u
#define MII_TG3_AUX_STAT                0x19u
#define PHY_BUSY_LOOPS                  5000u

/*------------------------------------------------------------ the region
 *
 * **The chip's rings are its own size, not ours.** e1000 chose 32
 * descriptors; here the send ring is TG3_TX_RING_SIZE 512, the standard
 * ring's size is written as 512 (TG3_RX_STD_MAX_SIZE_5717 is for 5717_PLUS;
 * the 57765 is 512, `tg3_rx_prodring_init`), and the return ring this sizes
 * at 512 - **Linux 1024** (TG3_RX_RET_MAX_SIZE_5700, chosen because the
 * chips are JUMBO_CAPABLE), **the PRG 32 to 512** (p. 147 step 36). It
 * follows the PRG, since nothing here posts jumbo frames to fill a larger
 * one, and this is a thing to look at first if receive misbehaves.
 *
 * Only 64 frames each way have buffers: a descriptor's buffer is its index
 * modulo 64, which is safe because both rings are consumed in order and at
 * most 63 are ever outstanding.
 */
#define TX_RING                 512u
#define STD_RING                512u
#define RET_RING                512u
#define BUFFERS                 64u
#define FRAME_SLOT              2048u       /* the 57766's TX DMA limit, too */
#define RX_BUFFER_BYTES         1536u       /* TG3_RX_STD_DMA_SZ */

#define STATUS_AT               0u
#define TX_DESC_AT              4096u
#define STD_DESC_AT             (TX_DESC_AT + TX_RING * TG3_TX_DESC_BYTES)
#define RET_DESC_AT             (STD_DESC_AT + STD_RING * TG3_RX_DESC_BYTES)
#define RX_FRAMES_AT            (RET_DESC_AT + RET_RING * TG3_RX_DESC_BYTES)
#define TX_FRAMES_AT            (RX_FRAMES_AT + BUFFERS * FRAME_SLOT)
#define REGION_BYTES            (TX_FRAMES_AT + BUFFERS * FRAME_SLOT)
#define REGION_PAGES            ((REGION_BYTES + 4095u) / 4096u)

#if defined(__x86_64__)
#define RING_BARRIER()  __asm__ volatile("" ::: "memory")
#else
#define RING_BARRIER()  __asm__ volatile("dmb ish" ::: "memory")
#endif

static long console = -1;
static long frames_endpoint = -1;

static struct {
    bool          present;
    uintptr_t     regs;
    unsigned      where;
    unsigned      device;
    unsigned      intid;
    long          irq;
    bool          msi;
    struct tg3_chip chip;
    struct tg3_rev rev;

    uintptr_t     mem;
    uint64_t      bus;

    uint8_t       mac[6];
    bool          have_mac;
    const char   *mac_from;
    bool          asf;
    uint32_t      phy_id;
    struct tg3_link link;
    bool          link_said;
    uint32_t      mac_mode;
    uint32_t      misc_host_ctrl;
    uint32_t      grc_mode;

    uint32_t      last_tag;
    unsigned      ret_next;         /* the return entry looked at next */
    unsigned      std_prod;         /* the standard ring's producer */
    struct tg3_tx_ring tx;
    unsigned long sent, received, dropped, refused;

    uint64_t      hz;
    unsigned long tick_hz;

    struct eth_ring *ring;
    long          ring_cap;
} card;

/*-------------------------------------------------------------- access */

static uint32_t reg_read(unsigned at)
{
    return mmio_read32(card.regs + at);
}

static void reg_write(unsigned at, uint32_t value)
{
    mmio_write32(card.regs + at, value);
}

/* `tw32_f`: a write, then a read of the same register, so that it has
 * reached the chip before whatever follows. */
static void reg_write_f(unsigned at, uint32_t value)
{
    mmio_write32(card.regs + at, value);
    (void)mmio_read32(card.regs + at);
}

static void mbox_write(unsigned at, uint32_t value)
{
    mmio_write32(card.regs + at + TG3_64BIT_REG_LOW, value);
}

/* `tg3_read_mem` and `tg3_write_mem`, the MMIO way: the window's base, the
 * word, and the base put back to zero, as Linux leaves it. */
static uint32_t sram_read(unsigned at)
{
    uint32_t v;

    reg_write_f(TG3PCI_MEM_WIN_BASE_ADDR, at);
    v = reg_read(TG3PCI_MEM_WIN_DATA);
    reg_write_f(TG3PCI_MEM_WIN_BASE_ADDR, 0);
    return v;
}

static void sram_write(unsigned at, uint32_t value)
{
    reg_write_f(TG3PCI_MEM_WIN_BASE_ADDR, at);
    reg_write_f(TG3PCI_MEM_WIN_DATA, value);
    reg_write_f(TG3PCI_MEM_WIN_BASE_ADDR, 0);
}

/* At least `us` microseconds, on the counter. Linux's udelay; the waits in
 * this chip's bring-up are tens of microseconds, below a scheduler tick. */
static void delay_us(unsigned us)
{
    uint64_t start = kosmos_ticks();
    uint64_t span = (uint64_t)us * card.hz / 1000000u + 1u;

    while (kosmos_ticks() - start < span) {
    }
}

static void sleep_ms(unsigned ms)
{
    unsigned long hz = card.tick_hz != 0 ? card.tick_hz : 250ul;

    kosmos_sleep((ms * hz + 999ul) / 1000ul + 1ul);
}

static uint8_t *tx_desc(unsigned i)
{
    return (uint8_t *)(card.mem + TX_DESC_AT + i * TG3_TX_DESC_BYTES);
}

static uint8_t *std_desc(unsigned i)
{
    return (uint8_t *)(card.mem + STD_DESC_AT + i * TG3_RX_DESC_BYTES);
}

static uint8_t *ret_desc(unsigned i)
{
    return (uint8_t *)(card.mem + RET_DESC_AT + i * TG3_RX_DESC_BYTES);
}

static void say_word(struct say_line *line, const char *what, uint32_t v)
{
    say_text(line, what);
    say_hex(line, v, 8);
}

/*-------------------------------------------------------------- the PHY */

/* `__tg3_readphy`, with MAC_MI_MODE's auto-polling never turned on here, so
 * Linux's dance around it is not needed. */
static bool phy_read(unsigned reg, uint16_t *out)
{
    unsigned i;
    uint32_t v = 0;

    reg_write_f(MAC_MI_COM, tg3_mi_read_frame(TG3_PHY_MII_ADDR, reg));

    for (i = 0; i < PHY_BUSY_LOOPS; i++) {
        delay_us(10);
        v = reg_read(MAC_MI_COM);

        if (!tg3_mi_busy(v)) {
            delay_us(5);
            v = reg_read(MAC_MI_COM);
            *out = tg3_mi_data(v);
            return true;
        }
    }

    return false;
}

static bool phy_write(unsigned reg, uint16_t value)
{
    unsigned i;

    reg_write_f(MAC_MI_COM, tg3_mi_write_frame(TG3_PHY_MII_ADDR, reg, value));

    for (i = 0; i < PHY_BUSY_LOOPS; i++) {
        delay_us(10);

        if (!tg3_mi_busy(reg_read(MAC_MI_COM))) {
            delay_us(5);
            return true;
        }
    }

    return false;
}

/* The link as the PHY has it: BMSR twice (its link bit latches low), then
 * the auxiliary status for speed and duplex (`tg3_setup_copper_phy`,
 * `tg3_aux_stat_to_speed_duplex`). */
static void link_read(struct tg3_link *out)
{
    uint16_t bmsr = 0, aux = 0;

    (void)phy_read(MII_BMSR, &bmsr);

    if (!phy_read(MII_BMSR, &bmsr)) {
        bmsr = 0;
    }

    if (!phy_read(MII_TG3_AUX_STAT, &aux)) {
        aux = 0;
    }

    tg3_decode_link(bmsr, aux, out);
}

/* The MAC set to what the link is, and the change acknowledged
 * (`tg3_clear_mac_status`, then MAC_MODE as `tg3_setup_copper_phy` ends). */
static void link_apply(void)
{
    link_read(&card.link);
    card.mac_mode = tg3_mac_mode_for(card.mac_mode, &card.link);
    reg_write_f(MAC_MODE, card.mac_mode);
    delay_us(40);
    reg_write_f(MAC_STATUS, MAC_STATUS_SYNC_CHANGED | MAC_STATUS_CFG_CHANGED
                            | MAC_STATUS_MI_COMPLETION
                            | MAC_STATUS_LNKSTATE_CHANGED);
    delay_us(40);
    reg_write_f(MAC_EVENT, MAC_EVENT_LNKSTATE_CHANGED);
    delay_us(40);
}

/*------------------------------------------------------------- the reset */

/* `tg3_nvram_lock`: the arbitration the boot code also takes. */
static bool nvram_lock(void)
{
    unsigned i;

    reg_write(NVRAM_SWARB, SWARB_REQ_SET1);

    for (i = 0; i < 8000u; i++) {
        if (reg_read(NVRAM_SWARB) & SWARB_GNT1) {
            return true;
        }

        delay_us(20);
    }

    reg_write(NVRAM_SWARB, SWARB_REQ_CLR1);
    return false;
}

static void nvram_unlock(void)
{
    reg_write_f(NVRAM_SWARB, SWARB_REQ_CLR1);
}

/* `tg3_poll_fw`: the boot code answers the driver's MAGIC1 with its
 * complement when it has finished. Up to a second (100000 times 10 us). */
static bool poll_fw(void)
{
    unsigned i;

    for (i = 0; i < 100000u; i++) {
        if (tg3_fw_ready(sram_read(NIC_SRAM_FIRMWARE_MBOX))) {
            if (card.rev.a0) {
                sleep_ms(10);           /* "Chip id 57765 A0 needs" 10 ms */
            }

            return true;
        }

        delay_us(10);
    }

    return false;
}

/*
 * `tg3_chip_reset` for the 57765 class, in its order.
 *
 * **The reset is a core-clock reset, and what it clears is the open
 * question.** Linux follows it with `tg3_restore_pci_state`, all through
 * configuration space: PCI_COMMAND (memory and bus master back), MISC_HOST_
 * CTRL, PCISTATE; and on PCIe it clears DEVCTL's relaxed-ordering and
 * no-snoop bits and DEVSTA. The PRG (p. 145, steps 1, 9 and 11) says the
 * same of the command register. **Kosmos has no configuration write.**
 * This draft writes MISC_HOST_CTRL and PCISTATE through their MMIO mirror -
 * which needs memory decode to have survived - and *reads* PCI_COMMAND
 * afterwards to say whether it did. If it did not, nothing after the reset
 * reaches the chip, and the main session needs SYS_DEV_CONFIG to write.
 *
 * Two disagreements, followed as Linux has them: Linux sets
 * GRC_MISC_CFG_KEEP_GPHY_POWER (bit 26) only when there is no CPMU, so not
 * here, where the PRG's step 6 sets bits 26 and 29; and Linux waits 120 us
 * twice where the PRG asks for at least a millisecond - this waits the
 * millisecond, which costs nothing.
 */
static bool chip_reset(struct say_line *line)
{
    long command;
    bool locked = nvram_lock();

    /* `tg3_write_sig_pre_reset`: the word the boot code will complement. */
    sram_write(NIC_SRAM_FIRMWARE_MBOX, NIC_SRAM_FIRMWARE_MBOX_MAGIC1);

    reg_write(GRC_FASTBOOT_PC, 0);

    /* "Workaround": bit 29 first, then the reset with it (tg3_chip_reset). */
    reg_write(GRC_MISC_CFG, 1u << 29);
    reg_write(GRC_MISC_CFG, GRC_MISC_CFG_CORECLK_RESET | (1u << 29));

    sleep_ms(1);

    command = kosmos_dev_config(DEV_BROADCOM_ETHERNET, 0, PCI_COMMAND_WORD);

    if (command < 0 || (command & 0x2) == 0 || (command & 0x4) == 0) {
        say_begin(line);
        say_text(line, "tg3: after its reset the command register is ");
        say_hex(line, command < 0 ? 0xFFFFu : (unsigned long)command & 0xFFFFu,
                4);
        say_text(line, "; memory or bus master is off, and Kosmos cannot "
                       "set them back");
        say_send(console, line);
    }

    /* `tg3_restore_pci_state`, the two that have a mirror. */
    reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl);
    reg_write(TG3PCI_PCISTATE, PCISTATE_ROM_ENABLE | PCISTATE_ROM_RETRY_ENABLE);

    reg_write(MEMARB_MODE, reg_read(MEMARB_MODE) | MEMARB_MODE_ENABLE);

    if (!poll_fw()) {
        say_begin(line);
        say_text(line, "tg3: the boot code did not answer after the reset; "
                       "its mailbox says ");
        say_hex(line, sram_read(NIC_SRAM_FIRMWARE_MBOX), 8);
        say_send(console, line);

        if (locked) {
            nvram_unlock();
        }

        return false;
    }

    if (locked) {
        nvram_unlock();
    }

    reg_write(GRC_MODE, card.grc_mode);

    /* Linux writes MAC_MODE 0 here; the PRG's step 14 GMII and 40 ms. The
     * port mode is set from the link once it is known (`link_apply`). */
    card.mac_mode = 0;
    reg_write_f(MAC_MODE, card.mac_mode);
    delay_us(40);

    /* `tg3_mdio_start`: the MDIO clock CPMU chips want. */
    reg_write_f(MAC_MI_MODE, MAC_MI_MODE_500KHZ_CONST);
    delay_us(80);

    card.asf = sram_read(NIC_SRAM_DATA_SIG) == NIC_SRAM_DATA_SIG_MAGIC
               && (sram_read(NIC_SRAM_DATA_CFG) & NIC_SRAM_DATA_CFG_ASF_ENABLE);
    return true;
}

/*
 * `tg3_get_device_address`, without the parts that are not here: no device
 * tree or ACPI property is asked (Linux's first stop, and the open question
 * on a Mac), and no NVRAM is read (the second, at offset 0x7c; self-boot
 * chips have none, PRG Table 3). So the boot code's mailbox, then the MAC's
 * own registers, which the boot code fills.
 */
static void find_mac(void)
{
    card.have_mac = tg3_decode_mac_mbox(sram_read(NIC_SRAM_MAC_ADDR_HIGH_MBOX),
                                        sram_read(NIC_SRAM_MAC_ADDR_LOW_MBOX),
                                        card.mac);
    card.mac_from = "the boot code's mailbox";

    if (!card.have_mac) {
        card.have_mac = tg3_decode_mac_regs(reg_read(MAC_ADDR_0_HIGH),
                                            reg_read(MAC_ADDR_0_LOW), card.mac);
        card.mac_from = "MAC_ADDR_0";
    }

    if (card.have_mac && tg3_mac_is_default(card.mac)) {
        card.have_mac = false;          /* 00:10:18:00:00:00: nobody set it */
    }
}

/* `__tg3_set_mac_addr`: all four slots, and the backoff seed from it. */
static void set_mac(void)
{
    uint32_t hi, lo;
    unsigned i;

    tg3_encode_mac(card.mac, &hi, &lo);

    for (i = 0; i < 4u; i++) {
        reg_write(MAC_ADDR_0_HIGH + i * 8u, hi);
        reg_write(MAC_ADDR_0_LOW + i * 8u, lo);
    }

    reg_write(MAC_TX_BACKOFF_SEED, tg3_backoff_seed(card.mac));
}

static void bdinfo(unsigned sram, uint64_t host, uint32_t maxlen_flags)
{
    sram_write(sram + TG3_BDINFO_HOST_ADDR, (uint32_t)(host >> 32));
    sram_write(sram + TG3_BDINFO_HOST_ADDR + 4u, (uint32_t)host);
    sram_write(sram + TG3_BDINFO_MAXLEN_FLAGS, maxlen_flags);
}

/*
 * `tg3_rings_reset` and the posting `tg3_rx_prodring_alloc` does: every
 * ring empty, its control block written, and 64 receive buffers given.
 */
static void rings_reset(void)
{
    unsigned i;

    memset((void *)(card.mem + STATUS_AT), 0, TG3_STATUS_BYTES);
    memset((void *)(card.mem + TX_DESC_AT), 0,
           (size_t)(RX_FRAMES_AT - TX_DESC_AT));

    /* The send ring: index 0 in use; index 1 disabled (the class's limit
     * is two, `tg3_rings_reset`). NIC_ADDR is not written on 5705_PLUS;
     * the PRG's step 31 gives 0x4000. */
    bdinfo(NIC_SRAM_SEND_RCB, card.bus + TX_DESC_AT, tg3_bdinfo_ring(TX_RING));
    sram_write(NIC_SRAM_SEND_RCB + TG3_BDINFO_SIZE + TG3_BDINFO_MAXLEN_FLAGS,
               BDINFO_FLAGS_DISABLED);

    /* The return rings: 0 in use, 1 to 3 disabled (limit 4 for this
     * class). The PRG's step 36 gives NIC_ADDR 0x6000; Linux skips it. */
    bdinfo(NIC_SRAM_RCV_RET_RCB, card.bus + RET_DESC_AT,
           tg3_bdinfo_ring(RET_RING));

    for (i = 1; i < 4u; i++) {
        sram_write(NIC_SRAM_RCV_RET_RCB + i * TG3_BDINFO_SIZE
                   + TG3_BDINFO_MAXLEN_FLAGS, BDINFO_FLAGS_DISABLED);
    }

    mbox_write(MAILBOX_INTERRUPT_0, 1);          /* masked */
    mbox_write(MAILBOX_SNDHOST_PROD_IDX_0, 0);
    mbox_write(MAILBOX_RCVRET_CON_IDX_0, 0);

    reg_write(HOSTCC_STATUS_BLK_HOST_ADDR, (uint32_t)((card.bus + STATUS_AT) >> 32));
    reg_write(HOSTCC_STATUS_BLK_HOST_ADDR + 4u, (uint32_t)(card.bus + STATUS_AT));

    card.tx.slots = TX_RING;
    card.tx.limit = BUFFERS - 1u;
    card.tx.next = 0;
    card.tx.clean = 0;
    card.ret_next = 0;
    card.last_tag = 0;

    for (i = 0; i < BUFFERS; i++) {
        tg3_put_rx_std(std_desc(i), card.bus + RX_FRAMES_AT
                                    + (i % BUFFERS) * FRAME_SLOT,
                       i, RX_BUFFER_BYTES);
    }

    card.std_prod = BUFFERS;
    RING_BARRIER();
}

/*
 * `tg3_reset_hw`, the parts the 57765 class takes, in Linux's order. What is
 * left out is said: EEE (the 57766's patch is not loaded, so it stays off,
 * as Linux leaves it when the file is missing), TSO's firmware (none on
 * HW_TSO_3), jumbo frames, RSS and the statistics block's DMA.
 */
static bool reset_hw(struct say_line *line)
{
    uint32_t v;
    unsigned i;

    /* A 57765 A0 also has a PCIe PL-block step here (TG3_PCIE_PL_LO_PHYCTL5
     * DIS_L2CLKREQ, `tg3_reset_hw`) that this draft leaves out: neither Mac
     * is expected to carry an A0, and the first boot will say.
     *
     * Not AX (`tg3_decode_rev`: true of every chip of the class as Linux
     * reads it): the transmit hang fix. */
    if (!card.rev.ax) {
        reg_write(TG3_CPMU_PADRNG_CTL,
                  reg_read(TG3_CPMU_PADRNG_CTL) | TG3_CPMU_PADRNG_CTL_RDIV2);

        v = reg_read(GRC_MODE);
        reg_write(GRC_MODE, (v & ~GRC_MODE_PCIE_PORT_MASK) | GRC_MODE_PCIE_DL_SEL);
        reg_write(TG3_PCIE_TLDLPL_PORT + TG3_PCIE_DL_LO_FTSMAX,
                  (reg_read(TG3_PCIE_TLDLPL_PORT + TG3_PCIE_DL_LO_FTSMAX)
                   & ~TG3_PCIE_DL_LO_FTSMAX_MSK) | TG3_PCIE_DL_LO_FTSMAX_VAL);
        reg_write(GRC_MODE, v);
    }

    reg_write(TG3_CPMU_LSPD_10MB_CLK,
              (reg_read(TG3_CPMU_LSPD_10MB_CLK) & ~CPMU_LSPD_10MB_MACCLK_MASK)
              | CPMU_LSPD_10MB_MACCLK_6_25);

    /* `tg3_calc_dma_bndry` gives 57765_PLUS only DIS_CACHE_ALIGNMENT; A0
     * also has its read-request size cleared. */
    v = reg_read(TG3PCI_DMA_RW_CTRL) | DMA_RWCTRL_DIS_CACHE_ALIGNMENT;

    if (card.rev.a0) {
        v &= ~DMA_RWCTRL_CRDRDR_RDMA_MRRS_MSK;
    }

    reg_write(TG3PCI_DMA_RW_CTRL, v);

    reg_write(GRC_MODE, card.grc_mode | GRC_MODE_HOST_SENDBDS
                        | GRC_MODE_NO_TX_PHDR_CSUM | GRC_MODE_IRQ_ON_MAC_ATTN
                        | GRC_MODE_HOST_STACKUP);

    /* A 66 MHz clock to a 1 MHz timer: prescaler 65. */
    reg_write(GRC_MISC_CFG, (reg_read(GRC_MISC_CFG) & ~GRC_MISC_CFG_PRESCALAR_MASK)
                            | (65u << GRC_MISC_CFG_PRESCALAR_SHIFT));

    /* The buffer manager's watermarks, `tp->bufmgr_config` for 57765_PLUS
     * (DEFAULT_MB_*_57765), and the manager on. */
    reg_write(BUFMGR_MB_RDMA_LOW_WATER, 0);
    reg_write(BUFMGR_MB_MACRX_LOW_WATER, 0x2Au);
    reg_write(BUFMGR_MB_HIGH_WATER, 0xA0u);
    reg_write(BUFMGR_DMA_LOW_WATER, 5u);
    reg_write(BUFMGR_DMA_HIGH_WATER, 10u);
    reg_write(BUFMGR_MODE, BUFMGR_MODE_ENABLE | BUFMGR_MODE_ATTN_ENABLE);

    for (i = 0; i < 2000u; i++) {
        if (reg_read(BUFMGR_MODE) & BUFMGR_MODE_ENABLE) {
            break;
        }

        delay_us(10);
    }

    if (i == 2000u) {
        say_begin(line);
        say_text(line, "tg3: its buffer manager did not start");
        say_send(console, line);
        return false;
    }

    /* `tg3_setup_rxbd_thresholds`: the threshold is min(bdcache / 2 = 64,
     * max(rx_pending / 8, 1)) - 25, 0x19, at Linux's 200 pending, which is
     * the PRG's figure too (p. 146); here 64 are posted, so 8. The low
     * water mark is the whole cache, TG3_SRAM_RX_STD_BDCACHE_SIZE_5700 128,
     * on 57765_PLUS, where the PRG gives 0x20. */
    reg_write(RCVBDI_STD_THRESH, BUFFERS / 8u);
    reg_write(STD_REPLENISH_LWM, 128u);

    rings_reset();

    /* The standard ring's control block, by MMIO; NIC_ADDR is written for
     * every chip that is not 5717_PLUS. The jumbo ring disabled. */
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_HOST_ADDR,
              (uint32_t)((card.bus + STD_DESC_AT) >> 32));
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_HOST_ADDR + 4u,
              (uint32_t)(card.bus + STD_DESC_AT));
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_MAXLEN_FLAGS,
              tg3_bdinfo_std(STD_RING, RX_BUFFER_BYTES));
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_NIC_ADDR, NIC_SRAM_RX_BUFFER_DESC);
    reg_write(RCVDBDI_JUMBO_BD + TG3_BDINFO_MAXLEN_FLAGS, BDINFO_FLAGS_DISABLED);

    mbox_write(MAILBOX_RCV_STD_PROD_IDX, card.std_prod);
    mbox_write(MAILBOX_RCV_JUMBO_PROD_IDX, 0);

    set_mac();

    reg_write(MAC_RX_MTU_SIZE, 1514u + 4u + 4u);    /* ETH_HLEN + FCS + VLAN */
    reg_write(MAC_TX_LENGTHS, (2u << 12) | (6u << 8) | 32u);
    reg_write(MAC_RCV_RULE_CFG, RCV_RULE_CFG_DEFAULT_CLASS);
    reg_write(RCVLPC_CONFIG, 0x181u);

    reg_write(TG3_RDMA_RSRVCTRL_REG,
              reg_read(TG3_RDMA_RSRVCTRL_REG) | TG3_RDMA_RSRVCTRL_FIFO_OFLW_FIX);

    reg_write(RCVLPC_STATS_ENABLE,
              reg_read(RCVLPC_STATS_ENABLE) & ~RCVLPC_STATSENAB_DACK_FIX);
    reg_write(RCVLPC_STATSCTRL, 1u);
    reg_write(SNDDATAI_STATSENAB, 0xFFFFFFu);
    reg_write(SNDDATAI_STATSCTRL, 3u);

    /* Coalescing off while it is set, then on. Linux's defaults with
     * tagged status (`tg3_init_coal`); the PRG's p. 147 swaps the two tick
     * figures. */
    reg_write(HOSTCC_MODE, 0);

    for (i = 0; i < 2000u && (reg_read(HOSTCC_MODE) & HOSTCC_MODE_ENABLE); i++) {
        delay_us(10);
    }

    reg_write(HOSTCC_RXCOL_TICKS, 0x14u);
    reg_write(HOSTCC_TXCOL_TICKS, 0x48u);
    reg_write(HOSTCC_RXMAX_FRAMES, 5u);
    reg_write(HOSTCC_TXMAX_FRAMES, 0x35u);
    reg_write(HOSTCC_RXCOAL_MAXF_INT, 5u);
    reg_write(HOSTCC_TXCOAL_MAXF_INT, 5u);
    reg_write(HOSTCC_MODE, HOSTCC_MODE_ENABLE | HOSTCC_MODE_32BYTE
                           | HOSTCC_MODE_CLRTICK_RXBD | HOSTCC_MODE_CLRTICK_TXBD);

    reg_write(RCVCC_MODE, 6u);
    reg_write(RCVLPC_MODE, 2u);

    card.mac_mode = MAC_MODE_TXSTAT_ENABLE | MAC_MODE_RXSTAT_ENABLE
                    | MAC_MODE_TDE_ENABLE | MAC_MODE_RDE_ENABLE
                    | MAC_MODE_FHDE_ENABLE | MAC_MODE_PORT_MODE_GMII;
    reg_write_f(MAC_MODE, card.mac_mode | MAC_MODE_RXSTAT_CLEAR
                          | MAC_MODE_TXSTAT_CLEAR);
    delay_us(40);

    reg_write_f(GRC_LOCAL_CTRL, GRC_LCLCTRL_INT_ON_ATTN | GRC_LCLCTRL_AUTO_SEEPROM
                                | GRC_LCLCTRL_GPIO_UART_SEL
                                | (reg_read(GRC_LOCAL_CTRL) & 0x0000FF00u));
    delay_us(100);

    reg_write_f(WDMAC_MODE, DMAC_MODE_ENABLES | WDMAC_MODE_STATUS_TAG_FIX);
    delay_us(40);

    v = DMAC_MODE_ENABLES | RDMAC_MODE_FIFO_LONG_BURST | RDMAC_MODE_IPV4_LSO_EN
        | RDMAC_MODE_IPV6_LSO_EN;

    if (card.rev.is_57766) {
        v |= RDMAC_MODE_JMB_2K_MMRR;
    }

    reg_write_f(RDMAC_MODE, v);
    delay_us(40);

    reg_write(RCVDCC_MODE, 6u);
    reg_write(SNDDATAC_MODE, 2u);
    reg_write(SNDBDC_MODE, 6u);
    reg_write(RCVBDI_MODE, 6u);
    reg_write(RCVDBDI_MODE, RCVDBDI_MODE_ENABLE | RCVDBDI_MODE_INV_RING_SZ);
    reg_write(SNDDATAI_MODE, 2u | 0x8u);            /* bit 3: Linux, for TSO */
    reg_write(SNDBDI_MODE, 6u);
    reg_write(SNDBDS_MODE, 6u);

    reg_write_f(MAC_TX_MODE, TX_MODE_ENABLE | TX_MODE_MBUF_LOCKUP_FIX);
    delay_us(100);
    reg_write_f(MAC_RX_MODE, RX_MODE_ENABLE | RX_MODE_IPV6_CSUM_ENABLE);
    delay_us(10);

    /* Linux writes `tp->led_ctrl`, from the NVRAM's configuration; the
     * PRG's step 50 gives 0x800. Left as the boot code set it. */

    reg_write_f(MAC_MI_STAT, MAC_MI_STAT_LNKSTAT_ATTN_ENAB);
    reg_write(MAC_LOW_WMARK_MAX_RX_FRAME, 1u);

    /* Every multicast, as `__tg3_set_rx_mode` does with IFF_ALLMULTI; the
     * stack filters. */
    for (i = 0; i < 4u; i++) {
        reg_write(MAC_HASH_REG_0 + i * 4u, 0xFFFFFFFFu);
    }

    /* The receive rules `tg3_reset_hw` writes, and the rest cleared. */
    reg_write(MAC_RCV_RULE_0, 0xC2000000u & 0x7FFFFFFFu);
    reg_write(MAC_RCV_VALUE_0, 0xFFFFFFFFu & 0x7FFFFFFFu);
    reg_write(MAC_RCV_RULE_0 + 8u, 0x86000004u & 0x7FFFFFFFu);
    reg_write(MAC_RCV_VALUE_0 + 8u, 0xFFFFFFFFu & 0x7FFFFFFFu);

    for (i = 4; i < 8u; i++) {
        reg_write(MAC_RCV_RULE_0 + i * 8u, 0);
        reg_write(MAC_RCV_VALUE_0 + i * 8u, 0);
    }

    return true;
}

/* Is MSI on? The HAL's `pci_enable` turns it on where it can; this only
 * reads which, because Linux enables MSI only with tagged status
 * (`tg3_ints_init`) and MSGINT_MODE has to agree. */
static bool msi_enabled(void)
{
    long at = kosmos_dev_config(DEV_BROADCOM_ETHERNET, 0, PCI_CAPABILITY_LIST);
    unsigned ptr = at < 0 ? 0 : (unsigned)at & 0xFCu;
    unsigned guard;

    for (guard = 0; ptr >= 0x40u && guard < 48u; guard++) {
        long cap = kosmos_dev_config(DEV_BROADCOM_ETHERNET, 0, ptr);

        if (cap < 0) {
            return false;
        }

        if ((cap & 0xFF) == PCI_CAP_ID_MSI) {
            return ((cap >> 16) & PCI_MSI_FLAGS_ENABLE) != 0;
        }

        ptr = (unsigned)(cap >> 8) & 0xFCu;
    }

    return false;
}

/* `tg3_enable_ints`, with tagged status: unmasked, the mailbox written with
 * the last tag (twice, for one-shot MSI), and an interrupt forced. */
static void enable_ints(void)
{
    reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl & ~MISC_HOST_CTRL_MASK_PCI_INT);
    mbox_write(MAILBOX_INTERRUPT_0, tg3_int_mbox_rearm(card.last_tag));
    mbox_write(MAILBOX_INTERRUPT_0, tg3_int_mbox_rearm(card.last_tag));
    reg_write(HOSTCC_MODE, HOSTCC_MODE_ENABLE | HOSTCC_MODE_32BYTE
                           | HOSTCC_MODE_CLRTICK_RXBD | HOSTCC_MODE_CLRTICK_TXBD
                           | HOSTCC_MODE_NOW);
}

static bool bring_up(struct say_line *line)
{
    struct dev_info dev;
    struct sysinfo info;
    long region, mapped, bus, regs, mhc, prodid;
    uint16_t id1 = 0, id2 = 0;

    if (kosmos_dev_find(DEV_BROADCOM_ETHERNET, 0, &dev) != 0) {
        return false;
    }

    if (!tg3_decode_id(TG3_VENDOR_BROADCOM, (uint16_t)dev.line, &card.chip)) {
        say_begin(line);
        say_text(line, "tg3: 14e4:");
        say_hex(line, dev.line, 4);
        say_text(line, " is not of the 57765 class this drives");
        say_send(console, line);
        return false;
    }

    regs = kosmos_dev_map(dev.base, (dev.size + 4095u) / 4096u);

    if (regs < 0) {
        say_begin(line);
        say_text(line, "tg3: the chip's registers could not be mapped");
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

    /* The revision: MISC_HOST_CTRL's top half, or the product register it
     * points at (`tg3_detect_asic_rev`). */
    mhc = kosmos_dev_config(DEV_BROADCOM_ETHERNET, 0, TG3PCI_MISC_HOST_CTRL);
    prodid = kosmos_dev_config(DEV_BROADCOM_ETHERNET, 0, card.chip.prodid_at);
    tg3_decode_rev(mhc < 0 ? 0 : (uint32_t)mhc, prodid < 0 ? 0 : (uint32_t)prodid,
                   &card.rev);

    card.misc_host_ctrl = MISC_HOST_CTRL_MASK_PCI_INT | MISC_HOST_CTRL_WORD_SWAP
                          | MISC_HOST_CTRL_INDIR_ACCESS
                          | MISC_HOST_CTRL_PCISTATE_RW
                          | MISC_HOST_CTRL_TAGGED_STATUS;
    card.grc_mode = GRC_MODE_WSWAP_DATA | GRC_MODE_BSWAP_DATA
                    | GRC_MODE_WSWAP_NONFRM_DATA;

    /* Quiet first: `tg3_disable_ints`. */
    reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl);
    mbox_write(MAILBOX_INTERRUPT_0, 1);

    region = kosmos_mem_create_flags(REGION_PAGES, MEM_CONTIGUOUS);
    mapped = region < 0 ? region : kosmos_mem_map(region);
    bus = mapped < 0 ? mapped : kosmos_mem_phys(region);

    if (region < 0 || mapped < 0 || bus <= 0) {
        say_begin(line);
        say_text(line, "tg3: no memory the chip can reach for its rings");
        say_send(console, line);
        return false;
    }

    card.mem = (uintptr_t)mapped;
    card.bus = (uint64_t)bus;

    if (!chip_reset(line)) {
        return false;
    }

    find_mac();

    if (!card.have_mac) {
        card.present = true;            /* said by the caller, then let go */
        return true;
    }

    /* `tg3_init_hw` polls the boot code again and puts the window at 0. */
    (void)poll_fw();
    reg_write(TG3PCI_MEM_WIN_BASE_ADDR, 0);

    if (!reset_hw(line)) {
        return false;
    }

    if (phy_read(MII_PHYSID1, &id1) && phy_read(MII_PHYSID2, &id2)) {
        card.phy_id = tg3_phy_id(id1, id2);
    }

    /* The boot code has brought the PHY up and set its advertisement; a
     * link that is not up yet is asked to negotiate again, and nothing
     * else of `tg3_phy_reset` and `tg3_phy_autoneg_cfg` is done. */
    link_read(&card.link);

    if (!card.link.up) {
        (void)phy_write(MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    }

    link_apply();

    card.msi = msi_enabled();

    if (card.msi) {
        /* MSGINT_MODE_ONE_SHOT_DISABLE stays clear: 1SHOT_MSI. */
        reg_write(MSGINT_MODE, reg_read(MSGINT_MODE) | MSGINT_MODE_ENABLE);
    }

    card.irq = kosmos_irq_claim(dev.intid);
    card.present = true;
    enable_ints();
    return true;
}

/*----------------------------------------------------------------- frames */

static void tx_reclaim(void)
{
    struct tg3_status st;

    RING_BARRIER();
    tg3_decode_status((const uint8_t *)(card.mem + STATUS_AT), &st);
    card.sent += tg3_tx_reclaim(&card.tx, st.tx_consumer);
}

/* A frame out, nothing waited for (`e1000.c`'s `send_frame` says why). The
 * chip does not pad: a frame shorter than 60 bytes is padded here, since
 * `tg3_start_xmit` relies on the stack for it and the SHORT_DMA_BUG would
 * refuse eight bytes or fewer anyway. */
static bool send_frame(const uint8_t *frame, unsigned length)
{
    unsigned slot, buffer, padded;
    uint8_t *data;

    if (!card.present || length < 14u || length > FRAME_SLOT) {
        return false;
    }

    tx_reclaim();

    if (!tg3_tx_room(&card.tx)) {
        card.refused++;
        return false;
    }

    slot = card.tx.next;
    buffer = slot % BUFFERS;
    data = (uint8_t *)(card.mem + TX_FRAMES_AT + buffer * FRAME_SLOT);
    padded = length < 60u ? 60u : length;

    memcpy(data, frame, length);

    if (padded > length) {
        memset(data + length, 0, padded - length);
    }

    if (!tg3_put_tx(tx_desc(slot), card.bus + TX_FRAMES_AT + buffer * FRAME_SLOT,
                    padded)) {
        card.refused++;
        return false;
    }

    RING_BARRIER();
    card.tx.next = (slot + 1u) % TX_RING;
    mbox_write(MAILBOX_SNDHOST_PROD_IDX_0, card.tx.next);
    return true;
}

/* `tg3_rx`: from where this end stopped to the producer the status block
 * says, each frame into the stack's ring, each buffer posted again at the
 * standard ring's producer, and both mailboxes told. */
static void take_frames(const struct tg3_status *st)
{
    unsigned taken = 0, posted = 0;

    RING_BARRIER();

    while (tg3_ring_ready(st->return_producer, card.ret_next, RET_RING) > 0) {
        struct tg3_rx got;
        unsigned buffer;

        tg3_decode_rx(ret_desc(card.ret_next), &got);
        buffer = got.index % BUFFERS;

        if (got.std && !got.error && got.length >= 14u
            && got.length <= ETH_RING_SLOT && card.ring != NULL) {
            uint32_t write = card.ring->in_write;
            uint32_t read = eth_ring_acquire(&card.ring->in_read);

            if (eth_ring_space(write, read) > 0) {
                memcpy(eth_ring_in(card.ring, write),
                       (const uint8_t *)(card.mem + RX_FRAMES_AT
                                         + buffer * FRAME_SLOT),
                       got.length);
                card.ring->in_length[write % ETH_RING_SLOTS] = got.length;
                eth_ring_publish(&card.ring->in_write, write + 1u);
                card.received++;
                taken++;
            } else {
                card.dropped++;
            }
        } else {
            card.dropped++;
        }

        if (got.std) {
            unsigned p = card.std_prod % STD_RING;

            /* In order, so the entry 64 after the one consumed has the
             * same buffer (`tg3_recycle_rx` keeps the cookie instead). */
            tg3_put_rx_std(std_desc(p), card.bus + RX_FRAMES_AT
                                        + (p % BUFFERS) * FRAME_SLOT,
                           p, RX_BUFFER_BYTES);
            card.std_prod = (card.std_prod + 1u) % STD_RING;
            posted++;
        }

        card.ret_next = (card.ret_next + 1u) % RET_RING;
    }

    RING_BARRIER();
    mbox_write(MAILBOX_RCVRET_CON_IDX_0, card.ret_next);

    if (posted > 0) {
        mbox_write(MAILBOX_RCV_STD_PROD_IDX, card.std_prod);
    }

    if (taken > 0) {
        (void)kosmos_net_wake();
    }
}

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

static void say_link(struct say_line *line)
{
    static struct tg3_link said;

    if (card.link_said && said.up == card.link.up
        && said.mbps == card.link.mbps
        && said.full_duplex == card.link.full_duplex) {
        return;
    }

    said = card.link;
    card.link_said = true;

    say_begin(line);
    say_text(line, "tg3: its link is ");

    if (!card.link.up) {
        say_text(line, "down");
    } else {
        say_text(line, "up at ");

        if (card.link.mbps == 0) {
            say_text(line, "a speed this does not know");
        } else {
            say_dec(line, card.link.mbps);
            say_text(line, " Mb/s");
        }

        say_text(line, card.link.full_duplex ? ", full duplex"
                                             : ", half duplex");
    }

    say_send(console, line);
}

/*--------------------------------------------------------------- the stack */

static void about(struct eth_reply *rep)
{
    rep->present = 1;
    rep->mtu = 1514;
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
            say_text(line, "tg3: the network stack has it");
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

static void say_found(struct say_line *line)
{
    unsigned i;

    say_begin(line);
    say_text(line, "tg3: 14e4:");
    say_hex(line, card.device, 4);
    say_text(line, ", a BCM");
    say_dec(line, card.chip.number);
    say_word(line, " revision ", card.rev.id);
    say_text(line, " at ");
    say_hex(line, (card.where >> 8) & 0xFFu, 2);
    say_text(line, ":");
    say_hex(line, (card.where >> 3) & 0x1Fu, 2);
    say_text(line, ".");
    say_dec(line, card.where & 0x7u);
    say_text(line, ", MAC ");

    for (i = 0; i < 6u; i++) {
        if (i != 0) {
            say_text(line, ":");
        }

        say_hex(line, card.mac[i], 2);
    }

    say_text(line, " from ");
    say_text(line, card.mac_from);
    say_word(line, ", PHY ", card.phy_id);

    if (!tg3_phy_is_57765(card.phy_id)) {
        say_text(line, " (not the BCM57765's)");
    }

    say_text(line, card.msi ? ", MSI " : ", interrupt ");
    say_dec(line, card.intid);

    if (card.irq < 0) {
        say_text(line, " not claimed, so polled");
    }

    if (card.asf) {
        say_text(line, ", ASF on");
    }

    say_send(console, line);
}

/*
 * **Answers with no chip too**, as e1000 does, because the stack asks every
 * driver it has an endpoint for. The 25-tick deadline on the interrupt
 * matters more here than there: the 57765 class can lose an MSI, which
 * Linux answers with `tg3_chk_missed_msi` on a timer, and this answers by
 * looking at the status block whenever it wakes.
 */
void tg3_server(long console_cap, long frames_cap);

void tg3_server(long console_cap, long frames_cap)
{
    struct say_line line;
    const long ends[1] = { frames_cap };

    console = console_cap;
    frames_endpoint = frames_cap;
    memset(&card, 0, sizeof(card));
    card.irq = -1;
    card.ring_cap = -1;

    if (bring_up(&line) && card.have_mac) {
        say_found(&line);
        say_link(&line);
    } else if (card.present) {
        say_begin(&line);
        say_text(&line, "tg3: the chip has no address of its own; not driven");
        say_send(console, &line);
        card.present = false;
    }

    for (;;) {
        long woke;
        long lines[1];

        if (card.present && card.irq >= 0) {
            lines[0] = card.irq;
            woke = kosmos_irq_wait_any(lines, 1, 25ul, ends, 1u);
        } else {
            woke = kosmos_irq_wait_any(NULL, 0, card.present ? 4ul : 0ul,
                                       ends, 1u);
        }

        if (woke < 0 && woke != SYS_NO_INTERRUPT) {
            kosmos_exit(0);
        }

        if (card.present) {
            struct tg3_status st;

            /* `tg3_interrupt_tagged`: masked while the block is read. */
            mbox_write(MAILBOX_INTERRUPT_0, 1);
            RING_BARRIER();
            tg3_decode_status((const uint8_t *)(card.mem + STATUS_AT), &st);
            card.last_tag = st.tag;

            if (st.link_changed) {
                /* `tg3_poll_link`: the bit cleared in the block, the PHY
                 * asked. */
                uint8_t *sb = (uint8_t *)(card.mem + STATUS_AT);

                put_le32(sb, (get_le32(sb) | 0x1u) & ~0x2u);
                link_apply();
                say_link(&line);
            }

            take_frames(&st);
            tx_reclaim();

            if (st.error) {
                say_begin(&line);
                say_text(&line, "tg3: the status block says an error");
                say_word(&line, "; flow attention ", reg_read(HOSTCC_FLOW_ATTN));
                say_send(console, &line);
            }

            mbox_write(MAILBOX_INTERRUPT_0, tg3_int_mbox_rearm(card.last_tag));

            if (card.irq >= 0) {
                (void)kosmos_irq_ack(card.irq);
            }
        }

        serve(&line);
    }
}
