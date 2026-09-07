/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef TESTS_FAULT_H
#define TESTS_FAULT_H

#include <setjmp.h>
#include <stdbool.h>

#include "trap.h"

/*
 * One way to say "this should fault", on either board.
 *
 * **The two architectures recover from a deliberate fault differently, and
 * they have to.** AArch64's handler steps ELR past the faulting instruction
 * - exact arithmetic, because every A64 instruction is four bytes. x86-64
 * has no such number: an instruction is one to fifteen bytes and its length
 * cannot be known without decoding it, so there is no next instruction to
 * step to. What that architecture has instead is the *unwind* form, which
 * ARM also has and used only for the stack-overflow case, where stepping
 * would have resumed straight back into the guard page.
 *
 * So the unwind form is what both use here. It costs ARM nothing - the
 * mechanism was already there and already tested - and it means a test
 * about the kernel is one piece of code rather than two that have to be
 * kept in step. The two ARM tests that are *about* stepping past an
 * instruction stay ARM-only and say so, because they are assertions about
 * that handler rather than about the kernel.
 *
 *     FAULT_EXPECT({ store_to(0, 1); });
 *
 *     if (!fault_expect_end(&info)) { ... it did not fault ... }
 *
 * The body must not contain a `return`, a `break`, or a `continue` that
 * leaves it: the arming would outlive the statement and the next fault
 * anywhere in the kernel would be swallowed. Every use below is a single
 * expression, which is the shape that cannot get this wrong.
 */
#define FAULT_EXPECT(body)                                                  \
    do {                                                                    \
        jmp_buf _fault_env;                                                 \
                                                                            \
        if (setjmp(_fault_env) == 0) {                                      \
            fault_expect_unwind(_fault_env);                                \
            body                                                            \
        }                                                                   \
    } while (0)

/*
 * What kind of fault that was.
 *
 * Named by what happened rather than by the number, because the numbers do
 * not survive the crossing: ARM says "EC 0, unknown reason" for an
 * undefined instruction and x86 says "vector 6, #UD", and a test that
 * asserted either one would be asserting about a board. What both boards
 * agree on is that the instruction was not one.
 */

#if defined(__aarch64__)

static inline bool fault_was_undefined_instruction(const struct fault_info *f)
{
    return ESR_EC(f->esr) == EC_UNKNOWN;
}

static inline bool fault_was_a_breakpoint(const struct fault_info *f)
{
    return ESR_EC(f->esr) == EC_BRK64;
}

/* Nothing was mapped there at all. Which level of the walk ran out of table
 * depends on how the map was built rather than on anything worth asserting,
 * so the level is masked off. */
static inline bool fault_was_not_mapped(const struct fault_info *f)
{
    return (ISS_DABT_DFSC(ESR_ISS(f->esr)) >> 2) == 1;
}

/* Something was mapped there and this access was not allowed. */
static inline bool fault_was_not_permitted(const struct fault_info *f)
{
    return (ISS_DABT_DFSC(ESR_ISS(f->esr)) >> 2) == 3;
}

/* Was it a write? The direction bit of a data abort's fault status. */
static inline bool fault_was_a_write(const struct fault_info *f)
{
    return ISS_DABT_WNR(ESR_ISS(f->esr)) == 1;
}

/* Where the access went. */
static inline uint64_t fault_address(const struct fault_info *f)
{
    return f->far;
}

#elif defined(__x86_64__)

#define X86_VECTOR_BREAKPOINT   3
#define X86_VECTOR_UNDEFINED    6
#define X86_VECTOR_PAGE_FAULT   14

/* Page-fault error code, bit 0: set means the page was there and the
 * access was refused; clear means there was no page. That single bit is
 * what ARM spends a six-bit fault status code on. */
#define X86_PF_PRESENT          1u

static inline bool fault_was_undefined_instruction(const struct fault_info *f)
{
    return f->vector == X86_VECTOR_UNDEFINED;
}

static inline bool fault_was_a_breakpoint(const struct fault_info *f)
{
    return f->vector == X86_VECTOR_BREAKPOINT;
}

static inline bool fault_was_not_mapped(const struct fault_info *f)
{
    return f->vector == X86_VECTOR_PAGE_FAULT
        && (f->error & X86_PF_PRESENT) == 0;
}

static inline bool fault_was_not_permitted(const struct fault_info *f)
{
    return f->vector == X86_VECTOR_PAGE_FAULT
        && (f->error & X86_PF_PRESENT) != 0;
}

/* Page-fault error code, bit 1: set means the access was a write. One bit
 * where ARM spends one bit too, and for once they line up. */
#define X86_PF_WRITE            2u

static inline bool fault_was_a_write(const struct fault_info *f)
{
    return f->vector == X86_VECTOR_PAGE_FAULT
        && (f->error & X86_PF_WRITE) != 0;
}

static inline uint64_t fault_address(const struct fault_info *f)
{
    return f->cr2;
}

#else
#error "no fault classification for this architecture"
#endif

#endif /* TESTS_FAULT_H */
