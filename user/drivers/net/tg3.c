/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A Broadcom 57765-class Ethernet controller, driven from a process.
 *
 *   /Network  <--- ethring.h ---  this  --- registers, SRAM, rings --->  the chip
 *
 * **UNPROVED: it has never run.** QEMU has no model of this chip, and no
 * machine has booted this file yet. What is checked is the layer beneath
 * it, `tg3_decode.c`, by `tools/test_tg3decode.c` on the host; this file is
 * Linux's sequence written down in Kosmos's shape, and the Mac Pro's first
 * boot is its first test. So it says what it is doing at every step, in an
 * order a photograph of the screen can be read in - `tg3: 1 ...` to
 * `tg3: 10 ...` - and when something is not what Linux expects it says
 * `tg3: STOP at N: ...`, stops touching the chip, and goes on answering the
 * network stack that there is no adapter.
 *
 * **Why it exists.** Two of Diego's Macs have this family on their board:
 * the Mac Pro 2013 two BCM57762s (14e4:1682) and, believed but not yet seen,
 * the iMac 2019 a BCM57766 (14e4:1686). `docs/drivers.md`, *Broadcom
 * Ethernet*, has the reading this is built from and the decisions.
 *
 * **Where every number comes from.** Linux v6.12's `drivers/net/ethernet/
 * broadcom/tg3.c` and `tg3.h`, and `include/uapi/linux/pci_regs.h` and
 * `mii.h` beside them. Each register is named as tg3.h names it, so a grep
 * in either finds the other, and every `#define` here was compared with
 * those headers by a script rather than by eye. Where this departs from
 * Linux the comment says so and why.
 *
 * **No firmware is loaded.** The 57762 is HW_TSO_3 and takes none; the
 * 57766 wants `tigon/tg357766.bin` only when it has no NVRAM, and that file
 * is the EEE patch - without it Linux says "EEE capability disabled" and
 * works. EEE stays off here on both.
 *
 * **It answers the same protocol the Intel driver does** (`ethproto.h`),
 * so `net.c` attaches to whichever has a card. The shape is `e1000.c`'s.
 *
 * **Nothing waits on hardware inside a request.** Bring-up has waits Linux
 * puts there - up to a second for the boot code, a tenth of one for each
 * block that stops - and the stack asks every driver it knows about once a
 * second, synchronously. So the waits answer it as they go (`keep_answering`)
 * with "no adapter", which is true until the last step, and the stack asks
 * again a second later.
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
 * The device kind: 7, which `kernel/syscall.h` keeps for this lane. The
 * kernel and the board learn it from the main session's patch (`hal.h`
 * HAL_DEV_BROADCOM_ETHERNET, a vendor 0x14E4 class 02/00 match in
 * `hal/pc/devices.c`, and `hal_device_config` for it); until then this
 * names it itself.
 */
#ifndef DEV_BROADCOM_ETHERNET
#define DEV_BROADCOM_ETHERNET   7u
#endif

/* The two ports the Mac Pro has; the board keeps as many. */
#define PORTS                   2u

/*------------------------------------------------------------- PCI config
 *
 * Read through SYS_DEV_CONFIG; written through SYS_DEV_CONFIG_SET where the
 * kernel has it (the main session's patch). Without it, the two registers
 * of this chip that have an MMIO mirror below 0x100 are written there, which
 * works only while memory decoding survives the reset - and the driver
 * reads the command register to say whether it did.
 */
#define PCI_COMMAND                     0x04    /* pci_regs.h */
#define  PCI_COMMAND_IO                 0x1
#define  PCI_COMMAND_MEMORY             0x2
#define  PCI_COMMAND_MASTER             0x4
#define PCI_CAP_ID_PM                   0x01
#define PCI_CAP_ID_MSI                  0x05
#define PCI_CAP_ID_EXP                  0x10
#define PCI_CAP_ID_MSIX                 0x11
#define PCI_PM_CTRL                     4
#define  PCI_PM_CTRL_STATE_MASK         0x0003
#define  PCI_PM_CTRL_PME_STATUS         0x8000
#define PCI_MSI_FLAGS                   0x02
#define  PCI_MSI_FLAGS_ENABLE           0x0001
#define PCI_MSIX_FLAGS                  2
#define  PCI_MSIX_FLAGS_ENABLE          0x8000
#define PCI_EXP_DEVCTL                  0x08

/*------------------------------------------------------------- registers
 *
 * Offsets into the first BAR, as tg3.h names them.
 */
#define TG3PCI_MISC_HOST_CTRL           0x00000068
#define  MISC_HOST_CTRL_MASK_PCI_INT    0x00000002
#define  MISC_HOST_CTRL_WORD_SWAP       0x00000008
#define  MISC_HOST_CTRL_PCISTATE_RW     0x00000010
#define  MISC_HOST_CTRL_INDIR_ACCESS    0x00000080
#define  MISC_HOST_CTRL_TAGGED_STATUS   0x00000200
#define TG3PCI_DMA_RW_CTRL              0x0000006c
#define  DMA_RWCTRL_DIS_CACHE_ALIGNMENT 0x00000001
#define  DMA_RWCTRL_CRDRDR_RDMA_MRRS_MSK 0x00000380
#define TG3PCI_PCISTATE                 0x00000070
#define  PCISTATE_ROM_ENABLE            0x00000020
#define  PCISTATE_ROM_RETRY_ENABLE      0x00000040
#define TG3PCI_MEM_WIN_BASE_ADDR        0x0000007c
#define TG3PCI_MEM_WIN_DATA             0x00000084

/* Mailboxes: each is a 64-bit slot and the 32-bit write goes to its low
 * word, TG3_64BIT_REG_LOW. */
#define TG3_64BIT_REG_LOW               0x04
#define MAILBOX_INTERRUPT_0             0x00000200
#define MAILBOX_RCV_STD_PROD_IDX        0x00000268
#define MAILBOX_RCV_JUMBO_PROD_IDX      0x00000270
#define MAILBOX_RCVRET_CON_IDX_0        0x00000280
#define MAILBOX_SNDHOST_PROD_IDX_0      0x00000300

#define MAC_MODE                        0x00000400
#define  MAC_MODE_PORT_MODE_GMII        0x00000008
#define  MAC_MODE_RXSTAT_ENABLE         0x00000800
#define  MAC_MODE_RXSTAT_CLEAR          0x00001000
#define  MAC_MODE_TXSTAT_ENABLE         0x00004000
#define  MAC_MODE_TXSTAT_CLEAR          0x00008000
#define  MAC_MODE_TDE_ENABLE            0x00200000
#define  MAC_MODE_RDE_ENABLE            0x00400000
#define  MAC_MODE_FHDE_ENABLE           0x00800000
#define MAC_STATUS                      0x00000404
#define  MAC_STATUS_SYNC_CHANGED        0x00000010
#define  MAC_STATUS_CFG_CHANGED         0x00000008
#define  MAC_STATUS_LNKSTATE_CHANGED    0x00001000
#define  MAC_STATUS_MI_COMPLETION       0x00400000
#define MAC_EVENT                       0x00000408
#define  MAC_EVENT_LNKSTATE_CHANGED     0x00001000
#define MAC_ADDR_0_HIGH                 0x00000410
#define MAC_ADDR_0_LOW                  0x00000414
#define MAC_TX_BACKOFF_SEED             0x00000438
#define MAC_RX_MTU_SIZE                 0x0000043c
#define MAC_MI_COM                      0x0000044c
#define MAC_MI_STAT                     0x00000450
#define  MAC_MI_STAT_LNKSTAT_ATTN_ENAB  0x00000001
#define MAC_MI_MODE                     0x00000454
#define  MAC_MI_MODE_AUTO_POLL          0x00000010
#define  MAC_MI_MODE_500KHZ_CONST       0x00008000
#define MAC_TX_MODE                     0x0000045c
#define  TX_MODE_ENABLE                 0x00000002
#define  TX_MODE_MBUF_LOCKUP_FIX        0x00000100
#define MAC_TX_LENGTHS                  0x00000464
#define MAC_RX_MODE                     0x00000468
#define  RX_MODE_ENABLE                 0x00000002
#define  RX_MODE_KEEP_VLAN_TAG          0x00000400
#define  RX_MODE_IPV6_CSUM_ENABLE       0x01000000
#define MAC_HASH_REG_0                  0x00000470
#define MAC_RCV_RULE_0                  0x00000480
#define MAC_RCV_VALUE_0                 0x00000484
#define MAC_RCV_RULE_CFG                0x00000500
#define  RCV_RULE_CFG_DEFAULT_CLASS     0x00000008
#define MAC_LOW_WMARK_MAX_RX_FRAME      0x00000504

/* Every block's mode register keeps its enable in bit 1 (tg3.h:
 * SNDDATAI_MODE_ENABLE ... WDMAC_MODE_ENABLE, each 0x00000002). */
#define SNDDATAI_MODE                   0x00000c00
#define  SNDDATAI_MODE_ENABLE           0x00000002
#define SNDDATAI_STATSCTRL              0x00000c08
#define SNDDATAI_STATSENAB              0x00000c0c
#define SNDDATAC_MODE                   0x00001000
#define  SNDDATAC_MODE_ENABLE           0x00000002
#define SNDBDS_MODE                     0x00001400
#define  SNDBDS_MODE_ENABLE             0x00000002
#define SNDBDI_MODE                     0x00001800
#define  SNDBDI_MODE_ENABLE             0x00000002
#define SNDBDC_MODE                     0x00001c00
#define  SNDBDC_MODE_ENABLE             0x00000002
#define RCVLPC_MODE                     0x00002000
#define  RCVLPC_MODE_ENABLE             0x00000002
#define RCVLPC_CONFIG                   0x00002010
#define RCVLPC_STATSCTRL                0x00002014
#define RCVLPC_STATS_ENABLE             0x00002018
#define  RCVLPC_STATSENAB_DACK_FIX      0x00040000
#define RCVDBDI_MODE                    0x00002400
#define  RCVDBDI_MODE_ENABLE            0x00000002
#define  RCVDBDI_MODE_INV_RING_SZ       0x00000010
#define RCVDBDI_JUMBO_BD                0x00002440
#define RCVDBDI_STD_BD                  0x00002450
#define RCVDCC_MODE                     0x00002800
#define  RCVDCC_MODE_ENABLE             0x00000002
#define RCVBDI_MODE                     0x00002c00
#define  RCVBDI_MODE_ENABLE             0x00000002
#define RCVBDI_STD_THRESH               0x00002c18
#define STD_REPLENISH_LWM               0x00002d00
#define RCVCC_MODE                      0x00003000
#define  RCVCC_MODE_ENABLE              0x00000002

#define TG3_CPMU_LSPD_10MB_CLK          0x00003604
#define  CPMU_LSPD_10MB_MACCLK_MASK     0x001f0000
#define  CPMU_LSPD_10MB_MACCLK_6_25     0x00130000
#define TG3_CPMU_PADRNG_CTL             0x00003668
#define  TG3_CPMU_PADRNG_CTL_RDIV2      0x00040000

#define HOSTCC_MODE                     0x00003c00
#define  HOSTCC_MODE_ENABLE             0x00000002
#define  HOSTCC_MODE_NOW                0x00000008
#define  HOSTCC_MODE_32BYTE             0x00000100
#define  HOSTCC_MODE_CLRTICK_RXBD       0x00000200
#define  HOSTCC_MODE_CLRTICK_TXBD       0x00000400
#define HOSTCC_RXCOL_TICKS              0x00003c08
#define HOSTCC_TXCOL_TICKS              0x00003c0c
#define HOSTCC_RXMAX_FRAMES             0x00003c10
#define HOSTCC_TXMAX_FRAMES             0x00003c14
#define HOSTCC_RXCOAL_MAXF_INT          0x00003c20
#define HOSTCC_TXCOAL_MAXF_INT          0x00003c24
#define HOSTCC_STATUS_BLK_HOST_ADDR     0x00003c38
#define HOSTCC_FLOW_ATTN                0x00003c48

#define MEMARB_MODE                     0x00004000
#define  MEMARB_MODE_ENABLE             0x00000002
#define BUFMGR_MODE                     0x00004400
#define  BUFMGR_MODE_ENABLE             0x00000002
#define  BUFMGR_MODE_ATTN_ENABLE        0x00000004
#define BUFMGR_MB_RDMA_LOW_WATER        0x00004410
#define BUFMGR_MB_MACRX_LOW_WATER       0x00004414
#define BUFMGR_MB_HIGH_WATER            0x00004418
#define BUFMGR_DMA_LOW_WATER            0x00004434
#define BUFMGR_DMA_HIGH_WATER           0x00004438
#define RDMAC_MODE                      0x00004800
#define  RDMAC_MODE_ENABLE              0x00000002
#define  RDMAC_MODE_FIFO_LONG_BURST     0x00030000
#define  RDMAC_MODE_JMB_2K_MMRR         0x00800000
#define  RDMAC_MODE_IPV4_LSO_EN         0x08000000
#define  RDMAC_MODE_IPV6_LSO_EN         0x10000000
#define TG3_RDMA_RSRVCTRL_REG           0x00004900
#define  TG3_RDMA_RSRVCTRL_FIFO_OFLW_FIX 0x00000004
#define WDMAC_MODE                      0x00004c00
#define  WDMAC_MODE_ENABLE              0x00000002
#define  WDMAC_MODE_STATUS_TAG_FIX      0x20000000

/* RDMAC_MODE's and WDMAC_MODE's bits 1-9 are the same nine enables in both
 * (tg3.h: RDMAC_MODE_ENABLE .. RDMAC_MODE_LNGREAD_ENAB, WDMAC_MODE_ENABLE ..
 * WDMAC_MODE_LNGREAD_ENAB), all of which `tg3_reset_hw` sets. */
#define DMAC_MODE_ENABLES               0x000003FEu

#define MSGINT_MODE                     0x00006000
#define  MSGINT_MODE_ENABLE             0x00000002
#define FTQ_RESET                       0x00005c00

#define GRC_MODE                        0x00006800
#define  GRC_MODE_WSWAP_NONFRM_DATA     0x00000004
#define  GRC_MODE_BSWAP_DATA            0x00000010
#define  GRC_MODE_WSWAP_DATA            0x00000020
#define  GRC_MODE_HOST_STACKUP          0x00010000
#define  GRC_MODE_HOST_SENDBDS          0x00020000
#define  GRC_MODE_NO_TX_PHDR_CSUM       0x00100000
#define  GRC_MODE_IRQ_ON_MAC_ATTN       0x04000000
#define  GRC_MODE_PCIE_DL_SEL           0x20000000
/* GRC_MODE_PCIE_PORT_MASK in tg3.h is (TL_SEL | PL_SEL | DL_SEL | HI_1K_EN):
 * 0x00000000 | 0x00400000 | 0x20000000 | 0x80000000. */
#define GRC_MODE_PCIE_PORT_BITS         0xA0400000u
#define GRC_MISC_CFG                    0x00006804
#define  GRC_MISC_CFG_CORECLK_RESET     0x00000001
#define  GRC_MISC_CFG_PRESCALAR_SHIFT   1
#define GRC_LOCAL_CTRL                  0x00006808
#define  GRC_LCLCTRL_AUTO_SEEPROM       0x01000000
#define GRC_EEPROM_ADDR                 0x00006838
#define  EEPROM_ADDR_FSM_RESET          0x20000000
#define  EEPROM_ADDR_CLKPERD_SHIFT      16
#define  EEPROM_DEFAULT_CLOCK_PERIOD    0x60
#define GRC_FASTBOOT_PC                 0x00006894

#define NVRAM_CMD                       0x00007000
#define NVRAM_ADDR                      0x0000700c
#define  NVRAM_ADDR_MSK                 0x07ffffff
#define NVRAM_RDDATA                    0x00007010
#define NVRAM_CFG1                      0x00007014
#define NVRAM_SWARB                     0x00007020
#define  SWARB_REQ_SET1                 0x00000002
#define  SWARB_REQ_CLR1                 0x00000020
#define  SWARB_GNT1                     0x00000200
#define NVRAM_ACCESS                    0x00007024
#define  ACCESS_ENABLE                  0x00000001

#define TG3_PCIE_TLDLPL_PORT            0x00007c00
#define TG3_PCIE_DL_LO_FTSMAX           0x0000000c
#define  TG3_PCIE_DL_LO_FTSMAX_MSK      0x000000ff
#define  TG3_PCIE_DL_LO_FTSMAX_VAL      0x0000002c

/*-------------------------------------------------------------- NIC SRAM
 *
 * Reached through the memory window, never mapped.
 */
#define NIC_SRAM_SEND_RCB               0x00000100
#define NIC_SRAM_RCV_RET_RCB            0x00000200
#define NIC_SRAM_FIRMWARE_MBOX          0x00000b50
#define  NIC_SRAM_FIRMWARE_MBOX_MAGIC1  0x4B657654
#define NIC_SRAM_DATA_SIG               0x00000b54
#define  NIC_SRAM_DATA_SIG_MAGIC        0x4b657654
#define NIC_SRAM_DATA_CFG               0x00000b58
#define  NIC_SRAM_DATA_CFG_ASF_ENABLE   0x00000080
#define NIC_SRAM_MAC_ADDR_HIGH_MBOX     0x00000c14
#define NIC_SRAM_MAC_ADDR_LOW_MBOX      0x00000c18
#define NIC_SRAM_RX_BUFFER_DESC         0x00006000

/* A ring control block, `TG3_BDINFO_*`: the host address in two words (high
 * first), then MAXLEN_FLAGS, then NIC_ADDR - which `tg3_set_bdinfo` writes
 * only on chips before 5705_PLUS for the send and return rings. */
#define TG3_BDINFO_HOST_ADDR            0x0
#define TG3_BDINFO_MAXLEN_FLAGS         0x8
#define TG3_BDINFO_NIC_ADDR             0xc
#define TG3_BDINFO_SIZE                 0x10
#define BDINFO_FLAGS_DISABLED           0x00000002

/* The 57765 class's ring control blocks: two send, four return
 * (`tg3_tx_rcbs_disable`, `tg3_rx_ret_rcbs_disable`). */
#define SEND_RCBS                       2u
#define RETURN_RCBS                     4u

/* The PHY, through MAC_MI_COM (mii.h names, and tg3.h's own). */
#define TG3_PHY_MII_ADDR                0x01
#define MII_BMCR                        0x00
#define  BMCR_ANRESTART                 0x0200
#define  BMCR_ANENABLE                  0x1000
#define MII_BMSR                        0x01
#define MII_PHYSID1                     0x02
#define MII_PHYSID2                     0x03
#define MII_ADVERTISE                   0x04
#define MII_CTRL1000                    0x09
#define MII_TG3_AUX_STAT                0x19
#define MII_TG3_ISTAT                   0x1a
#define MII_TG3_IMASK                   0x1b
#define PHY_BUSY_LOOPS                  5000

/* `tg3_stop_block`'s and `tg3_abort_hw`'s patience: 1000 times 100 us. */
#define MAX_WAIT_CNT                    1000
/* `tg3_nvram_exec_cmd`'s: 10000 times at least 10 us. */
#define NVRAM_CMD_TIMEOUT               10000

/*------------------------------------------------------------ the region
 *
 * **The chip's rings are its own size, not ours.** The send ring is
 * TG3_TX_RING_SIZE 512; the standard receive ring TG3_RX_STD_MAX_SIZE_5700
 * 512 (`tg3_rx_prodring_init`; the 5717's larger size is not this class's);
 * and the return ring TG3_RX_RET_MAX_SIZE_5700 1024, because the class is
 * JUMBO_CAPABLE and not 5780_CLASS (`tg3_get_invariants`). The draft of 14
 * November had 512 there, following the PRG; the chip writes as many
 * entries as its control block says, so the host's ring has to be the size
 * Linux tells it, and this follows Linux.
 *
 * Only 64 frames each way have buffers: a descriptor's buffer is its index
 * modulo 64, which is safe because both rings are consumed in order and at
 * most 63 are ever outstanding.
 */
#define TX_RING                 512u
#define STD_RING                512u
#define RET_RING                1024u
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
    bool          present;          /* the last step done: frames may flow */
    bool          stopped;          /* a STOP was said: the chip is let be */
    uintptr_t     regs;
    unsigned      index;            /* which port of DEV_BROADCOM_ETHERNET */
    unsigned      where;
    unsigned      device;
    unsigned      intid;
    long          irq;
    const char   *irq_kind;
    struct tg3_chip chip;
    struct tg3_rev rev;

    uint32_t      cfg[64];          /* the configuration space, as found */
    unsigned      pcie_cap, pm_cap, msi_cap, msix_cap;
    uint32_t      command;          /* `tp->pci_cmd`, put back after a reset */
    bool          config_writes;

    uintptr_t     mem;
    uint64_t      bus;

    struct tg3_nvram nvram;
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
    bool          said_status, said_in, said_out;

    uint64_t      hz;
    unsigned long tick_hz;
    uint64_t      answered_at;      /* the counter when the stack was last answered */
    uint64_t      link_polled_at;

    struct eth_ring *ring;
    long          ring_cap;
} card;

static void keep_answering(void);

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

static void mbox_write_f(unsigned at, uint32_t value)
{
    reg_write_f(at + TG3_64BIT_REG_LOW, value);
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

static uint32_t config_read(unsigned index, unsigned offset)
{
    long v = kosmos_dev_config(DEV_BROADCOM_ETHERNET, index, offset);

    return v < 0 ? 0xFFFFFFFFu : (uint32_t)v;
}

/* A configuration word written, where the kernel can (SYS_DEV_CONFIG_SET,
 * the main session's patch); false where it cannot. The kernel allows only
 * the words a driver has business with, and refuses the rest. */
static bool config_write(unsigned offset, uint32_t value)
{
#ifdef SYS_DEV_CONFIG_SET
    return sys4(SYS_DEV_CONFIG_SET, (long)DEV_BROADCOM_ETHERNET,
                (long)card.index, (long)offset, (long)value) == 0;
#else
    (void)offset;
    (void)value;
    return false;
#endif
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

/* A wait inside a loop that may run long: the wait, and the stack answered
 * if it has been more than a few milliseconds. */
static void wait_us(unsigned us)
{
    delay_us(us);
    keep_answering();
}

static void sleep_ms(unsigned ms)
{
    unsigned long hz = card.tick_hz != 0 ? card.tick_hz : 250ul;

    kosmos_sleep((ms * hz + 999ul) / 1000ul + 1ul);
    keep_answering();
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

/*-------------------------------------------------------------- saying */

static void say_word(struct say_line *line, const char *what, uint32_t v)
{
    say_text(line, what);
    say_hex(line, v, 8);
}

static void say_where(struct say_line *line, unsigned where)
{
    say_hex(line, (where >> 8) & 0xFFu, 2);
    say_text(line, ":");
    say_hex(line, (where >> 3) & 0x1Fu, 2);
    say_text(line, ".");
    say_dec(line, where & 0x7u);
}

static void say_mac(struct say_line *line, const uint8_t mac[6])
{
    unsigned i;

    for (i = 0; i < 6u; i++) {
        if (i != 0) {
            say_text(line, ":");
        }

        say_hex(line, mac[i], 2);
    }
}

/* The start of a numbered line, `tg3: N `; the caller adds and sends. */
static void step(struct say_line *line, unsigned n)
{
    say_begin(line);
    say_text(line, "tg3: ");
    say_dec(line, n);
    say_text(line, " ");
}

/* **A stop, said loudly**: the step it happened at and what was found, and
 * the chip let be from here. Always false, so a caller can return it. */
static bool stop_at(struct say_line *line, unsigned n, const char *why,
                    const char *what, uint32_t value)
{
    say_begin(line);
    say_text(line, "tg3: STOP at ");
    say_dec(line, n);
    say_text(line, ": ");
    say_text(line, why);

    if (what != NULL) {
        say_word(line, what, value);
    }

    say_send(console, line);
    card.stopped = true;
    card.present = false;
    return false;
}

/*-------------------------------------------------------------- the PHY */

/* `__tg3_readphy`. Auto-polling is turned off where it is found on and left
 * off (`tg3_mdio_start` does the same); a probe of a port this does not
 * drive puts MAC_MI_MODE back itself. */
static bool phy_read(unsigned reg, uint16_t *out)
{
    unsigned i;
    uint32_t v, mode = reg_read(MAC_MI_MODE);

    if (mode & MAC_MI_MODE_AUTO_POLL) {
        reg_write_f(MAC_MI_MODE, mode & ~(uint32_t)MAC_MI_MODE_AUTO_POLL);
        delay_us(80);
    }

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

/*----------------------------------------------------------- the ports */

/*
 * **Which of the two ports.** The Mac Pro has two of these chips, each a
 * device of its own; one process drives one, so this one drives the first
 * that has a cable in it, and the first if neither has. Each is asked
 * before anything is changed but auto-polling, which is put back.
 */
static bool probe_port(unsigned index, struct dev_info *dev, uintptr_t *regs,
                       struct tg3_chip *chip, bool *link_up,
                       struct say_line *line)
{
    long mapped;
    uintptr_t saved_regs = card.regs;
    uint32_t mode;
    struct tg3_link link;

    if (kosmos_dev_find(DEV_BROADCOM_ETHERNET, index, dev) != 0) {
        return false;
    }

    step(line, 1);
    say_text(line, "port ");
    say_dec(line, index);
    say_text(line, ": 14e4:");
    say_hex(line, dev->line, 4);
    say_text(line, " at ");
    say_where(line, dev->where);

    if (!tg3_decode_id(TG3_VENDOR_BROADCOM, (uint16_t)dev->line, chip)) {
        say_text(line, ", not of the 57765 class this drives");
        say_send(console, line);
        return false;
    }

    say_text(line, ", a BCM");
    say_dec(line, chip->number);

    mapped = kosmos_dev_map(dev->base, (dev->size + 4095u) / 4096u);

    if (mapped < 0) {
        say_text(line, "; its registers could not be mapped");
        say_send(console, line);
        return false;
    }

    *regs = (uintptr_t)mapped;
    card.regs = *regs;
    mode = reg_read(MAC_MI_MODE);
    link_read(&link);

    if (mode & MAC_MI_MODE_AUTO_POLL) {
        reg_write_f(MAC_MI_MODE, mode);
    }

    card.regs = saved_regs;
    *link_up = link.up;
    say_text(line, link.up ? ", a link" : ", no link");
    say_word(line, ", MI mode ", mode);
    say_send(console, line);
    return true;
}

/*---------------------------------------------------- the configuration */

static void read_config(void)
{
    unsigned i;

    for (i = 0; i < 64u; i++) {
        card.cfg[i] = config_read(card.index, i * 4u);
    }

    card.pcie_cap = tg3_pci_cap(card.cfg, PCI_CAP_ID_EXP);
    card.pm_cap = tg3_pci_cap(card.cfg, PCI_CAP_ID_PM);
    card.msi_cap = tg3_pci_cap(card.cfg, PCI_CAP_ID_MSI);
    card.msix_cap = tg3_pci_cap(card.cfg, PCI_CAP_ID_MSIX);
    card.command = card.cfg[PCI_COMMAND / 4u] & 0xFFFFu;
}

/* Which interrupt the board switched the chip to: MSI, MSI-X, or neither
 * (the line). Read from the configuration space now, not as found, since a
 * core-clock reset clears MSI's enable on some of this family's chips
 * (`tg3_restore_pci_state`, 5780_CLASS) and the log should say if it did. */
static const char *irq_kind_now(void)
{
    if (card.msi_cap != 0
        && ((config_read(card.index, card.msi_cap) >> 16) & PCI_MSI_FLAGS_ENABLE)) {
        return "MSI";
    }

    if (card.msix_cap != 0
        && ((config_read(card.index, card.msix_cap) >> 16) & PCI_MSIX_FLAGS_ENABLE)) {
        return "MSI-X";
    }

    return NULL;
}

/* MISC_HOST_CTRL as `tg3_get_invariants` writes it: by configuration
 * space, or through its mirror. */
static void write_misc_host_ctrl(void)
{
    if (!config_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl)) {
        reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl);
    }
}

/* `tg3_disable_ints`. */
static void disable_ints(void)
{
    reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl | MISC_HOST_CTRL_MASK_PCI_INT);
    mbox_write_f(MAILBOX_INTERRUPT_0, 1);
}

/* D0, which `tg3_power_up` asks for before anything else. The Mac's
 * firmware should leave it there; if it did not, the registers do not
 * answer until it is. */
static bool power_up(struct say_line *line)
{
    uint32_t pmcsr;

    if (card.pm_cap == 0) {
        return true;
    }

    pmcsr = config_read(card.index, card.pm_cap + PCI_PM_CTRL);

    if ((pmcsr & PCI_PM_CTRL_STATE_MASK) == 0) {
        return true;
    }

    if (!config_write(card.pm_cap + PCI_PM_CTRL,
                      pmcsr & ~(uint32_t)(PCI_PM_CTRL_STATE_MASK
                                          | PCI_PM_CTRL_PME_STATUS))) {
        return stop_at(line, 2, "the chip is not in D0, and this kernel "
                       "cannot write configuration space to wake it",
                       "; PMCSR ", pmcsr);
    }

    sleep_ms(10);                       /* D3hot to D0, PCI PM 1.2 5.6.1 */
    pmcsr = config_read(card.index, card.pm_cap + PCI_PM_CTRL);

    if ((pmcsr & PCI_PM_CTRL_STATE_MASK) != 0) {
        return stop_at(line, 2, "the chip would not come to D0", "; PMCSR ",
                       pmcsr);
    }

    return true;
}

/*------------------------------------------------------------- NVRAM */

/* `tg3_nvram_lock`: the arbitration the boot code also takes. */
static bool nvram_lock(void)
{
    unsigned i;

    reg_write(NVRAM_SWARB, SWARB_REQ_SET1);

    for (i = 0; i < 8000u; i++) {
        if (reg_read(NVRAM_SWARB) & SWARB_GNT1) {
            return true;
        }

        wait_us(20);
    }

    reg_write(NVRAM_SWARB, SWARB_REQ_CLR1);
    return false;
}

static void nvram_unlock(void)
{
    reg_write_f(NVRAM_SWARB, SWARB_REQ_CLR1);
}

/* `tg3_enable_nvram_access` and `_disable_`, for 5750_PLUS without
 * PROTECTED_NVRAM. */
static void nvram_access(bool on)
{
    uint32_t v = reg_read(NVRAM_ACCESS);

    reg_write(NVRAM_ACCESS, on ? v | ACCESS_ENABLE : v & ~(uint32_t)ACCESS_ENABLE);
}

/* `tg3_nvram_init` for the 57765 class: the serial EEPROM's state machine
 * reset, its automatic access on, and NVRAM_CFG1 read for what part it is
 * (`tg3_get_57780_nvram_info`, decoded in `tg3_decode.c`). */
static void nvram_init(struct say_line *line)
{
    uint32_t cfg1;

    memset(&card.nvram, 0, sizeof(card.nvram));
    card.nvram.part = "not read";

    reg_write_f(GRC_EEPROM_ADDR, EEPROM_ADDR_FSM_RESET
                                 | (EEPROM_DEFAULT_CLOCK_PERIOD
                                    << EEPROM_ADDR_CLKPERD_SHIFT));
    sleep_ms(1);
    reg_write_f(GRC_LOCAL_CTRL, reg_read(GRC_LOCAL_CTRL) | GRC_LCLCTRL_AUTO_SEEPROM);
    delay_us(100);

    if (!nvram_lock()) {
        step(line, 3);
        say_text(line, "NVRAM: its lock was not granted; read as none");
        say_send(console, line);
        return;
    }

    nvram_access(true);
    cfg1 = reg_read(NVRAM_CFG1);
    tg3_decode_nvram_cfg1(cfg1, &card.nvram);

    if (card.nvram.cfg1_write != cfg1) {
        reg_write(NVRAM_CFG1, card.nvram.cfg1_write);
    }

    nvram_access(false);
    nvram_unlock();

    step(line, 3);
    say_text(line, "NVRAM: ");
    say_text(line, card.nvram.part);
    say_word(line, ", CFG1 ", cfg1);

    if (card.nvram.page_bytes != 0) {
        say_text(line, ", pages of ");
        say_dec(line, card.nvram.page_bytes);
    }

    if (card.nvram.translate) {
        say_text(line, ", addresses translated");
    }

    say_text(line, tg3_wants_eee_patch(&card.rev, card.nvram.present)
                   ? "; Linux would load tg357766.bin, the EEE patch: not "
                     "loaded, EEE off"
                   : "; no firmware wanted");
    say_send(console, line);
}

/* `tg3_nvram_read`: one word, at an offset Atmel's pages may translate. */
static bool nvram_read(uint32_t offset, uint32_t *out)
{
    uint32_t phys = tg3_nvram_phys_addr(&card.nvram, offset);
    bool done = false;
    unsigned i;

    if (!card.nvram.present || phys > NVRAM_ADDR_MSK || !nvram_lock()) {
        return false;
    }

    nvram_access(true);
    reg_write(NVRAM_ADDR, phys);
    reg_write(NVRAM_CMD, tg3_nvram_read_cmd());

    for (i = 0; i < NVRAM_CMD_TIMEOUT; i++) {
        wait_us(10);

        if (tg3_nvram_done(reg_read(NVRAM_CMD))) {
            delay_us(10);
            done = true;
            break;
        }
    }

    if (done) {
        *out = reg_read(NVRAM_RDDATA);
    }

    nvram_access(false);
    nvram_unlock();
    return done;
}

/*
 * `tg3_get_device_address`, in its order, before the reset as Linux reads
 * it: the boot code's mailbox (its 0x484B signature), then NVRAM at 0x7C,
 * then MAC_ADDR_0 as the boot code left it. No device-tree or ACPI property
 * is asked, which is Linux's first stop and the open question on a Mac.
 *
 * NVRAM's two words pack the address as MAC_ADDR_0 does: `tg3_nvram_read_
 * be32` byte-swaps the word, and Linux then takes bytes 2-3 of the first and
 * all of the second, which are the low half of the first word high byte
 * first and the second word high byte first - `tg3_decode_mac_regs`.
 * Linux stops at NVRAM when both reads succeed even if the address is no
 * good; this goes on to the registers, which can only help.
 */
static void find_mac(void)
{
    uint32_t hi, lo;

    card.have_mac = tg3_decode_mac_mbox(sram_read(NIC_SRAM_MAC_ADDR_HIGH_MBOX),
                                        sram_read(NIC_SRAM_MAC_ADDR_LOW_MBOX),
                                        card.mac)
                    && tg3_mac_valid(card.mac);
    card.mac_from = "the boot code's mailbox";

    if (!card.have_mac && nvram_read(TG3_NVRAM_MAC_AT, &hi)
        && nvram_read(TG3_NVRAM_MAC_AT + 4u, &lo)) {
        card.have_mac = tg3_decode_mac_regs(hi, lo, card.mac)
                        && tg3_mac_valid(card.mac);
        card.mac_from = "NVRAM";
    }

    if (!card.have_mac) {
        card.have_mac = tg3_decode_mac_regs(reg_read(MAC_ADDR_0_HIGH),
                                            reg_read(MAC_ADDR_0_LOW), card.mac)
                        && tg3_mac_valid(card.mac);
        card.mac_from = "MAC_ADDR_0";
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

/*------------------------------------------------------------ the halt */

/* `tg3_stop_block`: the enable cleared, and up to a tenth of a second for
 * the block to say it stopped. False if it did not. */
static bool stop_block(unsigned at, uint32_t enable)
{
    unsigned i;

    reg_write_f(at, reg_read(at) & ~enable);

    for (i = 0; i < MAX_WAIT_CNT; i++) {
        wait_us(100);

        if ((reg_read(at) & enable) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * `tg3_abort_hw`, for a chip the Mac's firmware left running (`tg3_init_one`
 * halts one whose host coalescing or write DMA is on, since a DMA in flight
 * across the reset lands wherever its old rings were). Linux calls it
 * silently there, and so the blocks that would not stop are named and the
 * reset goes ahead anyway. RCVLSC, DMAC, MBFREE, BUFMGR and MEMARB are not
 * stopped on 5705_PLUS (`tg3_stop_block`).
 */
static void abort_hw(struct say_line *line)
{
    static const struct { unsigned at; uint32_t bit; const char *name; } order[] = {
        { RCVBDI_MODE,   RCVBDI_MODE_ENABLE,   "RCVBDI" },
        { RCVLPC_MODE,   RCVLPC_MODE_ENABLE,   "RCVLPC" },
        { RCVDBDI_MODE,  RCVDBDI_MODE_ENABLE,  "RCVDBDI" },
        { RCVDCC_MODE,   RCVDCC_MODE_ENABLE,   "RCVDCC" },
        { RCVCC_MODE,    RCVCC_MODE_ENABLE,    "RCVCC" },
        { SNDBDS_MODE,   SNDBDS_MODE_ENABLE,   "SNDBDS" },
        { SNDBDI_MODE,   SNDBDI_MODE_ENABLE,   "SNDBDI" },
        { SNDDATAI_MODE, SNDDATAI_MODE_ENABLE, "SNDDATAI" },
        { RDMAC_MODE,    RDMAC_MODE_ENABLE,    "RDMAC" },
        { SNDDATAC_MODE, SNDDATAC_MODE_ENABLE, "SNDDATAC" },
        { SNDBDC_MODE,   SNDBDC_MODE_ENABLE,   "SNDBDC" },
    };
    unsigned i, stuck = 0;

    step(line, 5);
    say_word(line, "the firmware left it running (HOSTCC ", reg_read(HOSTCC_MODE));
    say_word(line, ", WDMAC ", reg_read(WDMAC_MODE));
    say_text(line, "); halting");

    reg_write(MEMARB_MODE, MEMARB_MODE_ENABLE);
    reg_write_f(MAC_RX_MODE, reg_read(MAC_RX_MODE) & ~(uint32_t)RX_MODE_ENABLE);
    delay_us(10);

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (!stop_block(order[i].at, order[i].bit)) {
            say_text(line, stuck++ == 0 ? "; would not stop: " : ", ");
            say_text(line, order[i].name);
        }
    }

    reg_write_f(MAC_MODE, reg_read(MAC_MODE) & ~(uint32_t)MAC_MODE_TDE_ENABLE);
    delay_us(40);
    reg_write_f(MAC_TX_MODE, reg_read(MAC_TX_MODE) & ~(uint32_t)TX_MODE_ENABLE);

    for (i = 0; i < MAX_WAIT_CNT; i++) {
        wait_us(100);

        if ((reg_read(MAC_TX_MODE) & TX_MODE_ENABLE) == 0) {
            break;
        }
    }

    if (i == MAX_WAIT_CNT) {
        say_text(line, stuck++ == 0 ? "; would not stop: " : ", ");
        say_text(line, "MAC_TX");
    }

    if (!stop_block(HOSTCC_MODE, HOSTCC_MODE_ENABLE)) {
        say_text(line, stuck++ == 0 ? "; would not stop: " : ", ");
        say_text(line, "HOSTCC");
    }

    if (!stop_block(WDMAC_MODE, WDMAC_MODE_ENABLE)) {
        say_text(line, stuck++ == 0 ? "; would not stop: " : ", ");
        say_text(line, "WDMAC");
    }

    reg_write(FTQ_RESET, 0xFFFFFFFFu);
    reg_write(FTQ_RESET, 0);

    say_text(line, stuck == 0 ? "; every block stopped" : "; resetting anyway");
    say_send(console, line);
}

/*------------------------------------------------------------- the reset */

/* `tg3_poll_fw`: the boot code answers the driver's MAGIC1 with its
 * complement when it has finished. Up to a second (100000 times 10 us),
 * and **not an error when it does not**: "Chip might not be fitted with
 * firmware", Linux says, and reports "No firmware running" once. */
static bool poll_fw(void)
{
    unsigned i;
    bool ready = false;

    for (i = 0; i < 100000u; i++) {
        if (tg3_fw_ready(sram_read(NIC_SRAM_FIRMWARE_MBOX))) {
            ready = true;
            break;
        }

        wait_us(10);
    }

    if (card.rev.a0) {
        sleep_ms(10);                   /* "The 57765 A0 needs a little more time" */
    }

    return ready;
}

/*
 * `tg3_chip_reset` for the 57765 class, in its order.
 *
 * **The core-clock reset clears memory decoding.** Linux: "GRC_MISC_CFG core
 * clock reset will clear the memory enable bit in PCI register 4". It puts
 * it back through configuration space (`tg3_restore_pci_state`), with
 * MISC_HOST_CTRL and PCISTATE, after clearing PCIe Device Control's relaxed
 * ordering and no-snoop and Device Status's errors. So does this, through
 * SYS_DEV_CONFIG_SET. Without it the MMIO mirror is tried and the command
 * register read, and a cleared memory enable is a STOP: nothing after it
 * would reach the chip.
 *
 * No NVRAM unlock follows the lock: "chip reset below will undo the nvram
 * lock". KEEP_GPHY_POWER is not set: Linux sets it only without a CPMU.
 */
static bool chip_reset(struct say_line *line)
{
    uint32_t command;

    (void)nvram_lock();

    /* The status block forgotten, as Linux zeroes `hw_status` here. */
    memset((void *)(card.mem + STATUS_AT), 0, TG3_STATUS_BYTES);
    card.last_tag = 0;

    reg_write(GRC_FASTBOOT_PC, 0);

    /* PCI Express and not 5750 A0: bit 29 first, then the reset with it. */
    reg_write(GRC_MISC_CFG, 1u << 29);
    reg_write(GRC_MISC_CFG, GRC_MISC_CFG_CORECLK_RESET | (1u << 29));

    delay_us(120);
    (void)config_read(card.index, PCI_COMMAND);     /* the posted write pushed */
    delay_us(120);
    sleep_ms(1);

    command = config_read(card.index, PCI_COMMAND) & 0xFFFFu;

    step(line, 6);
    say_word(line, "reset; command register ", command);

    if (card.config_writes) {
        uint32_t want = (card.command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)
                        & ~(uint32_t)PCI_COMMAND_IO;

        if (card.pcie_cap != 0) {
            (void)config_write(card.pcie_cap + PCI_EXP_DEVCTL,
                               tg3_pcie_devctl_after_reset(
                                   config_read(card.index,
                                               card.pcie_cap + PCI_EXP_DEVCTL)));
        }

        (void)config_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl);
        (void)config_write(TG3PCI_PCISTATE, PCISTATE_ROM_ENABLE
                                            | PCISTATE_ROM_RETRY_ENABLE);

        /* The status half written as 0, which clears nothing (RW1C). */
        if (!config_write(PCI_COMMAND, want)) {
            say_text(line, "; the kernel refused it back");
        }

        command = config_read(card.index, PCI_COMMAND) & 0xFFFFu;
        say_word(line, ", put back as ", command);
    } else if (tg3_pci_command_ok(command)) {
        reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl);
        reg_write(TG3PCI_PCISTATE, PCISTATE_ROM_ENABLE | PCISTATE_ROM_RETRY_ENABLE);
        say_text(line, ", which survived; restored by the MMIO mirror");
    }

    say_send(console, line);

    if (!tg3_pci_command_ok(command)) {
        return stop_at(line, 6, card.config_writes
                       ? "memory or bus master is off after the reset"
                       : "the reset cleared memory decoding, and this kernel "
                         "has no SYS_DEV_CONFIG_SET to put it back",
                       "; command ", command);
    }

    /* MEMARB_MODE, plainly: `val` is read first only on 5780_CLASS. */
    reg_write(MEMARB_MODE, MEMARB_MODE_ENABLE);

    step(line, 6);

    if (poll_fw()) {
        say_text(line, "the boot code answered");
    } else {
        say_word(line, "no firmware running (Linux goes on too); its mailbox ",
                 sram_read(NIC_SRAM_FIRMWARE_MBOX));
    }

    reg_write(GRC_MODE, card.grc_mode);

    /* MAC_MODE 0 for a copper PHY, then `tg3_mdio_start`'s MDIO clock. */
    card.mac_mode = 0;
    reg_write_f(MAC_MODE, 0);
    delay_us(40);
    reg_write_f(MAC_MI_MODE, MAC_MI_MODE_500KHZ_CONST);
    delay_us(80);

    /* "Reprobe ASF enable state." */
    card.asf = sram_read(NIC_SRAM_DATA_SIG) == NIC_SRAM_DATA_SIG_MAGIC
               && (sram_read(NIC_SRAM_DATA_CFG) & NIC_SRAM_DATA_CFG_ASF_ENABLE);

    if (card.asf) {
        say_text(line, "; ASF is on, and this does not speak to it");
    }

    say_send(console, line);
    return true;
}

static void bdinfo_sram(unsigned sram, uint64_t host, uint32_t maxlen_flags)
{
    sram_write(sram + TG3_BDINFO_HOST_ADDR, (uint32_t)(host >> 32));
    sram_write(sram + TG3_BDINFO_HOST_ADDR + 4u, (uint32_t)host);
    sram_write(sram + TG3_BDINFO_MAXLEN_FLAGS, maxlen_flags);
}

/* `tg3_rx_prodring_alloc`'s posting, in host memory: every ring empty and
 * 64 receive buffers given. The chip is told in `reset_hw`. */
static void post_buffers(void)
{
    unsigned i;

    memset((void *)(card.mem + TX_DESC_AT), 0, (size_t)(RX_FRAMES_AT - TX_DESC_AT));

    for (i = 0; i < BUFFERS; i++) {
        tg3_put_rx_std(std_desc(i), card.bus + RX_FRAMES_AT + i * FRAME_SLOT,
                       i, RX_BUFFER_BYTES);
    }

    card.std_prod = BUFFERS;
    card.ret_next = 0;
    card.tx.slots = TX_RING;
    card.tx.limit = BUFFERS - 1u;
    card.tx.next = 0;
    card.tx.clean = 0;
    RING_BARRIER();
}

/* `tg3_rings_reset`: every send and return control block disabled, the
 * mailboxes zeroed and the interrupt masked, the status block cleared and
 * its address given, then ring 0 of each set up. */
static void rings_reset(void)
{
    unsigned i;

    for (i = 0; i < SEND_RCBS; i++) {
        sram_write(NIC_SRAM_SEND_RCB + i * TG3_BDINFO_SIZE + TG3_BDINFO_MAXLEN_FLAGS,
                   BDINFO_FLAGS_DISABLED);
    }

    for (i = 0; i < RETURN_RCBS; i++) {
        sram_write(NIC_SRAM_RCV_RET_RCB + i * TG3_BDINFO_SIZE
                   + TG3_BDINFO_MAXLEN_FLAGS, BDINFO_FLAGS_DISABLED);
    }

    mbox_write_f(MAILBOX_INTERRUPT_0, 1);
    mbox_write(MAILBOX_SNDHOST_PROD_IDX_0, 0);
    mbox_write(MAILBOX_RCVRET_CON_IDX_0, 0);

    memset((void *)(card.mem + STATUS_AT), 0, TG3_STATUS_BYTES);
    RING_BARRIER();
    reg_write(HOSTCC_STATUS_BLK_HOST_ADDR, (uint32_t)((card.bus + STATUS_AT) >> 32));
    reg_write(HOSTCC_STATUS_BLK_HOST_ADDR + 4u, (uint32_t)(card.bus + STATUS_AT));

    /* NIC_ADDR is not written on 5705_PLUS (`tg3_set_bdinfo`). */
    bdinfo_sram(NIC_SRAM_SEND_RCB, card.bus + TX_DESC_AT, tg3_bdinfo_ring(TX_RING));
    bdinfo_sram(NIC_SRAM_RCV_RET_RCB, card.bus + RET_DESC_AT,
                tg3_bdinfo_ring(RET_RING));
    card.last_tag = 0;
}

/*
 * `tg3_reset_hw` after the reset, the parts the 57765 class takes, in
 * Linux's order. Left out, and said: EEE (off, no patch), TSO's firmware
 * (none on HW_TSO_3), jumbo frames, RSS, the statistics block's DMA, and
 * MAC_LED_CTRL, which Linux writes from NVRAM's configuration and this
 * leaves as the boot code set it.
 */
static bool reset_hw(struct say_line *line)
{
    uint32_t v;
    unsigned i;

    /* A 57765 A0 also has a PCIe PL-block step here (TG3_PCIE_PL_LO_PHYCTL5
     * DIS_L2CLKREQ) that this leaves out; the log says the revision.
     *
     * Not AX (`tg3_decode_rev`): the transmit hang fix. */
    if (!card.rev.ax) {
        reg_write(TG3_CPMU_PADRNG_CTL,
                  reg_read(TG3_CPMU_PADRNG_CTL) | TG3_CPMU_PADRNG_CTL_RDIV2);

        v = reg_read(GRC_MODE);
        reg_write(GRC_MODE, (v & ~GRC_MODE_PCIE_PORT_BITS) | GRC_MODE_PCIE_DL_SEL);
        reg_write(TG3_PCIE_TLDLPL_PORT + TG3_PCIE_DL_LO_FTSMAX,
                  (reg_read(TG3_PCIE_TLDLPL_PORT + TG3_PCIE_DL_LO_FTSMAX)
                   & ~(uint32_t)TG3_PCIE_DL_LO_FTSMAX_MSK) | TG3_PCIE_DL_LO_FTSMAX_VAL);
        reg_write(GRC_MODE, v);
    }

    reg_write(TG3_CPMU_LSPD_10MB_CLK,
              (reg_read(TG3_CPMU_LSPD_10MB_CLK) & ~(uint32_t)CPMU_LSPD_10MB_MACCLK_MASK)
              | CPMU_LSPD_10MB_MACCLK_6_25);

    /* `tg3_calc_dma_bndry` gives 57765_PLUS only DIS_CACHE_ALIGNMENT; A0
     * also has its read-request size cleared. */
    v = reg_read(TG3PCI_DMA_RW_CTRL) | DMA_RWCTRL_DIS_CACHE_ALIGNMENT;

    if (card.rev.a0) {
        v &= ~(uint32_t)DMA_RWCTRL_CRDRDR_RDMA_MRRS_MSK;
    }

    reg_write(TG3PCI_DMA_RW_CTRL, v);

    reg_write(GRC_MODE, card.grc_mode | GRC_MODE_HOST_SENDBDS
                        | GRC_MODE_NO_TX_PHDR_CSUM | GRC_MODE_IRQ_ON_MAC_ATTN
                        | GRC_MODE_HOST_STACKUP);

    /* A 66 MHz clock to a 1 MHz timer: `val &= ~0xff`, prescaler 65. */
    reg_write(GRC_MISC_CFG, (reg_read(GRC_MISC_CFG) & ~0xFFu)
                            | (65u << GRC_MISC_CFG_PRESCALAR_SHIFT));

    /* The buffer manager's watermarks for 57765_PLUS (DEFAULT_MB_*_57765),
     * and the manager on. */
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

        wait_us(10);
    }

    if (i == 2000u) {
        return stop_at(line, 7, "the buffer manager did not start", "; BUFMGR_MODE ",
                       reg_read(BUFMGR_MODE));
    }

    /* `tg3_setup_rxbd_thresholds`: min(bdcache / 2 = 64, max(posted / 8, 1)),
     * and the low water mark the whole cache, 128, on 57765_PLUS. */
    reg_write(RCVBDI_STD_THRESH, BUFFERS / 8u);
    reg_write(STD_REPLENISH_LWM, 128u);

    /* The standard ring's control block, by MMIO, high word first; NIC_ADDR
     * for every chip that is not 5717_PLUS. The jumbo ring disabled. Then
     * its producer, before `tg3_rings_reset`, in Linux's order. */
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_HOST_ADDR,
              (uint32_t)((card.bus + STD_DESC_AT) >> 32));
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_HOST_ADDR + 4u,
              (uint32_t)(card.bus + STD_DESC_AT));
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_NIC_ADDR, NIC_SRAM_RX_BUFFER_DESC);
    reg_write(RCVDBDI_STD_BD + TG3_BDINFO_MAXLEN_FLAGS,
              tg3_bdinfo_std(STD_RING, RX_BUFFER_BYTES));
    reg_write(RCVDBDI_JUMBO_BD + TG3_BDINFO_MAXLEN_FLAGS, BDINFO_FLAGS_DISABLED);

    mbox_write(MAILBOX_RCV_STD_PROD_IDX, card.std_prod);
    mbox_write(MAILBOX_RCV_JUMBO_PROD_IDX, 0);

    rings_reset();
    set_mac();

    reg_write(MAC_RX_MTU_SIZE, 14u + 1500u + 4u + 4u);  /* ETH_HLEN, mtu, FCS, VLAN */
    reg_write(MAC_TX_LENGTHS, (2u << 12) | (6u << 8) | 32u);
    reg_write(MAC_RCV_RULE_CFG, RCV_RULE_CFG_DEFAULT_CLASS);
    reg_write(RCVLPC_CONFIG, 0x181u);

    reg_write(TG3_RDMA_RSRVCTRL_REG,
              reg_read(TG3_RDMA_RSRVCTRL_REG) | TG3_RDMA_RSRVCTRL_FIFO_OFLW_FIX);

    reg_write(RCVLPC_STATS_ENABLE,
              reg_read(RCVLPC_STATS_ENABLE) & ~(uint32_t)RCVLPC_STATSENAB_DACK_FIX);
    reg_write(RCVLPC_STATSCTRL, 1u);
    reg_write(SNDDATAI_STATSENAB, 0xFFFFFFu);
    reg_write(SNDDATAI_STATSCTRL, 3u);

    /* Coalescing off while it is set, then on: Linux's defaults with tagged
     * status (`tg3_init_coal`, the CLRTICKS figures). */
    reg_write(HOSTCC_MODE, 0);

    for (i = 0; i < 2000u && (reg_read(HOSTCC_MODE) & HOSTCC_MODE_ENABLE); i++) {
        wait_us(10);
    }

    reg_write(HOSTCC_RXCOL_TICKS, 0x14u);
    reg_write(HOSTCC_TXCOL_TICKS, 0x48u);
    reg_write(HOSTCC_RXMAX_FRAMES, 5u);
    reg_write(HOSTCC_TXMAX_FRAMES, 0x35u);
    reg_write(HOSTCC_RXCOAL_MAXF_INT, 5u);
    reg_write(HOSTCC_TXCOAL_MAXF_INT, 5u);
    reg_write(HOSTCC_MODE, HOSTCC_MODE_ENABLE | HOSTCC_MODE_32BYTE
                           | HOSTCC_MODE_CLRTICK_RXBD | HOSTCC_MODE_CLRTICK_TXBD);

    reg_write(RCVCC_MODE, 6u);                  /* ENABLE | ATTN_ENABLE */
    reg_write(RCVLPC_MODE, 2u);

    card.mac_mode = MAC_MODE_TXSTAT_ENABLE | MAC_MODE_RXSTAT_ENABLE
                    | MAC_MODE_TDE_ENABLE | MAC_MODE_RDE_ENABLE
                    | MAC_MODE_FHDE_ENABLE | MAC_MODE_PORT_MODE_GMII;
    reg_write_f(MAC_MODE, card.mac_mode | MAC_MODE_RXSTAT_CLEAR
                          | MAC_MODE_TXSTAT_CLEAR);
    delay_us(40);

    /* GRC_LOCAL_CTRL with the GPIO bits as the boot code set them, and no
     * others (`tg3_grc_local_ctrl`; the draft kept 0xFF00, which is not the
     * GPIO mask). */
    reg_write_f(GRC_LOCAL_CTRL, tg3_grc_local_ctrl(reg_read(GRC_LOCAL_CTRL)));
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
    reg_write(SNDDATAI_MODE, 2u | 0x8u);        /* bit 3: Linux's, for HW_TSO_3 */
    reg_write(SNDBDI_MODE, 6u);
    reg_write(SNDBDS_MODE, 6u);

    reg_write_f(MAC_TX_MODE, TX_MODE_ENABLE | TX_MODE_MBUF_LOCKUP_FIX);
    delay_us(100);

    /* Linux's receive mode, with the tag kept as `__tg3_set_rx_mode` keeps
     * it when there is no VLAN support and no ASF: this stack knows no
     * VLANs, so a tagged frame should arrive whole and be refused there,
     * not arrive stripped and be taken for an untagged one. */
    reg_write_f(MAC_RX_MODE, RX_MODE_ENABLE | RX_MODE_IPV6_CSUM_ENABLE
                             | (card.asf ? 0u : RX_MODE_KEEP_VLAN_TAG));
    delay_us(10);

    reg_write_f(MAC_MI_STAT, MAC_MI_STAT_LNKSTAT_ATTN_ENAB);
    reg_write(MAC_LOW_WMARK_MAX_RX_FRAME, 1u);

    /* Every multicast, as `__tg3_set_rx_mode` does with IFF_ALLMULTI; the
     * stack filters. */
    for (i = 0; i < 4u; i++) {
        reg_write(MAC_HASH_REG_0 + i * 4u, 0xFFFFFFFFu);
    }

    /* The receive rules `tg3_reset_hw` writes, and the rest of the eight
     * this class has cleared. */
    reg_write(MAC_RCV_RULE_0, 0xC2000000u & 0x7FFFFFFFu);
    reg_write(MAC_RCV_VALUE_0, 0xFFFFFFFFu & 0x7FFFFFFFu);
    reg_write(MAC_RCV_RULE_0 + 8u, 0x86000004u & 0x7FFFFFFFu);
    reg_write(MAC_RCV_VALUE_0 + 8u, 0xFFFFFFFFu & 0x7FFFFFFFu);

    for (i = 4; i < 8u; i++) {
        reg_write(MAC_RCV_RULE_0 + i * 8u, 0);
        reg_write(MAC_RCV_VALUE_0 + i * 8u, 0);
    }

    step(line, 7);
    say_text(line, "rings: send 512, standard 512 with 64 posted, return 1024");
    say_word(line, "; host memory at ", (uint32_t)(card.bus >> 32));
    say_hex(line, (uint32_t)card.bus, 8);
    say_send(console, line);
    return true;
}

/*
 * The PHY as `tg3_setup_copper_phy` begins with it: its interrupts cleared
 * (ISTAT read twice) and masked (IMASK ~0, not FET and not MI_INTERRUPT -
 * the MAC watches the link through MI_STAT), and if there is no link yet,
 * what it may offer written and negotiation restarted - without waiting
 * for it, which arrives as a link change in the status block.
 */
static void phy_start(struct say_line *line)
{
    uint16_t id1 = 0, id2 = 0, scratch = 0;

    if (phy_read(MII_PHYSID1, &id1) && phy_read(MII_PHYSID2, &id2)) {
        card.phy_id = tg3_phy_id(id1, id2);
    }

    (void)phy_read(MII_TG3_ISTAT, &scratch);
    (void)phy_read(MII_TG3_ISTAT, &scratch);
    (void)phy_write(MII_TG3_IMASK, 0xFFFFu);

    link_read(&card.link);

    step(line, 8);
    say_word(line, "PHY ", card.phy_id);
    say_text(line, tg3_phy_is_57765(card.phy_id) ? " (the BCM57765's)"
                                                 : " (not the BCM57765's)");

    if (card.link.up) {
        say_text(line, "; a link already");
    } else {
        (void)phy_write(MII_ADVERTISE, tg3_mii_advertise());
        (void)phy_write(MII_CTRL1000, tg3_mii_ctrl1000(card.chip.fast_only));
        (void)phy_write(MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
        say_text(line, "; no link; negotiating 10/100");
        say_text(line, card.chip.fast_only ? "" : "/1000");
        say_text(line, ", no pause");
    }

    say_send(console, line);
    link_apply();
}

/* `tg3_enable_ints`, with tagged status: unmasked, the mailbox written with
 * the last tag (twice, for one-shot MSI), and an interrupt forced. */
static void enable_ints(void)
{
    reg_write(TG3PCI_MISC_HOST_CTRL, card.misc_host_ctrl & ~(uint32_t)MISC_HOST_CTRL_MASK_PCI_INT);
    mbox_write_f(MAILBOX_INTERRUPT_0, tg3_int_mbox_rearm(card.last_tag));
    mbox_write_f(MAILBOX_INTERRUPT_0, tg3_int_mbox_rearm(card.last_tag));
    reg_write(HOSTCC_MODE, HOSTCC_MODE_ENABLE | HOSTCC_MODE_32BYTE
                           | HOSTCC_MODE_CLRTICK_RXBD | HOSTCC_MODE_CLRTICK_TXBD
                           | HOSTCC_MODE_NOW);
}

/*------------------------------------------------------------ bring-up */

/* The chip chosen, mapped and named, or false if there is none here. */
static bool choose(struct say_line *line, struct dev_info *out)
{
    struct dev_info dev[PORTS];
    uintptr_t regs[PORTS] = { 0 };
    struct tg3_chip chip[PORTS];
    bool usable[PORTS] = { false }, link[PORTS] = { false };
    unsigned i, pick = PORTS;

    for (i = 0; i < PORTS; i++) {
        usable[i] = probe_port(i, &dev[i], &regs[i], &chip[i], &link[i], line);

        if (usable[i] && (pick == PORTS || (link[i] && !link[pick]))) {
            pick = i;
        }
    }

    for (i = 0; i < PORTS; i++) {
        if (regs[i] != 0 && i != pick) {
            (void)kosmos_unmap(regs[i], (dev[i].size + 4095u) / 4096u);
        }
    }

    if (pick == PORTS) {
        return false;
    }

    card.index = pick;
    card.regs = regs[pick];
    card.chip = chip[pick];
    card.where = dev[pick].where;
    card.device = dev[pick].line;
    card.intid = dev[pick].intid;
    *out = dev[pick];
    (void)line;
    return true;
}

static bool bring_up(struct say_line *line)
{
    long region, mapped, bus;
    uint32_t prodid;

    read_config();

#ifdef SYS_DEV_CONFIG_SET
    card.config_writes = true;
#endif

    /* The revision: MISC_HOST_CTRL's top half, or the product register it
     * points at (`tg3_detect_asic_rev`). */
    prodid = config_read(card.index, card.chip.prodid_at);
    tg3_decode_rev(card.cfg[TG3PCI_MISC_HOST_CTRL / 4u], prodid, &card.rev);

    step(line, 2);
    say_text(line, "driving port ");
    say_dec(line, card.index);
    say_word(line, ", revision ", card.rev.id);
    say_word(line, ", command ", card.command);
    say_text(line, card.pcie_cap != 0 ? ", PCIe" : ", not PCIe?");
    say_text(line, card.config_writes ? ", config writes" : ", no config writes");
    say_send(console, line);

    if (!card.rev.class_57765) {
        return stop_at(line, 2, "the revision is not of the 57765 class",
                       "; MISC_HOST_CTRL ", card.cfg[TG3PCI_MISC_HOST_CTRL / 4u]);
    }

    if (!tg3_pci_command_ok(card.command)) {
        return stop_at(line, 2, "the board left memory or bus master off",
                       "; command ", card.command);
    }

    if (!power_up(line)) {
        return false;
    }

    card.misc_host_ctrl = MISC_HOST_CTRL_MASK_PCI_INT | MISC_HOST_CTRL_WORD_SWAP
                          | MISC_HOST_CTRL_INDIR_ACCESS
                          | MISC_HOST_CTRL_PCISTATE_RW
                          | MISC_HOST_CTRL_TAGGED_STATUS;
    card.grc_mode = GRC_MODE_WSWAP_DATA | GRC_MODE_BSWAP_DATA
                    | GRC_MODE_WSWAP_NONFRM_DATA;

    /* `tg3_get_invariants`: MISC_HOST_CTRL, then quiet, then the memory
     * arbiter on for SRAM ("other entities such as system netboot code
     * might disable it"), then the MDIO clock (`tg3_mdio_start`). */
    write_misc_host_ctrl();
    disable_ints();
    reg_write(MEMARB_MODE, reg_read(MEMARB_MODE) | MEMARB_MODE_ENABLE);
    reg_write_f(MAC_MI_MODE, MAC_MI_MODE_500KHZ_CONST);
    delay_us(80);

    region = kosmos_mem_create_flags(REGION_PAGES, MEM_CONTIGUOUS);
    mapped = region < 0 ? region : kosmos_mem_map(region);
    bus = mapped < 0 ? mapped : kosmos_mem_phys(region);

    if (region < 0 || mapped < 0 || bus <= 0) {
        return stop_at(line, 2, "no memory the chip can reach for its rings",
                       NULL, 0);
    }

    card.mem = (uintptr_t)mapped;
    card.bus = (uint64_t)bus;

    nvram_init(line);
    find_mac();

    step(line, 4);

    if (!card.have_mac) {
        say_text(line, "no address in the mailbox, NVRAM or MAC_ADDR_0");
        say_send(console, line);
        return stop_at(line, 4, "the chip has no address of its own", NULL, 0);
    }

    say_text(line, "MAC ");
    say_mac(line, card.mac);
    say_text(line, " from ");
    say_text(line, card.mac_from);

    if (tg3_mac_is_default(card.mac)) {
        say_text(line, " (Broadcom's default; nobody set it)");
    }

    say_send(console, line);

    if (tg3_chip_busy(reg_read(HOSTCC_MODE), reg_read(WDMAC_MODE))) {
        sram_write(NIC_SRAM_FIRMWARE_MBOX, NIC_SRAM_FIRMWARE_MBOX_MAGIC1);
        abort_hw(line);
    } else {
        step(line, 5);
        say_text(line, "idle as found");
        say_send(console, line);
    }

    /* `tg3_reset_hw` from the top: quiet, the signature the boot code will
     * complement, the reset. */
    disable_ints();
    sram_write(NIC_SRAM_FIRMWARE_MBOX, NIC_SRAM_FIRMWARE_MBOX_MAGIC1);
    post_buffers();

    if (!chip_reset(line)) {
        return false;
    }

    if (!reset_hw(line)) {
        return false;
    }

    phy_start(line);

    /* MSGINT_MODE with MSI or MSI-X, one-shot left on (1SHOT_MSI is set for
     * 5755_PLUS, so ONE_SHOT_DISABLE stays clear). */
    card.irq_kind = irq_kind_now();

    if (card.irq_kind != NULL) {
        reg_write(MSGINT_MODE, reg_read(MSGINT_MODE) | MSGINT_MODE_ENABLE);
    }

    card.irq = kosmos_irq_claim(card.intid);

    step(line, 9);
    say_text(line, "interrupt ");
    say_dec(line, card.intid);
    say_text(line, card.irq_kind != NULL ? " by " : " on its line");

    if (card.irq_kind != NULL) {
        say_text(line, card.irq_kind);
    } else if (card.msi_cap != 0 || card.msix_cap != 0) {
        say_text(line, " (it has MSI, and it is off)");
    }

    say_text(line, card.irq >= 0 ? ", claimed" : ", not claimed: polled");
    say_send(console, line);

    card.link_polled_at = kosmos_ticks();
    card.present = true;
    enable_ints();

    step(line, 10);
    say_text(line, "ready: answering the network stack as ");
    say_mac(line, card.mac);
    say_send(console, line);
    return true;
}

/*----------------------------------------------------------------- frames */

static void tx_reclaim(struct say_line *line)
{
    struct tg3_status st;
    unsigned done;

    RING_BARRIER();
    tg3_decode_status((const uint8_t *)(card.mem + STATUS_AT), &st);
    done = tg3_tx_reclaim(&card.tx, st.tx_consumer);
    card.sent += done;

    if (done > 0 && !card.said_out && line != NULL) {
        card.said_out = true;
        say_begin(line);
        say_text(line, "tg3: the first frame out was sent by the chip");
        say_send(console, line);
    }
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

    tx_reclaim(NULL);

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
static void take_frames(const struct tg3_status *st, struct say_line *line)
{
    unsigned taken = 0, posted = 0;

    RING_BARRIER();

    while (tg3_ring_ready(st->return_producer, card.ret_next, RET_RING) > 0) {
        struct tg3_rx got;
        unsigned buffer;

        tg3_decode_rx(ret_desc(card.ret_next), &got);
        buffer = got.index % BUFFERS;

        if (!card.said_in) {
            card.said_in = true;
            say_begin(line);
            say_text(line, "tg3: the first frame in, ");
            say_dec(line, got.length);
            say_text(line, got.error ? " bytes, with an error"
                                     : got.std ? " bytes" : " bytes, not standard");
            say_send(console, line);
        }

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
    say_text(line, "tg3: link ");

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

    if (rep.error == ETH_OK && !card.present) {
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

    card.answered_at = kosmos_ticks();
}

/* From inside bring-up's waits: the stack answered "no adapter" if it has
 * waited more than about five milliseconds. Its own line, so a numbered
 * line being composed is not overwritten. */
static void keep_answering(void)
{
    static struct say_line own;

    if (card.hz != 0 && kosmos_ticks() - card.answered_at > card.hz / 200u) {
        serve(&own);
    }
}

/*
 * The link as the PHY has it, about once a second, besides the status
 * block's link-change bit: a backstop, because the bit is the one path
 * here nobody has seen work, and a cable that is never noticed looks like
 * a broken stack.
 */
static void poll_link(struct say_line *line)
{
    if (kosmos_ticks() - card.link_polled_at < card.hz) {
        return;
    }

    card.link_polled_at = kosmos_ticks();
    {
        struct tg3_link now;

        link_read(&now);

        if (now.up != card.link.up || now.mbps != card.link.mbps
            || now.full_duplex != card.link.full_duplex) {
            link_apply();
            say_link(line);
        }
    }
}

/*
 * The driver. **False, and nothing touched, when there is no 57765-class
 * chip here**, so init's role can try the Intel driver instead; otherwise
 * it never returns, and after a STOP it goes on answering "no adapter", as
 * e1000 does with no card, because the stack asks every driver it has an
 * endpoint for.
 *
 * The 25-tick deadline on the interrupt matters more here than there: the
 * 57765 class can lose an MSI, which Linux answers with `tg3_chk_missed_
 * msi` on a timer, and this answers by looking at the status block
 * whenever it wakes.
 */
bool tg3_server(long console_cap, long frames_cap);

bool tg3_server(long console_cap, long frames_cap)
{
    static const char name[] = "tg3";
    struct say_line line;
    struct dev_info dev;
    struct sysinfo info;
    const long ends[1] = { frames_cap };

    console = console_cap;
    frames_endpoint = frames_cap;
    memset(&card, 0, sizeof(card));
    card.irq = -1;
    card.ring_cap = -1;
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

    if (!choose(&line, &dev)) {
        return false;
    }

    (void)kosmos_setname(name, sizeof(name) - 1u);
    card.answered_at = kosmos_ticks();

    if (bring_up(&line)) {
        say_link(&line);
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

            if (st.updated && !card.said_status) {
                card.said_status = true;
                say_begin(&line);
                say_word(&line, "tg3: the first status block from the chip, tag ",
                         st.tag);
                say_send(console, &line);
            }

            if (st.link_changed) {
                /* `tg3_poll_link`: the bit cleared in the block, the PHY
                 * asked. */
                uint8_t *sb = (uint8_t *)(card.mem + STATUS_AT);

                put_le32(sb, (get_le32(sb) | 0x1u) & ~0x2u);
                link_apply();
                say_link(&line);
            }

            take_frames(&st, &line);
            tx_reclaim(&line);

            if (st.error) {
                say_begin(&line);
                say_text(&line, "tg3: the status block says an error");
                say_word(&line, "; flow attention ", reg_read(HOSTCC_FLOW_ATTN));
                say_send(console, &line);
            }

            poll_link(&line);
            mbox_write(MAILBOX_INTERRUPT_0, tg3_int_mbox_rearm(card.last_tag));

            if (card.irq >= 0) {
                (void)kosmos_irq_ack(card.irq);
            }
        }

        serve(&line);
    }
}
