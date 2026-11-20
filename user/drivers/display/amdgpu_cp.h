/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_DISPLAY_AMDGPU_CP_H
#define KOSMOS_DRIVERS_DISPLAY_AMDGPU_CP_H

/*
 * **A Southern Islands GPU's command processor** (`docs/amdgpu.md` V3, V4):
 * the firmware it runs, the RLC beside it, its ring, and the PM4 packets a
 * ring and an indirect buffer are made of.
 *
 * The CP is three small processors - the PFP that fetches, the ME that
 * executes, the CE that keeps constants - and none of them does anything
 * until its microcode is written into it through a register pair, word by
 * word; the RLC, which runs the GPU's power and the clear state, the same.
 * The microcode is AMD's, in `radeon/` of linux-firmware, in files with a
 * header (`radeon_ucode.h` 156-198), and is read here, checked, and handed
 * to the driver as words.
 *
 * Pure, as the rest: the packets are built into a buffer, the bring-ups
 * are programs (`amdgpu_ih.h`), and `tools/test_amdgpu_engine.c` holds
 * them to Linux v6.12's amdgpu `gfx_v6_0.c` and radeon's `si.c` and `sid.h`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "amdgpu_ih.h"

/*----------------------------------------------------------------------
 * The registers (`sid.h` 982-1037, 1245-1365; the test holds each to
 * `gfx_6_0_d.h`'s dword).
 *--------------------------------------------------------------------*/

#define AMDGPU_GRBM_SOFT_RESET       0x8020u
#define   AMDGPU_SOFT_RESET_RLC      (1u << 2)
#define AMDGPU_SCRATCH_REG0          0x8500u
#define AMDGPU_SCRATCH_UMSK          0x8540u
#define AMDGPU_SCRATCH_ADDR          0x8544u
#define AMDGPU_CP_SEM_WAIT_TIMER     0x85bcu
#define AMDGPU_CP_SEM_INCOMPLETE     0x85c8u
#define AMDGPU_CP_COHER_CNTL2        0x85e8u
#define   AMDGPU_CP_CE_HALT          (1u << 24)
#define   AMDGPU_CP_PFP_HALT         (1u << 26)
#define   AMDGPU_CP_ME_HALT          (1u << 28)
#define AMDGPU_CP_RB0_RPTR           0x8700u
#define AMDGPU_CP_RB_WPTR_DELAY      0x8704u
#define AMDGPU_SPI_LB_CU_MASK        0x9354u
#define AMDGPU_DB_DEPTH_INFO         0x2803cu    /* `gfx_6_0_d.h` 0xA00F: a gfx register, read */
#define AMDGPU_CP_RB0_BASE           0xc100u     /* the ring's MC address >> 8 */
#define AMDGPU_CP_RB0_CNTL           0xc104u
#define   AMDGPU_RB_RPTR_WR_ENA      (1u << 31)
#define AMDGPU_CP_RB0_RPTR_ADDR      0xc10cu
#define AMDGPU_CP_RB0_RPTR_ADDR_HI   0xc110u
#define AMDGPU_CP_RB0_WPTR           0xc114u     /* dwords */
#define AMDGPU_CP_PFP_UCODE_ADDR     0xc150u
#define AMDGPU_CP_PFP_UCODE_DATA     0xc154u
#define AMDGPU_CP_ME_RAM_RADDR       0xc158u
#define AMDGPU_CP_ME_RAM_WADDR       0xc15cu
#define AMDGPU_CP_ME_RAM_DATA        0xc160u
#define AMDGPU_CP_CE_UCODE_ADDR      0xc168u
#define AMDGPU_CP_CE_UCODE_DATA      0xc16cu
#define AMDGPU_CP_DEBUG              0xc1fcu
#define AMDGPU_RLC_RL_BASE           0xc304u
#define AMDGPU_RLC_RL_SIZE           0xc308u
#define AMDGPU_RLC_LB_CNTL           0xc30cu
#define AMDGPU_RLC_SAVE_RESTORE_BASE 0xc310u
#define AMDGPU_RLC_LB_CNTR_MAX       0xc314u
#define AMDGPU_RLC_LB_CNTR_INIT      0xc318u
#define AMDGPU_RLC_CLEAR_STATE_BASE  0xc320u
#define AMDGPU_RLC_UCODE_ADDR        0xc32cu
#define AMDGPU_RLC_UCODE_DATA        0xc330u
#define AMDGPU_RLC_MC_CNTL           0xc344u
#define AMDGPU_RLC_UCODE_CNTL        0xc348u
#define AMDGPU_RLC_STAT              0xc34cu
#define   AMDGPU_RLC_BUSY_STATUS     (1u << 0)
#define   AMDGPU_GFX_POWER_STATUS    (1u << 1)
#define   AMDGPU_GFX_CLOCK_STATUS    (1u << 2)
#define   AMDGPU_GFX_LS_STATUS       (1u << 3)
#define AMDGPU_RLC_LB_INIT_CU_MASK   0xc41cu
#define AMDGPU_RLC_SERDES_BUSY_0     0xc464u
#define AMDGPU_RLC_SERDES_BUSY_1     0xc468u
#define   AMDGPU_RLC_ENABLE          (1u << 0)

/*----------------------------------------------------------------------
 * Firmware.
 *--------------------------------------------------------------------*/

enum amdgpu_fw_kind { AMDGPU_FW_PFP, AMDGPU_FW_ME, AMDGPU_FW_CE, AMDGPU_FW_RLC };

/* What each holds, in dwords (`radeon_ucode.h` 35-37 and 51; `si.c`
 * 1668-1671 requires exactly these). */
#define AMDGPU_PFP_WORDS   2144u
#define AMDGPU_ME_WORDS    2144u
#define AMDGPU_CE_WORDS    2144u
#define AMDGPU_RLC_WORDS   2048u

struct amdgpu_fw {
    const uint8_t *words;           /* little-endian, `count` of them */
    uint32_t count;
    uint32_t ucode_version;         /* the header's: what a line shows */
    uint32_t feature_version;
};

/*
 * A firmware file of the new kind (`radeon_ucode.h` 156-198), checked: the
 * size it says is the size it is; header 1.0, for IP 6.0 (Southern
 * Islands); the payload inside the file and as long as the engine takes.
 * NULL, or the sentence for a line saying what is wrong. The header's
 * crc32 is not checked: it is not zlib's over the payload, and what it is
 * over is written nowhere this driver has read.
 */
const char *amdgpu_fw_parse(enum amdgpu_fw_kind kind, const uint8_t *bytes, size_t len,
                            struct amdgpu_fw *out);

/* Word `i` of the payload. */
uint32_t amdgpu_fw_word(const struct amdgpu_fw *fw, uint32_t i);

/*----------------------------------------------------------------------
 * Bring-up, in `gfx_v6_0_hw_init`'s order: the RLC (`gfx_v6_0_rlc_resume`,
 * 2480-2524), then the CP's microcode (`gfx_v6_0_cp_gfx_load_microcode`,
 * 1927-1980), then its ring (`gfx_v6_0_cp_gfx_resume`, 2047-2099) and the
 * first packets (`gfx_v6_0_cp_gfx_start`, 1982-2045).
 *
 * The microcode itself is too long for a program - 2144 words each, 2048
 * for the RLC - so a program says what comes before and after, and the
 * driver writes the words between, by the loops `amdgpu_*_load` describe.
 *--------------------------------------------------------------------*/

/* `gfx_v6_0_rlc_stop` and `_reset` (2440-2463): the RLC off, the GUI's idle
 * interrupt off with the wait Linux makes for it (2214-2239), the SERDES
 * waited for, and the soft reset pulsed. `cp_int_cntl` and `soft_reset`
 * are what CP_INT_CNTL_RING0 and GRBM_SOFT_RESET held. */
void amdgpu_rlc_stop(struct amdgpu_prog *p, uint32_t cp_int_cntl, uint32_t soft_reset);

/* `gfx_v6_0_init_pg` without power gating (2896-2899), then 2496-2504:
 * the save-and-restore and clear-state buffers' MC addresses, 256-byte
 * aligned, and the load balancer's counters. False when misaligned. */
bool amdgpu_rlc_prepare(struct amdgpu_prog *p, uint64_t save_restore_mc, uint64_t clear_state_mc);

/* The RLC's microcode: for each word i, RLC_UCODE_ADDR = i, RLC_UCODE_DATA
 * = it (2510-2514); then RLC_UCODE_ADDR = 0. Written by the driver. */

/* 2517-2518 and `gfx_v6_0_rlc_start`: load balancing off - GDDR5 here, and
 * Linux has it only for DDR3 (2465-2474) - and the RLC running, its GUI
 * idle interrupt back on (`cp_int_cntl` as the stop left it). */
void amdgpu_rlc_start(struct amdgpu_prog *p, uint32_t cp_int_cntl);

/* `gfx_v6_0_cp_gfx_enable(false)` (1914-1925): the three halted. */
void amdgpu_cp_halt(struct amdgpu_prog *p);

/* The CP's microcode: PFP_UCODE_ADDR = 0, the PFP's words into
 * PFP_UCODE_DATA, ADDR = 0; the CE's the same; ME_RAM_WADDR = 0, the ME's
 * into ME_RAM_DATA, WADDR = 0 - then `amdgpu_cp_loaded`. */
void amdgpu_cp_loaded(struct amdgpu_prog *p);

/* CP_RB0_CNTL for a ring of `bytes` (`gfx_v6_0.c` 2066-2067): log2 of its
 * quadwords in 5:0, and log2 of a 4 KB page's quadwords - 9 - in 13:8. */
uint32_t amdgpu_cp_rb_cntl(uint32_t ring_bytes);

/* `gfx_v6_0_cp_gfx_resume` to the ring's base; false for a ring not a power
 * of two from 4 KB to 1 MB, or misaligned. The 1 ms Linux waits is kept. */
bool amdgpu_cp_resume(struct amdgpu_prog *p, uint64_t ring_mc, uint32_t ring_bytes,
                      uint64_t rptr_mc);

/* `gfx_v6_0_cp_gfx_enable(true)`. */
void amdgpu_cp_run(struct amdgpu_prog *p);

/*----------------------------------------------------------------------
 * PM4: type-3 packets (`sid.h` 1595-1597). Each builder writes its dwords
 * at `out` and returns how many.
 *--------------------------------------------------------------------*/

#define AMDGPU_PM4_NOP               0x10u
#define AMDGPU_PM4_SET_BASE          0x11u
#define AMDGPU_PM4_CLEAR_STATE       0x12u
#define AMDGPU_PM4_IB_CONST          0x31u
#define AMDGPU_PM4_IB                0x32u
#define AMDGPU_PM4_WRITE_DATA        0x37u
#define AMDGPU_PM4_WAIT_REG_MEM      0x3cu
#define AMDGPU_PM4_PFP_SYNC_ME       0x42u
#define AMDGPU_PM4_SURFACE_SYNC      0x43u
#define AMDGPU_PM4_ME_INITIALIZE     0x44u
#define AMDGPU_PM4_EVENT_WRITE       0x46u
#define AMDGPU_PM4_EVENT_WRITE_EOP   0x47u
#define AMDGPU_PM4_PREAMBLE_CNTL     0x4au
#define AMDGPU_PM4_SET_CONFIG_REG    0x68u
#define AMDGPU_PM4_SET_CONTEXT_REG   0x69u
#define AMDGPU_PM4_SET_SH_REG        0x76u
#define AMDGPU_PM4_SWITCH_BUFFER     0x8bu

#define AMDGPU_CONFIG_REG_START      0x8000u
#define AMDGPU_CONFIG_REG_END        0xb000u
#define AMDGPU_CONTEXT_REG_START     0x28000u
#define AMDGPU_SH_REG_START          0xb000u

/* A header: `n` is the count of dwords after it, less one. */
uint32_t amdgpu_pm4(uint32_t op, uint32_t n);

/* SET_CONFIG_REG of one register (its byte offset, 0x8000 to 0xb000). 0
 * for a register outside the range: nothing written. */
unsigned amdgpu_pm4_set_config(uint32_t *out, uint32_t reg, uint32_t value);

/* WRITE_DATA of one dword to memory, confirmed, by the ME (sid.h 1636-1647). */
unsigned amdgpu_pm4_write_mem(uint32_t *out, uint64_t addr, uint32_t value);

/* WRITE_DATA of one register, by the PFP, as `si_vm_flush` writes them
 * (`si.c` 5060-5067: ENGINE_SEL 1, DST_SEL 0). */
unsigned amdgpu_pm4_write_reg(uint32_t *out, uint32_t reg, uint32_t value);

/* `gfx_v6_0_cp_gfx_start`'s first packets: ME_INITIALIZE for
 * `max_hw_contexts` (8 on every SI part, `gfx_v6_0.c` 1631), then SET_BASE
 * of the CE partition. */
unsigned amdgpu_pm4_me_init(uint32_t *out, uint32_t max_hw_contexts);

/* The ring test (1766-1794): SCRATCH_REG0, which the driver set to
 * 0xCAFEDEAD, given 0xDEADBEEF by the CP. */
unsigned amdgpu_pm4_ring_test(uint32_t *out);

/*
 * A fence (`gfx_v6_0_ring_emit_fence`, 1803-1829): the read caches over
 * the GART flushed - CP_COHER_CNTL2 nought, a SURFACE_SYNC of the texture
 * and shader caches - and an EVENT_WRITE_EOP that writes `seq` at `addr`,
 * 64 bits when `wide`, and raises source 181 when `interrupt`.
 */
unsigned amdgpu_pm4_fence(uint32_t *out, uint64_t addr, uint64_t seq, bool wide, bool interrupt);

/* An indirect buffer (`gfx_v6_0_ring_emit_ib`, 1831-1863): `dwords` at
 * `addr` (dword aligned) in `vmid`; the constant engine's when `ce`, and a
 * SWITCH_BUFFER before it when `switch_buffer`, the first of a frame. */
unsigned amdgpu_pm4_ib(uint32_t *out, uint64_t addr, uint32_t dwords, unsigned vmid,
                       bool ce, bool switch_buffer);

/*
 * A program's page directory given to `vmid` and the TLB flushed from the
 * GFX ring (`si_vm_flush`, `si.c` 5056-5099): the base by WRITE_DATA, the
 * HDP flushed, the invalidate, a WAIT_REG_MEM on VM_INVALIDATE_REQUEST
 * with function "always" - a read that lets the write land, as Linux has
 * it - and PFP_SYNC_ME so the fetcher does not run ahead.
 */
unsigned amdgpu_pm4_vm_flush(uint32_t *out, unsigned vmid, uint64_t pd_mc);

/* Type-3 NOPs (or one type-2) to pad `count` dwords: a NOP header saying
 * `count - 2` dwords follow, or a PACKET2 when one is wanted. */
unsigned amdgpu_pm4_pad(uint32_t *out, unsigned count);

#endif /* KOSMOS_DRIVERS_DISPLAY_AMDGPU_CP_H */
