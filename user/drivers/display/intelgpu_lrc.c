/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Gen9's context images, descriptors, status buffer and page tables
 * (`intelgpu_lrc.h`, `docs/gpu3d.md` G6a). Memory only: the driver writes
 * the registers.
 */

#include "intelgpu_lrc.h"

#include <string.h>

/*
 * **Which registers an image holds, and in what order** - i915's
 * `gen9_rcs_offsets` and `gen9_xcs_offsets` (`intel_lrc.c`, Linux 6.12,
 * MIT), byte for byte in i915's own encoding so that the two can be read
 * side by side:
 *
 *   NOP(x)        skip x dwords                     1xxxxxxx
 *   LRI(n, f)     MI_LOAD_REGISTER_IMM of n pairs   ffnnnnnn, f bit 0 = posted
 *   REG(x)        a register below 0x200            0rrrrrrr  (x >> 2)
 *   REG16(x)      one below 0x10000, two bytes      1hhhhhhh 0lllllll
 *   END           0
 */
#define NOP(x)       (0x80u | (x))
#define LRI(n, f)    (((f) << 6) | (n))
#define POSTED       1u
#define REG(x)       ((x) >> 2)
#define REG16(x)     (((x) >> 9) | 0x80u), (((x) >> 2) & 0x7fu)
#define END          0u

/* The 44 registers both tables end with. */
#define COMMON_TAIL \
    NOP(13), \
    LRI(44, POSTED), \
    REG(0x028), REG(0x09c), REG(0x0c0), REG(0x178), REG(0x17c), REG16(0x358), \
    REG(0x170), REG(0x150), REG(0x154), REG(0x158), REG16(0x41c), \
    REG16(0x600), REG16(0x604), REG16(0x608), REG16(0x60c), REG16(0x610), \
    REG16(0x614), REG16(0x618), REG16(0x61c), REG16(0x620), REG16(0x624), \
    REG16(0x628), REG16(0x62c), REG16(0x630), REG16(0x634), REG16(0x638), \
    REG16(0x63c), REG16(0x640), REG16(0x644), REG16(0x648), REG16(0x64c), \
    REG16(0x650), REG16(0x654), REG16(0x658), REG16(0x65c), REG16(0x660), \
    REG16(0x664), REG16(0x668), REG16(0x66c), REG16(0x670), REG16(0x674), \
    REG16(0x678), REG16(0x67c), REG(0x068), \
    END

/* The 23 registers both tables begin with: the context's control, the
 * ring, the batch, and the page directories. */
#define COMMON_HEAD \
    NOP(1), \
    LRI(14, POSTED), \
    REG16(0x244), REG(0x034), REG(0x030), REG(0x038), REG(0x03c), REG(0x168), \
    REG(0x140), REG(0x110), REG(0x11c), REG(0x114), REG(0x118), REG(0x1c0), \
    REG(0x1c4), REG(0x1c8), \
    NOP(3), \
    LRI(9, POSTED), \
    REG16(0x3a8), REG16(0x28c), REG16(0x288), REG16(0x284), REG16(0x280), \
    REG16(0x27c), REG16(0x278), REG16(0x274), REG16(0x270)

static const uint8_t gen9_rcs_offsets[] = {
    COMMON_HEAD,
    NOP(13),
    LRI(1, 0),
    REG(0x0c8),                          /* R_PWR_CLK_STATE: the render engine's slices */
    COMMON_TAIL
};

static const uint8_t gen9_xcs_offsets[] = {
    COMMON_HEAD,
    NOP(13),
    LRI(1, POSTED),
    REG16(0x200),
    COMMON_TAIL
};

#undef NOP
#undef LRI
#undef POSTED
#undef REG
#undef REG16
#undef END

/* Command encodings (`intel_gpu_commands.h`). */
#define MI_LOAD_REGISTER_IMM(n)  ((0x22u << 23) | (2u * (n) - 1u))
#define MI_LRI_FORCE_POSTED      (1u << 12)

/* Registers the image is filled with, from the engine's base
 * (`intel_engine_regs.h`). */
#define R_CONTEXT_CONTROL  0x244u
#define R_RING_HEAD        0x034u
#define R_RING_TAIL        0x030u
#define R_RING_START       0x038u
#define R_RING_CTL         0x03cu
#define R_BB_STATE         0x110u
#define R_PDP0_LDW         0x270u
#define R_PDP0_UDW         0x274u
#define R_MI_MODE          0x09cu

#define CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT (1u << 0)
#define CTX_CTRL_RS_CTX_ENABLE              (1u << 1)
#define CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT    (1u << 2)
#define CTX_CTRL_INHIBIT_SYN_CTX_SWITCH     (1u << 3)
#define STOP_RING                           (1u << 8)
#define RING_VALID                          (1u << 0)

#define MASKED_ENABLE(b)   (((b) << 16) | (b))
#define MASKED_DISABLE(b)  ((b) << 16)

/* `set_offsets`, without its close: the commands and the registers'
 * addresses laid out, the values left for the filling. */
static void lay_out(uint32_t *regs, const uint8_t *data, uint32_t base)
{
    while (*data) {
        uint8_t count, flags;

        if (*data & 0x80u) {                 /* a skip */
            regs += *data++ & 0x7fu;
            continue;
        }

        count = *data & 0x3fu;
        flags = (uint8_t)(*data >> 6);
        data++;

        *regs = MI_LOAD_REGISTER_IMM(count);
        if (flags & 1u) {
            *regs |= MI_LRI_FORCE_POSTED;
        }
        regs++;

        do {
            uint32_t offset = 0;
            uint8_t v;

            do {
                v = *data++;
                offset <<= 7;
                offset |= v & 0x7fu;
            } while (v & 0x80u);

            regs[0] = base + (offset << 2);
            regs += 2;
        } while (--count);
    }
}

int igpu_lrc_find(const uint32_t *state, uint32_t base, uint32_t offset)
{
    unsigned i = 0;

    /* Walk the commands rather than search the page: an address is only
     * an address where a load says it is, and a value could look like one. */
    while (i < 1024u) {
        uint32_t dw = state[i];

        if ((dw >> 23) == 0x22u) {
            unsigned pairs = ((dw & 0xffu) + 1u) / 2u, k;

            for (k = 0; k < pairs && i + 2u + 2u * k <= 1023u; k++) {
                if (state[i + 1u + 2u * k] == base + offset) {
                    return (int)(i + 2u + 2u * k);
                }
            }

            i += 1u + 2u * pairs;
        } else {
            i++;                             /* a skipped dword */
        }
    }

    return -1;
}

bool igpu_lrc_fill(uint32_t *state, enum igpu_lrc_kind kind, const struct igpu_lrc_init *in)
{
    if (in->ring_pages == 0 || in->ring_pages > 512u || (in->ring_ggtt & 0xfffu) != 0) {
        return false;
    }

    memset(state, 0, 4096);
    lay_out(state, kind == IGPU_LRC_RENDER ? gen9_rcs_offsets : gen9_xcs_offsets, in->base);
    return igpu_lrc_patch(state, in, false);
}

/*
 * What a context's own image says, written over a laid-out one - a new
 * image, or one copied from the engine's default state (G6e): the context
 * control, no batch running, the page table, STOP_RING off, and the ring.
 */
bool igpu_lrc_patch(uint32_t *state, const struct igpu_lrc_init *in, bool restore)
{
    int at;

    if (in->ring_pages == 0 || in->ring_pages > 512u || (in->ring_ggtt & 0xfffu) != 0) {
        return false;
    }

    /* `init_common_regs`, for a context never run: switching to it does
     * not restore what is in an image it never saved, saving it back is
     * on, and so is the engine's own context state. An image copied from
     * the engine's default state is restored: that is what it is for. */
    if ((at = igpu_lrc_find(state, in->base, R_CONTEXT_CONTROL)) < 0) {
        return false;
    }
    state[at] = restore ? igpu_lrc_restore_control()
              : MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH)
              | MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT)
              | CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT
              | MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);

    if ((at = igpu_lrc_find(state, in->base, R_BB_STATE)) < 0) {
        return false;
    }
    state[at] = 0;                           /* `lrc_ring_bb_offset`: no batch running */

    /* `init_ppgtt_regs`: four levels, so PDP0 holds the root and the
     * others are ignored. */
    if ((at = igpu_lrc_find(state, in->base, R_PDP0_UDW)) < 0) {
        return false;
    }
    state[at] = (uint32_t)(in->pml4 >> 32);
    if ((at = igpu_lrc_find(state, in->base, R_PDP0_LDW)) < 0) {
        return false;
    }
    state[at] = (uint32_t)in->pml4;

    /* `__reset_stop_ring`. */
    if ((at = igpu_lrc_find(state, in->base, R_MI_MODE)) < 0) {
        return false;
    }
    state[at] = MASKED_DISABLE(STOP_RING);

    /* `lrc_update_regs`: the ring. RING_CTL_SIZE is (pages - 1) << 12. */
    if ((at = igpu_lrc_find(state, in->base, R_RING_START)) < 0) {
        return false;
    }
    state[at] = in->ring_ggtt;
    if ((at = igpu_lrc_find(state, in->base, R_RING_HEAD)) < 0) {
        return false;
    }
    state[at] = in->head;
    if ((at = igpu_lrc_find(state, in->base, R_RING_CTL)) < 0) {
        return false;
    }
    state[at] = ((in->ring_pages - 1u) << 12) | RING_VALID;

    return igpu_lrc_set_tail(state, in->base, in->tail);
}

/* The context control a never-run image is given: its restore inhibited. */
uint32_t igpu_lrc_inhibit_control(void)
{
    return MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH)
         | MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT)
         | CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT
         | MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
}

uint32_t igpu_lrc_restore_control(void)
{
    return MASKED_ENABLE(CTX_CTRL_INHIBIT_SYN_CTX_SWITCH)
         | MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_RESTORE_INHIBIT)
         | MASKED_DISABLE(CTX_CTRL_ENGINE_CTX_SAVE_INHIBIT | CTX_CTRL_RS_CTX_ENABLE);
}

bool igpu_lrc_set_tail(uint32_t *state, uint32_t base, uint32_t tail)
{
    int at = igpu_lrc_find(state, base, R_RING_TAIL);

    if (at < 0) {
        return false;
    }

    state[at] = tail;
    return true;
}

/* `intel_lrc.h`. */
#define GEN8_CTX_VALID                   (1u << 0)
#define GEN8_CTX_PRIVILEGE               (1u << 8)
#define GEN8_CTX_ADDRESSING_MODE_SHIFT   3
#define INTEL_LEGACY_64B_CONTEXT         3u
#define GEN8_CTX_ID_WIDTH                21

uint64_t igpu_lrc_descriptor(uint32_t lrca, uint32_t id)
{
    uint32_t low = (lrca & ~0xfffu)
                 | (INTEL_LEGACY_64B_CONTEXT << GEN8_CTX_ADDRESSING_MODE_SHIFT)
                 | GEN8_CTX_VALID | GEN8_CTX_PRIVILEGE;

    return (uint64_t)(id & ((1u << GEN8_CTX_ID_WIDTH) - 1u)) << 32 | low;
}

void igpu_elsp_words(uint64_t element0, uint64_t element1, uint32_t out[4])
{
    out[0] = (uint32_t)(element1 >> 32);
    out[1] = (uint32_t)element1;
    out[2] = (uint32_t)(element0 >> 32);
    out[3] = (uint32_t)element0;
}

unsigned igpu_csb_read(const volatile uint32_t *status_page, unsigned *read,
                       struct igpu_csb_event *out)
{
    unsigned head = *read, tail = status_page[IGPU_CSB_WRITE_AT] & 0x7u, n = 0;

    if (tail >= IGPU_CSB_ENTRIES) {          /* 6 and 7 are not entries: nothing yet */
        return 0;
    }

    while (head != tail && n < IGPU_CSB_ENTRIES) {
        if (++head == IGPU_CSB_ENTRIES) {
            head = 0;
        }

        out[n].status = status_page[IGPU_CSB_AT + 2u * head];
        out[n].id = status_page[IGPU_CSB_AT + 2u * head + 1u];
        n++;
    }

    *read = head;
    return n;
}

/* `intel_gtt.h`: the entry's private-PAT index is three of x86's bits. */
#define PAGE_PRESENT   (1ull << 0)
#define PAGE_RW        (1ull << 1)
#define PAGE_PWT       (1ull << 3)
#define PAGE_PCD       (1ull << 4)
#define PAGE_PAT       (1ull << 7)
#define ADDRESS_MASK   0x0000fffffffff000ull

uint64_t igpu_pte(uint64_t phys, unsigned pat)
{
    return (phys & ADDRESS_MASK) | PAGE_PRESENT | PAGE_RW
         | ((pat & 1u) ? PAGE_PWT : 0) | ((pat & 2u) ? PAGE_PCD : 0) | ((pat & 4u) ? PAGE_PAT : 0);
}

uint64_t igpu_pde(uint64_t phys)
{
    return (phys & ADDRESS_MASK) | PAGE_PRESENT | PAGE_RW;     /* PPAT_CACHED_PDE: index 0 */
}

/* `GEN8_PPAT`, as `bdw_setup_private_ppat` writes it on Gen9. */
#define PPAT(i, x)   ((uint64_t)(x) << ((i) * 8))
#define PPAT_AGE(x)  ((x) << 4)
#define PPAT_LLCeLLC (3u << 2)
#define PPAT_LLCELLC (2u << 2)
#define PPAT_LLC     (1u << 2)
#define PPAT_WB      3u
#define PPAT_WC      1u
#define PPAT_UC      0u

uint64_t igpu_private_pat(void)
{
    return PPAT(0, PPAT_WB | PPAT_LLC)
         | PPAT(1, PPAT_WC | PPAT_LLCELLC)
         | PPAT(2, PPAT_WB)                  /* GEN8_PPAT_ELLC_OVERRIDE is 0 */
         | PPAT(3, PPAT_UC)
         | PPAT(4, PPAT_WB | PPAT_LLCELLC | PPAT_AGE(0))
         | PPAT(5, PPAT_WB | PPAT_LLCELLC | PPAT_AGE(1))
         | PPAT(6, PPAT_WB | PPAT_LLCELLC | PPAT_AGE(2))
         | PPAT(7, PPAT_WB | PPAT_LLCELLC | PPAT_AGE(3));
}

bool igpu_ppgtt_init(struct igpu_ppgtt *pt, const struct igpu_pt_pages *pages)
{
    pt->pages = *pages;
    pt->tables = 0;
    pt->pml4 = pages->page(pages->ctx, &pt->pml4_phys);

    if (pt->pml4 == NULL) {
        return false;
    }

    pt->tables = 1;
    return true;
}

static unsigned index_at(uint64_t gpu, unsigned level)    /* level 3 is the PML4 */
{
    return (unsigned)(gpu >> (12u + 9u * level)) & 511u;
}

/* The table below `table[i]`, made when `make` and it is not there. */
static uint64_t *below(struct igpu_ppgtt *pt, uint64_t *table, unsigned i, bool make)
{
    uint64_t phys;
    uint64_t *next;

    if (table[i] & PAGE_PRESENT) {
        return pt->pages.cpu(pt->pages.ctx, table[i] & ADDRESS_MASK);
    }

    if (!make || (next = pt->pages.page(pt->pages.ctx, &phys)) == NULL) {
        return NULL;
    }

    pt->tables++;
    table[i] = igpu_pde(phys);
    return next;
}

static uint64_t *leaf(struct igpu_ppgtt *pt, uint64_t gpu, bool make)
{
    uint64_t *table = pt->pml4;
    unsigned level;

    for (level = 3; level > 0 && table != NULL; level--) {
        table = below(pt, table, index_at(gpu, level), make);
    }

    return table == NULL ? NULL : &table[index_at(gpu, 0)];
}

bool igpu_ppgtt_map(struct igpu_ppgtt *pt, uint64_t gpu, uint64_t phys, unsigned pat)
{
    uint64_t *entry;

    if ((gpu & 0xfffu) != 0 || (phys & 0xfffu) != 0 || gpu >= (1ull << 48) || pat > 7u) {
        return false;
    }

    if ((entry = leaf(pt, gpu, true)) == NULL) {
        return false;
    }

    *entry = igpu_pte(phys, pat);
    return true;
}

void igpu_ppgtt_unmap(struct igpu_ppgtt *pt, uint64_t gpu)
{
    uint64_t *entry = gpu < (1ull << 48) ? leaf(pt, gpu, false) : NULL;

    if (entry != NULL) {
        *entry = 0;
    }
}

bool igpu_ppgtt_lookup(const struct igpu_ppgtt *pt, uint64_t gpu, uint64_t *phys, unsigned *pat)
{
    uint64_t *entry;

    if (gpu >= (1ull << 48)) {
        return false;
    }

    entry = leaf((struct igpu_ppgtt *)pt, gpu & ~0xfffull, false);

    if (entry == NULL || (*entry & PAGE_PRESENT) == 0) {
        return false;
    }

    *phys = (*entry & ADDRESS_MASK) | (gpu & 0xfffu);
    *pat = ((*entry & PAGE_PWT) ? 1u : 0) | ((*entry & PAGE_PCD) ? 2u : 0) | ((*entry & PAGE_PAT) ? 4u : 0);
    return true;
}
