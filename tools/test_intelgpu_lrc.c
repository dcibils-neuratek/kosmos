/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Gen9's context images, descriptors, status buffer and page tables,
 * held on the Mac (`user/drivers/display/intelgpu_lrc.h`, `docs/gpu3d.md`
 * G6a). A wrong bit in a context image hangs the engine rather than
 * answering an error, so what can be checked without one is checked here.
 *
 * The expectations are not computed by the code under test: they are
 * i915's fixed indices (`intel_lrc_reg.h`, Linux 6.12), commands worked
 * out by hand from `intel_gpu_commands.h`, and a page-table walk of this
 * file's own with the address's bits cut where `intel_gtt.h` draws them.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../user/drivers/display/intelgpu_lrc.h"

static int failures, checks;

#define CHECK(cond, ...) do {                                   \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            printf("FAIL: " __VA_ARGS__);                       \
            printf("\n");                                       \
        }                                                       \
    } while (0)

/* `intel_lrc_reg.h`: where each value is in Gen8 to Gen12's image. */
#define CTX_CONTEXT_CONTROL   (0x02 + 1)
#define CTX_RING_HEAD         (0x04 + 1)
#define CTX_RING_TAIL         (0x06 + 1)
#define CTX_RING_START        (0x08 + 1)
#define CTX_RING_CTL          (0x0a + 1)
#define CTX_BB_STATE          (0x10 + 1)
#define CTX_TIMESTAMP         (0x22 + 1)
#define CTX_PDP3_UDW          (0x24 + 1)
#define CTX_PDP3_LDW          (0x26 + 1)
#define CTX_PDP2_UDW          (0x28 + 1)
#define CTX_PDP2_LDW          (0x2a + 1)
#define CTX_PDP1_UDW          (0x2c + 1)
#define CTX_PDP1_LDW          (0x2e + 1)
#define CTX_PDP0_UDW          (0x30 + 1)
#define CTX_PDP0_LDW          (0x32 + 1)
#define CTX_R_PWR_CLK_STATE   (0x42 + 1)
#define GEN9_CTX_RING_MI_MODE 0x54

static void image(enum igpu_lrc_kind kind, uint32_t base, const char *name)
{
    static uint32_t state[1024];
    struct igpu_lrc_init in = {
        .base = base, .ring_ggtt = 0x00abc000u, .ring_pages = 4, .head = 0x40, .tail = 0x80,
        .pml4 = 0x0000000123456000ull,
    };
    struct { uint32_t offset; int index; } at[] = {
        { 0x244, CTX_CONTEXT_CONTROL }, { 0x034, CTX_RING_HEAD }, { 0x030, CTX_RING_TAIL },
        { 0x038, CTX_RING_START }, { 0x03c, CTX_RING_CTL }, { 0x110, CTX_BB_STATE },
        { 0x3a8, CTX_TIMESTAMP }, { 0x28c, CTX_PDP3_UDW }, { 0x288, CTX_PDP3_LDW },
        { 0x284, CTX_PDP2_UDW }, { 0x280, CTX_PDP2_LDW }, { 0x27c, CTX_PDP1_UDW },
        { 0x278, CTX_PDP1_LDW }, { 0x274, CTX_PDP0_UDW }, { 0x270, CTX_PDP0_LDW },
        { 0x09c, GEN9_CTX_RING_MI_MODE + 1 },
    };
    unsigned i;
    bool zero_after = true;

    memset(state, 0x5a, sizeof state);       /* filling must clear what was there */
    CHECK(igpu_lrc_fill(state, kind, &in), "%s: the image was not filled", name);

    for (i = 0; i < sizeof at / sizeof at[0]; i++) {
        int found = igpu_lrc_find(state, base, at[i].offset);

        CHECK(found == at[i].index, "%s: register %#x's value at %d, i915 has it at %d",
              name, at[i].offset, found, at[i].index);
        CHECK(found < 1 || state[found - 1] == base + at[i].offset,
              "%s: the address before %#x's value is %#x", name, at[i].offset, state[found - 1]);
    }

    /* The loads, by hand: MI_LOAD_REGISTER_IMM is 0x22 << 23, its length
     * 2n - 1, MI_LRI_FORCE_POSTED bit 12. */
    CHECK(state[0x00] == 0, "%s: dword 0 is %#x, a NOP(1)", name, state[0]);
    CHECK(state[0x01] == 0x1100101bu, "%s: the first load is %#x, not 14 posted", name, state[0x01]);
    CHECK(state[0x21] == 0x11001011u, "%s: the second load is %#x, not 9 posted", name, state[0x21]);
    CHECK(state[0x41] == (kind == IGPU_LRC_RENDER ? 0x11000001u : 0x11001001u),
          "%s: the third load is %#x", name, state[0x41]);
    CHECK(state[0x42] == base + (kind == IGPU_LRC_RENDER ? 0x0c8u : 0x200u),
          "%s: the third load's register is %#x", name, state[0x42]);
    CHECK(state[0x51] == 0x11001057u, "%s: the fourth load is %#x, not 44 posted", name, state[0x51]);
    CHECK(state[0xa8] == base + 0x068u, "%s: the last register is %#x, not 0x68's", name, state[0xa8]);

    if (kind == IGPU_LRC_RENDER) {
        CHECK(igpu_lrc_find(state, base, 0x0c8) == CTX_R_PWR_CLK_STATE,
              "%s: R_PWR_CLK_STATE is not at i915's index", name);
    }

    for (i = 0xaa; i < 1024; i++) {
        zero_after = zero_after && state[i] == 0;
    }
    CHECK(zero_after, "%s: the page after the last register is not zero", name);

    /* The values, by hand from `init_common_regs`: SYN_CTX_SWITCH inhibited
     * (0x00080008), restore inhibited for a first run (mask 0x00010000 and
     * the bit), save and RS enabled (mask 0x00060000). */
    CHECK(state[CTX_CONTEXT_CONTROL] == 0x000f0009u, "%s: context control %#x", name,
          state[CTX_CONTEXT_CONTROL]);
    /* And run again (G6e): the same masks, the restore no longer inhibited. */
    CHECK(igpu_lrc_restore_control() == 0x000f0008u, "%s: a saved image's context control %#x", name,
          igpu_lrc_restore_control());
    CHECK(igpu_lrc_inhibit_control() == 0x000f0009u, "%s: an inhibited context control %#x", name,
          igpu_lrc_inhibit_control());
    {
        /* Patched over a copy whose registers hold something else - the
         * engine's default state - it says what a new image says, but the
         * control restored, and leaves every other register as it was. */
        static uint32_t copy[1024];
        struct igpu_lrc_init other = in;
        unsigned i, differ = 0;

        for (i = 0; i < 1024u; i++) {
            copy[i] = state[i];
        }
        copy[CTX_TIMESTAMP] = 0x12345678u;
        other.ring_ggtt = 0x00def000u;
        CHECK(igpu_lrc_patch(copy, &other, true), "%s: the patch refused", name);
        CHECK(copy[CTX_CONTEXT_CONTROL] == 0x000f0008u && copy[CTX_RING_START] == 0x00def000u
              && copy[CTX_TIMESTAMP] == 0x12345678u,
              "%s: the patch wrote control %#x, ring %#x, timestamp %#x", name, copy[CTX_CONTEXT_CONTROL],
              copy[CTX_RING_START], copy[CTX_TIMESTAMP]);
        for (i = 0; i < 1024u; i++) {
            differ += copy[i] != state[i];
        }
        CHECK(differ == 3, "%s: the patch changed %u dwords, not control, ring and timestamp", name, differ);
    }
    CHECK(state[CTX_RING_START] == 0x00abc000u, "%s: ring start %#x", name, state[CTX_RING_START]);
    CHECK(state[CTX_RING_HEAD] == 0x40u && state[CTX_RING_TAIL] == 0x80u, "%s: head %#x tail %#x",
          name, state[CTX_RING_HEAD], state[CTX_RING_TAIL]);
    CHECK(state[CTX_RING_CTL] == 0x3001u, "%s: ring control %#x, not four pages valid", name,
          state[CTX_RING_CTL]);
    CHECK(state[CTX_PDP0_UDW] == 0x1u && state[CTX_PDP0_LDW] == 0x23456000u,
          "%s: the root %#x:%#x", name, state[CTX_PDP0_UDW], state[CTX_PDP0_LDW]);
    CHECK(state[CTX_PDP1_LDW] == 0 && state[CTX_PDP3_UDW] == 0, "%s: other PDPs not empty", name);
    CHECK(state[GEN9_CTX_RING_MI_MODE + 1] == 0x01000000u, "%s: MI_MODE %#x, not STOP_RING off",
          name, state[GEN9_CTX_RING_MI_MODE + 1]);
    CHECK(state[CTX_BB_STATE] == 0, "%s: BB_STATE %#x", name, state[CTX_BB_STATE]);

    CHECK(igpu_lrc_set_tail(state, base, 0x1f8) && state[CTX_RING_TAIL] == 0x1f8u,
          "%s: the tail did not move", name);

    /* Controls: a value that looks like an address is not found as one,
     * and a ring no size or not page-aligned is refused. */
    state[CTX_RING_START] = base + 0x200u;
    CHECK(kind == IGPU_LRC_RENDER ? igpu_lrc_find(state, base, 0x200) == -1
                                  : igpu_lrc_find(state, base, 0x200) == 0x43,
          "%s: a value was taken for an address", name);
    in.ring_pages = 0;
    CHECK(!igpu_lrc_fill(state, kind, &in), "%s: a ring of no pages was taken", name);
    in.ring_pages = 1;
    in.ring_ggtt = 0x00abc100u;
    CHECK(!igpu_lrc_fill(state, kind, &in), "%s: a ring not on a page was taken", name);
}

static void descriptors(void)
{
    uint32_t words[4];
    uint64_t d0 = igpu_lrc_descriptor(0x00123000u, 7), d1 = igpu_lrc_descriptor(0x00456fffu, 0x3fffff);

    /* By hand: the address, legacy 64-bit (3 << 3), valid, privileged. */
    CHECK(d0 == 0x0000000700123119ull, "descriptor %#llx", (unsigned long long)d0);
    CHECK(d1 == 0x001fffff00456119ull, "descriptor %#llx: the ID is 21 bits, the address a page's",
          (unsigned long long)d1);

    igpu_elsp_words(d0, d1, words);
    CHECK(words[0] == 0x001fffffu && words[1] == 0x00456119u && words[2] == 7u && words[3] == 0x00123119u,
          "the submit port's order %#x %#x %#x %#x", words[0], words[1], words[2], words[3]);
    igpu_elsp_words(d0, 0, words);
    CHECK(words[0] == 0 && words[1] == 0 && words[3] == 0x00123119u, "an empty second element");
}

static void status_buffer(void)
{
    static uint32_t page[1024];
    struct igpu_csb_event ev[IGPU_CSB_ENTRIES];
    unsigned read = IGPU_CSB_ENTRIES - 1u, n;

    /* As reset: the write pointer at the last entry, nothing to read. */
    memset(page, 0xff, sizeof page);
    page[IGPU_CSB_WRITE_AT] = 5;
    CHECK(igpu_csb_read(page, &read, ev) == 0 && read == 5, "a reset buffer read something");

    /* The engine wrote entries 0 and 1: idle to active, then complete. */
    page[0x10] = 0x1; page[0x11] = 7;
    page[0x12] = 0x18; page[0x13] = 7;
    page[IGPU_CSB_WRITE_AT] = 1;
    n = igpu_csb_read(page, &read, ev);
    CHECK(n == 2 && read == 1, "read %u, the pointer at %u", n, read);
    CHECK(n == 2 && ev[0].status == IGPU_CSB_IDLE_ACTIVE && ev[0].id == 7
          && ev[1].status == (IGPU_CSB_COMPLETE | IGPU_CSB_ACTIVE_IDLE) && ev[1].id == 7,
          "the events read are not the ones written");

    /* Round the end: from 1 to 0 is five entries, 2 to 5 and 0. */
    page[IGPU_CSB_WRITE_AT] = 0;
    n = igpu_csb_read(page, &read, ev);
    CHECK(n == 5 && read == 0, "round the end: %u read, at %u", n, read);

    /* A write pointer of 6 or 7 is no entry. */
    page[IGPU_CSB_WRITE_AT] = 7;
    CHECK(igpu_csb_read(page, &read, ev) == 0, "pointer 7 read as entries");
}

/* Pages for a page table: a pool whose physical addresses are made up. */
#define POOL 8192u                       /* about three tables a scattered page */
#define POOL_BASE 0x0000000200000000ull
static uint64_t pool[POOL][512];
static unsigned used;

static void *pool_page(void *ctx, uint64_t *phys)
{
    (void)ctx;
    if (used == POOL) {
        return NULL;
    }
    memset(pool[used], 0, sizeof pool[used]);
    *phys = POOL_BASE + (uint64_t)used * 4096u;
    return pool[used++];
}

static void *pool_cpu(void *ctx, uint64_t phys)
{
    (void)ctx;
    return pool[(phys - POOL_BASE) / 4096u];
}

/* The walk, this file's own: 47:39, 38:30, 29:21, 20:12. */
static int walk(uint64_t root, uint64_t gpu, uint64_t *phys)
{
    uint64_t entry = root | 1u;
    int shift;

    for (shift = 39; shift >= 12; shift -= 9) {
        uint64_t *table;

        if ((entry & 1u) == 0) {
            return 0;
        }
        if (shift != 39 && (entry & 0x80u) != 0) {
            return -1;                       /* an upper level marked large */
        }
        table = pool_cpu(NULL, entry & 0x0000fffffffff000ull);
        entry = table[(gpu >> shift) & 511u];
    }

    if ((entry & 3u) != 3u) {
        return 0;
    }

    *phys = entry & 0x0000fffffffff000ull;
    return 1;
}

static void page_tables(void)
{
    struct igpu_pt_pages pages = { pool_page, pool_cpu, NULL };
    struct igpu_ppgtt pt;
    static uint64_t gpus[2000], physs[2000];
    unsigned i, wrong = 0, missing = 0, pats = 0;
    uint64_t phys;
    unsigned pat;
    uint64_t seed = 0x4b6f736d6f73ull;

    /* By hand: present, writable, and the index in PWT 3, PCD 4, PAT 7. */
    CHECK(igpu_pte(0x1234000u, 0) == 0x1234003ull, "a write-back entry %#llx",
          (unsigned long long)igpu_pte(0x1234000u, 0));
    CHECK(igpu_pte(0x1234000u, 3) == 0x123401bull, "an uncached entry %#llx",
          (unsigned long long)igpu_pte(0x1234000u, 3));
    CHECK(igpu_pte(0x1234000u, 4) == 0x1234083ull, "an LLC and eLLC entry %#llx",
          (unsigned long long)igpu_pte(0x1234000u, 4));
    CHECK(igpu_pde(0x0fedcba987654321ull) == 0x0000cba987654003ull, "an upper entry %#llx",
          (unsigned long long)igpu_pde(0x0fedcba987654321ull));

    /* i915's private PAT on Gen9, by hand: WB|LLC, WC|LLCELLC, WB, UC,
     * then WB|LLCELLC at four ages. */
    CHECK(igpu_private_pat() == 0x3b2b1b0b00030907ull, "private PAT %#llx",
          (unsigned long long)igpu_private_pat());

    CHECK(igpu_ppgtt_init(&pt, &pages) && pt.tables == 1, "no root");

    /* Two thousand pages: the bottom and top of the range, neighbours
     * sharing tables, and the rest anywhere in the 48 bits. */
    for (i = 0; i < 2000; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        gpus[i] = i == 0 ? 0 : i == 1 ? (1ull << 48) - 4096u
                : i < 100 ? 0x0000100000000000ull + (uint64_t)i * 4096u
                : (seed >> 8) & 0x0000fffffffff000ull;
        physs[i] = 0x0000004000000000ull + (uint64_t)i * 4096u;
    }

    for (i = 0; i < 2000; i++) {
        CHECK(igpu_ppgtt_map(&pt, gpus[i], physs[i], i % 8u), "page %u at %#llx not mapped", i,
              (unsigned long long)gpus[i]);
    }

    /* A later mapping of the same address wins; the random ones may have
     * met, so each is checked against the last that set it. */
    for (i = 0; i < 2000; i++) {
        unsigned j, last = i;
        int r;

        for (j = i + 1; j < 2000; j++) {
            if (gpus[j] == gpus[i]) {
                last = j;
            }
        }

        r = walk(pt.pml4_phys, gpus[i], &phys);
        if (r != 1) {
            missing++;
        } else if (phys != physs[last]) {
            wrong++;
        }

        if (igpu_ppgtt_lookup(&pt, gpus[i] + 0x123u, &phys, &pat)) {
            pats += pat == last % 8u && phys == physs[last] + 0x123u;
        }
    }

    CHECK(missing == 0 && wrong == 0, "the walk: %u missing, %u to the wrong page", missing, wrong);
    CHECK(pats == 2000, "lookup gave the page and its index for %u of 2000", pats);

    /* The tables made: one root, and a neighbourhood of 98 pages sharing
     * one PDP, one PD and one PT. */
    CHECK(pt.tables == used, "the table count %u, the pool gave %u", pt.tables, used);
    CHECK(used < 1 + 3u * 2000u, "%u tables for 2000 pages", used);

    /* Controls: an address past 48 bits, one not on a page, a PAT index
     * past 7 are refused, and what was never mapped is not found. */
    CHECK(!igpu_ppgtt_map(&pt, 1ull << 48, 0x1000, 0), "an address past 48 bits mapped");
    CHECK(!igpu_ppgtt_map(&pt, 0x1800, 0x1000, 0), "an address off a page mapped");
    CHECK(!igpu_ppgtt_map(&pt, 0x2000, 0x1000, 8), "PAT index 8 mapped");
    CHECK(!igpu_ppgtt_lookup(&pt, 0x0000100000000000ull + 200u * 4096u, &phys, &pat),
          "an address never mapped was found");

    igpu_ppgtt_unmap(&pt, gpus[5]);
    CHECK(!igpu_ppgtt_lookup(&pt, gpus[5], &phys, &pat) && walk(pt.pml4_phys, gpus[5], &phys) == 0,
          "an address unmapped was still found");
    CHECK(igpu_ppgtt_lookup(&pt, gpus[6], &phys, &pat), "its neighbour went with it");
}

int main(void)
{
    image(IGPU_LRC_RENDER, 0x2000u, "render");
    image(IGPU_LRC_OTHER, IGPU_VIDEO_BASE, "video");
    image(IGPU_LRC_OTHER, 0x22000u, "blitter");
    descriptors();
    status_buffer();
    page_tables();

    if (failures) {
        printf("FAIL: %d of %d checks on Gen9's context images and page tables\n", failures, checks);
        return 1;
    }

    printf("PASS: %d checks on Gen9's context images and page tables - three engines' images at"
           " i915's indices, descriptors, the status buffer, 2000 pages walked\n", checks);
    return 0;
}
