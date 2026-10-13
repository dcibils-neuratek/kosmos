/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Virtual CPUs and the second stage of translation on x86-64: VT-x, the C
 * half of `vmx.S` - `docs/sandboxes.md` X1, and `kernel/vm.h`'s
 * architecture half as `arch/aarch64/vcpu.c` is on ARM.
 *
 * The same four syscalls and the same objects above it: a guest's tables
 * are EPT, Intel's second stage, and a vCPU is a VMCS with the pages it
 * needs beside it. What a guest *sees* - its serial port, its timer, its
 * disk - is the monitor's, in user space, exactly as on ARM; this file
 * enters a guest and says why it came back.
 *
 * Every constant here is from the Intel SDM vol. 3 (§24 the VMCS, §25
 * what non-root operation exits on, §26 entry, §27 exit, §28 EPT,
 * appendix A the capability MSRs, appendix B the field encodings, appendix
 * C the exit reasons), checked against Linux v6.18's `asm/vmx.h` and
 * `asm/msr-index.h`, which spell them out.
 *
 * **What a core needs before it can enter a guest is VMX operation**,
 * which `vmxon` starts and nothing here stops: CR4.VMXE set, CR0 and CR4
 * inside what the processor allows (`IA32_VMX_CR*_FIXED*`), and a page of
 * its own the processor keeps. Done the first time a vCPU runs on that
 * core rather than at boot, so a machine that never runs a guest never
 * enters it.
 *
 * **A VMCS is cleared after every run**, so each entry is a `vmlaunch`
 * and the next run may be on any core. A thread does not migrate, but a
 * vCPU is run by whichever thread of its process asks (`vm.h`), and a VMCS
 * current on one core must be cleared there before another may load it -
 * the alternative is remembering where each one was and sending the core
 * an interrupt to let go. A `vmclear` and a `vmptrld` a run is the price,
 * and it can be measured later.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "mmu.h"
#include "page.h"
#include "../../kernel/percpu.h"
#include "../../kernel/pmm.h"
#include "../../kernel/syscall.h"
#include "../../kernel/vm.h"

long vmx_run(uint64_t *gpr);                        /* vmx.S */

/*
 * **The probe** (`docs/sandboxes.md`, S5): whether the processor has VMX,
 * and what its firmware decided. Intel SDM vol. 2, CPUID leaf 1, ECX bit 5
 * is VMX; vol. 3 §24.7 and vol. 4, `IA32_FEATURE_CONTROL` (MSR 0x3A): bit
 * 0 locks it until reset, bit 2 allows VMXON outside SMX. The MSR is read
 * only when CPUID says VMX exists - on a processor without it, reading it
 * faults.
 */
#define CPUID_VMX               (1u << 5)
#define MSR_FEATURE_CONTROL     0x3Au
#define FEATURE_LOCKED          (1u << 0)
#define FEATURE_VMX_OUTSIDE_SMX (1u << 2)

/* The capability MSRs (appendix A). */
#define MSR_VMX_BASIC           0x480u
#define MSR_VMX_PINBASED        0x481u
#define MSR_VMX_PROCBASED       0x482u
#define MSR_VMX_EXIT            0x483u
#define MSR_VMX_ENTRY           0x484u
#define MSR_VMX_CR0_FIXED0      0x486u
#define MSR_VMX_CR0_FIXED1      0x487u
#define MSR_VMX_CR4_FIXED0      0x488u
#define MSR_VMX_CR4_FIXED1      0x489u
#define MSR_VMX_PROCBASED2      0x48Bu
#define MSR_VMX_EPT_VPID        0x48Cu
#define MSR_VMX_TRUE_PIN        0x48Du
#define MSR_VMX_TRUE_PROC       0x48Eu
#define MSR_VMX_TRUE_EXIT       0x48Fu
#define MSR_VMX_TRUE_ENTRY      0x490u

#define BASIC_TRUE_CONTROLS     (1ul << 55)

#define EPT_CAP_WALK4           (1ul << 6)
#define EPT_CAP_WB              (1ul << 14)
#define EPT_CAP_INVEPT          (1ul << 20)
#define EPT_CAP_INVEPT_ALL      (1ul << 26)

/* The MSRs a guest's `syscall` and `swapgs` use, and the host's EFER. */
#define MSR_EFER                0xC0000080u
#define MSR_STAR                0xC0000081u
#define MSR_LSTAR               0xC0000082u
#define MSR_CSTAR               0xC0000083u
#define MSR_SFMASK              0xC0000084u
#define MSR_GS_BASE             0xC0000101u
#define MSR_KERNEL_GS_BASE      0xC0000102u
#define MSR_SYSENTER_CS         0x174u
#define MSR_SYSENTER_ESP        0x175u
#define MSR_SYSENTER_EIP        0x176u

#define EFER_LMA                (1ul << 10)
#define CR4_VMXE                (1ul << 13)
#define RFLAGS_IF               (1ul << 9)

/* The controls asked for (§24.6 to §24.8). */
#define PIN_EXTERNAL            (1u << 0)
#define PIN_NMI                 (1u << 3)
#define PROC_WINDOW             (1u << 2)
#define PROC_HLT                (1u << 7)
#define PROC_CR3_LOAD           (1u << 15)
#define PROC_CR3_STORE          (1u << 16)
#define PROC_IO                 (1u << 24)
#define PROC_MSR_BITMAP         (1u << 28)
#define PROC_SECONDARY          (1u << 31)
#define PROC2_EPT               (1u << 1)
#define EXIT_HOST_64            (1u << 9)
#define EXIT_SAVE_EFER          (1u << 20)
#define EXIT_LOAD_EFER          (1u << 21)
#define ENTRY_IA32E             (1u << 9)
#define ENTRY_LOAD_EFER         (1u << 15)

/* VMCS fields (appendix B). */
#define F_MSR_BITMAP            0x2004u
#define F_TSC_OFFSET            0x2010u
#define F_EPT_POINTER           0x201Au
#define F_GUEST_PHYSICAL        0x2400u
#define F_LINK_POINTER          0x2800u
#define F_GUEST_DEBUGCTL        0x2802u
#define F_GUEST_EFER            0x2806u
#define F_HOST_EFER             0x2C02u
#define F_PIN                   0x4000u
#define F_PROC                  0x4002u
#define F_EXCEPTIONS            0x4004u
#define F_PF_MASK               0x4006u
#define F_PF_MATCH              0x4008u
#define F_CR3_TARGETS           0x400Au
#define F_EXIT                  0x400Cu
#define F_EXIT_MSR_STORES       0x400Eu
#define F_EXIT_MSR_LOADS        0x4010u
#define F_ENTRY                 0x4012u
#define F_ENTRY_MSR_LOADS       0x4014u
#define F_ENTRY_INTERRUPT       0x4016u
#define F_PROC2                 0x401Eu
#define F_INSTRUCTION_ERROR     0x4400u
#define F_EXIT_REASON           0x4402u
#define F_EXIT_INTERRUPT        0x4404u
#define F_EXIT_LENGTH           0x440Cu
#define F_GUEST_ES_LIMIT        0x4800u     /* then CS SS DS FS GS LDTR TR, 2 apart */
#define F_GUEST_GDTR_LIMIT      0x4810u
#define F_GUEST_IDTR_LIMIT      0x4812u
#define F_GUEST_ES_RIGHTS       0x4814u     /* the same order */
#define F_GUEST_INTERRUPTIBILITY 0x4824u
#define F_GUEST_ACTIVITY        0x4826u
#define F_GUEST_SYSENTER_CS     0x482Au
#define F_HOST_SYSENTER_CS      0x4C00u
#define F_CR0_MASK              0x6000u
#define F_CR4_MASK              0x6002u
#define F_CR0_SHADOW            0x6004u
#define F_CR4_SHADOW            0x6006u
#define F_QUALIFICATION         0x6400u
#define F_GUEST_CR0             0x6800u
#define F_GUEST_CR3             0x6802u
#define F_GUEST_CR4             0x6804u
#define F_GUEST_ES_BASE         0x6806u     /* the same order */
#define F_GUEST_GDTR_BASE       0x6816u
#define F_GUEST_IDTR_BASE       0x6818u
#define F_GUEST_DR7             0x681Au
#define F_GUEST_RSP             0x681Cu
#define F_GUEST_RIP             0x681Eu
#define F_GUEST_RFLAGS          0x6820u
#define F_GUEST_PENDING_DEBUG   0x6822u
#define F_GUEST_SYSENTER_ESP    0x6824u
#define F_GUEST_SYSENTER_EIP    0x6826u
#define F_GUEST_ES_SELECTOR     0x0800u     /* the same order */
#define F_HOST_ES_SELECTOR      0x0C00u     /* then CS SS DS FS GS TR */
#define F_HOST_CR0              0x6C00u
#define F_HOST_CR3              0x6C02u
#define F_HOST_CR4              0x6C04u
#define F_HOST_FS_BASE          0x6C06u
#define F_HOST_GS_BASE          0x6C08u
#define F_HOST_TR_BASE          0x6C0Au
#define F_HOST_GDTR_BASE        0x6C0Cu
#define F_HOST_IDTR_BASE        0x6C0Eu
#define F_HOST_SYSENTER_ESP     0x6C10u
#define F_HOST_SYSENTER_EIP     0x6C12u

/* The segment registers in the VMCS's order, and the guest's encodings of each. */
enum { SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS, SEG_LDTR, SEG_TR };

#define EXIT_EXCEPTION_NMI      0u
#define EXIT_EXTERNAL           1u
#define EXIT_FAILED_ENTRY       (1u << 31)
#define INTERRUPT_TYPE_NMI      (2u << 8)

/*
 * **What the processor offers, read once at boot.** A control asked for
 * is `(wanted | must) & may` - the low half of its MSR the bits that must
 * be 1, the high half those that may - and a machine that cannot have one
 * this file depends on cannot run a guest. The TRUE MSRs are used where
 * they exist (they do on every processor with EPT), since only they let CR3
 * loads and stores go unexited, which EPT makes safe.
 */
static struct {
    bool usable;
    uint32_t revision;
    uint32_t pin, proc, proc2, exit, entry;
    bool window;                    /* the interrupt window may be asked for */
    uint64_t cr0_must, cr0_may, cr4_must, cr4_may;
} vmx;

static uint8_t *vmxon_page[NR_CPUS];
static bool vmx_on[NR_CPUS];

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)) : "memory");
}

/* VMX instructions say failure in CF or ZF; `setna` is either. */
static inline bool vmwrite(uint64_t field, uint64_t value)
{
    uint8_t bad;

    __asm__ volatile("vmwrite %2, %1; setna %0"
                     : "=q"(bad) : "r"(field), "rm"(value) : "cc", "memory");
    return !bad;
}

static inline uint64_t vmread(uint64_t field)
{
    uint64_t value = 0;

    __asm__ volatile("vmread %1, %0" : "=rm"(value) : "r"(field) : "cc");
    return value;
}

static inline bool vmx_op(int which, uint64_t pa)
{
    uint8_t bad;

    if (which == 0) {
        __asm__ volatile("vmxon %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    } else if (which == 1) {
        __asm__ volatile("vmptrld %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    } else {
        __asm__ volatile("vmclear %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    }

    return !bad;
}

#define VMXON   0
#define VMPTRLD 1
#define VMCLEAR 2

/* Every guest translation this core holds, dropped (§28.3.3.1). */
static inline void invept_all(void)
{
    struct { uint64_t eptp, zero; } d = { 0, 0 };

    __asm__ volatile("invept %0, %1" :: "m"(d), "r"(2ul) : "cc", "memory");
}

static bool control(uint32_t msr, uint32_t wanted, uint32_t *out)
{
    uint64_t caps = rdmsr(msr);
    uint32_t must = (uint32_t)caps, may = (uint32_t)(caps >> 32);

    *out = (wanted | must) & may;
    return (*out & wanted) == wanted;
}

unsigned arch_vm_probe(void)
{
    uint32_t a, b, c, d, lo, hi;
    uint64_t basic, ept;
    bool true_msrs;
    unsigned state;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));

    if ((c & CPUID_VMX) == 0) {
        return VIRT_NONE;
    }

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_FEATURE_CONTROL));
    (void)hi;

    if ((lo & FEATURE_LOCKED) == 0) {
        return VIRT_UNLOCKED;
    }

    state = (lo & FEATURE_VMX_OUTSIDE_SMX) != 0 ? VIRT_ON : VIRT_OFF;

    if (state != VIRT_ON) {
        return state;
    }

    /*
     * On: what it offers. A VMCS of at most a page, write-back, and EPT
     * walked in four levels, write-back, with an INVEPT that drops
     * everything - which is all `invept_all` asks.
     */
    basic = rdmsr(MSR_VMX_BASIC);
    ept = rdmsr(MSR_VMX_EPT_VPID);
    true_msrs = (basic & BASIC_TRUE_CONTROLS) != 0;
    vmx.revision = (uint32_t)basic & 0x7fffffffu;

    vmx.usable = ((basic >> 32) & 0x1fff) <= PAGE_SIZE
        && ((basic >> 50) & 0xf) == 6
        && (ept & (EPT_CAP_WALK4 | EPT_CAP_WB | EPT_CAP_INVEPT | EPT_CAP_INVEPT_ALL))
               == (EPT_CAP_WALK4 | EPT_CAP_WB | EPT_CAP_INVEPT | EPT_CAP_INVEPT_ALL)
        && control(true_msrs ? MSR_VMX_TRUE_PIN : MSR_VMX_PINBASED,
                   PIN_EXTERNAL | PIN_NMI, &vmx.pin)
        && control(true_msrs ? MSR_VMX_TRUE_PROC : MSR_VMX_PROCBASED,
                   PROC_HLT | PROC_IO | PROC_MSR_BITMAP | PROC_SECONDARY, &vmx.proc)
        && control(MSR_VMX_PROCBASED2, PROC2_EPT, &vmx.proc2)
        && control(true_msrs ? MSR_VMX_TRUE_EXIT : MSR_VMX_EXIT,
                   EXIT_HOST_64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER, &vmx.exit)
        && control(true_msrs ? MSR_VMX_TRUE_ENTRY : MSR_VMX_ENTRY,
                   ENTRY_LOAD_EFER, &vmx.entry)
        && (rdmsr(true_msrs ? MSR_VMX_TRUE_ENTRY : MSR_VMX_ENTRY) >> 32 & ENTRY_IA32E) != 0
        && (vmx.proc & (PROC_CR3_LOAD | PROC_CR3_STORE)) == 0;

    vmx.window = (rdmsr(true_msrs ? MSR_VMX_TRUE_PROC : MSR_VMX_PROCBASED) >> 32
                  & PROC_WINDOW) != 0;
    vmx.cr0_must = rdmsr(MSR_VMX_CR0_FIXED0);
    vmx.cr0_may = rdmsr(MSR_VMX_CR0_FIXED1);
    vmx.cr4_must = rdmsr(MSR_VMX_CR4_FIXED0);
    vmx.cr4_may = rdmsr(MSR_VMX_CR4_FIXED1);
    return state;
}

bool arch_vm_available(void)
{
    return vmx.usable;
}

/* Every core is the same processor, and the firmware set them all. */
bool arch_vm_here(void)
{
    return vmx.usable && this_cpu()->index < NR_CPUS;
}

/*
 * No VPIDs: without them every entry and exit drops the guest's linear
 * translations and the host's, which costs speed and nothing else. The
 * tag `vm.c` hands out is not used, so there are as many as it likes.
 */
unsigned arch_vm_tags(void)
{
    return 65535u;
}

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

/*
 * **This core into VMX operation**, once: CR0 and CR4 into what the
 * processor allows - CR4.VMXE, and CR0.NE, which the boot did not set -
 * and `vmxon` with a page of its own. Then every guest translation it may
 * hold from before dropped, so the first EPT any vCPU brings here is the
 * only one. Interrupts are off.
 */
static bool vmx_root(void)
{
    unsigned core = this_cpu()->index;
    uint64_t cr0, cr4;

    if (vmx_on[core]) {
        return true;
    }

    if (vmxon_page[core] == NULL) {
        uint64_t *page = zeroed_page();

        if (page == NULL) {
            return false;
        }

        *(uint32_t *)page = vmx.revision;
        vmxon_page[core] = (uint8_t *)page;
    }

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movq %0, %%cr0" :: "r"((cr0 | vmx.cr0_must) & vmx.cr0_may));
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("movq %0, %%cr4" :: "r"((cr4 | vmx.cr4_must | CR4_VMXE) & vmx.cr4_may));

    if (!vmx_op(VMXON, virt_to_phys(vmxon_page[core]))) {
        return false;
    }

    invept_all();
    vmx_on[core] = true;
    return true;
}

/*
 * **A guest's memory, as EPT sees it**: four levels of 4 KB tables, 48
 * bits of guest physical address, each page readable, writable and
 * executable and cached write-back regardless of what the guest's PAT says
 * (bit 6, "ignore PAT") - the monitor and the guest share these pages, and
 * one of them seeing them uncached would be a coherence bug on a machine
 * that has none. What is not mapped exits, with the address in
 * `F_GUEST_PHYSICAL`; that is the whole of the isolation.
 */
#define EPT_RWX         7ul
#define EPT_WB          (6ul << 3)
#define EPT_IGNORE_PAT  (1ul << 6)
#define EPT_ADDR        0x000ffffffffff000ul

static uint64_t *below(uint64_t *table, unsigned index, bool make)
{
    uint64_t *next;

    if ((table[index] & EPT_RWX) != 0) {
        return phys_to_virt(table[index] & EPT_ADDR);
    }

    if (!make || (next = zeroed_page()) == NULL) {
        return NULL;
    }

    table[index] = virt_to_phys(next) | EPT_RWX;
    return next;
}

void *arch_guest_tables(void)
{
    return zeroed_page();
}

bool arch_guest_map(void *tables, uint64_t ipa, uintptr_t pa)
{
    uint64_t *l3, *l2, *l1;

    if ((ipa | pa) & (PAGE_SIZE - 1) || ipa >= (1ul << 48)) {
        return false;
    }

    if ((l3 = below(tables, (unsigned)(ipa >> 39) & 511, true)) == NULL
        || (l2 = below(l3, (unsigned)(ipa >> 30) & 511, true)) == NULL
        || (l1 = below(l2, (unsigned)(ipa >> 21) & 511, true)) == NULL) {
        return false;
    }

    l1[(ipa >> 12) & 511] = (pa & EPT_ADDR) | EPT_IGNORE_PAT | EPT_WB | EPT_RWX;
    return true;
}

/* The tables, not what they map: the guest's pages are its owner's. */
static void free_level(uint64_t *table, unsigned level)
{
    unsigned i;

    if (level > 1) {
        for (i = 0; i < 512; i++) {
            uint64_t *next = below(table, i, false);

            if (next != NULL) {
                free_level(next, level - 1);
            }
        }
    }

    pmm_free_page(table);
}

void arch_guest_free(void *tables)
{
    if (tables != NULL) {
        free_level(tables, 4);
    }
}

/*
 * **A vCPU is three pages**: its VMCS, its MSR bitmap, and this - the
 * guest's FP registers and the host's while the guest has the core, the
 * five MSRs the VMCS does not swap, and where its EPT is.
 *
 * The bitmap exits on every MSR but those a guest's `syscall`, `swapgs`
 * and TLS use, which it reaches thousands of times a second: FS and GS
 * bases, kept by the VMCS; SYSENTER's three, kept by it too; and STAR,
 * LSTAR, CSTAR, SFMASK and KERNEL_GS_BASE, which nothing keeps and this
 * file swaps by hand around each entry. Everything else - EFER among them,
 * which the monitor keeps in `vcpu_regs` - is the monitor's to answer.
 */
#define SWAPPED 5

static const uint32_t swapped[SWAPPED] = {
    MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SFMASK, MSR_KERNEL_GS_BASE,
};

struct vmx_vcpu {
    uint8_t guest_fx[512];          /* first: fxsave wants 16-byte alignment */
    uint8_t host_fx[512];
    uint64_t guest_msr[SWAPPED];
    uint64_t *vmcs;
    uint8_t *bitmap;
    uint64_t eptp;
    bool written;                   /* the controls are in the VMCS */
};

_Static_assert(sizeof(struct vmx_vcpu) <= PAGE_SIZE, "a vCPU is a page");

static void pass(uint8_t *bitmap, uint32_t msr)
{
    unsigned bit = msr & 0x1fff;
    unsigned high = msr >= 0xC0000000u ? 1024 : 0;

    bitmap[high + bit / 8] &= (uint8_t)~(1u << (bit % 8));            /* reads */
    bitmap[2048 + high + bit / 8] &= (uint8_t)~(1u << (bit % 8));     /* writes */
}

void *arch_vcpu_new(void *tables, unsigned tag)
{
    struct vmx_vcpu *v = (struct vmx_vcpu *)zeroed_page();
    unsigned i;

    (void)tag;

    if (v == NULL) {
        return NULL;
    }

    if ((v->vmcs = zeroed_page()) == NULL
        || (v->bitmap = (uint8_t *)zeroed_page()) == NULL) {
        if (v->vmcs != NULL) {
            pmm_free_page(v->vmcs);
        }

        pmm_free_page(v);
        return NULL;
    }

    *(uint32_t *)v->vmcs = vmx.revision;

    for (i = 0; i < PAGE_SIZE; i++) {
        v->bitmap[i] = 0xff;
    }

    pass(v->bitmap, MSR_FS_BASE);
    pass(v->bitmap, MSR_GS_BASE);
    pass(v->bitmap, MSR_SYSENTER_CS);
    pass(v->bitmap, MSR_SYSENTER_ESP);
    pass(v->bitmap, MSR_SYSENTER_EIP);

    for (i = 0; i < SWAPPED; i++) {
        pass(v->bitmap, swapped[i]);
    }

    /* As a processor comes out of reset: x87 control 0x37F, MXCSR 0x1F80. */
    v->guest_fx[0] = 0x7f;
    v->guest_fx[1] = 0x03;
    v->guest_fx[24] = 0x80;
    v->guest_fx[25] = 0x1f;

    v->eptp = virt_to_phys(tables) | (3ul << 3) | 6ul;     /* four levels, WB */
    return v;
}

void arch_vcpu_free(void *state)
{
    struct vmx_vcpu *v = state;

    if (v != NULL) {
        pmm_free_page(v->vmcs);
        pmm_free_page(v->bitmap);
        pmm_free_page(v);
    }
}

/*
 * The controls, once per vCPU: what exits, where its EPT and bitmap are,
 * and the bits of CR0 and CR4 the guest may not own - those the processor
 * fixes in VMX operation, CR4.VMXE among them. A guest reads the shadow
 * for those and writing them differently exits.
 */
static bool write_controls(struct vmx_vcpu *v)
{
    return vmwrite(F_PIN, vmx.pin)
        && vmwrite(F_PROC2, vmx.proc2)
        && vmwrite(F_EXIT, vmx.exit)
        && vmwrite(F_EXCEPTIONS, 0)
        && vmwrite(F_PF_MASK, 0)
        && vmwrite(F_PF_MATCH, 0)
        && vmwrite(F_CR3_TARGETS, 0)
        && vmwrite(F_EXIT_MSR_STORES, 0)
        && vmwrite(F_EXIT_MSR_LOADS, 0)
        && vmwrite(F_ENTRY_MSR_LOADS, 0)
        && vmwrite(F_TSC_OFFSET, 0)
        && vmwrite(F_MSR_BITMAP, virt_to_phys(v->bitmap))
        && vmwrite(F_EPT_POINTER, v->eptp)
        && vmwrite(F_LINK_POINTER, ~0ul)
        && vmwrite(F_CR0_MASK, vmx.cr0_must | ~vmx.cr0_may)
        && vmwrite(F_CR4_MASK, vmx.cr4_must | ~vmx.cr4_may | CR4_VMXE);
}

/*
 * **Flat 64-bit, as Linux's 64-bit boot protocol asks**: CS `__BOOT_CS`,
 * the rest `__BOOT_DS`, every one based at 0 with a 4 GB limit; a TR
 * that exists only to pass the entry's checks (a busy 64-bit TSS) and no
 * LDT; the GDT where the monitor put it; debug registers, SYSENTER and
 * interruptibility at their reset values.
 */
static bool write_system(const struct vcpu_regs *io)
{
    unsigned s;
    bool ok = true;

    for (s = SEG_ES; s <= SEG_TR; s++) {
        uint64_t selector = s == SEG_CS ? VCPU_SEL_CODE
                          : s == SEG_LDTR ? 0
                          : s == SEG_TR ? VCPU_SEL_DATA + 8 : VCPU_SEL_DATA;
        uint64_t rights = s == SEG_CS ? 0xA09B          /* code, L, G */
                        : s == SEG_LDTR ? 0x10000       /* unusable */
                        : s == SEG_TR ? 0x8B : 0xC093;  /* busy TSS; data, D, G */
        uint64_t limit = s == SEG_TR ? 0x67 : s == SEG_LDTR ? 0 : 0xffffffff;

        ok = ok && vmwrite(F_GUEST_ES_SELECTOR + 2 * s, selector)
                && vmwrite(F_GUEST_ES_LIMIT + 2 * s, limit)
                && vmwrite(F_GUEST_ES_RIGHTS + 2 * s, rights)
                && vmwrite(F_GUEST_ES_BASE + 2 * s, 0);
    }

    return ok
        && vmwrite(F_GUEST_GDTR_BASE, io->gdt_base)
        && vmwrite(F_GUEST_GDTR_LIMIT, io->gdt_limit)
        && vmwrite(F_GUEST_IDTR_BASE, 0)
        && vmwrite(F_GUEST_IDTR_LIMIT, 0)
        && vmwrite(F_GUEST_DR7, 0x400)
        && vmwrite(F_GUEST_DEBUGCTL, 0)
        && vmwrite(F_GUEST_SYSENTER_CS, 0)
        && vmwrite(F_GUEST_SYSENTER_ESP, 0)
        && vmwrite(F_GUEST_SYSENTER_EIP, 0)
        && vmwrite(F_GUEST_INTERRUPTIBILITY, 0)
        && vmwrite(F_GUEST_ACTIVITY, 0)
        && vmwrite(F_GUEST_PENDING_DEBUG, 0);
}

struct table_register {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/*
 * **Where the host comes back to**, read as it is now rather than
 * remembered: this core's GDT, IDT and TSS, the page tables of the process
 * asking, the kernel's GS base. Data selectors are 0 - the processor wants
 * a ring-0 selector or none, and a process's DS is ring 3 - and put back
 * by hand after the exit. An exit also leaves the GDT's and IDT's limits
 * at 0xFFFF, which `arch_vcpu_run` puts back too.
 */
static bool write_host(const struct table_register *gdt,
                       const struct table_register *idt)
{
    uint64_t cr0, cr3, cr4, cs, ss, tr_base, lo, hi;
    uint16_t tr;
    const uint64_t *table = (const uint64_t *)gdt->base;

    __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("movq %%cs, %0" : "=r"(cs));
    __asm__ volatile("movq %%ss, %0" : "=r"(ss));
    __asm__ volatile("str %0" : "=r"(tr));

    /* A 64-bit system descriptor: base 23:0 at bits 16, 31:24 at 56, 63:32 next. */
    lo = table[tr / 8];
    hi = table[tr / 8 + 1];
    tr_base = (lo >> 16 & 0xffffff) | (lo >> 56 & 0xff) << 24 | (hi & 0xffffffff) << 32;

    return vmwrite(F_HOST_CR0, cr0)
        && vmwrite(F_HOST_CR3, cr3)
        && vmwrite(F_HOST_CR4, cr4)
        && vmwrite(F_HOST_ES_SELECTOR + 2 * SEG_ES, 0)
        && vmwrite(F_HOST_ES_SELECTOR + 2 * SEG_CS, cs & ~7ul)
        && vmwrite(F_HOST_ES_SELECTOR + 2 * SEG_SS, ss & ~7ul)
        && vmwrite(F_HOST_ES_SELECTOR + 2 * SEG_DS, 0)
        && vmwrite(F_HOST_ES_SELECTOR + 2 * SEG_FS, 0)
        && vmwrite(F_HOST_ES_SELECTOR + 2 * SEG_GS, 0)
        && vmwrite(F_HOST_ES_SELECTOR + 12, tr)            /* TR follows GS */
        && vmwrite(F_HOST_FS_BASE, rdmsr(MSR_FS_BASE))
        && vmwrite(F_HOST_GS_BASE, rdmsr(MSR_GS_BASE))
        && vmwrite(F_HOST_TR_BASE, tr_base)
        && vmwrite(F_HOST_GDTR_BASE, gdt->base)
        && vmwrite(F_HOST_IDTR_BASE, idt->base)
        && vmwrite(F_HOST_SYSENTER_CS, rdmsr(MSR_SYSENTER_CS))
        && vmwrite(F_HOST_SYSENTER_ESP, rdmsr(MSR_SYSENTER_ESP))
        && vmwrite(F_HOST_SYSENTER_EIP, rdmsr(MSR_SYSENTER_EIP))
        && vmwrite(F_HOST_EFER, rdmsr(MSR_EFER));
}

/*
 * **The guest's registers in.** CR0 and CR4 as the guest asked, with what
 * VMX fixes forced, and the guest's own values kept in the shadows, which
 * are what it reads back. EFER through the VMCS, and IA-32e mode on entry
 * exactly when EFER says long mode is active, which the entry checks.
 */
static bool write_guest(const struct vcpu_regs *io)
{
    uint32_t proc = vmx.proc, entry = vmx.entry;

    if ((io->flags & VCPU_WANT_WINDOW) != 0 && vmx.window) {
        proc |= PROC_WINDOW;
    }

    if ((io->efer & EFER_LMA) != 0) {
        entry |= ENTRY_IA32E;
    }

    return vmwrite(F_PROC, proc)
        && vmwrite(F_ENTRY, entry)
        && vmwrite(F_ENTRY_INTERRUPT,
                   (io->inject & VCPU_INJECT) != 0 ? (uint32_t)io->inject & 0x800000ffu : 0)
        && vmwrite(F_GUEST_RSP, io->gpr[VCPU_GPR_RSP])
        && vmwrite(F_GUEST_RIP, io->rip)
        && vmwrite(F_GUEST_RFLAGS, io->rflags | 2)
        && vmwrite(F_GUEST_CR0, (io->cr0 | vmx.cr0_must) & vmx.cr0_may)
        && vmwrite(F_CR0_SHADOW, io->cr0)
        && vmwrite(F_GUEST_CR3, io->cr3)
        && vmwrite(F_GUEST_CR4, (io->cr4 | vmx.cr4_must | CR4_VMXE) & vmx.cr4_may)
        && vmwrite(F_CR4_SHADOW, io->cr4)
        && vmwrite(F_GUEST_EFER, io->efer);
}

/* What the guest sees in a control register: its own bits, and the shadow's where masked. */
static uint64_t seen(uint64_t real, uint64_t field_mask, uint64_t field_shadow)
{
    uint64_t mask = vmread(field_mask);

    return (real & ~mask) | (vmread(field_shadow) & mask);
}

long arch_vcpu_run(void *state, struct vcpu_regs *io)
{
    struct vmx_vcpu *v = state;
    struct table_register gdt, idt;
    uint64_t host_msr[SWAPPED], flags, pa = virt_to_phys(v->vmcs), interruption;
    uint16_t ds, es;
    uint32_t reason;
    long failed;
    unsigned i;

    flags = cpu_interrupts_save();

    if (!vmx_root() || !vmx_op(VMPTRLD, pa)) {
        cpu_interrupts_restore(flags);
        io->exit = VCPU_EXIT_TRAP;
        io->reason = ~0u;
        io->error = 0;
        return VCPU_EXIT_TRAP;
    }

    __asm__ volatile("sgdt %0" : "=m"(gdt));
    __asm__ volatile("sidt %0" : "=m"(idt));
    __asm__ volatile("movw %%ds, %0" : "=r"(ds));
    __asm__ volatile("movw %%es, %0" : "=r"(es));

    failed = 1;

    if ((v->written || (v->written = write_controls(v)))
        && ((io->flags & VCPU_SET_SYSTEM) == 0 || write_system(io))
        && write_host(&gdt, &idt)
        && write_guest(io)) {
        /* What nothing else swaps, and the FP registers, eagerly. */
        for (i = 0; i < SWAPPED; i++) {
            host_msr[i] = rdmsr(swapped[i]);
            wrmsr(swapped[i], v->guest_msr[i]);
        }

        __asm__ volatile("fxsave64 %0" : "=m"(v->host_fx));
        __asm__ volatile("fxrstor64 %0" :: "m"(v->guest_fx));

        failed = vmx_run(io->gpr);

        __asm__ volatile("fxsave64 %0" : "=m"(v->guest_fx));
        __asm__ volatile("fxrstor64 %0" :: "m"(v->host_fx));

        for (i = 0; i < SWAPPED; i++) {
            v->guest_msr[i] = rdmsr(swapped[i]);
            wrmsr(swapped[i], host_msr[i]);
        }

        /* An exit leaves both limits at 0xFFFF and the data selectors null. */
        __asm__ volatile("lgdt %0" :: "m"(gdt));
        __asm__ volatile("lidt %0" :: "m"(idt));
        __asm__ volatile("movw %0, %%ds" :: "r"(ds));
        __asm__ volatile("movw %0, %%es" :: "r"(es));
    }

    if (failed) {
        io->exit = VCPU_EXIT_TRAP;
        io->reason = ~0u;
        io->error = vmread(F_INSTRUCTION_ERROR);
    } else {
        reason = (uint32_t)vmread(F_EXIT_REASON);
        interruption = vmread(F_EXIT_INTERRUPT);

        io->rip = vmread(F_GUEST_RIP);
        io->rflags = vmread(F_GUEST_RFLAGS);
        io->gpr[VCPU_GPR_RSP] = vmread(F_GUEST_RSP);
        io->cr0 = seen(vmread(F_GUEST_CR0), F_CR0_MASK, F_CR0_SHADOW);
        io->cr3 = vmread(F_GUEST_CR3);
        io->cr4 = seen(vmread(F_GUEST_CR4), F_CR4_MASK, F_CR4_SHADOW);
        io->efer = vmread(F_GUEST_EFER);
        io->reason = reason;
        io->qualification = vmread(F_QUALIFICATION);
        io->gpa = vmread(F_GUEST_PHYSICAL);
        io->length = vmread(F_EXIT_LENGTH);
        io->interruptible = (io->rflags & RFLAGS_IF) != 0
                            && (vmread(F_GUEST_INTERRUPTIBILITY) & 3) == 0;
        io->error = 0;
        io->inject = 0;

        /*
         * The host's: an interrupt, which is still pending at its
         * controller and taken as soon as this restores the flags, and an
         * NMI, which nothing in Kosmos sends.
         */
        io->exit = (reason == EXIT_EXTERNAL
                    || (reason == EXIT_EXCEPTION_NMI
                        && (interruption & 0x700) == INTERRUPT_TYPE_NMI))
                   ? VCPU_EXIT_HOST : VCPU_EXIT_TRAP;
    }

    vmx_op(VMCLEAR, pa);
    cpu_interrupts_restore(flags);
    return (long)io->exit;
}
