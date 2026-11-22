/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef INTELGPU_LRC_H
#define INTELGPU_LRC_H

/*
 * **Contexts and page tables for Gen9's execlists** (`docs/gpu3d.md` G6a):
 * what the driver writes into memory so that an engine runs a program's
 * work in a context of its own, through a GPU page table of its own. No
 * register is touched here - the driver does that - so all of it runs on
 * the Mac too, held by `tools/test_intelgpu_lrc.c`.
 *
 * From Linux 6.12's i915, `build/downloads/i915/`: the context image is
 * `intel_lrc.c`'s (`set_offsets`, `__lrc_init_regs`, `lrc_descriptor`),
 * its fixed indices `intel_lrc_reg.h`'s, the status buffer
 * `intel_execlists_submission.c`'s and `intel_engine.h`'s, and the page
 * table `gen8_ppgtt.c`'s and `intel_gtt.h`'s.
 *
 * **A context image** is pages in the global GTT: the first a per-process
 * status page, the second the register state - "a big batch buffer with
 * several MI_LOAD_REGISTER_IMM commands followed by (reg, value) pairs",
 * which the engine loads when it switches to the context and writes back
 * when it leaves. Gen9's render engine's is 22 pages, every other engine's
 * two (`intel_engine_cs.c`).
 *
 * **A page table** is 48-bit and four levels - PML4, PDP, PD, PT, 512
 * entries of eight bytes in a page each, bits 47:39, 38:30, 29:21 and 20:12
 * of the GPU address choosing them - and its root goes in the context
 * image where the first PDP would be (`ASSIGN_CTX_PML4`).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IGPU_LRC_STATE_PAGE      1u          /* LRC_STATE_PN: the register state, after the PPHWSP */
#define IGPU_LRC_PAGES_RENDER    22u         /* GEN9_LR_CONTEXT_RENDER_SIZE */
#define IGPU_LRC_PAGES_OTHER     2u          /* GEN8_LR_CONTEXT_OTHER_SIZE */

/* Engines' register bases (`i915_reg.h`). */
#define IGPU_VIDEO_BASE          0x12000u    /* GEN6_BSD_RING_BASE: VCS0 */

/* An engine's registers an execlist needs, from its base
 * (`intel_engine_regs.h`). */
#define IGPU_RING_ELSP           0x230u      /* the submit port: four writes, two descriptors */
#define IGPU_RING_EXECLIST_STATUS 0x234u
#define IGPU_RING_CONTEXT_STATUS_PTR 0x3a0u  /* read pointer 10:8, write pointer 2:0, masked */
#define IGPU_GFX_RUN_LIST_ENABLE (1u << 15)  /* in RING_MODE_GEN7 */

/* The status buffer, in the engine's status page (`intel_engine.h`): six
 * qword entries from dword 0x10, the write pointer at dword 0x1f. */
#define IGPU_CSB_AT              0x10u
#define IGPU_CSB_WRITE_AT        0x1fu
#define IGPU_CSB_ENTRIES         6u

/* An entry's low dword (`intel_execlists_submission.c`); the high dword is
 * the context's ID, as its descriptor gave it. */
#define IGPU_CSB_IDLE_ACTIVE     (1u << 0)
#define IGPU_CSB_PREEMPTED       (1u << 1)
#define IGPU_CSB_ELEMENT_SWITCH  (1u << 2)
#define IGPU_CSB_ACTIVE_IDLE     (1u << 3)
#define IGPU_CSB_COMPLETE        (1u << 4)
#define IGPU_CSB_LITE_RESTORE    (1u << 15)

/* Which of i915's tables an engine's image is laid out by. */
enum igpu_lrc_kind {
    IGPU_LRC_RENDER,                         /* gen9_rcs_offsets */
    IGPU_LRC_OTHER,                          /* gen9_xcs_offsets: video, video enhancement, blitter */
};

/* What a new context's image says, besides its layout. */
struct igpu_lrc_init {
    uint32_t base;                           /* the engine's registers' base */
    uint32_t ring_ggtt;                      /* its ring, in the global GTT */
    uint32_t ring_pages;                     /* how big, in pages: 1 to 512 */
    uint32_t head, tail;                     /* in bytes */
    uint64_t pml4;                           /* the page table's root, physical */
};

/*
 * The register-state page of a new context, 1024 dwords, zeroed and laid
 * out from the engine's table - each MI_LOAD_REGISTER_IMM and each
 * register's address - then filled as i915 fills a context the engine has
 * never run: its restore inhibited, so the engine starts from the image
 * rather than from what a previous one left; the ring; the page table's
 * root; and STOP_RING cleared. False if a register it fills is not in the
 * layout, which would be a table copied wrong.
 */
bool igpu_lrc_fill(uint32_t *state, enum igpu_lrc_kind kind, const struct igpu_lrc_init *in);

/* Where the image says a register's value is - the dword after its
 * address - or -1. `offset` is from the engine's base. */
int igpu_lrc_find(const uint32_t *state, uint32_t base, uint32_t offset);

/* The ring's tail moved, in an image the engine has saved back. */
bool igpu_lrc_set_tail(uint32_t *state, uint32_t base, uint32_t tail);

/*
 * **Kept across runs** (G6e). The context control an image the engine has
 * saved back is submitted with again - as a new one's, but its registers
 * restored from it, every time. Until 18.994 the restore was inhibited when the
 * same context ran last, to save 88 us of restoring 22 pages
 * (`testing.md` 18.930): the engine does not hold that state across the
 * GT's sleep, and saved its reset defaults over the image.
 */
uint32_t igpu_lrc_restore_control(void);

/* A saved image made ready to run again: its tail, and its restore. */
void igpu_lrc_again(volatile uint32_t *state, unsigned tail_at, unsigned control_at, uint32_t tail);

/* What a context's own image says - its control, restored or inhibited,
 * no batch, its page table, STOP_RING off and its ring - written over a
 * state page already laid out: a new one (`igpu_lrc_fill` is the layout
 * and this), or a copy of the engine's default state. */
bool igpu_lrc_patch(uint32_t *state, const struct igpu_lrc_init *in, bool restore);

/*
 * A context's descriptor, as an element of the submit port takes it
 * (`lrc_descriptor`): the image's address in the global GTT, valid,
 * privileged, a 48-bit four-level page table, and `id` - 21 bits, the
 * high dword - which the status buffer names it by.
 */
uint64_t igpu_lrc_descriptor(uint32_t lrca, uint32_t id);

/* The four dwords written to the submit port for these two elements, in
 * the order they must be: element 1 high and low, then element 0 - the
 * last write is what submits (`write_desc`). An empty element is 0. */
void igpu_elsp_words(uint64_t element0, uint64_t element1, uint32_t out[4]);

/* A status-buffer entry, read. */
struct igpu_csb_event {
    uint32_t status;                         /* IGPU_CSB_ */
    uint32_t id;                             /* the context's, as its descriptor gave it */
};

/* The entries the engine wrote since `*read`, a pointer that is advanced:
 * at most IGPU_CSB_ENTRIES, from the status page's buffer and its write
 * pointer. Answers how many were put in `out`. */
unsigned igpu_csb_read(const volatile uint32_t *status_page, unsigned *read,
                       struct igpu_csb_event *out);

/*
 * **The page table.** Its pages come from whoever builds it: `page` gives
 * a zeroed one - its address for the CPU and its physical address - and
 * `cpu` finds a page already given by its physical address.
 */
struct igpu_pt_pages {
    void *(*page)(void *ctx, uint64_t *phys);
    void *(*cpu)(void *ctx, uint64_t phys);
    void *ctx;
};

struct igpu_ppgtt {
    uint64_t            *pml4;
    uint64_t             pml4_phys;
    struct igpu_pt_pages pages;
    unsigned             tables;             /* pages given it, the root among them */
};

/* Private-PAT indices an entry chooses (`bdw_setup_private_ppat`, which
 * the driver programs): 0 write-back in the LLC, 3 uncached, 4 write-back
 * in the LLC and eLLC. */
#define IGPU_PAT_WB_LLC          0u
#define IGPU_PAT_UC              3u
#define IGPU_PAT_WB_LLCELLC      4u

/* The 64 bits GEN8_PRIVATE_PAT_LO and _HI are written with, i915's table. */
uint64_t igpu_private_pat(void);

bool igpu_ppgtt_init(struct igpu_ppgtt *pt, const struct igpu_pt_pages *pages);

/* One page mapped: `gpu` and `phys` page-aligned, `gpu` below 2^48. False
 * when a table could not be had, or the address is out of range. */
bool igpu_ppgtt_map(struct igpu_ppgtt *pt, uint64_t gpu, uint64_t phys, unsigned pat);

/* A mapping undone: its entry cleared, the tables kept. */
void igpu_ppgtt_unmap(struct igpu_ppgtt *pt, uint64_t gpu);

/* Where `gpu` goes, by walking the four levels, and the entry's PAT index;
 * false when it is not mapped. */
bool igpu_ppgtt_lookup(const struct igpu_ppgtt *pt, uint64_t gpu, uint64_t *phys, unsigned *pat);

/* A page-table entry, as `gen8_pte_encode` makes one; and an upper level's,
 * as `gen8_pde_encode` does - always index 0, since bit 7 there means a
 * large page rather than PAT. */
uint64_t igpu_pte(uint64_t phys, unsigned pat);
uint64_t igpu_pde(uint64_t phys);

#endif
