#include <stdbool.h>
#include <stdint.h>

#include "test.h"
#include "console.h"
#include "semihosting.h"
#include "trap.h"
#include "pmm.h"
#include "page.h"
#include "mmu.h"
#include "kernel.h"
#include "hal.h"

#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/* The Kosmos configuration first, exactly as the Lua build does it. Every
 * hook it overrides is guarded upstream by #if !defined, so arriving after
 * lua.h means upstream's default wins and this one is a redefinition. */
#include "kosmos_lua.h"
#include "lua.h"
#include "lauxlib.h"

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

/*
 * A store the compiler is not allowed to reason about.
 *
 * Writing `*(volatile int *)0 = 1` in C does not work. Dereferencing a null
 * pointer is undefined behaviour, so GCC is entitled to assume it cannot
 * happen: it emits the store and then treats everything after it as
 * unreachable, appending a `brk`. The store faults, the handler steps elr
 * past it, and execution lands on the brk. That is a second fault with the
 * expectation already spent, and the kernel panics.
 *
 * Found exactly that way. In assembly the store is just a store.
 */
static void store_to(uintptr_t addr, uint32_t value)
{
    __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(addr) : "memory");
}

/* Any translation fault, whatever level it happened at. Which level depends
 * on where the walk ran out of table, which is a detail of how the map was
 * built rather than a property worth asserting on. */
static bool is_translation_fault(unsigned iss)
{
    return (ISS_DABT_DFSC(iss) >> 2) == 1;
}

static bool is_permission_fault(unsigned iss)
{
    return (ISS_DABT_DFSC(iss) >> 2) == 3;
}

/*
 * M1's definition of done, the half that matters most: a deliberate null
 * dereference reports a readable data abort naming the address, instead of
 * hanging.
 *
 * This replaces the unaligned-store test that stood here before the MMU
 * existed. That one worked only because every access is Device memory while
 * translation is off, and unaligned accesses to Device memory are required
 * to fault. With the MMU on, RAM is Normal memory and the same store
 * succeeds silently.
 */
static bool test_null_dereference_faults(void)
{
    struct fault_info f;
    unsigned iss;

    fault_expect_begin();
    store_to(0, 1);

    if (!fault_expect_end(&f)) {
        return false;
    }

    iss = (unsigned)ESR_ISS(f.esr);

    return ESR_EC(f.esr) == EC_DABT_SAME
        && ISS_DABT_WNR(iss) == 1       /* it was a write */
        && is_translation_fault(iss)
        && f.far == 0;                  /* and it names the address */
}

static bool test_mmu_is_on(void)
{
    return mmu_is_enabled();
}

static bool test_stack_guard_page_is_unmapped(void)
{
    /*
     * The page below the boot stack has no translation, so an overflow is an
     * abort naming the address rather than silent corruption of .bss.
     */
    extern char __stack_guard[];
    struct fault_info f;

    fault_expect_begin();
    store_to((uintptr_t)__stack_guard, 1);

    return fault_expect_end(&f)
        && is_translation_fault((unsigned)ESR_ISS(f.esr))
        && f.far == (uint64_t)(uintptr_t)__stack_guard;
}

static bool test_kernel_text_is_not_writable(void)
{
    /* W^X in the kernel: .text is mapped read-only and executable. A write
     * has to be a permission fault, not a translation fault, because the
     * page is mapped. */
    extern char __text_start[];
    struct fault_info f;

    fault_expect_begin();
    store_to((uintptr_t)__text_start, 0);

    return fault_expect_end(&f)
        && is_permission_fault((unsigned)ESR_ISS(f.esr))
        && f.far == (uint64_t)(uintptr_t)__text_start;
}

static bool test_rodata_is_not_writable(void)
{
    extern char __rodata_start[];
    struct fault_info f;

    fault_expect_begin();
    store_to((uintptr_t)__rodata_start, 0);

    return fault_expect_end(&f)
        && is_permission_fault((unsigned)ESR_ISS(f.esr));
}

static bool test_memory_still_works_through_translation(void)
{
    /* Trivial on its face, and the thing that fails when MAIR, the shared
     * attributes or the access flag are wrong: the map looks right and every
     * access comes back as garbage or faults on first touch. */
    volatile uint64_t *p = pmm_alloc_page();
    bool ok;

    if (p == NULL) {
        return false;
    }

    p[0] = 0xa5a5a5a5a5a5a5a5ULL;
    p[511] = 0x5a5a5a5a5a5a5a5aULL;
    ok = p[0] == 0xa5a5a5a5a5a5a5a5ULL && p[511] == 0x5a5a5a5a5a5a5a5aULL;

    pmm_free_page((void *)p);
    return ok;
}

static bool test_unexpected_fault_is_not_swallowed(void)
{
    /* Arming is one-shot: the second fault in a row would reach the panic
     * path. Nothing here faults twice; this only checks that end() reports
     * false when nothing fired, so a broken test cannot pass by accident. */
    fault_expect_begin();
    return fault_expect_end(NULL) == false;
}

/*
 * M3: the exception stack.
 */
extern char __exception_stack_bottom[], __exception_stack_top[];
extern char __stack_bottom[], __stack_top[];

static bool test_kernel_runs_on_sp_el0(void)
{
    /* SPSel bit 0: 0 means SP_EL0 is the current stack pointer. The whole
     * separation rests on this being 0 in ordinary code. */
    uint64_t spsel;
    __asm__ volatile("mrs %0, spsel" : "=r"(spsel));
    return (spsel & 1) == 0;
}

static bool test_handler_runs_on_the_exception_stack(void)
{
    /*
     * Direct evidence that the two stacks are actually separate, rather than
     * the code merely looking as though it set them up that way.
     */
    struct fault_info f;

    fault_expect_begin();
    __asm__ volatile("udf #0");

    if (!fault_expect_end(&f)) {
        return false;
    }

    return f.handler_sp > (uint64_t)(uintptr_t)__exception_stack_bottom
        && f.handler_sp <= (uint64_t)(uintptr_t)__exception_stack_top;
}

static volatile int recursion_guard;

/*
 * GCC is right that this never returns, and that is the point: it is
 * terminated by a hardware fault, which no amount of static analysis can
 * see. The warning is correct and the code is deliberate, so it is silenced
 * here and nowhere else.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"

static uint64_t burn_stack(uint64_t depth)
{
    /*
     * Recurses until the stack runs into its guard page. The volatile
     * counter and the returned sum are there so the compiler cannot turn
     * this into a loop or prove anything about where it ends: it has to
     * actually build a frame each time.
     */
    volatile uint64_t pad[16];
    uint64_t i;
    uint64_t sum = depth;

    recursion_guard++;

    for (i = 0; i < 16; i++) {
        pad[i] = depth + i;
    }

    sum += burn_stack(depth + 1);

    for (i = 0; i < 16; i++) {
        sum += pad[i];
    }

    return sum;
}

#pragma GCC diagnostic pop

static bool test_a_stack_overflow_is_survivable(void)
{
    /*
     * The reason the exception stack exists, tested end to end.
     *
     * Before this, an overflow was a double fault: the handler built its
     * 288-byte frame on the stack that had just run out, faulted again inside
     * the vector, and the kernel hung with nothing printed. Now the fault
     * lands on a different stack, is recognised, and unwinds back here.
     *
     * Stepping ELR would be no use: resuming the faulting instruction just
     * recurses into the guard page again. This is what fault_expect_unwind is
     * for, and it is the case testing.md §18.2 had in mind.
     */
    jmp_buf recover;
    volatile bool unwound = false;
    volatile uint64_t sp_after;

    recursion_guard = 0;

    if (setjmp(recover) == 0) {
        fault_expect_unwind(recover);
        (void)burn_stack(0);
        /* Unreachable: the recursion cannot return. */
    } else {
        unwound = true;
    }

    if (!fault_expect_end(NULL)) {
        return false;
    }

    /* And the stack pointer is back where setjmp saw it, not thousands of
     * frames down where the fault happened. */
    __asm__ volatile("mov %0, sp" : "=r"(sp_after));

    /*
     * The depth bound is deliberately loose. How many frames fit in 16 KB
     * depends on what the compiler decided this function's frame should be,
     * and that is not the property under test: what matters is that it
     * really recursed and consumed a stack rather than faulting immediately.
     * It happens to reach exactly 100, which is about 163 bytes a frame.
     */
    return unwound
        && recursion_guard > 10
        && sp_after > (uint64_t)(uintptr_t)__stack_bottom
        && sp_after <= (uint64_t)(uintptr_t)__stack_top;
}

/*
 * M1: the physical page allocator.
 *
 * The panic paths (a misaligned free, an out-of-range free, a double free)
 * are deliberately untested. They halt the kernel by design, which is the
 * right behaviour for a programmer error and the wrong thing to assert on
 * from inside the kernel being tested. Turning them into return codes just
 * to make them testable would invite callers to ignore them.
 */
static bool test_pmm_alloc_is_page_aligned_and_in_ram(void)
{
    void *p = pmm_alloc_page();
    bool ok;

    if (p == NULL) {
        return false;
    }

    ok = ((uintptr_t)p & PAGE_MASK) == 0;
    pmm_free_page(p);
    return ok;
}

static bool test_pmm_two_allocations_differ(void)
{
    void *a = pmm_alloc_page();
    void *b = pmm_alloc_page();
    bool ok = a != NULL && b != NULL && a != b;

    if (a != NULL) { pmm_free_page(a); }
    if (b != NULL) { pmm_free_page(b); }
    return ok;
}

static bool test_pmm_count_tracks_alloc_and_free(void)
{
    size_t before = pmm_free_pages();
    void *p = pmm_alloc_page();

    if (p == NULL || pmm_free_pages() != before - 1) {
        return false;
    }

    pmm_free_page(p);
    return pmm_free_pages() == before;
}

static bool test_pmm_page_is_writable(void)
{
    volatile uint64_t *p = pmm_alloc_page();
    bool ok;

    if (p == NULL) {
        return false;
    }

    /* Both ends, so a page handed out with the wrong size shows up here
     * rather than as corruption of whatever lives next to it. */
    p[0] = 0x0123456789abcdefULL;
    p[PAGE_SIZE / sizeof(uint64_t) - 1] = 0xfedcba9876543210ULL;

    ok = p[0] == 0x0123456789abcdefULL
      && p[PAGE_SIZE / sizeof(uint64_t) - 1] == 0xfedcba9876543210ULL;

    pmm_free_page((void *)p);
    return ok;
}

static bool test_pmm_kernel_pages_are_not_free(void)
{
    /* The image and the bitmap are inside RAM but must never be handed out.
     * If they were, the count would equal the total. */
    return pmm_free_pages() < pmm_total_pages();
}

static bool test_pmm_exhaustion_returns_null(void)
{
    /*
     * Allocates every remaining page, checks the count reaches zero and the
     * next call returns NULL, then gives them all back.
     *
     * The pages are threaded into a list through their own first eight
     * bytes, which is the only way to remember 130,000 addresses without
     * somewhere to put them. It also proves every page handed out is
     * genuinely writable.
     */
    size_t before = pmm_free_pages();
    size_t taken = 0;
    void *head = NULL;
    void *p;
    bool ok;

    while ((p = pmm_alloc_page()) != NULL) {
        *(void **)p = head;
        head = p;
        taken++;
    }

    ok = (taken == before)
      && (pmm_free_pages() == 0)
      && (pmm_alloc_page() == NULL);

    while (head != NULL) {
        void *next = *(void **)head;
        pmm_free_page(head);
        head = next;
    }

    return ok && pmm_free_pages() == before;
}

/*
 * M2: the heap and snprintf.
 */
static bool test_heap_alloc_is_aligned_and_usable(void)
{
    /* 16 bytes, because a double and a long double both want it and Lua puts
     * both inside its values. An 8-aligned heap works until something takes
     * a 16-byte load across the boundary. */
    void *a = malloc(1);
    void *b = malloc(17);
    void *c = malloc(4096);
    bool ok = a != NULL && b != NULL && c != NULL
           && ((uintptr_t)a % 16) == 0
           && ((uintptr_t)b % 16) == 0
           && ((uintptr_t)c % 16) == 0;

    free(a); free(b); free(c);
    return ok;
}

static bool test_heap_coalesces_in_both_directions(void)
{
    /*
     * The property the whole allocator rests on. Three neighbours freed
     * middle-last must end as one block, or the heap fills with holes that
     * are physically adjacent and individually too small, and Lua's GC can
     * never get memory back.
     */
    size_t before = heap_used();
    void *a = malloc(2048);
    void *b = malloc(2048);
    void *c = malloc(2048);
    void *big;
    bool ok;

    if (a == NULL || b == NULL || c == NULL) {
        return false;
    }

    free(a);
    free(c);
    free(b);   /* last, so it has to merge with a free block on each side */

    big = malloc(6000);
    ok = big != NULL;
    free(big);

    return ok && heap_used() == before;
}

static bool test_heap_realloc_preserves_contents(void)
{
    unsigned char *p = malloc(64);
    unsigned char *q;
    bool ok = true;
    int i;

    if (p == NULL) {
        return false;
    }

    for (i = 0; i < 64; i++) {
        p[i] = (unsigned char)(i * 7);
    }

    q = realloc(p, 4096);
    if (q == NULL) {
        free(p);
        return false;
    }

    for (i = 0; i < 64; i++) {
        if (q[i] != (unsigned char)(i * 7)) {
            ok = false;
        }
    }

    free(q);
    return ok;
}

static bool test_heap_exhaustion_returns_null(void)
{
    /* Bigger than the whole heap. Returning NULL rather than a pointer into
     * nothing is what lets Lua's allocator report an out-of-memory error
     * instead of corrupting the heap. */
    void *p = malloc(heap_size() + 1);

    if (p != NULL) {
        free(p);
        return false;
    }

    return true;
}

static bool str_is(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static bool test_snprintf_integers_and_strings(void)
{
    char b[64];

    snprintf(b, sizeof(b), "%d", -42);            if (!str_is(b, "-42"))      return false;
    snprintf(b, sizeof(b), "%5d|", 42);           if (!str_is(b, "   42|"))   return false;
    snprintf(b, sizeof(b), "%-5d|", 42);          if (!str_is(b, "42   |"))   return false;
    snprintf(b, sizeof(b), "%05d", 42);           if (!str_is(b, "00042"))    return false;
    snprintf(b, sizeof(b), "%x", 0xdeadbeefu);    if (!str_is(b, "deadbeef")) return false;
    snprintf(b, sizeof(b), "%s!", "hi");          if (!str_is(b, "hi!"))      return false;
    snprintf(b, sizeof(b), "%.2s", "hello");      if (!str_is(b, "he"))       return false;
    snprintf(b, sizeof(b), "%lld", 9223372036854775807LL);
    if (!str_is(b, "9223372036854775807")) return false;

    /* The one that overflows if the magnitude is taken by plain negation. */
    snprintf(b, sizeof(b), "%lld", (long long)-9223372036854775807LL - 1);
    return str_is(b, "-9223372036854775808");
}

static bool test_snprintf_truncates_and_reports_the_full_length(void)
{
    /* Returning what it would have needed is how a caller sizes a buffer,
     * and writing the terminator inside the cap is what keeps the result a
     * string. */
    char b[5];
    int n = snprintf(b, sizeof(b), "%s", "abcdefgh");

    return n == 8 && str_is(b, "abcd");
}

static bool test_snprintf_floats(void)
{
    char b[64];

    /* Lua's default number format is %.14g, so that is the one that matters. */
    snprintf(b, sizeof(b), "%.14g", 1.5);      if (!str_is(b, "1.5"))    return false;
    snprintf(b, sizeof(b), "%.14g", 100.0);    if (!str_is(b, "100"))    return false;
    snprintf(b, sizeof(b), "%.14g", 0.5);      if (!str_is(b, "0.5"))    return false;
    snprintf(b, sizeof(b), "%.14g", -0.25);    if (!str_is(b, "-0.25"))  return false;
    snprintf(b, sizeof(b), "%.14g", 0.0);      if (!str_is(b, "0"))      return false;
    snprintf(b, sizeof(b), "%.3f",  1.5);      if (!str_is(b, "1.500"))  return false;
    snprintf(b, sizeof(b), "%.14g", 1e20);     if (!str_is(b, "1e+20"))  return false;
    snprintf(b, sizeof(b), "%.14g", 1e-7);     if (!str_is(b, "1e-07"))  return false;

    return true;
}

static bool test_our_math_matches_its_definition(void)
{
    int e = 999;
    double m;

    if (floor(2.7) != 2.0 || floor(-2.7) != -3.0) return false;
    if (ceil(2.1)  != 3.0 || ceil(-2.1)  != -2.0) return false;
    if (trunc(2.7) != 2.0 || trunc(-2.7) != -2.0) return false;
    if (fabs(-3.5) != 3.5 || fabs(3.5)   != 3.5)  return false;

    /* frexp returns a mantissa in [0.5, 1) and an exponent that rebuilds the
     * value exactly. 8.0 is 0.5 * 2^4. */
    m = frexp(8.0, &e);
    if (m != 0.5 || e != 4) return false;

    m = frexp(0.0, &e);
    if (m != 0.0 || e != 0) return false;

    if (ldexp(0.5, 4) != 8.0) return false;
    if (ldexp(1.0, 0) != 1.0) return false;

    /* And newlib's half, so a broken link shows up here rather than inside
     * Lua's arithmetic. */
    return pow(2.0, 10.0) == 1024.0 && fmod(7.0, 3.0) == 1.0;
}

/*
 * M2: Lua.
 *
 * One state, opened on first use and kept. Opening one per test would be a
 * better isolation story and a worse test: the interesting failures are the
 * ones that only appear after the collector has run a few times over a heap
 * that has been used.
 */
static struct lua_State *shared;

static struct lua_State *lua_state(void)
{
    if (shared == NULL) {
        shared = kosmos_lua_open();
    }

    return shared;
}

/* Runs a chunk that returns a boolean, and reports what it returned. A chunk
 * that fails to compile or raises is a false, which is what a test wants. */
static bool lua_says_true(const char *src)
{
    struct lua_State *L = lua_state();
    bool ok;

    if (L == NULL) {
        return false;
    }

    if (kosmos_lua_dostring(L, "=test", src) != LUA_OK) {
        lua_pop((lua_State *)L, 1);
        return false;
    }

    ok = lua_toboolean((lua_State *)L, -1) != 0;
    lua_pop((lua_State *)L, 1);
    return ok;
}

static bool test_lua_arithmetic(void)
{
    /* M2's definition of done, as an assertion rather than a printed line. */
    return lua_says_true("return 2 + 2 == 4")
        && lua_says_true("return 7 // 2 == 3 and 7 % 2 == 1")
        && lua_says_true("return math.type(1) == 'integer'")
        && lua_says_true("return math.type(1.0) == 'float'");
}

static bool test_lua_floats(void)
{
    /*
     * Exercises the whole number path at once: the parser calls strtod on
     * the literal, the arithmetic runs in FP registers that only exist
     * because CPACR was opened, and tostring goes back out through our
     * snprintf.
     */
    return lua_says_true("return 1 / 2 == 0.5")
        && lua_says_true("return tostring(1.5) == '1.5'")
        && lua_says_true("return tostring(0.25) == '0.25'")
        && lua_says_true("return tonumber('3.5') == 3.5")
        && lua_says_true("return tonumber('1e3') == 1000.0")
        && lua_says_true("return 2.0 ^ 10 == 1024.0");
}

static bool test_lua_strings_and_tables(void)
{
    return lua_says_true("return ('a' .. 'b' .. 'c') == 'abc'")
        && lua_says_true("return #'hello' == 5")
        && lua_says_true("return ('hello'):upper() == 'HELLO'")
        && lua_says_true("return string.format('%d-%s', 7, 'x') == '7-x'")
        && lua_says_true("local t = {} for i=1,100 do t[i]=i*i end return t[10] == 100 and #t == 100")
        && lua_says_true("local s=0 for _,v in ipairs{1,2,3} do s=s+v end return s == 6");
}

static bool test_lua_closures(void)
{
    return lua_says_true(
        "local function counter() local n = 0 "
        "  return function() n = n + 1 return n end end "
        "local c = counter() c() c() return c() == 3");
}

static bool test_lua_coroutines(void)
{
    /*
     * The single most important Lua feature for this design. `design.md`
     * §4.5 rests the entire server model on coroutines: they are what makes
     * synchronous IPC writable as sequential code instead of a state
     * machine. If they did not work, the architecture would not.
     */
    return lua_says_true(
        "local co = coroutine.create(function(a) "
        "  local b = coroutine.yield(a + 1) "
        "  return b * 2 end) "
        "local _, x = coroutine.resume(co, 1) "
        "local _, y = coroutine.resume(co, 10) "
        "return x == 2 and y == 20 and coroutine.status(co) == 'dead'");
}

static bool test_lua_errors_are_caught(void)
{
    /*
     * This is the setjmp/longjmp test that matters. The unit tests for them
     * jump within one function; here Lua raises from inside its own VM,
     * across frames it built, and unwinds to a pcall. `setup.md` warns that
     * a wrong longjmp only shows up the first time something raises, and
     * this is that first time.
     */
    return lua_says_true("local ok, e = pcall(function() error('boom') end) "
                         "return ok == false and e:find('boom') ~= nil")
        && lua_says_true("local ok = pcall(function() return nil + 1 end) "
                         "return ok == false")
        && lua_says_true("local ok, e = pcall(function() error({code=42}) end) "
                         "return ok == false and e.code == 42")
        /* And that the state is still usable afterwards, which is the part
         * a botched stack restore breaks. */
        && lua_says_true("return 1 + 1 == 2");
}

static bool test_lua_errors_nest(void)
{
    /* A pcall inside a pcall, with the inner one rethrowing. Nested jmp_bufs
     * are where an incorrect saved sp shows up as a corrupted outer frame. */
    return lua_says_true(
        "local ok, e = pcall(function() "
        "  local ok2 = pcall(function() error('inner') end) "
        "  error('outer:' .. tostring(ok2)) end) "
        "return ok == false and e:find('outer:false') ~= nil");
}

static bool test_lua_math_library(void)
{
    /* Ours and newlib's, reached through Lua rather than called directly. */
    return lua_says_true("return math.floor(2.7) == 2 and math.ceil(2.1) == 3")
        && lua_says_true("return math.abs(-3) == 3")
        && lua_says_true("return math.sqrt(16.0) == 4.0")
        && lua_says_true("return math.max(1,5,3) == 5")
        && lua_says_true("return math.fmod(7, 3) == 1.0")
        && lua_says_true("return type(math.random()) == 'number'");
}

static bool test_lua_garbage_collector_reclaims(void)
{
    /*
     * The GC is the design's known risk (`design.md` §5.2) and the first
     * thing to check is simply that it gets memory back at all. A collector
     * running against an allocator that does not coalesce would show up
     * here as memory that never drops.
     *
     * Four thousand tables, not twenty. Twenty thousand runs the 2 MB heap
     * out, which is itself worth knowing: a Lua table is around 56 bytes and
     * this allocator adds 32 more per block, so the overhead is not a
     * rounding error. That is the number roadmap.md M4 puts on the bench.
     */
    return lua_says_true(
        "collectgarbage() "
        "local before = collectgarbage('count') "
        "local t = {} for i = 1, 4000 do t[i] = {i} end "
        "local peak = collectgarbage('count') "
        "t = nil "
        "collectgarbage() "
        "local after = collectgarbage('count') "
        "return peak > before * 2 and after < peak / 2");
}

static bool test_lua_dangerous_libraries_are_absent(void)
{
    /*
     * `design.md` §5.3: the list of libraries inside a state is part of the
     * security model, not a configuration detail. Asserting the absences
     * keeps a later "just open everything" from passing quietly.
     */
    return lua_says_true("return io == nil")        /* no global tree to open */
        && lua_says_true("return os == nil")        /* no wall clock, no exec */
        && lua_says_true("return debug == nil")     /* breaks every abstraction */
        && lua_says_true("return package == nil")   /* wants dlopen */
        && lua_says_true("return dofile == nil and loadfile == nil");
}

static bool test_lua_refuses_bytecode(void)
{
    /*
     * `design.md` §5.3 forbids precompiled bytecode outright: the undump
     * loader validates almost nothing, so a crafted chunk is arbitrary
     * execution inside the state.
     *
     * Upstream's default mode is "bt", which accepts either, so this is only
     * true because every load in lua/kosmos/ says "t". The test is here to
     * fail the day someone drops the argument.
     */
    return lua_says_true(
        "local dumped = string.dump(function() return 1 end) "
        "local f, err = load(dumped, 'evil', 't') "
        "return f == nil and err ~= nil")
        /* and that the same chunk as source loads fine, so the test is
         * really about the mode and not about load being broken */
        && lua_says_true("return load('return 1', 'ok', 't')() == 1");
}

/*
 * M1: the interrupt controller and the timer.
 */
static uint64_t cntfrq(void)
{
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz;
}

static uint64_t cntpct(void)
{
    uint64_t now;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    return now;
}

static bool test_irqs_are_unmasked(void)
{
    /* DAIF bit 7 is the I mask. start.S enters EL1 with everything masked;
     * kmain clears it once there is a handler and a source. */
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (1UL << 7)) == 0;
}

static bool test_timer_ticks_advance(void)
{
    /*
     * Bounded by the counter rather than by a loop count, so a dead timer
     * fails in a tenth of a second instead of hanging until the host gives
     * up. Ten periods is a generous window for one interrupt.
     */
    unsigned long start = hal_ticks();
    uint64_t begin = cntpct();
    uint64_t limit = cntfrq() / 10;

    while (cntpct() - begin < limit) {
        if (hal_ticks() != start) {
            return true;
        }
    }

    return false;
}

static bool test_timer_period_matches_the_rate(void)
{
    /*
     * The test that would have caught the drift.
     *
     * Rearming the timer from "now" rather than from the previous deadline
     * folds the cost of taking the interrupt into every period. Nothing else
     * here notices: ticks still advance, the count is still exact, and a
     * nominal 100 Hz quietly runs at 73. Only measuring the counter across
     * several periods shows it.
     *
     * Five percent, because the last period is only bounded by when this
     * loop happens to observe it, not by the timer.
     */
    uint64_t expected = (cntfrq() / TICK_HZ) * 20;
    uint64_t tolerance = expected / 20;
    unsigned long start;
    uint64_t t0, elapsed;

    /* Start on a tick boundary so the first period is a whole one. */
    start = hal_ticks();
    while (hal_ticks() == start) { }

    t0 = cntpct();
    start = hal_ticks();
    while (hal_ticks() - start < 20) { }
    elapsed = cntpct() - t0;

    return elapsed > expected - tolerance
        && elapsed < expected + tolerance;
}

/*
 * M2: the freestanding libc.
 */
static bool test_memcpy_and_memcmp(void)
{
    char src[16] = "0123456789abcde";
    char dst[16];

    memset(dst, 0x5a, sizeof(dst));
    memcpy(dst, src, sizeof(src));

    return memcmp(dst, src, sizeof(src)) == 0;
}

static bool test_memset_fills_exactly(void)
{
    /* Guards on both sides, because the failure that matters is writing one
     * byte too many, and a fill that is correct in the middle looks correct. */
    unsigned char buf[16];

    memset(buf, 0x00, sizeof(buf));
    memset(buf + 4, 0xab, 8);

    return buf[3] == 0x00 && buf[4] == 0xab
        && buf[11] == 0xab && buf[12] == 0x00;
}

static bool test_memmove_handles_overlap(void)
{
    /*
     * The whole reason memmove exists. Shifting up by one overlaps, and a
     * forward copy reads bytes it has already written, smearing the first
     * byte across the buffer. memcpy is allowed to do exactly that.
     */
    char buf[8] = { 1, 2, 3, 4, 5, 6, 7, 0 };

    memmove(buf + 1, buf, 6);

    return buf[0] == 1 && buf[1] == 1 && buf[2] == 2
        && buf[3] == 3 && buf[6] == 6;
}

static bool test_string_functions(void)
{
    const char *s = "kosmos";

    return strlen("") == 0
        && strlen(s) == 6
        && strcmp("a", "a") == 0
        && strcmp("a", "b") < 0
        && strcmp("b", "a") > 0
        && strncmp("abcd", "abce", 3) == 0
        && strncmp("abcd", "abce", 4) < 0
        && strchr(s, 'm') == s + 3   /* k o s m */
        && strchr(s, 'z') == NULL
        && strchr(s, '\0') == s + 6;   /* the terminator is findable */
}

static bool test_fp_is_usable_at_el1(void)
{
    /*
     * CPACR_EL1.FPEN resets to "trap everything", and setjmp saves d8 to
     * d15. Without the enable in start.S, the first setjmp is an EC 0x07
     * trap whose name says nothing about floating point unless you go
     * looking. That is how it was found.
     *
     * Reading CPACR is not enough on its own, so this also executes an FP
     * instruction. The arithmetic is in assembly because the kernel is built
     * -mgeneral-regs-only and the compiler will not emit one.
     */
    uint64_t cpacr;
    uint64_t bits;

    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    if (((cpacr >> 20) & 3) != 3) {
        return false;
    }

    __asm__ volatile(
        "fmov d0, #1.0\n"
        "fadd d0, d0, d0\n"
        "fmov %0, d0\n"
        : "=r"(bits) : : "d0");

    return bits == 0x4000000000000000UL;    /* 2.0 as an IEEE-754 double */
}

/*
 * setjmp and longjmp.
 *
 * The piece that has to be right the first time. Lua raises every error
 * through it, so a mistake is invisible until the first error is raised,
 * which can be weeks after this was written.
 */
static jmp_buf test_jmp;

static void jump_back(int value)
{
    longjmp(test_jmp, value);
}

static bool test_setjmp_returns_zero_directly(void)
{
    return setjmp(test_jmp) == 0;
}

static bool test_longjmp_delivers_its_value(void)
{
    volatile int seen = -1;
    int r = setjmp(test_jmp);

    if (r == 0) {
        jump_back(42);
    }

    seen = r;
    return seen == 42;
}

static bool test_longjmp_turns_zero_into_one(void)
{
    /* The standard requires it, and Lua depends on it: setjmp has to be able
     * to tell a direct call from an arrival. */
    int r = setjmp(test_jmp);

    if (r == 0) {
        jump_back(0);
    }

    return r == 1;
}

static volatile unsigned long seed = 0x1000;

static bool test_longjmp_restores_callee_saved_registers(void)
{
    /*
     * Ten values live across the jump, taken from a volatile so the compiler
     * cannot rematerialise them as constants. With that many it has to hold
     * some in x19 to x28, which is exactly the set longjmp must restore. If
     * it does not, they come back as whatever the deeper frame left there.
     *
     * This is the test that catches a jmp_buf whose offsets drifted from the
     * assembly.
     */
    unsigned long a = seed + 1, b = seed + 2, c = seed + 3, d = seed + 4;
    unsigned long e = seed + 5, f = seed + 6, g = seed + 7, h = seed + 8;
    unsigned long i = seed + 9, j = seed + 10;
    volatile bool jumped = false;

    if (setjmp(test_jmp) == 0) {
        jumped = true;
        jump_back(1);
    }

    return jumped
        && a == seed + 1 && b == seed + 2 && c == seed + 3 && d == seed + 4
        && e == seed + 5 && f == seed + 6 && g == seed + 7 && h == seed + 8
        && i == seed + 9 && j == seed + 10;
}

static void jump_from_a_deep_frame(void)
{
    /*
     * A big frame rather than a recursive one. Recursion would say the same
     * thing, but GCC refuses to compile it: the only non-recursive path ends
     * in longjmp, which is noreturn, so it concludes the function never
     * returns and reports infinite recursion. It is not wrong about what it
     * can see. Four kilobytes moves sp far enough to prove the point.
     */
    volatile char pad[4096];

    pad[0] = 1;
    pad[sizeof(pad) - 1] = 2;

    if (pad[0] + pad[sizeof(pad) - 1] == 3) {
        longjmp(test_jmp, 9);
    }
}

static bool test_longjmp_restores_the_stack_pointer(void)
{
    /*
     * Jumping out of a deep call has to put sp back where setjmp saw it. If
     * it does not, the stack keeps growing every time an error is raised,
     * and Lua raises a lot of them: the symptom is a stack overflow after
     * running fine for a while, which is the worst kind to chase.
     */
    volatile unsigned long before;
    volatile unsigned long after;
    int r;

    __asm__ volatile("mov %0, sp" : "=r"(before));

    r = setjmp(test_jmp);
    if (r == 0) {
        jump_from_a_deep_frame();
    }

    __asm__ volatile("mov %0, sp" : "=r"(after));

    return r == 9 && after == before;
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
    { "trap: no fault means end() is false",   test_unexpected_fault_is_not_swallowed },
    { "trap: the kernel runs on SP_EL0",       test_kernel_runs_on_sp_el0 },
    { "trap: the handler has its own stack",   test_handler_runs_on_the_exception_stack },
    { "trap: a stack overflow is survivable",  test_a_stack_overflow_is_survivable },
    { "mmu: translation is on",                test_mmu_is_on },
    { "mmu: a null dereference faults",        test_null_dereference_faults },
    { "mmu: the stack guard is unmapped",      test_stack_guard_page_is_unmapped },
    { "mmu: kernel text is not writable",      test_kernel_text_is_not_writable },
    { "mmu: rodata is not writable",           test_rodata_is_not_writable },
    { "mmu: memory works through translation", test_memory_still_works_through_translation },
    { "libc: memcpy and memcmp",               test_memcpy_and_memcmp },
    { "libc: memset fills exactly its range",  test_memset_fills_exactly },
    { "libc: memmove handles overlap",         test_memmove_handles_overlap },
    { "libc: the string functions",            test_string_functions },
    { "fp: EL1 may use FP and SIMD",           test_fp_is_usable_at_el1 },
    { "libc: setjmp returns 0 when called",    test_setjmp_returns_zero_directly },
    { "libc: longjmp delivers its value",      test_longjmp_delivers_its_value },
    { "libc: longjmp turns 0 into 1",          test_longjmp_turns_zero_into_one },
    { "libc: longjmp restores x19-x28",        test_longjmp_restores_callee_saved_registers },
    { "libc: longjmp restores sp",             test_longjmp_restores_the_stack_pointer },
    { "heap: allocations are 16-byte aligned", test_heap_alloc_is_aligned_and_usable },
    { "heap: free coalesces both ways",        test_heap_coalesces_in_both_directions },
    { "heap: realloc preserves contents",      test_heap_realloc_preserves_contents },
    { "heap: exhaustion returns NULL",         test_heap_exhaustion_returns_null },
    { "snprintf: integers and strings",        test_snprintf_integers_and_strings },
    { "snprintf: truncates, reports full len", test_snprintf_truncates_and_reports_the_full_length },
    { "snprintf: floats",                      test_snprintf_floats },
    { "math: ours and newlib's agree",         test_our_math_matches_its_definition },
    { "lua: arithmetic, 2+2 is 4",             test_lua_arithmetic },
    { "lua: floats parse and print",           test_lua_floats },
    { "lua: strings and tables",               test_lua_strings_and_tables },
    { "lua: closures keep their upvalues",     test_lua_closures },
    { "lua: coroutines yield and resume",      test_lua_coroutines },
    { "lua: pcall catches errors",             test_lua_errors_are_caught },
    { "lua: nested pcall unwinds correctly",   test_lua_errors_nest },
    { "lua: the math library",                 test_lua_math_library },
    { "lua: the collector reclaims memory",    test_lua_garbage_collector_reclaims },
    { "lua: io, os, debug are absent",         test_lua_dangerous_libraries_are_absent },
    { "lua: load refuses bytecode",            test_lua_refuses_bytecode },
    { "irq: interrupts are unmasked",          test_irqs_are_unmasked },
    { "timer: ticks advance",                  test_timer_ticks_advance },
    { "timer: the period matches the rate",    test_timer_period_matches_the_rate },
    { "pmm: a page is aligned and in RAM",     test_pmm_alloc_is_page_aligned_and_in_ram },
    { "pmm: two allocations differ",           test_pmm_two_allocations_differ },
    { "pmm: the count tracks alloc and free",  test_pmm_count_tracks_alloc_and_free },
    { "pmm: a page is writable end to end",    test_pmm_page_is_writable },
    { "pmm: kernel pages are never free",      test_pmm_kernel_pages_are_not_free },
    { "pmm: exhaustion returns NULL",          test_pmm_exhaustion_returns_null },
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
