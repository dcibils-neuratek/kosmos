/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The render engine's copy, built on the Mac** (`docs/m700-2d.md` 7c).
 *
 *   cc -std=c11 -I build/downloads/igt/out \
 *      -I build/downloads/igt/src/igt-gpu-tools-2.6/lib/intel/genxml \
 *      tools/render_batch.c -o build/render_batch
 *   build/render_batch copy  > user/drivers/display/intelgpu_render.h
 *   build/render_batch blend > user/drivers/display/intelgpu_blend.h
 *
 * A texture sampled through the whole 3D pipeline into a render target:
 * intel-gpu-tools' `_gen9_render_op` (`lib/rendercopy_gen9.c`, igt-gpu-tools
 * 2.6, MIT), followed command for command and packed by the same generated
 * genxml packers it uses (`gen90_pack.h`, from its `gen90.xml` by its
 * `gen_pack_header.py`) - so no field's bits are written here by hand.
 *
 * **Addresses are left for the driver.** The packers call
 * `__gen_combine_address` for every address field, and here that records
 * where in the batch the field is and whose address it is - the batch's
 * own, the destination's or the source's - packed as the offset into that
 * buffer. The driver adds the buffer's address in the GTT to the qword at
 * each place, as i915 relocates its starting state.
 *
 * **`blend`** is the same batch with the blender on, as the compositor
 * lays a window over the back buffer (`over4` in `kits/gfx/surface.h`):
 * colour by the source's alpha and its inverse, and alpha by one and the
 * inverse - which keeps an opaque destination opaque.
 *
 * Neither this nor its inputs are part of the build: it needs the downloads
 * above, and what it writes is committed, as `tools/lineicons.py`'s
 * pictures are.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* What `igt_genxml_defs.h` asks of its user: a batch, and a relocation. */
struct intel_bb {
    uint32_t *batch;
    uint32_t used;                      /* bytes */
};

#define RELOCS_MAX 16

static struct { uint32_t at, kind, delta; } relocs[RELOCS_MAX];
static unsigned reloc_count;

enum { BUF_BATCH = 1, BUF_DST = 2, BUF_SRC = 3 };

uint64_t intel_bb_offset_reloc_with_delta(struct intel_bb *ibb, uint32_t handle,
                                          uint32_t read_domains, uint32_t write_domain,
                                          uint32_t delta, uint32_t offset,
                                          uint64_t presumed_offset);

#include "igt_genxml_defs.h"
#include "gen90_pack.h"

uint64_t intel_bb_offset_reloc_with_delta(struct intel_bb *ibb, uint32_t handle,
                                          uint32_t read_domains, uint32_t write_domain,
                                          uint32_t delta, uint32_t offset,
                                          uint64_t presumed_offset)
{
    (void)ibb; (void)read_domains; (void)write_domain;

    if (reloc_count == RELOCS_MAX) {
        fprintf(stderr, "render_batch: too many relocations\n");
        _exit(1);
    }

    relocs[reloc_count].at = offset;
    relocs[reloc_count].kind = handle;
    relocs[reloc_count].delta = delta;
    reloc_count++;
    return presumed_offset + delta;
}

/* The batch: commands from 0, state from STATE_AT, as intel-gpu-tools
 * splits its own at BATCH_STATE_SPLIT. */
#define BATCH_BYTES 4096u
#define STATE_AT    2048u
#define W           64u                 /* the test's surfaces */
#define H           64u
#define PITCH       (W * 4u)
#define VERTEX_SIZE (3u * 4u)
/* Every MOCS field: index 1, i915's "PTE" - caching as the GTT entry says.
 * genxml lets none of them be 0, and Kosmos programs no MOCS table, so the
 * index is only what the field requires. */
#define PTE_MOCS    2
#define SURFACEFORMAT_B8G8R8A8_UNORM    0x0c0
#define SURFACEFORMAT_R32G32B32A32_FLOAT 0x000
#define SURFACEFORMAT_R16G16_SSCALED    0x0f6
#define SURFACEFORMAT_R32G32_FLOAT      0x085

static uint32_t words[BATCH_BYTES / 4u];
static struct intel_bb bb = { words, 0 };

static void *ptr(void) { return (uint8_t *)words + bb.used; }
static void align(uint32_t a) { bb.used = (bb.used + a - 1u) & ~(a - 1u); }
static uint32_t take(uint32_t bytes) { uint32_t at = bb.used; bb.used += bytes; return at; }
static void out(uint32_t w) { words[bb.used / 4u] = w; bb.used += 4u; }

static struct igt_address of(uint32_t kind, uint32_t offset)
{
    return (struct igt_address){ .offset = offset, .handle = kind, .presumed_offset = 0 };
}

#define EMIT(cmd, name) \
    for (struct cmd name = { cmd##_header }, *_d = (struct cmd *)ptr(); _d; \
         cmd##_pack(&bb, (void *)_d, &name), bb.used += cmd##_length * 4u, _d = NULL)

#define PACK(cmd, at, name) \
    for (struct cmd name = { 0 }, *_d = (struct cmd *)(at); _d; \
         cmd##_pack(&bb, (void *)_d, &name), _d = NULL)

/* intel-gpu-tools' pixel shader for the copy, `ps_kernel_gen9` (from
 * `lib/i915/shaders/ps/blit.g7a`): the texture sampled at the pixel's
 * coordinates, written to render target 0. */
static const uint32_t ps_kernel[][4] = {
    { 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
    { 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
    { 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
    { 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
};

static uint32_t surface_at[2];          /* the destination's and the source's state */
static uint32_t vertices_at;
static uint32_t drawrect_at;            /* 3DSTATE_DRAWING_RECTANGLE, its maximum in dword 2 */

static uint32_t bind(uint32_t kind)
{
    uint32_t at;

    align(64);
    at = take(GFX9_RENDER_SURFACE_STATE_length * 4u);

    PACK(GFX9_RENDER_SURFACE_STATE, (uint8_t *)words + at, ss) {
        ss.SurfaceType = GFX9_SURFTYPE_2D;
        ss.SurfaceFormat = SURFACEFORMAT_B8G8R8A8_UNORM;
        ss.SurfaceVerticalAlignment = GFX9_VALIGN_4;
        ss.SurfaceHorizontalAlignment = GFX9_HALIGN_4;
        ss.MOCS = PTE_MOCS;
        ss.Width = W - 1u;
        ss.Height = H - 1u;
        ss.SurfacePitch = PITCH - 1u;
        ss.ShaderChannelSelectRed = (int)GFX9_SCS_RED;
        ss.ShaderChannelSelectGreen = (int)GFX9_SCS_GREEN;
        ss.ShaderChannelSelectBlue = (int)GFX9_SCS_BLUE;
        ss.ShaderChannelSelectAlpha = (int)GFX9_SCS_ALPHA;
        ss.TileMode = GFX9_LINEAR;
        ss.RenderCacheReadWriteMode = GFX9_READWRITECACHE;
        ss.MipTailStartLOD = 1;
        ss.SurfaceBaseAddress = of(kind, 0);
    }

    surface_at[kind == BUF_DST ? 0 : 1] = at;
    return at;
}

static void vertex_2s(int16_t x, int16_t y) { out((uint32_t)(uint16_t)y << 16 | (uint16_t)x); }
static void vertex_f(float f) { union { float f; uint32_t u; } v = { f }; out(v.u); }

static bool blending;

int main(int argc, char **argv)
{
    const char *name, *NAME;
    uint32_t binding_table, sampler, kernel, cc_state, blend, cc_vp, sf_vp, scissor;
    unsigned i;

    if (argc != 2 || (strcmp(argv[1], "copy") != 0 && strcmp(argv[1], "blend") != 0)) {
        fprintf(stderr, "usage: render_batch copy|blend\n");
        return 2;
    }

    blending = strcmp(argv[1], "blend") == 0;
    name = blending ? "blend" : "render";
    NAME = blending ? "BLEND" : "RENDER";

    /* The state, from STATE_AT. */
    bb.used = STATE_AT;

    align(32);
    binding_table = take(32);
    words[binding_table / 4u] = bind(BUF_DST);
    words[binding_table / 4u + 1u] = bind(BUF_SRC);

    align(64);
    sampler = take(GFX9_SAMPLER_STATE_length * 4u);
    PACK(GFX9_SAMPLER_STATE, (uint8_t *)words + sampler, ss) {
        ss.MinModeFilter = GFX9_MAPFILTER_NEAREST;
        ss.MagModeFilter = GFX9_MAPFILTER_NEAREST;
        ss.TCZAddressControlMode = GFX9_TCM_CLAMP;
        ss.TCYAddressControlMode = GFX9_TCM_CLAMP;
        ss.TCXAddressControlMode = GFX9_TCM_CLAMP;
    }

    align(64);
    kernel = take(sizeof(ps_kernel));
    memcpy((uint8_t *)words + kernel, ps_kernel, sizeof(ps_kernel));

    /* Three vertices of a rectangle: the destination's corner in pixels,
     * the source's in normalised coordinates. */
    align(8);
    vertices_at = bb.used;
    vertex_2s(W, H);  vertex_f(1.0f); vertex_f(1.0f);
    vertex_2s(0, H);  vertex_f(0.0f); vertex_f(1.0f);
    vertex_2s(0, 0);  vertex_f(0.0f); vertex_f(0.0f);

    align(64);
    cc_state = take(GFX9_COLOR_CALC_STATE_length * 4u);
    PACK(GFX9_COLOR_CALC_STATE, (uint8_t *)words + cc_state, cc) { }

    align(64);
    blend = take((GFX9_BLEND_STATE_length + 16u * GFX9_BLEND_STATE_ENTRY_length) * 4u);
    PACK(GFX9_BLEND_STATE, (uint8_t *)words + blend, bs) { bs.IndependentAlphaBlendEnable = blending; }
    for (i = 0; i < 16; i++) {
        PACK(GFX9_BLEND_STATE_ENTRY, (uint8_t *)words + blend
             + (GFX9_BLEND_STATE_length + i * GFX9_BLEND_STATE_ENTRY_length) * 4u, e) {
            e.ColorBlendFunction = GFX9_BLENDFUNCTION_ADD;
            e.AlphaBlendFunction = GFX9_BLENDFUNCTION_ADD;
            e.PreBlendColorClampEnable = true;
            e.PostBlendColorClampEnable = blending;

            if (blending) {
                e.ColorBufferBlendEnable = true;
                e.SourceBlendFactor = GFX9_BLENDFACTOR_SRC_ALPHA;
                e.DestinationBlendFactor = GFX9_BLENDFACTOR_INV_SRC_ALPHA;
                e.SourceAlphaBlendFactor = GFX9_BLENDFACTOR_ONE;
                e.DestinationAlphaBlendFactor = GFX9_BLENDFACTOR_INV_SRC_ALPHA;
            } else {
                e.DestinationBlendFactor = GFX9_BLENDFACTOR_ZERO;
                e.SourceBlendFactor = GFX9_BLENDFACTOR_ONE;
            }
        }
    }

    align(32);
    cc_vp = take(GFX9_CC_VIEWPORT_length * 4u);
    PACK(GFX9_CC_VIEWPORT, (uint8_t *)words + cc_vp, vp) {
        vp.MinimumDepth = -1.e35;
        vp.MaximumDepth = 1.e35;
    }

    align(64);
    sf_vp = take(GFX9_SF_CLIP_VIEWPORT_length * 4u);
    PACK(GFX9_SF_CLIP_VIEWPORT, (uint8_t *)words + sf_vp, scv) {
        scv.XMinClipGuardband = 0;
        scv.XMaxClipGuardband = 1.0f;
        scv.YMinClipGuardband = 0;
        scv.YMaxClipGuardband = 1.0f;
    }

    align(64);
    scissor = take(GFX9_SCISSOR_RECT_length * 4u);
    PACK(GFX9_SCISSOR_RECT, (uint8_t *)words + scissor, sr) { }

    if (bb.used > BATCH_BYTES) {
        fprintf(stderr, "render_batch: the state ran past the page\n");
        return 1;
    }

    /* The commands, from 0, in `_gen9_render_op`'s order. */
    bb.used = 0;

    EMIT(GFX9_PIPELINE_SELECT, ps) { ps.PipelineSelection = GFX9_3D; ps.MaskBits = 3; }
    EMIT(GFX9_STATE_SIP, sip) { }

    EMIT(GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_VS, a) { }
    EMIT(GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_HS, a) { }
    EMIT(GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_DS, a) { }
    EMIT(GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_GS, a) { }
    EMIT(GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_PS, a) { }

    EMIT(GFX9_STATE_BASE_ADDRESS, sba) {
        sba.GeneralStateBaseAddressModifyEnable = true;
        sba.GeneralStateMOCS = PTE_MOCS;
        sba.StatelessDataPortAccessMOCS = PTE_MOCS;
        sba.SurfaceStateBaseAddressModifyEnable = true;
        sba.SurfaceStateMOCS = PTE_MOCS;
        sba.SurfaceStateBaseAddress = of(BUF_BATCH, 0);
        sba.DynamicStateBaseAddressModifyEnable = true;
        sba.DynamicStateMOCS = PTE_MOCS;
        sba.DynamicStateBaseAddress = of(BUF_BATCH, 0);
        sba.IndirectObjectMOCS = PTE_MOCS;
        sba.InstructionBaseAddressModifyEnable = true;
        sba.InstructionMOCS = PTE_MOCS;
        sba.InstructionBaseAddress = of(BUF_BATCH, 0);
        sba.GeneralStateBufferSizeModifyEnable = true;
        sba.GeneralStateBufferSize = 0xfffff;
        sba.DynamicStateBufferSizeModifyEnable = true;
        sba.DynamicStateBufferSize = 1;
        sba.IndirectObjectBufferSizeModifyEnable = true;
        sba.IndirectObjectBufferSize = 0xfffff;
        sba.InstructionBuffersizeModifyEnable = true;
        sba.InstructionBufferSize = 1;
        sba.BindlessSurfaceStateMOCS = PTE_MOCS;
    }

    EMIT(GFX9_3DSTATE_VIEWPORT_STATE_POINTERS_CC, vp) { vp.CCViewportPointer = cc_vp; }
    EMIT(GFX9_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, vp) { vp.SFClipViewportPointer = sf_vp; }

    EMIT(GFX9_3DSTATE_URB_VS, urb) {
        urb.VSNumberofURBEntries = 64;
        urb.VSURBEntryAllocationSize = 2 - 1;
        urb.VSURBStartingAddress = 4;
    }
    EMIT(GFX9_3DSTATE_URB_GS, urb) { urb.GSURBStartingAddress = 4; }
    EMIT(GFX9_3DSTATE_URB_HS, urb) { urb.HSURBStartingAddress = 4; }
    EMIT(GFX9_3DSTATE_URB_DS, urb) { urb.DSURBStartingAddress = 4; }

    EMIT(GFX9_3DSTATE_BLEND_STATE_POINTERS, bsp) {
        bsp.BlendStatePointer = blend;
        bsp.BlendStatePointerValid = true;
    }
    EMIT(GFX9_3DSTATE_CC_STATE_POINTERS, ccp) {
        ccp.ColorCalcStatePointer = cc_state;
        ccp.ColorCalcStatePointerValid = true;
    }

    EMIT(GFX9_3DSTATE_MULTISAMPLE, ms) { }
    EMIT(GFX9_3DSTATE_SAMPLE_MASK, sm) { sm.SampleMask = 1; }

    /* Every stage before the pixel shader null. */
    EMIT(GFX9_3DSTATE_WM_HZ_OP, hz) { }
    EMIT(GFX9_3DSTATE_CONSTANT_HS, c) { c.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_HS, s) { }
    EMIT(GFX9_3DSTATE_BINDING_TABLE_POINTERS_HS, b) { }
    EMIT(GFX9_3DSTATE_SAMPLER_STATE_POINTERS_HS, p) { }
    EMIT(GFX9_3DSTATE_TE, te) { }
    EMIT(GFX9_3DSTATE_CONSTANT_GS, c) { c.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_GS, s) { }
    EMIT(GFX9_3DSTATE_BINDING_TABLE_POINTERS_GS, b) { }
    EMIT(GFX9_3DSTATE_SAMPLER_STATE_POINTERS_GS, p) { }
    EMIT(GFX9_3DSTATE_CONSTANT_DS, c) { c.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_DS, s) { }
    EMIT(GFX9_3DSTATE_BINDING_TABLE_POINTERS_DS, b) { }
    EMIT(GFX9_3DSTATE_SAMPLER_STATE_POINTERS_DS, p) { }
    EMIT(GFX9_3DSTATE_CONSTANT_VS, c) { c.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_BINDING_TABLE_POINTERS_VS, b) { }
    EMIT(GFX9_3DSTATE_SAMPLER_STATE_POINTERS_VS, p) { }
    EMIT(GFX9_3DSTATE_VS, s) { }

    EMIT(GFX9_3DSTATE_STREAMOUT, so) { }
    EMIT(GFX9_3DSTATE_CLIP, clip) { }

    EMIT(GFX9_3DSTATE_SBE, sbe) {
        sbe.NumberofSFOutputAttributes = 1;
        sbe.ForceVertexURBEntryReadLength = true;
        sbe.ForceVertexURBEntryReadOffset = true;
        sbe.VertexURBEntryReadLength = 1;
        sbe.VertexURBEntryReadOffset = 1;
        sbe.AttributeActiveComponentFormat[0] = GFX9_ACTIVE_COMPONENT_XYZW;
    }
    EMIT(GFX9_3DSTATE_SBE_SWIZ, swiz) { }
    EMIT(GFX9_3DSTATE_RASTER, r) { r.FrontWinding = 1; r.CullMode = GFX9_CULLMODE_NONE; }
    EMIT(GFX9_3DSTATE_SF, sf) { }

    EMIT(GFX9_3DSTATE_WM, wm) { wm.BarycentricInterpolationMode = GFX9_BIM_PERSPECTIVE_PIXEL; }
    EMIT(GFX9_3DSTATE_CONSTANT_PS, c) { c.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_PS, ps) {
        ps.KernelStartPointer0 = kernel;
        ps.BindingTableEntryCount = 2;
        ps.SamplerCount = 1;
        ps._16PixelDispatchEnable = true;
        ps.MaximumNumberofThreadsPerPSD = 63 - 1;
        ps.DispatchGRFStartRegisterForConstantSetupData0 = 6;
    }
    /* Render target 0's blend again, as Mesa keeps it: the same as the
     * blend state's first entry. */
    EMIT(GFX9_3DSTATE_PS_BLEND, b) {
        b.HasWriteableRT = true;

        if (blending) {
            b.ColorBufferBlendEnable = true;
            b.IndependentAlphaBlendEnable = true;
            b.SourceBlendFactor = GFX9_BLENDFACTOR_SRC_ALPHA;
            b.DestinationBlendFactor = GFX9_BLENDFACTOR_INV_SRC_ALPHA;
            b.SourceAlphaBlendFactor = GFX9_BLENDFACTOR_ONE;
            b.DestinationAlphaBlendFactor = GFX9_BLENDFACTOR_INV_SRC_ALPHA;
        }
    }
    EMIT(GFX9_3DSTATE_PS_EXTRA, x) { x.PixelShaderValid = true; x.AttributeEnable = true; }

    EMIT(GFX9_3DSTATE_BINDING_TABLE_POINTERS_PS, bt) { bt.PointertoPSBindingTable = binding_table; }
    EMIT(GFX9_3DSTATE_SAMPLER_STATE_POINTERS_PS, sp) { sp.PointertoPSSamplerState = sampler; }
    EMIT(GFX9_3DSTATE_SCISSOR_STATE_POINTERS, ssp) { ssp.ScissorRectPointer = scissor; }

    EMIT(GFX9_3DSTATE_WM_DEPTH_STENCIL, wds) { }
    EMIT(GFX9_3DSTATE_DEPTH_BUFFER, db) { db.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_HIER_DEPTH_BUFFER, h) { h.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_STENCIL_BUFFER, s) { s.MOCS = PTE_MOCS; }
    EMIT(GFX9_3DSTATE_CLEAR_PARAMS, cp) { cp.DepthClearValueValid = true; }

    drawrect_at = bb.used;
    EMIT(GFX9_3DSTATE_DRAWING_RECTANGLE, dr) {
        dr.ClippedDrawingRectangleXMax = W - 1u;
        dr.ClippedDrawingRectangleYMax = H - 1u;
    }

    {
        struct GFX9_3DSTATE_VERTEX_BUFFERS vbs = { GFX9_3DSTATE_VERTEX_BUFFERS_header };

        GFX9_3DSTATE_VERTEX_BUFFERS_pack(&bb, ptr(), &vbs);
        bb.used += 4u;
        PACK(GFX9_VERTEX_BUFFER_STATE, ptr(), vb) {
            vb.VertexBufferIndex = 0;
            vb.AddressModifyEnable = true;
            vb.MOCS = PTE_MOCS;
            vb.BufferPitch = VERTEX_SIZE;
            vb.BufferStartingAddress = of(BUF_BATCH, vertices_at);
            vb.BufferSize = 3u * VERTEX_SIZE;
        }
        bb.used += GFX9_VERTEX_BUFFER_STATE_length * 4u;
    }

    {
        struct GFX9_3DSTATE_VERTEX_ELEMENTS ves = { GFX9_3DSTATE_VERTEX_ELEMENTS_header };

        ves.DWordLength = 3u * GFX9_VERTEX_ELEMENT_STATE_length - 1u;
        GFX9_3DSTATE_VERTEX_ELEMENTS_pack(&bb, ptr(), &ves);
        bb.used += 4u;
        PACK(GFX9_VERTEX_ELEMENT_STATE, ptr(), ve) {
            ve.Valid = true;
            ve.SourceElementFormat = SURFACEFORMAT_R32G32B32A32_FLOAT;
            ve.Component0Control = GFX9_VFCOMP_STORE_0;
            ve.Component1Control = GFX9_VFCOMP_STORE_0;
            ve.Component2Control = GFX9_VFCOMP_STORE_0;
            ve.Component3Control = GFX9_VFCOMP_STORE_0;
        }
        bb.used += GFX9_VERTEX_ELEMENT_STATE_length * 4u;
        PACK(GFX9_VERTEX_ELEMENT_STATE, ptr(), ve) {
            ve.Valid = true;
            ve.SourceElementFormat = SURFACEFORMAT_R16G16_SSCALED;
            ve.Component0Control = GFX9_VFCOMP_STORE_SRC;
            ve.Component1Control = GFX9_VFCOMP_STORE_SRC;
            ve.Component2Control = GFX9_VFCOMP_STORE_0;
            ve.Component3Control = GFX9_VFCOMP_STORE_1_FP;
        }
        bb.used += GFX9_VERTEX_ELEMENT_STATE_length * 4u;
        PACK(GFX9_VERTEX_ELEMENT_STATE, ptr(), ve) {
            ve.Valid = true;
            ve.SourceElementFormat = SURFACEFORMAT_R32G32_FLOAT;
            ve.SourceElementOffset = 4;
            ve.Component0Control = GFX9_VFCOMP_STORE_SRC;
            ve.Component1Control = GFX9_VFCOMP_STORE_SRC;
            ve.Component2Control = GFX9_VFCOMP_STORE_0;
            ve.Component3Control = GFX9_VFCOMP_STORE_1_FP;
        }
        bb.used += GFX9_VERTEX_ELEMENT_STATE_length * 4u;
    }

    EMIT(GFX9_3DSTATE_VF_TOPOLOGY, t) { t.PrimitiveTopologyType = GFX9_3DPRIM_RECTLIST; }
    EMIT(GFX9_3DSTATE_VF, vf) { }
    EMIT(GFX9_3DSTATE_VF_INSTANCING, vfi) { }
    EMIT(GFX9_3DPRIMITIVE, prim) { prim.VertexCountPerInstance = 3; prim.InstanceCount = 1; }

    out(0x05000000u);                   /* MI_BATCH_BUFFER_END */

    if (bb.used > STATE_AT) {
        fprintf(stderr, "render_batch: the commands ran into the state\n");
        return 1;
    }

    /* The header. */
    printf("/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */\n");
    printf("/*\n * **The render engine's %s** (`docs/m700-2d.md` 7c): a %ux%u B8G8R8A8\n",
           blending ? "blend" : "copy", W, H);
    printf(" * texture sampled through the 3D pipeline into a render target of the\n");
    printf(" * same size%s. Written by `tools/render_batch.c %s` from\n",
           blending ? ", blended over what is there by the source's alpha" : "", argv[1]);
    printf(" * intel-gpu-tools' `_gen9_render_op` and its generated genxml packers\n");
    printf(" * (igt-gpu-tools 2.6, MIT, Copyright (c) Intel Corporation), its pixel\n");
    printf(" * shader intel-gpu-tools' `ps_kernel_gen9` - so do not edit it: run the\n");
    printf(" * tool. Commands from byte 0 to the MI_BATCH_BUFFER_END, state from %u.\n *\n", STATE_AT);
    printf(" * Each relocation is a byte offset holding a qword that is an offset into\n");
    printf(" * a buffer - 1 the batch itself, 2 the destination, 3 the source - to which\n");
    printf(" * that buffer's address in the GTT is added.\n */\n");
    printf("#ifndef KOSMOS_INTELGPU_%s_H\n#define KOSMOS_INTELGPU_%s_H\n\n#include <stdint.h>\n\n", NAME, NAME);
    printf("#define INTELGPU_%s_W        %uu\n#define INTELGPU_%s_H        %uu\n", NAME, W, NAME, H);
    printf("#define INTELGPU_%s_PITCH    %uu\n", NAME, PITCH);
    printf("#define INTELGPU_%s_VERTICES 0x%03xu   /* the three vertices */\n", NAME, vertices_at);
    printf("#define INTELGPU_%s_SS_DST   0x%03xu   /* the surfaces' state */\n", NAME, surface_at[0]);
    printf("#define INTELGPU_%s_SS_SRC   0x%03xu\n", NAME, surface_at[1]);
    printf("#define INTELGPU_%s_DRAWRECT 0x%03xu   /* the drawing rectangle */\n\n", NAME, drawrect_at);
    printf("static const struct { uint32_t at, kind; } intelgpu_%s_relocs[] = {\n", name);
    for (i = 0; i < reloc_count; i++) {
        printf("    { 0x%03x, %u },\n", relocs[i].at, relocs[i].kind);
    }
    printf("};\n\nstatic const uint32_t intelgpu_%s[] = {\n", name);
    for (i = 0; i < BATCH_BYTES / 4u; i += 6) {
        unsigned j;

        printf("   ");
        for (j = i; j < i + 6 && j < BATCH_BYTES / 4u; j++) {
            printf(" 0x%08x,", words[j]);
        }
        printf("\n");
    }
    printf("};\n\n#endif\n");
    return 0;
}
