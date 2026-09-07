/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Exceptions on x86-64.
 *
 * The counterpart of `arch/aarch64/trap.c`, and the job is the same one:
 * when something goes wrong, say what and where, in enough detail that the
 * next person does not have to bisect a hang. That file exists because a
 * fault with no handler is a machine that stops with no output at all, and
 * the reasoning carries over exactly.
 *
 * What does not carry over is the mechanism. ARM has a vector *table* of
 * sixteen entries and one syndrome register that says what happened. x86
 * has 256 gate descriptors, each holding a code selector and a handler
 * address split across three fields, and the only thing identifying the
 * fault is which gate the processor went through.
 */

#include <stdint.h>

#include <setjmp.h>

#include "console.h"
#include "cpu.h"
#include "process.h"
#include "sched.h"
#include "thread.h"
#include "hal.h"
#include "trap.h"

/*
 * Thirty-two exceptions and sixteen hardware interrupts.
 *
 * The architecture reserves 0-31 and the PIC was remapped to 32, so the
 * table is exactly as long as there is something to put in it. There are
 * 256 possible vectors; the rest would be entries pointing at a handler
 * that says "this cannot happen", and it genuinely cannot until something
 * sends one.
 */
#define IDT_ENTRIES  48
#define IRQ_BASE     32

/*
 * A gate descriptor, and the reason it looks like this is history.
 *
 * The handler address is 64 bits split into three fields with the type in
 * between them, because the 64-bit descriptor was grown from the 32-bit one
 * by appending the high half rather than by redesigning it. There is no
 * meaning to find in the layout; it is a shape to match exactly.
 *
 * Intel SDM volume 3, figure 6-8.
 */
struct gate {
    uint16_t handler_low;
    uint16_t selector;
    uint8_t  ist;               /* 0: use the stack we are already on */
    uint8_t  flags;
    uint16_t handler_mid;
    uint32_t handler_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The two vectors that get a stack of their own, and the slot they use.
 * `trap_init` says why these two and not the rest. */
#define VECTOR_DOUBLE_FAULT     8
#define VECTOR_PAGE_FAULT       14
#define IST_EXCEPTION_STACK     1       /* tss.ist[0]; the field is 1-based */

static struct gate idt[IDT_ENTRIES];

/* The stubs in `vectors.S`, which is where the vector number comes from. */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr34(void);
extern void isr35(void); extern void isr36(void); extern void isr37(void);
extern void isr38(void); extern void isr39(void); extern void isr40(void);
extern void isr41(void); extern void isr42(void); extern void isr43(void);
extern void isr44(void); extern void isr45(void); extern void isr46(void);
extern void isr47(void);

static void (*const stubs[IDT_ENTRIES])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

/*
 * The names, because a vector number is not a diagnosis.
 *
 * `arch/aarch64/trap.c` decodes ESR_EL1 into a sentence for the same
 * reason: "sync exception, EC 0x25" sends you to the manual, and
 * "translation fault, level 2, on a write" tells you what to look at.
 */
static const char *const names[32] = {
    "divide error", "debug", "non-maskable interrupt", "breakpoint",
    "overflow", "bound range exceeded", "invalid opcode",
    "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack-segment fault",
    "general protection fault", "page fault", "reserved",
    "x87 floating-point error", "alignment check", "machine check",
    "SIMD floating-point error", "virtualisation exception",
    "control protection", "reserved", "reserved", "reserved", "reserved",
    "reserved", "reserved", "reserved", "VMM communication",
    "security exception", "reserved",
};

/*
 * Through the kernel's console rather than through a second private one.
 *
 * `arch/aarch64/trap.c` prints a fault with `kputs` for a reason this file
 * will need as soon as there is a framebuffer: a report that goes only to
 * the serial line is a machine that stops with a desktop on it and no
 * explanation anywhere a person is looking.
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

static void line(const char *label, uint64_t value)
{
    say("  ");
    say(label);
    say("  ");
    say_hex(value);
    say("\n");
}

void trap_init(void)
{
    struct idtr pointer;
    unsigned i;

    for (i = 0; i < IDT_ENTRIES; i++) {
        uint64_t at = (uint64_t)stubs[i];

        idt[i].handler_low  = (uint16_t)at;
        idt[i].handler_mid  = (uint16_t)(at >> 16);
        idt[i].handler_high = (uint32_t)(at >> 32);

        /* 0x08 is the code selector the boot GDT put at index 1. */
        idt[i].selector = 0x08;
        idt[i].ist      = 0;

        /* present | DPL 0 | 64-bit interrupt gate. An *interrupt* gate
         * rather than a trap gate: it clears IF on entry, so a handler is
         * not itself interrupted before it has saved anything. */
        idt[i].flags    = 0x8E;
        idt[i].reserved = 0;
    }

    /*
     * The two that land on a stack of their own.
     *
     * **Which is what makes a kernel stack overflow survivable**, and is
     * the piece this architecture has to be told and AArch64 gets for
     * free. There the kernel runs on SP_EL0, an exception switches to
     * SP_EL1, and the handler always has a stack that is not the one that
     * faulted. Here a fault from ring 0 stays on the faulting stack unless
     * the gate names an entry in the interrupt stack table - and with
     * every gate naming none, an overflow pushed the fault frame into the
     * guard page, faulted again delivering *that*, and triple-faulted the
     * machine. It reset with nothing printed, which is the exact failure
     * the whole exception dump exists to eliminate.
     *
     * `#PF` is where an overflow arrives, so it is the one that has to
     * move. `#DF` is the backstop: it fires when a fault could not be
     * delivered at all, which with #PF on a good stack now means only that
     * this stack is itself bad - and its own guard page is what catches
     * that.
     *
     * **Two, and not all of them**, because an IST stack is not reentrant:
     * the processor loads the same address every time, so a second fault
     * taken while one is being handled writes over the frame being
     * handled. Linux reserves IST for exactly this handful and for the
     * same reason. Everything else - an undefined instruction, a
     * breakpoint, a general protection fault - is a fault by something
     * that still has a working stack, and is better handled on it.
     *
     * The index is 1-based here and 0-based in the TSS, so 1 selects
     * `tss.ist[0]`. `gdt.c` sets it.
     */
    idt[VECTOR_DOUBLE_FAULT].ist = IST_EXCEPTION_STACK;
    idt[VECTOR_PAGE_FAULT].ist   = IST_EXCEPTION_STACK;

    pointer.limit = (uint16_t)(sizeof(idt) - 1);
    pointer.base  = (uint64_t)idt;

    __asm__ volatile("lidt %0" :: "m"(pointer));
}

/*
 * A process that was killed while it was running ends here, on its way back
 * to ring 3.
 *
 * `arch/aarch64/trap.c` has the same four lines and the same reasoning: a
 * kill cannot take effect where it is asked, because the target may be
 * anywhere at all. It is recorded, and every return to user level passes
 * through this. `process_exit` does not return.
 */
static void die_if_killed(void)
{
    if (process_should_die()) {
        process_exit(thread_current()->process, -1);
    }
}

/*
 * The armed-fault slot. A single one on purpose, for the reason
 * `arch/aarch64/trap.c` gives: a test arms it, causes exactly one fault,
 * and disarms. Nesting would mean a fault inside the handler, which is a
 * double fault and should be a panic rather than a feature.
 *
 * Only the unwind form here - `trap.h` says why, and it is that an x86
 * instruction has no length you can know without decoding it.
 */
static struct {
    bool armed;
    bool fired;
    unsigned long *unwind_to;
    struct fault_info info;
} expected;

void fault_expect_unwind(jmp_buf env)
{
    expected.armed = true;
    expected.fired = false;
    expected.unwind_to = env;
}

bool fault_expect_end(struct fault_info *out)
{
    bool fired = expected.fired;

    if (fired && out != NULL) {
        *out = expected.info;
    }

    expected.armed = false;
    expected.fired = false;
    expected.unwind_to = NULL;
    return fired;
}

void trap_handle(struct trapframe *f)
{
    uint64_t cr2;

    /*
     * A hardware interrupt, which is not a failure and must not print.
     *
     * Same entry path, same frame, one `isr_common` for both - the vector
     * is the only thing that tells them apart, which is why the stubs from
     * 32 up push a zero error code they do not have: so this function can
     * read one shape.
     *
     * `hal_irq_handle` is what the ARM vector calls at exactly this point,
     * and it takes no argument on purpose. Which interrupt it was is a
     * question for the board's controller, and asking it here would mean
     * this file knew what a PIC is.
     */
    if (f->vector >= IRQ_BASE) {
        hal_irq_handle();

        /*
         * And what an interrupt *means*, which acknowledging it does not
         * do. `arch/aarch64/trap.c` does all of this and the x86 side did
         * none of it for a while - which is a machine that boots to a
         * prompt, prints it, and then ignores everything typed at it,
         * because nothing was polling the serial line and nothing was
         * preempting the thread that had the processor.
         *
         * The sound device asking for a period is woken here rather than
         * inside the driver, because waking a thread is the kernel's
         * business and `hal/` may not reach into it - the same separation
         * that keeps hardware addresses out of everything above it.
         */
        if (hal_snd_wants()) {
            process_wake_audio();
        }

        if (hal_input_pending_peek()) {
            thread_wake_sleepers_now();
        }

        /*
         * The timer is the scheduler's clock, and it is the only interrupt
         * source unmasked, so every one of these is a tick. When there is a
         * second, `hal_irq_handle` has to say which fired rather than this
         * assuming.
         *
         * `thread_tick` only records what the policy wants; the switch
         * happens on the way out. `die_if_killed` is what a process that
         * was killed while it ran passes through, and it must come after
         * both - there is nothing to reschedule once it is gone.
         */
        thread_tick();
        console_tick();

        if (f->cs & 3) {
            die_if_killed();
        }

        return;
    }

    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));

    /*
     * An expected fault: record it and unwind, rather than report and stop.
     *
     * Before the report and before the ring test, because an armed fault is
     * a fault somebody asked for and printing a panic for it would be
     * noise. Everything below this line is the unexpected case.
     */
    if (expected.armed && !expected.fired) {
        uint64_t handler_sp;

        __asm__ volatile("movq %%rsp, %0" : "=r"(handler_sp));

        expected.fired = true;
        expected.info.vector = f->vector;
        expected.info.error  = f->error;
        expected.info.rip    = f->rip;
        expected.info.cr2    = cr2;
        expected.info.handler_sp = handler_sp;

        /*
         * Return into longjmp rather than into the faulting code. `iret`
         * restores rip, cs, rflags, rsp and ss from the frame, and
         * `isr_common` restores rdi and rsi from it on the way out - so
         * setting rdi, rsi and rip is a complete call to longjmp(env, 1).
         */
        f->rdi = (uint64_t)(uintptr_t)expected.unwind_to;
        f->rsi = 1;
        f->rip = (uint64_t)(uintptr_t)&longjmp;
        return;
    }

    /*
     * Who was running decides what this is. A fault at ring 0 is a broken
     * kernel and the machine stops; a fault at ring 3 is a broken process
     * and only the process stops. The low two bits of the saved CS are the
     * privilege level the processor was at, which it pushed for us.
     */
    say((f->cs & 3) ? "\nprocess died: " : "\n*** ");
    say(f->vector < 32 ? names[f->vector] : "unknown exception");
    say("\n");

    line("vector ", f->vector);
    line("error  ", f->error);
    line("rip    ", f->rip);
    line("cs     ", f->cs);
    line("rflags ", f->rflags);
    line("rsp    ", f->rsp);

    /*
     * CR2 only means anything for a page fault - it is where the faulting
     * access went. Printing it for every exception would be printing a
     * stale address from the last one, which is worse than printing
     * nothing because it looks like evidence.
     */
    /*
     * The word on top of the faulting stack, which is the return address
     * when a function faults on its own first instruction - and that is
     * more often than it sounds, because the first instruction is where a
     * bad `this` pointer is dereferenced.
     *
     * Only for a fault from ring 3, and only after the stack pointer has
     * been checked: reading it is reading memory a process chose, and a
     * fault handler that faults is a double fault. `process_may_read` is
     * the same check every syscall pointer goes through.
     */
    if ((f->cs & 3) != 0) {
        struct process *p = process_current();

        if (p != NULL && process_may_read(p, (uintptr_t)f->rsp,
                                          sizeof(uint64_t))) {
            line("caller ", *(const uint64_t *)(uintptr_t)f->rsp);
        }
    }

    if (f->vector == 14) {
        line("cr2    ", cr2);

        say("  ");
        say((f->error & 1) ? "protection" : "not present");
        say(", ");
        say((f->error & 2) ? "write" : "read");
        say(", ");
        say((f->error & 4) ? "user" : "kernel");
        say("\n");
    }

    /*
     * A process doing something it may not is a dead process, not a dead
     * machine. `arch/aarch64/trap.c` does exactly this and calls it "the
     * whole point of a microkernel, and one line of code because the
     * hardware already did the work".
     *
     * From ring 3 with no process is a kernel bug rather than a process
     * one, and falls through to the halt below.
     */
    if (f->cs & 3) {
        struct process *p = process_current();

        if (p != NULL) {
            process_exit(p, -1);    /* never returns */
        }
    }

    say("\nhalted.\n");

    for (;;) {
        cpu_irq_disable();
        cpu_wait_for_interrupt();
    }
}

const char *trap_describe(void)
{
    return "48 gates in an IDT: 32 exceptions and 16 interrupts";
}
