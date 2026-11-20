/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_AMDGPU_IH_H
#define KOSMOS_DRIVERS_DISPLAY_AMDGPU_IH_H

/*
 * **A Southern Islands GPU's interrupts** (`docs/amdgpu.md` V1): the
 * interrupt handler ring - the IH, a ring in memory the GPU writes a
 * 16-byte entry into for every event, and raises its MSI after - and what
 * the engines must be told to put there.
 *
 * Pure, as `amdgpu_decode.c` is: what to write is computed here as a
 * *program*, a list of register writes in the order Linux makes them, and
 * the driver performs it. So the order and every value is held on the Mac
 * by `tools/test_amdgpu_engine.c`, and the Mac Pro only says whether the
 * card agrees.
 *
 * **Every offset and field is Linux's**, v6.12: radeon's `sid.h` in bytes,
 * amdgpu's `oss_1_0_d.h` and `bif_3_0_d.h` in dwords (the test holds the
 * two equal); the order is amdgpu's `si_ih.c` (`si_ih_irq_init`, lines
 * 62-99) and radeon's `si.c` (`si_disable_interrupt_state`, 5928-5956).
 */

#include <stdbool.h>
#include <stdint.h>

/*----------------------------------------------------------------------
 * A program: register writes, in order. The driver writes each; an entry
 * whose `reg` is AMDGPU_WR_DELAY waits `value` microseconds instead, where
 * Linux has a udelay or an mdelay between two writes; and an entry whose
 * `reg` is AMDGPU_WR_POLL reads the register in its `value` until it
 * answers, under the mask in the next entry's `reg`, the value in that
 * entry's `value` - for at most AMDGPU_POLL_US, Linux's `usec_timeout`,
 * after which the program stops and says which entry it stopped at. A
 * mask of nought is a read that is answered at once: Linux's dummy reads.
 *--------------------------------------------------------------------*/

#define AMDGPU_PROG_MAX            96u
#define AMDGPU_WR_DELAY            0xffffffffu
#define AMDGPU_WR_POLL             0xfffffffeu
#define AMDGPU_POLL_US             100000u

struct amdgpu_wr {
    uint32_t reg, value;
};

struct amdgpu_prog {
    struct amdgpu_wr w[AMDGPU_PROG_MAX];
    unsigned n;
    bool     overflow;              /* more than AMDGPU_PROG_MAX asked for: run none of it */
};

void amdgpu_prog_clear(struct amdgpu_prog *p);
void amdgpu_prog_wr(struct amdgpu_prog *p, uint32_t reg, uint32_t value);
void amdgpu_prog_delay(struct amdgpu_prog *p, uint32_t us);
void amdgpu_prog_poll(struct amdgpu_prog *p, uint32_t reg, uint32_t mask, uint32_t value);

/* The last value the program writes to `reg`, or `otherwise`; for the test. */
uint32_t amdgpu_prog_last(const struct amdgpu_prog *p, uint32_t reg, uint32_t otherwise);

/*----------------------------------------------------------------------
 * The registers (`sid.h` 651-685, `oss_1_0_d.h` 225-235, `bif_3_0_d.h`
 * 633-634).
 *--------------------------------------------------------------------*/

#define AMDGPU_IH_RB_CNTL          0x3e00u
#define   AMDGPU_IH_RB_ENABLE      (1u << 0)
#define   AMDGPU_IH_WPTR_WRITEBACK (1u << 8)
#define   AMDGPU_IH_WPTR_OVERFLOW_ENABLE (1u << 16)
#define   AMDGPU_IH_WPTR_OVERFLOW_CLEAR  (1u << 31)
#define AMDGPU_IH_RB_BASE          0x3e04u     /* the ring's MC address >> 8 */
#define AMDGPU_IH_RB_RPTR          0x3e08u     /* bytes */
#define AMDGPU_IH_RB_WPTR          0x3e0cu     /* bytes in 17:2; RB_OVERFLOW 0 */
#define   AMDGPU_IH_RB_OVERFLOW    (1u << 0)
#define   AMDGPU_IH_WPTR_OFFSET    0x3fffcu
#define AMDGPU_IH_RB_WPTR_ADDR_HI  0x3e10u
#define AMDGPU_IH_RB_WPTR_ADDR_LO  0x3e14u
#define AMDGPU_IH_CNTL             0x3e18u
#define   AMDGPU_IH_ENABLE_INTR    (1u << 0)
#define   AMDGPU_IH_RPTR_REARM     (1u << 4)
#define   AMDGPU_IH_MC_WRREQ_CREDIT(x) ((uint32_t)(x) << 15)
#define   AMDGPU_IH_MC_WR_CLEAN_CNT(x) ((uint32_t)(x) << 20)
#define   AMDGPU_IH_MC_VMID(x)     ((uint32_t)(x) << 25)
#define AMDGPU_INTERRUPT_CNTL      0x5468u
#define   AMDGPU_IH_DUMMY_RD_OVERRIDE (1u << 0)
#define   AMDGPU_IH_REQ_NONSNOOP_EN   (1u << 3)
#define AMDGPU_INTERRUPT_CNTL2     0x546cu     /* the dummy read's MC address >> 8 */

/* What else raises an entry, put in a known state (`si_disable_interrupt_state`). */
#define AMDGPU_CP_INT_CNTL_RING0   0xc1a8u     /* sid.h 1276; gfx_6_0_d.h 0x306a */
#define AMDGPU_CP_INT_CNTL_RING1   0xc1acu
#define AMDGPU_CP_INT_CNTL_RING2   0xc1b0u
#define   AMDGPU_CNTX_BUSY_INT_ENABLE  (1u << 19)
#define   AMDGPU_CNTX_EMPTY_INT_ENABLE (1u << 20)
#define   AMDGPU_TIME_STAMP_INT_ENABLE (1u << 26)  /* an EOP's interrupt reaches the IH */
#define AMDGPU_CP_INT_STATUS_RING0 0xc1b4u
#define AMDGPU_GRBM_INT_CNTL       0x8060u     /* sid.h 1006 */
#define AMDGPU_SRBM_INT_CNTL       0x0ea0u     /* sid.h 362 */
#define AMDGPU_DMA_TRAP_ENABLE     (1u << 0)   /* in DMA_CNTL, sid.h 1834 */
#define AMDGPU_INT_MASK            0x6b40u     /* sid.h 819: a display controller's, + its offset */
#define AMDGPU_GRPH_INT_CONTROL    0x685cu     /* sid.h 864 */

/*
 * **The sources** an entry names (`si.c`'s `si_irq_process`, 6266-6411):
 * 1-6 a controller's vertical blank, 146 and 147 a VM fault, 176-178 a CP
 * ring's interrupt, 181 a CP end-of-pipe, 224 the first DMA engine's trap,
 * 244 the second's, 230 and 231 the thermal sensor, 233 the GUI idle.
 */
#define AMDGPU_SRC_VBLANK1         1u
#define AMDGPU_SRC_VM_FAULT        146u
#define AMDGPU_SRC_VM_FAULT2       147u
#define AMDGPU_SRC_CP_RING0        176u
#define AMDGPU_SRC_CP_EOP          181u
#define AMDGPU_SRC_DMA0_TRAP       224u
#define AMDGPU_SRC_DMA1_TRAP       244u
#define AMDGPU_SRC_GUI_IDLE        233u

/* 16 bytes an entry (`si_ih.c` 132-151); 64 KB a ring, as `si_ih_sw_init` makes it. */
#define AMDGPU_IV_BYTES            16u
#define AMDGPU_IH_RING_BYTES       0x10000u

/*----------------------------------------------------------------------
 * The ring set up and enabled.
 *--------------------------------------------------------------------*/

struct amdgpu_ih_cfg {
    uint64_t ring_mc;               /* 256-byte aligned */
    uint32_t ring_bytes;            /* a power of two */
    uint64_t wptr_mc;               /* where the write pointer is written back; dword aligned */
    bool     writeback;             /* false: read IH_RB_WPTR instead (radeon's path without wb) */
    uint64_t dummy_mc;              /* a page for the IH's dummy read */
    bool     in_vram;               /* the ring is in VRAM: IH_REQ_NONSNOOP_EN, as `si.c` 5986 says */
    bool     msi;                   /* RPTR_REARM only with MSIs (`si_ih.c` 91) */
    unsigned crtcs;                 /* the display controllers whose interrupts are masked */
    /* What the registers held, for the read-modify-writes. */
    uint32_t ih_rb_cntl, ih_cntl, interrupt_cntl, cp_int_cntl_ring0, dma_cntl[2];
};

/*
 * `si_ih_irq_init` in order - interrupts off and the pointers nought, the
 * dummy read, the ring's base, its size and write-back, the pointers, then
 * IH_CNTL - followed by `si_disable_interrupt_state` (every other source
 * off but the CP's context busy and empty), and last the enable
 * (`si_ih_enable_interrupts`). False when the configuration cannot be one:
 * a ring not a power of two from 4 KB to 256 KB, or misaligned.
 */
bool amdgpu_ih_setup(struct amdgpu_prog *p, const struct amdgpu_ih_cfg *cfg);

/* IH_RB_CNTL's size field: log2 of the ring's dwords, << 1 (`si_ih.c` 77-81). */
uint32_t amdgpu_ih_rb_size(uint32_t ring_bytes);

/*----------------------------------------------------------------------
 * Entries.
 *--------------------------------------------------------------------*/

struct amdgpu_iv {
    uint32_t src_id;                /* dw0 7:0 */
    uint32_t src_data;              /* dw1 27:0 */
    uint32_t ring_id;               /* dw2 7:0 */
    uint32_t vmid;                  /* dw2 15:8 */
};

/* `si_ih_decode_iv`: an entry's four dwords. */
void amdgpu_ih_decode(const uint32_t dw[4], struct amdgpu_iv *out);

/*
 * The write pointer as `si_get_ih_wptr` takes it (`si.c` 6190-6211): the
 * byte offset in 17:2 masked to the ring, and whether the ring overflowed -
 * in which case reading resumes at the oldest entry not overwritten,
 * `wptr + 16`, and IH_RB_CNTL's OVERFLOW_CLEAR is to be written.
 */
uint32_t amdgpu_ih_wptr(uint32_t raw, uint32_t ring_bytes, bool *overflow);

/* The read pointer after one entry, wrapped (`si.c` 6413-6415). */
uint32_t amdgpu_ih_next(uint32_t rptr, uint32_t ring_bytes);

/* What a source is, for a line: "DMA 0 trap", "VM fault", ... or NULL. */
const char *amdgpu_ih_source_name(uint32_t src_id);

#endif /* KOSMOS_DRIVERS_DISPLAY_AMDGPU_IH_H */
