/*
 * What happens after vectors.S has built the frame.
 *
 * The whole point of this file is the dump. An exception handler that says
 * which instruction faulted and on what address turns a silent hang into a
 * sentence, and it is the tool used for the rest of the project. Everything
 * else here is in service of that.
 */

#include <stdbool.h>
#include <stdint.h>

#include <setjmp.h>

#include "trap.h"
#include "console.h"
#include "hal.h"
#include "thread.h"
#include "process.h"
#include "syscall.h"

/* The table in vectors.S. */
extern char vectors[];

static const char *const vector_name[16] = {
    "sync (current EL, SP_EL0)",
    "irq (current EL, SP_EL0)",
    "fiq (current EL, SP_EL0)",
    "serror (current EL, SP_EL0)",
    "sync (current EL, SP_ELx)",
    "irq (current EL, SP_ELx)",
    "fiq (current EL, SP_ELx)",
    "serror (current EL, SP_ELx)",
    "sync (lower EL, AArch64)",
    "irq (lower EL, AArch64)",
    "fiq (lower EL, AArch64)",
    "serror (lower EL, AArch64)",
    "sync (lower EL, AArch32)",
    "irq (lower EL, AArch32)",
    "fiq (lower EL, AArch32)",
    "serror (lower EL, AArch32)",
};

/* ESR_EL1 exception class. ARM ARM D17.2.37, table of EC encodings. */
/* Lazy FP save, from arch/aarch64/fp.c. */
void fp_fault(void);

static const char *ec_name(unsigned ec)
{
    switch (ec) {
    case EC_UNKNOWN:    return "unknown reason";
    case 0x01:          return "trapped WFI or WFE";
    case 0x0e:          return "illegal execution state";
    case EC_SVC64:      return "SVC from AArch64";
    case 0x18:          return "trapped MSR, MRS or system instruction";
    case EC_IABT_LOWER: return "instruction abort from a lower EL";
    case EC_IABT_SAME:  return "instruction abort";
    case EC_PC_ALIGN:   return "PC alignment fault";
    case EC_DABT_LOWER: return "data abort from a lower EL";
    case EC_DABT_SAME:  return "data abort";
    case EC_SP_ALIGN:   return "SP alignment fault";
    case 0x2f:          return "SError";
    case EC_BRK64:      return "BRK instruction";
    default:            return "unhandled exception class";
    }
}

/* Data/instruction fault status code, ISS bits 5:0. ARM ARM D17.2.37. */
static const char *dfsc_name(unsigned dfsc)
{
    switch (dfsc) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return "address size fault";
    case 0x04: return "translation fault, level 0";
    case 0x05: return "translation fault, level 1";
    case 0x06: return "translation fault, level 2";
    case 0x07: return "translation fault, level 3";
    case 0x09: return "access flag fault, level 1";
    case 0x0a: return "access flag fault, level 2";
    case 0x0b: return "access flag fault, level 3";
    case 0x0d: return "permission fault, level 1";
    case 0x0e: return "permission fault, level 2";
    case 0x0f: return "permission fault, level 3";
    case 0x10: return "external abort";
    case 0x21: return "alignment fault";
    default:   return "unknown fault status";
    }
}

static bool is_abort(unsigned ec)
{
    return ec == EC_DABT_SAME  || ec == EC_DABT_LOWER
        || ec == EC_IABT_SAME  || ec == EC_IABT_LOWER;
}

/*
 * The armed-fault slot. A single one on purpose: a test arms it, causes
 * exactly one fault, and disarms. Nesting would mean a fault inside the
 * handler, which is a double fault and should be a panic, not a feature.
 */
static struct {
    bool armed;
    bool fired;
    unsigned long *unwind_to;   /* NULL means step past it instead */
    struct fault_info info;
} expected;

void fault_expect_begin(void)
{
    expected.armed = true;
    expected.fired = false;
    expected.unwind_to = NULL;
}

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

/* Everything after the first line, shared by the kernel's panic and the
 * report a dying process gets. The two differ in what they say happened and
 * in what follows; the registers are the same registers. */
static void dump_body(unsigned index, const struct trapframe *tf)
{
    unsigned ec  = (unsigned)ESR_EC(tf->esr);
    unsigned iss = (unsigned)ESR_ISS(tf->esr);

    kputs("\n  vector  ");
    kputu(index);
    kputs("  ");
    kputs(vector_name[index & 15]);

    kputs("\n  esr     0x");
    kputx(tf->esr, 16);
    kputs("   ec 0x");
    kputx(ec, 2);
    kputs("  iss 0x");
    kputx(iss, 7);

    kputs("\n  elr     0x");
    kputx(tf->elr, 16);
    kputs("   <- the instruction that faulted");

    kputs("\n  far     0x");
    kputx(tf->far, 16);
    kputs("   <- the address it touched");

    kputs("\n  spsr    0x");
    kputx(tf->spsr, 16);
    kputs("\n  sp      0x");
    kputx(tf->sp, 16);

    if (is_abort(ec)) {
        kputs("\n  cause   ");
        if (ec == EC_DABT_SAME || ec == EC_DABT_LOWER) {
            kputs(ISS_DABT_WNR(iss) ? "write, " : "read, ");
        }
        kputs(dfsc_name((unsigned)ISS_DABT_DFSC(iss)));
    }

    kputs("\n");
}

static void dump(unsigned index, const struct trapframe *tf)
{
    kputs("\nPANIC: ");
    kputs(ec_name((unsigned)ESR_EC(tf->esr)));
    dump_body(index, tf);
}

/*
 * A process that has been killed ends here, on its way back to EL0.
 *
 * Not where the kill was asked for: tearing a process down ends with the
 * thread doing it, so it has to be that process's own thread, in its own
 * context. Both places this is called from are exactly that - a syscall
 * about to return, and a timer interrupt taken from EL0 - and between them
 * they bound the delay at one timer period even for a process that has
 * stopped making syscalls, which is the case a kill exists for.
 *
 * `process_exit` does not return.
 */
static void die_if_killed(void)
{
    if (process_should_die()) {
        process_exit(thread_current()->process, -1);
    }
}

void trap_handler(unsigned index, struct trapframe *tf)
{
    /*
     * An armed test fault: record it and step over the instruction that
     * caused it. Every A64 instruction is four bytes, so advancing elr by
     * four resumes at exactly the next one.
     *
     * Only synchronous exceptions are recoverable this way. An IRQ did not
     * come from the instruction at elr, so stepping past it would skip a
     * perfectly good instruction.
     */
    bool synchronous = (index == 0 || index == 4 || index == 8 || index == 12);

    /*
     * A floating-point instruction while FP was disarmed, from either level.
     *
     * First, and before the fault-expectation machinery gets a look at it,
     * because this is not a fault in any sense worth reporting: it is how a
     * thread comes to own the registers. The switch turns FP off and the
     * first instruction that wants it lands here; `fp_fault` moves the
     * register file to its new owner and returns, and the instruction that
     * trapped is re-executed rather than stepped over.
     *
     * Not stepping `elr` is the whole difference between this and every
     * other synchronous exception here, and getting it wrong would silently
     * skip one arithmetic instruction per time slice.
     */
    if (synchronous && ESR_EC(tf->esr) == EC_SIMD_FP) {
        fp_fault();
        return;
    }

    /*
     * Where the handler itself is running. The stack overflow test asserts
     * this is the exception stack and not the thread's, which is the only
     * direct evidence that the two are actually separate.
     */
    uint64_t handler_sp;
    __asm__ volatile("mov %0, sp" : "=r"(handler_sp));

    if (expected.armed && !expected.fired && synchronous) {
        expected.fired     = true;
        expected.info.esr  = tf->esr;
        expected.info.far  = tf->far;
        expected.info.elr  = tf->elr;
        expected.info.handler_sp = handler_sp;

        if (expected.unwind_to != NULL) {
            /*
             * Return into longjmp rather than into the faulting code. The
             * eret restores x0 and x1 from the frame and jumps to elr, so
             * setting all three is a complete call: longjmp(env, 1), running
             * on SP_EL0 with SPSel back to 0 where it belongs.
             *
             * SP_EL0 is left where the fault found it, which may be inside
             * the guard page. That is safe because longjmp touches no stack
             * at all: it only loads from the buffer and then sets sp.
             */
            tf->x[0] = (uint64_t)(uintptr_t)expected.unwind_to;
            tf->x[1] = 1;
            tf->elr  = (uint64_t)(uintptr_t)&longjmp;
            return;
        }

        tf->elr += 4;
        return;
    }

    /*
     * An IRQ. Vector 1, because the kernel runs on SP_EL0: an exception
     * taken while SPSel is 0 lands in the "current EL, SP_EL0" quarter.
     * Vector 9 joins it when userland arrives at M4.
     */
    if (index == 1) {
        hal_irq_handle();

        /*
         * The timer is the scheduler's clock, and it is the only interrupt
         * source there is, so every IRQ is a tick. When there is a second
         * source, hal_irq_handle has to say which one fired rather than this
         * assuming.
         *
         * It only records what the policy wants. The switch itself happens
         * in the epilogue, after this returns.
         */
        thread_tick();
        console_tick();
        return;
    }

    /*
     * Vectors 4 through 7 are exceptions taken while SPSel was already 1,
     * which means inside the handler. Nothing else runs there, so this is a
     * fault in the fault path and the state is not to be trusted. Saying so
     * is worth more than the dump that follows, because the dump describes
     * the second fault and the interesting one was the first.
     */
    if (index >= 4 && index <= 7) {
        kputs("\nPANIC: double fault - an exception inside the handler\n");
    }

    /*
     * Everything from EL0: vectors 8 to 11.
     *
     * A syscall is the expected case. Anything else is a process doing
     * something it may not, and the answer is to kill the process rather
     * than the machine. That distinction is the whole point of a
     * microkernel, and it is one line of code because the hardware already
     * did the work.
     */
    if (index >= 8 && index <= 11) {
        struct process *p = process_current();

        if (index == 8 && ESR_EC(tf->esr) == EC_SVC64) {
            syscall_dispatch(tf);
            die_if_killed();
            return;
        }



        if (index == 9) {
            hal_irq_handle();

            /*
             * An input interrupt is a reason for somebody to stop sleeping,
             * and the only thing that knows a thread asked to sleep is the
             * scheduler. Waking every sleeper is right rather than lazy:
             * only the reader of input ever sleeps on it, because only it is
             * allowed to ask.
             */
            if (hal_input_pending_peek()) {
                thread_wake_sleepers_now();
            }

            thread_tick();
            console_tick();
            die_if_killed();
            return;
        }

        if (p != NULL) {
            /*
             * Reported before it is killed. A process dying silently is a
             * process whose bug you never find, and the dump names the
             * instruction and the address exactly as it does for the kernel.
             */
            kputs("\nprocess \"");
            kputs(p->name);
            kputs("\" died: ");
            kputs(ec_name((unsigned)ESR_EC(tf->esr)));
            dump_body(index, tf);

            process_exit(p, -1);    /* never returns */
        }

        /* From EL0 with no process is a kernel bug, not a process one. */
    }

    dump(index, tf);

    /* Nothing here knows how to recover. Halting is the honest answer, and
     * the dump above is the thing worth reading. */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void trap_init(void)
{
    __asm__ volatile(
        "msr vbar_el1, %0\n"
        /* The vector table has to be in place before anything can fault, so
         * the write must have taken effect before the next instruction is
         * fetched. That is what isb is for; a dmb would only order it. */
        "isb\n"
        : : "r"(vectors) : "memory");
}
