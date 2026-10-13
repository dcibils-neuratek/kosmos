/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Guests and virtual CPUs - `vm.h` has the argument, and
 * `docs/virtualization.md` step V2 the plan.
 *
 * Two pools that grow as every pool here does (`pool.h`), a VMID a guest,
 * and the four syscalls. What a guest's memory and registers *are* is the
 * architecture's: this file never touches a page table or a register.
 */

#include <stddef.h>
#include <stdint.h>

#include "vm.h"
#include "ipc.h"
#include "memobj.h"
#include "mmu.h"
#include "page.h"
#include "pmm.h"
#include "pool.h"
#include "process.h"
#include "spinlock.h"
#include "syscall.h"
#include "thread.h"

/* The regions one guest can hold: a page of pointers. A guest maps a
 * handful - its RAM, a ring or two - so a page is room to spare, and the
 * refusal past it is a sentence rather than a wall nobody will reach. */
#define GUEST_MAPS_MAX  (PAGE_SIZE / sizeof(struct memobj *))

struct guest {
    bool             in_use;
    unsigned         generation;
    struct process  *owner;
    unsigned         tag;               /* its VMID */
    void            *tables;            /* the architecture's second stage */
    struct memobj  **maps;              /* the regions it holds */
    unsigned         nmaps;
};

struct vcpu {
    bool             in_use;
    unsigned         generation;
    struct process  *owner;
    struct guest    *guest;
    void            *state;             /* the architecture's registers */
    bool             running;
};

static struct pool guests, vcpus;
static struct spinlock vm_lock = SPINLOCK("vm");

/* Which VMIDs are taken: one bit each, 0 being the host's. 65,536 is the
 * most an architecture has (ARM's 16-bit VMIDs); `arch_vm_tags` says how
 * many this core has. */
static uint8_t tags_taken[65536 / 8];

/* Generation 1 for a slot never used, so 0 never names anything. */
static void fresh(void *object)
{
    ((struct guest *)object)->generation = 1;
}

static void fresh_vcpu(void *object)
{
    ((struct vcpu *)object)->generation = 1;
}

void vm_init(void)
{
    pool_init(&guests, "guests", sizeof(struct guest),
              pool_ceiling_for(sizeof(struct guest) + 4 * PAGE_SIZE, 16), 4, fresh);
    pool_init(&vcpus, "vcpus", sizeof(struct vcpu),
              pool_ceiling_for(sizeof(struct vcpu) + PAGE_SIZE, 16), 4, fresh_vcpu);
}

static unsigned take_tag(void)
{
    unsigned most = arch_vm_tags(), i;

    for (i = 1; i <= most && i < 65536; i++) {
        if ((tags_taken[i / 8] & (1u << (i % 8))) == 0) {
            tags_taken[i / 8] |= (uint8_t)(1u << (i % 8));
            return i;
        }
    }

    return 0;
}

static void give_tag(unsigned tag)
{
    tags_taken[tag / 8] &= (uint8_t)~(1u << (tag % 8));
}

/*
 * A free slot, claimed under the lock before anything is built in it, or
 * one more slab and look again - `memobj_create`'s shape, for its reason.
 */
static void *claim(struct pool *p, size_t in_use_at)
{
    unsigned i;

again:
    for (i = 0; i < pool_slots(p); i++) {
        char *o = pool_at(p, i);
        bool *in_use = (bool *)(o + in_use_at);
        unsigned long flags = spin_lock(&vm_lock);

        if (!*in_use) {
            *in_use = true;
            spin_unlock(&vm_lock, flags);
            return o;
        }

        spin_unlock(&vm_lock, flags);
    }

    if (pool_grow(p)) {
        goto again;
    }

    return NULL;
}

bool vm_guest_current(const struct guest *g, unsigned generation)
{
    return g != NULL && g->in_use && g->generation == generation;
}

bool vm_vcpu_current(const struct vcpu *v, unsigned generation)
{
    return v != NULL && v->in_use && v->generation == generation;
}

/* A capability to a guest of this process, still the one it named. */
static struct guest *guest_of(struct thread *t, long index)
{
    unsigned generation = 0;
    struct guest *g = ipc_resolve_object(t, (cap_t)index, CAP_GUEST, &generation);

    return vm_guest_current(g, generation) && g->owner == t->process ? g : NULL;
}

static struct vcpu *vcpu_of(struct thread *t, long index)
{
    unsigned generation = 0;
    struct vcpu *v = ipc_resolve_object(t, (cap_t)index, CAP_VCPU, &generation);

    return vm_vcpu_current(v, generation) && v->owner == t->process ? v : NULL;
}

long vm_guest_create(struct thread *t)
{
    struct guest *g;
    unsigned long flags;
    long cap;

    if (!arch_vm_available()) {
        return SYS_ERR_NO_DEVICE;
    }

    if ((g = claim(&guests, offsetof(struct guest, in_use))) == NULL) {
        return SYS_ERR_NO_ROOM;
    }

    g->owner = t->process;
    g->nmaps = 0;
    g->tables = arch_guest_tables();
    g->maps = pmm_alloc_page();

    flags = spin_lock(&vm_lock);
    g->tag = take_tag();
    spin_unlock(&vm_lock, flags);

    if (g->tables == NULL || g->maps == NULL || g->tag == 0
        || (cap = ipc_install_object(t, CAP_GUEST, g, g->generation)) < 0) {
        long why = (g->tables == NULL || g->maps == NULL || g->tag == 0)
                   ? SYS_ERR_NO_ROOM : SYS_ERR_NO_CAPS;

        if (g->tables != NULL) {
            arch_guest_free(g->tables);
        }

        if (g->maps != NULL) {
            pmm_free_page(g->maps);
        }

        flags = spin_lock(&vm_lock);

        if (g->tag != 0) {
            give_tag(g->tag);
        }

        g->generation++;
        g->in_use = false;
        spin_unlock(&vm_lock, flags);
        return why;
    }

    return cap;
}

/*
 * **A region, at a guest physical address**: every page of it, held by the
 * guest until the guest ends, so a monitor dropping its own capability
 * cannot leave the guest's second stage pointing at freed pages.
 */
long vm_guest_map(struct thread *t, long guest, long region, uint64_t ipa)
{
    struct guest *g = guest_of(t, guest);
    struct memobj *m;
    size_t i;

    if (g == NULL) {
        return SYS_ERR_DENIED;
    }

    if ((ipa & (PAGE_SIZE - 1)) != 0) {
        return SYS_ERR_FAULT;
    }

    if (g->nmaps >= GUEST_MAPS_MAX) {
        return SYS_ERR_NO_ROOM;
    }

    if ((m = ipc_hold_memory(t, (cap_t)region)) == NULL) {
        return SYS_ERR_DENIED;
    }

    for (i = 0; i < m->pages; i++) {
        if (!arch_guest_map(g->tables, ipa + i * PAGE_SIZE,
                            virt_to_phys(memobj_page(m, i)))) {
            /* What was mapped stays mapped, and held: the guest's tables
             * are the guest's, and the region goes with it. */
            g->maps[g->nmaps++] = m;
            return SYS_ERR_NO_ROOM;
        }
    }

    g->maps[g->nmaps++] = m;
    return 0;
}

long vm_vcpu_create(struct thread *t, long guest)
{
    struct guest *g = guest_of(t, guest);
    struct vcpu *v;
    long cap;
    unsigned long flags;

    if (g == NULL) {
        return SYS_ERR_DENIED;
    }

    if ((v = claim(&vcpus, offsetof(struct vcpu, in_use))) == NULL) {
        return SYS_ERR_NO_ROOM;
    }

    v->owner = t->process;
    v->guest = g;
    v->running = false;
    v->state = arch_vcpu_new(g->tables, g->tag);

    if (v->state == NULL
        || (cap = ipc_install_object(t, CAP_VCPU, v, v->generation)) < 0) {
        long why = v->state == NULL ? SYS_ERR_NO_ROOM : SYS_ERR_NO_CAPS;

        if (v->state != NULL) {
            arch_vcpu_free(v->state);
        }

        flags = spin_lock(&vm_lock);
        v->generation++;
        v->in_use = false;
        spin_unlock(&vm_lock, flags);
        return why;
    }

    return cap;
}

/*
 * **Once into the guest, and back with why.** One thread at a time, on a
 * core that can enter a guest; the registers copied in before and out
 * after, so the monitor's copy is the only one it has to think about.
 */
long vm_vcpu_run(struct thread *t, long vcpu, uintptr_t regs)
{
    struct vcpu *v = vcpu_of(t, vcpu);
    struct vcpu_regs io;
    unsigned long flags;
    long why;

    if (v == NULL) {
        return SYS_ERR_DENIED;
    }

    if (!process_may_write(t->process, regs, sizeof(io))) {
        return SYS_ERR_FAULT;
    }

    if (!arch_vm_here()) {
        return SYS_ERR_NO_DEVICE;
    }

    flags = spin_lock(&vm_lock);

    if (v->running) {
        spin_unlock(&vm_lock, flags);
        return SYS_ERR_BUSY;
    }

    v->running = true;
    spin_unlock(&vm_lock, flags);

    io = *(const struct vcpu_regs *)regs;
    why = arch_vcpu_run(v->state, &io);
    *(struct vcpu_regs *)regs = io;

    flags = spin_lock(&vm_lock);
    v->running = false;
    spin_unlock(&vm_lock, flags);

    return why;
}

/*
 * Everything `p` made, gone with it: vCPUs first, since each names its
 * guest; then each guest's tables, its regions let go of, and its VMID
 * given back. Called once every thread of `p` has left, so nothing runs a
 * vCPU while it goes - and before the capability table is emptied, whose
 * slots name these and are made stale by the generations moving on.
 *
 * **A VMID given back may still have translations cached** for the guest
 * that had it. They are dropped at the next guest's first entry, which the
 * architecture does for a vCPU whose tables are new - before anything can
 * use them.
 */
void vm_release_owned_by(struct process *p)
{
    unsigned i, k;

    for (i = 0; i < pool_slots(&vcpus); i++) {
        struct vcpu *v = pool_at(&vcpus, i);

        if (v->in_use && v->owner == p) {
            arch_vcpu_free(v->state);
            v->state = NULL;
            v->guest = NULL;
            v->owner = NULL;
            v->generation++;
            v->in_use = false;
        }
    }

    for (i = 0; i < pool_slots(&guests); i++) {
        struct guest *g = pool_at(&guests, i);
        unsigned long flags;

        if (!g->in_use || g->owner != p) {
            continue;
        }

        arch_guest_free(g->tables);

        for (k = 0; k < g->nmaps; k++) {
            memobj_unref(g->maps[k]);
        }

        pmm_free_page(g->maps);

        flags = spin_lock(&vm_lock);
        give_tag(g->tag);
        g->tables = NULL;
        g->maps = NULL;
        g->nmaps = 0;
        g->owner = NULL;
        g->generation++;
        g->in_use = false;
        spin_unlock(&vm_lock, flags);
    }
}

unsigned vm_guests_in_use(void)
{
    unsigned i, n = 0;

    for (i = 0; i < pool_slots(&guests); i++) {
        n += ((struct guest *)pool_at(&guests, i))->in_use ? 1u : 0u;
    }

    return n;
}

unsigned vm_vcpus_in_use(void)
{
    unsigned i, n = 0;

    for (i = 0; i < pool_slots(&vcpus); i++) {
        n += ((struct vcpu *)pool_at(&vcpus, i))->in_use ? 1u : 0u;
    }

    return n;
}
