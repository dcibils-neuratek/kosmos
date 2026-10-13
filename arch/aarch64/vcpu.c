/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Virtual CPUs and the second stage of translation, the C half of
 * `hyp.S` - `docs/virtualization.md` step V1.
 *
 * What is here is the mechanism and nothing above it: whether this core
 * can enter a guest, and a guest's memory as the processor's second stage
 * of translation sees it. Which guest, with what, for whom - the
 * capability objects and the monitor - are V2 and V3, and none of that
 * belongs in this file.
 */

#include <stddef.h>
#include <stdint.h>

#include "context.h"
#include "cpu.h"
#include "hyp.h"
#include "mmu.h"
#include "page.h"
#include "../../kernel/percpu.h"
#include "../../kernel/pmm.h"
#include "../../kernel/syscall.h"
#include "../../kernel/vm.h"

_Static_assert(HYP_CORES >= NR_CPUS, "hyp.S keeps a flag and a stack per core");
_Static_assert(offsetof(struct hyp_vcpu, pc) == HYP_OFF_PC, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, pstate) == HYP_OFF_PSTATE, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, sys) == HYP_OFF_SYS, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, host_sys) == HYP_OFF_HOST_SYS, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, host_gp) == HYP_OFF_HOST_GP, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, host_elr) == HYP_OFF_HOST_ELR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, host_spsr) == HYP_OFF_HOST_SPSR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, vttbr) == HYP_OFF_VTTBR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, flush) == HYP_OFF_FLUSH, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, exit) == HYP_OFF_EXIT, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, esr) == HYP_OFF_ESR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, far) == HYP_OFF_FAR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, hpfar) == HYP_OFF_HPFAR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, lr) == HYP_OFF_LR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, vmcr) == HYP_OFF_VMCR, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, cntv_ctl) == HYP_OFF_CNTV_CTL, "hyp.h");
_Static_assert(offsetof(struct hyp_vcpu, cntv_cval) == HYP_OFF_CNTV_CVAL, "hyp.h");
_Static_assert(sizeof(struct hyp_vcpu) == HYP_VCPU_BYTES, "hyp.h");
_Static_assert(HYP_CSSELR == 22, "hyp.S saves 23 registers");

extern volatile uint8_t hyp_ready[HYP_CORES];      /* hyp.S */

void fp_lend(struct context *guest);                /* fp.c */
void fp_take_back(struct context *guest);

/*
 * **Per core, not per machine.** A core is started at EL2 or not by
 * whoever starts it: the board's firmware for core zero, PSCI for the rest
 * - and QEMU's PSCI starts a core at the level its caller runs at, which is
 * EL1. A guest can only be entered on a core that arrived at EL2.
 */
bool hyp_present(void)
{
    unsigned core = this_cpu()->index;

    return core < HYP_CORES && hyp_ready[core] != 0;
}

/*
 * **A guest's memory, as the second stage sees it**: a guest physical
 * address in, a real one out, through three levels of 4 KB tables walked
 * from level 1 - the shape `hyp_install` wrote into VTCR_EL2, 39 bits of
 * guest address space.
 *
 * A page is mapped normal, write-back cached, inner shareable, readable
 * and writable, with its access flag set so the first touch does not
 * fault. What is not mapped faults to EL2, and the guest stops with the
 * address in HPFAR_EL2 - which is the whole of the isolation: a guest can
 * name only what these tables give it.
 */
#define S2_VALID        (1ul << 0)
#define S2_TABLE        (1ul << 1)          /* at levels 1 and 2 */
#define S2_PAGE         (1ul << 1)          /* at level 3 */
#define S2_NORMAL_WB    (0xful << 2)        /* MemAttr: outer and inner WB */
#define S2_RW           (3ul << 6)          /* S2AP */
#define S2_INNER        (3ul << 8)          /* SH */
#define S2_AF           (1ul << 10)
#define S2_ADDR         0x0000fffffffff000ul

static uint64_t *zeroed_page(void)
{
    uint64_t *p = pmm_alloc_page();
    unsigned i;

    if (p == NULL) {
        return NULL;
    }

    for (i = 0; i < PAGE_SIZE / sizeof(uint64_t); i++) {
        p[i] = 0;
    }

    return p;
}

uint64_t *stage2_new(void)
{
    return zeroed_page();
}

/* The table one level down from `table[index]`, made if `make`. */
static uint64_t *below(uint64_t *table, unsigned index, bool make)
{
    uint64_t *next;

    if ((table[index] & S2_VALID) != 0) {
        return phys_to_virt(table[index] & S2_ADDR);
    }

    if (!make || (next = zeroed_page()) == NULL) {
        return NULL;
    }

    table[index] = virt_to_phys(next) | S2_TABLE | S2_VALID;
    return next;
}

bool stage2_map(uint64_t *root, uint64_t ipa, uintptr_t pa)
{
    uint64_t *l2, *l3;

    if ((ipa | pa) & (PAGE_SIZE - 1) || ipa >= (1ul << 39)) {
        return false;
    }

    if ((l2 = below(root, (unsigned)(ipa >> 30) & 511, true)) == NULL
        || (l3 = below(l2, (unsigned)(ipa >> 21) & 511, true)) == NULL) {
        return false;
    }

    l3[(ipa >> 12) & 511] = (pa & S2_ADDR) | S2_AF | S2_INNER | S2_RW
                            | S2_NORMAL_WB | S2_PAGE | S2_VALID;
    return true;
}

/* The tables, not what they map: the guest's pages are its owner's. */
void stage2_free(uint64_t *root)
{
    unsigned i, j;

    for (i = 0; i < 512; i++) {
        uint64_t *l2 = below(root, i, false);

        if (l2 == NULL) {
            continue;
        }

        for (j = 0; j < 512; j++) {
            uint64_t *l3 = below(l2, j, false);

            if (l3 != NULL) {
                pmm_free_page(l3);
            }
        }

        pmm_free_page(l2);
    }

    pmm_free_page(root);
}

/*
 * **Code written here and run there.** The kernel writes a guest's
 * instructions through its data cache; a guest running with its own
 * translation off fetches them uncached. So each line is cleaned to the
 * point of coherency (`dc cvac`), then the instruction cache dropped
 * (`ic iallu`) - with a `dsb` after each so the second does not begin
 * before the first has finished, and an `isb` last. QEMU models no caches
 * and would run without it; a real core would run what was there before.
 */
void stage2_code_written(const void *start, unsigned long bytes)
{
    uintptr_t at = (uintptr_t)start & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)start + bytes;

    for (; at < end; at += 64) {
        __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    }

    __asm__ volatile("dsb ish\n\tic iallu\n\tdsb ish\n\tisb" ::: "memory");
}

/*
 * **The architecture's half of `kernel/vm.h`**: a guest's tables are a
 * stage-2 root, its tag a VMID, and a vCPU's state a `hyp_vcpu` in a page
 * of its own - EL2 reads it with its translation off, at the address the
 * identity map gives it.
 */
bool arch_vm_available(void)
{
    unsigned i;

    for (i = 0; i < HYP_CORES; i++) {
        if (hyp_ready[i] != 0) {
            return true;
        }
    }

    return false;
}

bool arch_vm_here(void)
{
    return hyp_present();
}

/* 8 or 16 bits of VMID, as ID_AA64MMFR1_EL1.VMIDBits says - and 0 is the
 * host's, which is why it is one fewer. */
unsigned arch_vm_tags(void)
{
    uint64_t mmfr1;

    __asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));

    return ((mmfr1 >> 4) & 0xf) == 2 ? 65535u : 255u;
}

void *arch_guest_tables(void)
{
    return stage2_new();
}

bool arch_guest_map(void *tables, uint64_t ipa, uintptr_t pa)
{
    return stage2_map(tables, ipa, pa);
}

void arch_guest_free(void *tables)
{
    if (tables != NULL) {
        stage2_free(tables);
    }
}

/*
 * A vCPU as reset: at EL1 with its own translation off, every interrupt
 * masked, and SCTLR_EL1 holding only its RES1 bits - where an operating
 * system expects to be started. Its tables' translations dropped on its
 * first entry, since its VMID may have been somebody else's.
 */
void *arch_vcpu_new(void *tables, unsigned tag)
{
    struct hyp_vcpu *v = pmm_alloc_page();
    unsigned char *b = (unsigned char *)v;
    unsigned i;

    if (v == NULL) {
        return NULL;
    }

    for (i = 0; i < sizeof(*v); i++) {
        b[i] = 0;
    }

    v->vttbr = virt_to_phys(tables) | ((uint64_t)tag << 48);
    v->flush = 1;
    v->pstate = 0x3c5;                  /* EL1h, D A I F masked */
    v->sys[HYP_SCTLR] = 0x30d00800;
    v->vmcr = 0;                        /* everything masked until it says */
    return v;
}

void arch_vcpu_free(void *state)
{
    if (state != NULL) {
        pmm_free_page(state);
    }
}

/* The guest's FP and SIMD registers, kept in its page after the vCPU. */
#define GUEST_FP_AT     1024u

_Static_assert(GUEST_FP_AT >= sizeof(struct hyp_vcpu)
               && GUEST_FP_AT + sizeof(struct context) <= PAGE_SIZE,
               "a vCPU and its FP registers share a page");

long arch_vcpu_run(void *state, struct vcpu_regs *io)
{
    struct hyp_vcpu *v = state;
    struct context *fp = (struct context *)((char *)state + GUEST_FP_AT);
    unsigned long why;
    uint64_t daif;
    unsigned i;

    for (i = 0; i < 31; i++) {
        v->gp[i] = io->x[i];
    }

    v->pc = io->pc;
    v->pstate = io->pstate;

    for (i = 0; i < HYP_LRS; i++) {
        v->lr[i] = io->lr[i];
    }

    daif = cpu_interrupts_save();
    fp_lend(fp);
    why = hyp_run(v);
    fp_take_back(fp);
    cpu_interrupts_restore(daif);

    for (i = 0; i < 31; i++) {
        io->x[i] = v->gp[i];
    }

    io->pc = v->pc;
    io->pstate = v->pstate;

    for (i = 0; i < HYP_LRS; i++) {
        io->lr[i] = v->lr[i];
    }

    io->timer_ctl = v->cntv_ctl;
    io->timer_cval = v->cntv_cval;
    io->exit = why == HYP_EXIT_SYNC ? VCPU_EXIT_TRAP : VCPU_EXIT_HOST;
    io->syndrome = v->esr;
    io->address = v->far;
    io->ipa = (v->hpfar >> 4) << 12;
    return (long)io->exit;
}
