/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **Skylake's workarounds** (`testing.md` 18.955): the registers i915 sets
 * on a Gen9 GT before anything runs on it, which the hardware needs and
 * nothing in a program's batch provides.
 *
 * **The whitelist at the end is what the 3D Kit's frame needed**, held to
 * a control: without it the HD 530's command streamer stood for good at
 * the PIPE_CONTROL after a mesh drawn behind the sky, every unit done but
 * it; with it the frame finishes, and taking it out alone brings the hang
 * back. The rest are i915's for this GT and carried as i915 carries them;
 * the 3D Kit's 1x1 upload, which had hung, finished from the first run
 * with them, which no control has tied to any one.
 *
 * **The list is i915's**, Linux 6.12's `gt/intel_workarounds.c`
 * (`gen9_ctx_workarounds_init`, `skl_ctx_workarounds_init`,
 * `gen9_gt_workarounds_init`, `skl_gt_workarounds_init`, and
 * `rcs_engine_wa_init` where it says Gen9 or Skylake) and
 * `intel_clock_gating.c` (`skl_init_clock_gating`, the GT's part), fetched
 * into `build/downloads/i915` (SHA-256 e2737b65... and c341c664...); the
 * registers and bits are `gt/intel_gt_regs.h`'s. Each line names i915's
 * workaround.
 *
 * Two kinds, because the hardware keeps them in two places:
 *
 * - **A context's**: registers saved and restored with a context, so a
 *   write to the register is undone at the next restore. i915 writes them
 *   with MI_LOAD_REGISTER_IMM in the request that records the engine's
 *   default state (`intel_engine_emit_ctx_wa`), and every context made from
 *   that state has them. So does Kosmos: `golden_capture` runs them.
 * - **The GT's and the engine's**: registers outside any context, written
 *   once over MMIO - and again after an engine reset, which clears the
 *   engine's own (i915's `intel_engine_apply_workarounds`).
 *
 * A masked register takes the bits to change in its upper half, so its
 * value here is `(mask << 16) | bits`. A plain one is read, `clear`ed and
 * `set`.
 *
 * Left out, with i915's reasons for them not applying here:
 * `skl_tune_iz_hashing` (only for a slice with exactly one 7-EU subslice -
 * the HD 530 is three of eight), `WaInPlaceDecompressionHang` (steppings
 * before H0), `gen9_wa_init_mcr` (steering for reads, not a setting), and
 * the display's clock-gating bits, which are the display's.
 */
#ifndef KOSMOS_INTELGPU_WA_H
#define KOSMOS_INTELGPU_WA_H

#include <stdint.h>

#define IGPU_WA_MASKED(bits)       (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define IGPU_WA_MASKED_OFF(bits)   ((uint32_t)(bits) << 16)

/* MI_LOAD_REGISTER_IMM of `n` (register, value) pairs (`intel_gpu_commands.h`). */
#define IGPU_MI_LRI(n)             ((0x22u << 23) | (2u * (uint32_t)(n) - 1u))

struct igpu_wa_ctx {
    uint32_t reg;
    uint32_t value;                 /* masked: (mask << 16) | bits */
};

/* A context's: run at the default state's recording. */
static const struct igpu_wa_ctx intelgpu_wa_ctx[] = {
    /* WaCompressedResourceSamplerPbeMediaNewHashMode:skl (an LLC):
     * COMMON_SLICE_CHICKEN2, GEN9_PBE_COMPRESSED_HASH_SELECTION on; and
     * WaDisableGatherAtSetShaderCommonSlice, from i915's indirect context
     * batch (`intel_lrc.c`'s `gen9_init_indirectctx_bb`):
     * DISABLE_GATHER_AT_SET_SHADER_COMMON_SLICE, bit 12, off */
    { 0x7014u, ((1u << 13 | 1u << 12) << 16) | (1u << 13) },
    /* The indirect batch's other two, the bspec's provoking-vertex fixes:
     * FF_SLICE_CHICKEN, CL_PROVOKING_VERTEX_FIX (11391), and _3D_CHICKEN3,
     * SF_PROVOKING_VERTEX_FIX (11299) */
    { 0x2088u, IGPU_WA_MASKED(1u << 1) },
    { 0x2090u, IGPU_WA_MASKED(1u << 12) },
    /* The same, and WaEnableYV12BugFixInHalfSliceChicken7,
     * WaEnableSamplerGPGPUPreemptionSupport: GEN9_HALF_SLICE_CHICKEN7,
     * SAMPLER_HASH_COMPRESSED_READ_ADDR, ENABLE_YV12_BUGFIX,
     * ENABLE_GPGPU_PREEMPTION */
    { 0xe194u, IGPU_WA_MASKED((1u << 8) | (1u << 4) | (1u << 2)) },
    /* WaClearFlowControlGpgpuContextSave, WaDisablePartialInstShootdown:
     * GEN8_ROW_CHICKEN, FLOW_CONTROL_ENABLE,
     * PARTIAL_INSTRUCTION_SHOOTDOWN_DISABLE */
    { 0xe4f0u, IGPU_WA_MASKED((1u << 15) | (1u << 8)) },
    /* Wa4x4STCOptimizationDisable, WaDisablePartialResolveInVc:
     * CACHE_MODE_1, 4x4_STC_OPTIMIZATION_DISABLE,
     * PARTIAL_RESOLVE_IN_VC_DISABLE */
    { 0x7004u, IGPU_WA_MASKED((1u << 6) | (1u << 1)) },
    /* WaCcsTlbPrefetchDisable: GEN9_HALF_SLICE_CHICKEN5,
     * CCS_TLB_PREFETCH_ENABLE off */
    { 0xe188u, IGPU_WA_MASKED_OFF(1u << 3) },
    /* WaForceContextSaveRestoreNonCoherent, WaForceEnableNonCoherent:
     * HDC_CHICKEN0, FORCE_CONTEXT_SAVE_RESTORE_NON_COHERENT,
     * FORCE_CSR_NON_COHERENT_OVR_DISABLE, FORCE_NON_COHERENT - "a possible
     * hang in the unlikely event a TLB invalidation occurs during a PSD
     * flush" */
    { 0x7300u, IGPU_WA_MASKED((1u << 5) | (1u << 15) | (1u << 4)) },
    /* WaDisableSamplerPowerBypassForSOPingPong: GEN8_HALF_SLICE_CHICKEN3,
     * SAMPLER_POWER_BYPASS_DIS */
    { 0xe184u, IGPU_WA_MASKED(1u << 1) },
    /* WaDisableSTUnitPowerOptimization: HALF_SLICE_CHICKEN2, ST_PO_DISABLE */
    { 0xe180u, IGPU_WA_MASKED(1u << 13) },
    /* WaDisable3DMidCmdPreemption, WaDisableGPGPUMidCmdPreemption:
     * GEN8_CS_CHICKEN1, PREEMPT_3D_OBJECT_LEVEL off and the GPGPU level
     * (bits 2:1) at command level, GEN9_PREEMPT_GPGPU_LEVEL(1, 0) */
    { 0x2580u, (0x7u << 16) | (1u << 2) },
};

struct igpu_wa_mmio {
    uint32_t reg;
    uint32_t clear, set;
    uint32_t masked;                /* 1: written as (mask << 16) | bits, the bits on;
                                     * 2: the same, the bits off */
};

/* The GT's and the render engine's: over MMIO, at every engine enable. */
static const struct igpu_wa_mmio intelgpu_wa_mmio[] = {
    /* WaDisableKillLogic, WaDisableHDCInvalidation: GAM_ECOCHK,
     * ECOCHK_DIS_TLB, BDW_DISABLE_HDC_INVALIDATION */
    { 0x4090u, 0, (1u << 8) | (1u << 25), 0 },
    /* WaCompressedResourceSamplerPbeMediaNewHashMode (an LLC):
     * MMCD_MISC_CTRL, MMCD_PCLA, MMCD_HOTSPOT_EN */
    { 0x4ddcu, 0, (1u << 31) | (1u << 27), 0 },
    /* WaDisableGafsUnitClkGating: GEN7_UCGCTL4,
     * EU_GAUNIT_CLOCK_GATE_DISABLE */
    { 0x940cu, 0, 1u << 14, 0 },
    /* WaDisableDopClockGating (`skl_init_clock_gating`): GEN7_MISCCPCTL,
     * DOP_CLOCK_GATE_ENABLE off */
    { 0x9424u, 1u << 0, 0, 0 },
    /* Per-context preemption control: GEN7_FF_SLICE_CS_CHICKEN1,
     * FFSC_PERCTX_PREEMPT_CTRL */
    { 0x20e0u, 0, 1u << 14, 1 },
    /* WaEnableGapsTsvCreditFix: GEN8_GARBCNTL, GAPS_TSV_CREDIT_DISABLE */
    { 0xb004u, 0, 1u << 7, 0 },
    /* WaContextSwitchWithConcurrentTLBInvalidate: GEN9_CSFE_CHICKEN1_RCS,
     * PREEMPT_GPGPU_SYNC_SWITCH_DISABLE */
    { 0x20d4u, 0, 1u << 2, 1 },
    /* WaEnableLbsSlaRetryTimerDecrement, and EVICTION_PERF_FIX_ENABLE off:
     * BDW_SCRATCH1 / GEN9_SCRATCH1 */
    { 0xb11cu, 1u << 8, 1u << 2, 0 },
    /* WaOCLCoherentLineFlush, and no non-IA-coherent atomics in L3 - "to
     * prevent unrecoverable hangs": GEN8_L3SQCREG4 */
    { 0xb118u, 1u << 22, 1u << 21, 0 },
    /* The same atomics: GEN9_SCRATCH_LNCF1 */
    { 0xb008u, 1u << 0, 0, 0 },
    /* The indirect context batch's three, here too: i915 runs them at
     * every restore, which this driver does not, so they are written where
     * the engine holds them as well as in the default state. */
    { 0x7014u, 0, 1u << 12, 2 },
    { 0x2088u, 0, 1u << 1, 1 },
    { 0x2090u, 0, 1u << 12, 1 },
};

/*
 * **The render engine's whitelist** (`gen9_whitelist_build`,
 * `skl_whitelist_build`): the registers a program's unprivileged batch may
 * write with MI_LOAD_REGISTER_IMM, in RING_FORCE_TO_NONPRIV's twelve slots
 * (the engine's base + 0x4d0); the slots left name RING_NOPID (base +
 * 0x94), which is harmless, as i915's `intel_engine_apply_whitelist` does.
 */
#define IGPU_NONPRIV_AT        0x4d0u
#define IGPU_NONPRIV_SLOTS     12u
#define IGPU_RING_NOPID        0x94u

static const uint32_t intelgpu_wa_whitelist[] = {
    0x2248u,    /* WaVFEStateAfterPipeControlwithMediaStateClear: GEN9_CTX_PREEMPT_REG */
    0x2580u,    /* WaEnablePreemptionGranularityControlByUMD: GEN8_CS_CHICKEN1 */
    0x7304u,    /* WaAllowUMDToModifyHDCChicken1: GEN8_HDC_CHICKEN1 */
    0x7014u,    /* WaSendPushConstantsFromMMIO: COMMON_SLICE_CHICKEN2 */
    0xb118u,    /* WaDisableLSQCROPERFforOCL: GEN8_L3SQCREG4 */
};

#endif
