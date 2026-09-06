/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The first C that runs on x86-64, and for now the only C.
 *
 * `kernel/main.c` is the real one and this is not it. Bringing a second
 * architecture up means proving one thing at a time, and the first thing -
 * the thing every other thing needs before it can report a failure - is
 * that the machine got into long mode and can speak.
 *
 * The order this project used on ARM, and the reason it is worth repeating:
 * `docs/roadmap.md` records the first milestone as "boot under QEMU and
 * print", because a kernel that cannot print is a kernel debugged by
 * bisecting a hang.
 */

#include <stdint.h>

#include "context.h"
#include "console.h"
#include "cpu.h"
#include "gdt.h"
#include "hal.h"
#include "mmu.h"
#include "page.h"
#include "panic.h"
#include "pmm.h"
#include "trap.h"
#include "user.h"

void hal_ram_from_multiboot(uint32_t at);


/*
 * The kernel's own console, which is the first thing from `kernel/` that
 * runs on this machine.
 *
 * `kputs` is not `hal_putchar` in a loop: it is the serial line *and* the
 * screen when there is one, it keeps the log a process can read back, and
 * it is what `panic` prints through. There is no screen here yet, and
 * `console.c` has always handled that - `attached` stays false and the
 * screen half of it never runs - which is why it could come across before
 * `hal/pc/` has a framebuffer.
 */
static void say(const char *s)
{
    kputs(s);
}

static void say_hex(uint64_t v)
{
    kputs("0x");
    kputx(v, 16);
}

/*
 * `\n` and not `\r\n` anywhere below, which is not a style choice.
 *
 * `kputc` writes the carriage return itself, for the serial line and for
 * the screen separately, because a console that made every caller remember
 * would eventually meet one that did not. Writing both here produced a
 * blank line between every line of this file's output - the one visible
 * symptom of a private `say` being replaced by the real one.
 */

/*
 * What long mode looks like from the inside, reported rather than assumed.
 *
 * CR0.PG and EFER.LMA together are the only honest answer to "did the mode
 * switch work": LME is what was *asked* for and LMA is what the processor
 * says it *did*. They are set in different instructions and a boot that got
 * one and not the other is exactly the failure worth naming.
 */
extern char __text_start[], __rodata_start[], __stack_guard[];
extern char __stack_top[];

/* What the map should say about the three kinds of page it distinguishes. */
static void check_map(void)
{
    struct { const char *what; uintptr_t at; uint64_t want; uint64_t mask; }
    cases[] = {
        { "text   ", (uintptr_t)__text_start,   MAP_TEXT, PTE_P | PTE_RW | PTE_NX },
        { "rodata ", (uintptr_t)__rodata_start, MAP_RO,   PTE_P | PTE_RW | PTE_NX },
    };
    unsigned i;
    uint64_t *guard;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t *e = mmu_page_entry(cases[i].at);

        say("  ");
        say(cases[i].what);
        if (e == NULL) {
            say("  NO PAGE ENTRY\n");
        } else if ((*e & cases[i].mask) != (cases[i].want & cases[i].mask)) {
            say("  WRONG: "); say_hex(*e); say("\n");
        } else {
            say("  "); say_hex(*e); say("\n");
        }
    }

    /*
     * The address as well as the verdict, so that something outside this
     * machine can check it is the address the linker chose. A test that
     * only reads back the sentence this file printed is a restatement of
     * it, and `tools/run_headless.py` already learned that lesson about
     * counting `/bin` on both sides.
     */
    guard = mmu_page_entry((uintptr_t)__stack_guard);
    say("  guard    ");
    say_hex((uint64_t)(uintptr_t)__stack_guard);
    say((guard != NULL && (*guard & PTE_P) == 0) ? "  unmapped\n"
                                                 : "  STILL MAPPED\n");
}

/* ------------------------------------------------------------------ */
/* The context switch                                                   */
/* ------------------------------------------------------------------ */

static struct context main_ctx;
static struct context worker_ctx;

/* 16-aligned because `thread_entry` says why: the `call` in it needs
 * rsp+8 aligned to 16, and nothing called `thread_entry` to arrange that. */
static uint8_t worker_stack[8192] __attribute__((aligned(16)));

static volatile uint64_t worker_arg;
static volatile uint64_t worker_sum;
static volatile int worker_rounds;
static volatile int worker_returned;

/*
 * `thread_exit`, on loan alongside `panic` above.
 *
 * The real one is `kernel/thread.c`: it takes the thread off the runqueue
 * and switches away for good. There is no runqueue here, so this does the
 * one part that can be done - switch away and never come back - which is
 * enough to show that a thread running off its own end reaches it at all.
 */
void thread_exit(void)
{
    worker_returned = 1;
    context_switch(&worker_ctx, &main_ctx);

    panic("thread_exit: switched back into a thread that had finished");
}

static void worker(void *arg)
{
    /*
     * Six values that have to live across the switch, derived from `arg` so
     * the compiler cannot fold them away and has to keep them somewhere -
     * which under -O2 means the callee-saved registers, or the stack. This
     * checks the round trip preserves *something*; which of the two it
     * lands in is the compiler's choice, so the register-by-register test
     * is `tests/tests.c`'s job once the kernel builds here.
     */
    uint64_t m = (uint64_t)(uintptr_t)arg;
    uint64_t a = m + 1, b = m + 2, c = m + 3;
    uint64_t d = m + 4, e = m + 5, f = m + 6;

    worker_arg = m;
    worker_rounds++;

    context_switch(&worker_ctx, &main_ctx);

    worker_rounds++;
    worker_sum = a + b + c + d + e + f;

    /* And return, so `thread_entry` falls into `thread_exit`. */
}

/*
 * Two contexts, handed the processor back and forth.
 *
 * The worker's is built by hand exactly as `thread_create` will build one:
 * a stack, `thread_entry` as the resume address, the function in rbx and
 * its argument in r12. Nothing has ever switched away from it, so every
 * other field is zero - which is the point of planting rip rather than
 * relying on a `call` having happened.
 */
static void check_switch(void)
{
    const uint64_t marker = 0x5EED;
    uint64_t mine = 0xC0FFEE;

    /*
     * Through `context_init`, which is what `thread_create` will call: a
     * bring-up that builds a context by hand tests a path nothing else
     * uses. The worker has one stack rather than two, because a kernel
     * thread's exceptions land wherever it already is.
     */
    context_init(&worker_ctx, worker, (void *)(uintptr_t)marker,
                 &worker_stack[sizeof worker_stack], __stack_top);

    context_switch(&main_ctx, &worker_ctx);     /* into the worker */
    context_switch(&main_ctx, &worker_ctx);     /* and again, to finish it */

    say("  switch   ");
    if (worker_arg != marker) {
        say("THE ARGUMENT DID NOT ARRIVE\n");
    } else if (worker_rounds != 2) {
        say("THE WORKER DID NOT RUN TWICE\n");
    } else if (worker_sum != 6 * marker + 21) {
        say("VALUES DID NOT SURVIVE\n");
    } else if (!worker_returned) {
        say("thread_entry DID NOT REACH thread_exit\n");
    } else if (mine != 0xC0FFEE) {
        say("THE CALLER'S OWN LOCALS DID NOT SURVIVE\n");
    } else {
        say("two contexts, two round trips, and an exit\n");
    }
}

/* ------------------------------------------------------------------ */
/* Ring 3                                                               */
/* ------------------------------------------------------------------ */

/*
 * A program, in the only form there is one yet: eleven instructions and no
 * loader.
 *
 * It has to be position independent, because it is copied into a fresh page
 * and mapped at an address that has nothing to do with where it was linked
 * - which is what every program here will be, so it is worth the constraint
 * being real rather than arranged.
 *
 * It asks the kernel to double a number, checks nothing, hands the answer
 * back with a second call that does not return, and spins if it somehow
 * does. rax says what to do and rdi what to do it to.
 */
__asm__(
    ".section .text                 \n"
    ".globl probe_code              \n"
    "probe_code:                    \n"
    "    movq  $1, %rax             \n"     /* double this */
    "    syscall                    \n"
    "    movq  %rax, %rdi           \n"     /* and here is what you said */
    "    movq  $2, %rax             \n"
    "    syscall                    \n"     /* answers with a kernel address */
    "    movq  (%rax), %rdx         \n"     /* which must not be readable */
    "    movq  %rdx, %rdi           \n"     /* and if it was, say so */
    "    movq  $3, %rax             \n"
    "    syscall                    \n"
    "1:  jmp   1b                   \n"
    ".globl probe_code_end          \n"
    "probe_code_end:                \n"
);

extern char probe_code[], probe_code_end[];

#define PROBE_TEXT_VA   USER_VA_BASE
#define PROBE_STACK_VA  (USER_VA_BASE + 0x10000UL)
#define PROBE_MARKER    0x2A2A

static struct context ring3_ctx;
static struct context caller_ctx;
static uint8_t ring3_kstack[8192] __attribute__((aligned(16)));
static struct addrspace *ring3_space;

static volatile uint64_t ring3_answer;
static volatile int ring3_calls;
static volatile int ring3_read_kernel_memory;
static volatile uint64_t ring3_fault_error;
static volatile int ring3_faulted;

/*
 * What `syscall` ends up calling, on loan like `panic` and `thread_exit`.
 *
 * The real one is `kernel/syscall.c` and it takes a declared struct rather
 * than two integers. This one exists to show that the boundary works in
 * both directions: a number goes in, an answer comes back through
 * `sysretq`, and the second call leaves ring 3 for good.
 */
static void finish_ring3(void) __attribute__((noreturn));

uint64_t x86_syscall(uint64_t op, uint64_t arg)
{
    ring3_calls++;

    if (op == 1) {
        return arg * 2;
    }

    if (op == 2) {
        /*
         * The answer, and then an address the process may not read.
         *
         * `.text` is mapped in this address space - every space contains
         * the kernel, because there is no split like TTBR1 - so the page is
         * present and the only thing standing between a process and the
         * kernel's code is one bit in the entry describing it. Handing the
         * address over deliberately is the strongest form of the test:
         * there is nothing left for the process to fail to guess.
         */
        ring3_answer = arg;
        return (uint64_t)(uintptr_t)__text_start;
    }

    /* op 3: the read succeeded, which is the one answer that is a failure. */
    ring3_read_kernel_memory = 1;
    finish_ring3();
}

/*
 * What `process_exit` does, in the only part of it that exists here: put
 * the kernel's own address space back and return to whoever started the
 * process. It is the only way out, since `enter_user` does not return and
 * the frames it left on this stack were overwritten by the first syscall.
 */
static void finish_ring3(void)
{
    as_switch(NULL);
    context_switch(&ring3_ctx, &caller_ctx);

    panic("finish_ring3: returned into a process that had finished");
}

/*
 * A fault at ring 3, on loan alongside `panic` and `thread_exit`.
 *
 * `arch/aarch64/trap.c` calls `process_exit` at this point and this will
 * too once `kernel/process.c` builds here. The report has already been
 * printed by the time this runs; what is left is to stop the process
 * without stopping the machine, which is the distinction the whole design
 * rests on.
 */
void trap_user_fault(struct trapframe *f)
{
    ring3_faulted = 1;
    ring3_fault_error = f->error;

    finish_ring3();
    panic("trap_user_fault: unreachable");
}

static void ring3_thread(void *arg)
{
    (void)arg;

    as_switch(ring3_space);
    enter_user(PROBE_TEXT_VA, PROBE_STACK_VA + PAGE_SIZE, PROBE_MARKER);
}

static void check_ring3(void)
{
    uint8_t *code = pmm_alloc_page();
    void *stack = pmm_alloc_page();
    size_t len = (size_t)(probe_code_end - probe_code);
    size_t i;

    ring3_space = as_create();

    if (code == NULL || stack == NULL || ring3_space == NULL) {
        say("  ring3    NO PAGES\n");
        return;
    }

    for (i = 0; i < len; i++) {
        code[i] = (uint8_t)probe_code[i];
    }

    if (as_map(ring3_space, PROBE_TEXT_VA, (uintptr_t)code, 1,
               MAP_USER_RX) != AS_OK
        || as_map(ring3_space, PROBE_STACK_VA, (uintptr_t)stack, 1,
                  MAP_USER_RW) != AS_OK) {
        say("  ring3    MAP REFUSED\n");
        return;
    }

    /*
     * Its own kernel stack, and the same one twice: the thread runs on it
     * until `enter_user`, and an entry from ring 3 restarts from the top of
     * it - which is safe because `enter_user` does not return and the
     * frames below are dead the moment it is called.
     */
    context_init(&ring3_ctx, ring3_thread, NULL,
                 &ring3_kstack[sizeof ring3_kstack],
                 &ring3_kstack[sizeof ring3_kstack]);

    caller_ctx.kernel_stack = (uint64_t)(uintptr_t)__stack_top;

    context_switch(&caller_ctx, &ring3_ctx);

    say("\n  ring3    ");
    if (ring3_calls < 2) {
        say("THE PROCESS DID NOT MAKE TWO CALLS\n");
    } else if (ring3_answer != 2 * PROBE_MARKER) {
        say("THE ANSWER DID NOT COME BACK\n");
    } else if (ring3_read_kernel_memory) {
        say("A PROCESS READ THE KERNEL'S OWN TEXT\n");
    } else if (!ring3_faulted) {
        say("NEITHER FAULTED NOR REPORTED\n");
    } else {
        say_hex(ring3_answer);
        say(" from ");
        say_hex(PROBE_MARKER);
        say(", doubled at ring 0 and returned to ring 3\n");

        say("  confined ");
        /* present | read | user: the page is there and the process is not
         * allowed to see it, which is exactly the bit being tested. */
        say((ring3_fault_error & 0x5) == 0x5
            ? "the kernel's text is present and unreadable to it\n"
            : "IT FAULTED, BUT NOT FOR THE RIGHT REASON\n");
    }
}

/*
 * An address space, used and given back.
 *
 * The count either side is the part that matters. A space allocates its
 * PML4, its PDPT and a table per level it touches; `as_destroy` has to hand
 * back exactly those and none of the kernel's, and a leak here is the shape
 * of bug that only shows up on the fiftieth process.
 */
static void check_spaces(void)
{
    size_t before = pmm_free_pages();
    struct addrspace *as = as_create();
    uint64_t *entry;

    if (as == NULL) {
        say("  as       COULD NOT CREATE\n");
        return;
    }

    if (as_map(as, USER_VA_BASE, (uintptr_t)__rodata_start, 1,
               MAP_USER_RW) != AS_OK) {
        say("  as       MAP REFUSED\n");
        return;
    }

    /* Below the user region the tables are the kernel's, and writing there
     * would edit every space at once. It has to be refused. */
    if (as_map(as, PAGE_SIZE, PAGE_SIZE, 1, MAP_USER_RW) != AS_ERR_RANGE) {
        say("  as       THE KERNEL REGION WAS WRITABLE\n");
        return;
    }

    entry = as_page_entry(as, USER_VA_BASE);
    say("  as       ");
    if (entry == NULL || (*entry & PTE_US) == 0) {
        say("NO USER PAGE\n");
    } else {
        say_hex(*entry);
        say("\n");
    }

    /* And the kernel is still in it, sharing rather than copied. */
    say("  as text  ");
    entry = as_page_entry(as, (uintptr_t)__text_start);
    say((entry != NULL && *entry == *mmu_page_entry((uintptr_t)__text_start))
        ? "shared with the kernel\n" : "MISSING OR DIFFERENT\n");

    as_destroy(as);

    say("  as pages ");
    if (pmm_free_pages() == before) {
        say("all returned\n");
    } else {
        say_hex(before - pmm_free_pages());
        say(" LEAKED\n");
    }
}

void kmain_x86(uint32_t multiboot)
{
    uint64_t cr0, cr3, efer;

    hal_early_init();

    say("\nKosmos on x86-64.\n");

    __asm__ volatile ("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("movq %%cr3, %0" : "=r"(cr3));
    /*
     * `rdmsr` answers in EDX:EAX - two 32-bit halves, always, even in long
     * mode. The `"=A"` constraint means that pair on 32-bit x86 and means
     * "rax or rdx, whichever" on x86-64, so asking for it here reads half
     * the register and looks like it worked.
     */
    {
        uint32_t lo, hi;

        __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        efer = ((uint64_t)hi << 32) | lo;
    }

    say("  cr0      "); say_hex(cr0);  say("\n");
    say("  cr3      "); say_hex(cr3);  say("\n");
    say("  efer     "); say_hex(efer); say("\n");
    say("  multiboot "); say_hex(multiboot); say("\n");

    say(((cr0 >> 31) & 1) ? "  paging on\n" : "  PAGING OFF\n");
    say(((efer >> 10) & 1) ? "  long mode active\n"
                           : "  LONG MODE NOT ACTIVE\n");

    /* Where the RAM is, before anything asks for a page of it. */
    hal_ram_from_multiboot(multiboot);

    {
        struct memrange ram;

        hal_ram_range(&ram);
        say("  ram at   "); say_hex(ram.base);
        say(" for ");       say_hex(ram.size);
        say(" ("); 
        say_hex(ram.size >> 20);
        say(" MB)\n");
    }

    trap_init();
    say("  idt      installed, 48 vectors\n");

    /*
     * The descriptor tables, before interrupts and before the map.
     *
     * Before interrupts because an interrupt loads CS from a GDT
     * descriptor, and the processor *writes* the accessed bit when it does.
     * `start.S`'s table is in .rodata, which `mmu_init` is about to narrow
     * to read-only - and a descriptor whose accessed bit is still clear
     * would then fault on the next interrupt, from inside the interrupt.
     * The table `gdt_init` builds lives in .bss and is writable, which
     * removes the question rather than relying on the first tick having
     * already set the bit.
     */
    gdt_init();
    user_init();
    say("  gdt      5 descriptors, a tss, and syscall armed\n");

    /*
     * The interrupt path, end to end, before anything depends on it.
     *
     * The controller has to be remapped before `sti` or the first tick
     * arrives as a double fault, so the order here is not a style: PIC,
     * then timer, then interrupts on, then wait and see whether the count
     * moved. `hlt` rather than a spin, because a spin cannot tell the
     * difference between a tick arriving and the loop being slow.
     */
    hal_irq_init();
    hal_timer_init(100);
    say("  pic      remapped to 32, timer at 100 Hz\n");

    cpu_irq_enable();

    {
        unsigned long before = hal_ticks();
        int waits;

        for (waits = 0; waits < 20; waits++) {
            __asm__ volatile ("hlt");
        }

        say("  ticks    ");
        say_hex(hal_ticks());
        say(hal_ticks() > before ? "  (rising)\n"
                                 : "  NO TICK ARRIVED\n");
    }

    /*
     * The page allocator, and then the real address space.
     *
     * This order is `kernel/main.c`'s and it is forced: the page tables
     * come out of `pmm_alloc_page`, so there has to be an allocator before
     * there can be a map.
     */
    pmm_init();
    say("  pmm      ");
    say_hex(pmm_free_pages());
    say(" of ");
    say_hex(pmm_total_pages());
    say(" pages free\n");

    mmu_init();
    say("  mmu      four levels, loaded");
    say(mmu_is_enabled() ? "\n" : "  BUT PAGING IS OFF\n");

    /*
     * Printing this sentence is most of the proof, and it is worth saying
     * why: it went through the UART driver in .text, a string in .rodata
     * and a stack frame in the guarded stack, all three of which are now
     * described by tables this file built rather than by the three static
     * ones in `start.S`.
     */
    check_map();
    check_spaces();
    check_switch();
    check_ring3();

    /*
     * And a fault on purpose, because an exception handler that has never
     * run is an exception handler that does not work - and because a
     * permission nothing has tested is a permission that might not be
     * there.
     *
     * Writing to .text rather than to unmapped memory: the boot map made
     * every page writable, so this fault can only come from the narrowing
     * pass in `mmu_init`. The error code should say `protection, write,
     * kernel` rather than `not present`, and that difference is the whole
     * of what is being shown.
     */
    say("\nWriting to __text_start, which is mapped read-only:\n");

    *(volatile uint64_t *)(uintptr_t)__text_start = 1;

    say("*** the write succeeded, so .text is not read-only\n");

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
