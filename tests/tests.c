#include <stdbool.h>
#include <stdint.h>

#include "test.h"
#include "console.h"
#include "semihosting.h"
#include "trap.h"

/* Defined by the linker script. Declared as arrays so taking the address is
 * the address, with no accidental dereference. */
extern char __bss_start[];
extern char __bss_end[];
extern char __stack_bottom[];
extern char __stack_top[];

/*
 * A .bss global nobody ever assigns to. `volatile` is load-bearing: without
 * it the compiler knows a static that is never written is still zero, folds
 * the comparison to true, and the test passes without reading memory at all.
 */
static volatile uint64_t bss_canary;

static bool test_bss_zeroed(void)
{
    return bss_canary == 0;
}

static bool test_bss_bounds_aligned(void)
{
    /*
     * start.S zeroes .bss sixteen bytes at a time with `stp` and no tail.
     * If the linker script ever stops aligning both ends, the last few bytes
     * are quietly left dirty and the failure shows up somewhere else
     * entirely, months later.
     */
    return ((uintptr_t)__bss_start % 16) == 0
        && ((uintptr_t)__bss_end % 16) == 0;
}

static bool test_running_at_el1(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return ((el >> 2) & 3) == 1;
}

static bool test_sp_inside_boot_stack(void)
{
    /*
     * The stack lives above __bss_end, so this failing means either that the
     * linker script let the two overlap or that start.S never set sp.
     */
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));

    return sp > (uint64_t)(uintptr_t)__stack_bottom
        && sp <= (uint64_t)(uintptr_t)__stack_top;
}

/*
 * M1: the exception vector.
 *
 * An undefined instruction is used rather than a null dereference, because
 * these run before the MMU exists. With translation off, address 0 on QEMU
 * virt is flash rather than a hole, so a write there does not necessarily
 * fault. The null-dereference test arrives with the MMU, once page 0 is
 * deliberately left unmapped.
 */
static bool test_undefined_instruction_faults(void)
{
    struct fault_info f;

    fault_expect_begin();
    __asm__ volatile("udf #0");

    if (!fault_expect_end(&f)) {
        return false;
    }

    /* An undefined instruction reports EC 0, "unknown reason". */
    return ESR_EC(f.esr) == EC_UNKNOWN;
}

static bool test_brk_faults(void)
{
    struct fault_info f;

    fault_expect_begin();
    __asm__ volatile("brk #0");

    return fault_expect_end(&f) && ESR_EC(f.esr) == EC_BRK64;
}

static bool test_elr_points_at_the_faulting_instruction(void)
{
    struct fault_info f;
    uint64_t here;

    /*
     * The label sits on the faulting instruction itself, so elr has to match
     * it exactly. This is the property the whole dump rests on: if elr is
     * off by an instruction, every fault report sends you to the wrong line.
     */
    fault_expect_begin();
    __asm__ volatile(
        "adr %0, 1f\n"
        "1: udf #0\n"
        : "=r"(here));

    return fault_expect_end(&f) && f.elr == here;
}

static bool test_execution_resumes_after_an_expected_fault(void)
{
    /*
     * The handler steps elr past the faulting instruction. If it advanced by
     * the wrong amount, control would resume in the middle of the next one
     * and this would not return at all.
     */
    volatile int reached = 0;

    fault_expect_begin();
    __asm__ volatile("udf #0");
    reached = 1;

    (void)fault_expect_end(NULL);
    return reached == 1;
}

static bool test_data_abort_is_decoded(void)
{
    /*
     * The only test that exercises the abort decoding path: exception class,
     * direction, fault status code and far. An undefined instruction proves
     * the vector works but carries none of that.
     *
     * An unaligned store is used because it faults with the MMU off. Every
     * access is Device-nGnRnE while translation is disabled, and unaligned
     * accesses to Device memory are architecturally required to fault.
     */
    static char buf[64];
    volatile unsigned *unaligned = (volatile unsigned *)(void *)(buf + 1);
    struct fault_info f;
    unsigned iss;

    fault_expect_begin();
    *unaligned = 0xdeadbeefu;

    if (!fault_expect_end(&f)) {
        return false;
    }

    iss = (unsigned)ESR_ISS(f.esr);

    return ESR_EC(f.esr) == EC_DABT_SAME
        && ISS_DABT_WNR(iss) == 1        /* it was a write */
        && ISS_DABT_DFSC(iss) == 0x21    /* alignment fault */
        && f.far == (uint64_t)(uintptr_t)unaligned;
}

static bool test_unexpected_fault_is_not_swallowed(void)
{
    /* Arming is one-shot: the second fault in a row would reach the panic
     * path. Nothing here faults twice; this only checks that end() reports
     * false when nothing fired, so a broken test cannot pass by accident. */
    fault_expect_begin();
    return fault_expect_end(NULL) == false;
}

static const struct test tests[] = {
    { "boot: .bss is zeroed",                  test_bss_zeroed          },
    { "boot: .bss bounds are 16-byte aligned", test_bss_bounds_aligned  },
    { "boot: running at EL1",                  test_running_at_el1      },
    { "boot: sp is inside the boot stack",     test_sp_inside_boot_stack },
    { "trap: an undefined instruction faults", test_undefined_instruction_faults },
    { "trap: brk reports EC 0x3c",             test_brk_faults },
    { "trap: elr is the faulting instruction", test_elr_points_at_the_faulting_instruction },
    { "trap: execution resumes after a fault", test_execution_resumes_after_an_expected_fault },
    { "trap: a data abort decodes correctly",  test_data_abort_is_decoded },
    { "trap: no fault means end() is false",   test_unexpected_fault_is_not_swallowed },
};

#define TEST_COUNT (sizeof(tests) / sizeof(tests[0]))

void tests_run(void)
{
    unsigned failed = 0;

    /* TAP wants the plan before the results, so a truncated run is
     * detectable: the host counts what arrived against what was promised. */
    kputs("1..");
    kputu(TEST_COUNT);
    kputs("\n");

    for (unsigned i = 0; i < TEST_COUNT; i++) {
        bool ok = tests[i].fn();

        if (!ok) {
            failed++;
        }

        kputs(ok ? "ok " : "not ok ");
        kputu(i + 1);
        kputs(" - ");
        kputs(tests[i].name);
        kputs("\n");
    }

    semihosting_exit(failed == 0 ? 0 : 1);
}
