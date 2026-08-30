#ifndef ARCH_AARCH64_CONTEXT_H
#define ARCH_AARCH64_CONTEXT_H

/*
 * What a switched-out thread consists of.
 *
 * Included from both C and assembly, so everything above the __ASSEMBLER__
 * guard is plain preprocessor and the _Static_asserts below are what keep the
 * offsets from drifting away from switch.S. Getting one of them wrong
 * corrupts a register in a way that surfaces five functions later, which is
 * the trap roadmap.md warns about for this milestone.
 */

#define CTX_X19     0
#define CTX_X21     16
#define CTX_X23     32
#define CTX_X25     48
#define CTX_X27     64
#define CTX_X29     80      /* x29, x30 */
#define CTX_SP      96      /* sp_el0, sp_el1 */
#define CTX_DAIF    112
#define CTX_D8      128
#define CTX_D10     144
#define CTX_D12     160
#define CTX_D14     176
#define CTX_SIZE    192

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

struct context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t x29;       /* frame pointer */
    uint64_t x30;       /* where the thread resumes */
    uint64_t sp_el0;    /* the thread's own stack */
    uint64_t sp_el1;    /* the stack its exceptions are taken on */

    /*
     * The interrupt mask this thread was running under. Saved and restored
     * rather than assumed, so a switch is correct whether or not the caller
     * had interrupts enabled. Once IPC starts switching from places that
     * already hold them masked, assuming would be a source of interrupts
     * silently re-enabled halfway through a critical section.
     */
    uint64_t daif;
    uint64_t _pad;      /* keeps the FP pairs on 16-byte boundaries */

    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;
};

_Static_assert(sizeof(struct context) == CTX_SIZE, "context size vs switch.S");
_Static_assert(offsetof(struct context, x19)    == CTX_X19, "CTX_X19");
_Static_assert(offsetof(struct context, x21)    == CTX_X21, "CTX_X21");
_Static_assert(offsetof(struct context, x23)    == CTX_X23, "CTX_X23");
_Static_assert(offsetof(struct context, x25)    == CTX_X25, "CTX_X25");
_Static_assert(offsetof(struct context, x27)    == CTX_X27, "CTX_X27");
_Static_assert(offsetof(struct context, x29)    == CTX_X29, "CTX_X29");
_Static_assert(offsetof(struct context, sp_el0) == CTX_SP,   "CTX_SP");
_Static_assert(offsetof(struct context, daif)   == CTX_DAIF, "CTX_DAIF");
_Static_assert(offsetof(struct context, d8)     == CTX_D8,  "CTX_D8");
_Static_assert(offsetof(struct context, d10)    == CTX_D10, "CTX_D10");
_Static_assert(offsetof(struct context, d12)    == CTX_D12, "CTX_D12");
_Static_assert(offsetof(struct context, d14)    == CTX_D14, "CTX_D14");

/* Saves into prev, loads from next, and returns inside next. */
void context_switch(struct context *prev, struct context *next);

/* Where a hand-built context starts. Never called directly. */
void thread_entry(void);

#endif /* !__ASSEMBLER__ */

#endif /* ARCH_AARCH64_CONTEXT_H */
