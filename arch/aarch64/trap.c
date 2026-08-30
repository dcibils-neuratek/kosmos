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

#include "trap.h"
#include "console.h"

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
    struct fault_info info;
} expected;

void fault_expect_begin(void)
{
    expected.armed = true;
    expected.fired = false;
}

bool fault_expect_end(struct fault_info *out)
{
    bool fired = expected.fired;

    if (fired && out != NULL) {
        *out = expected.info;
    }

    expected.armed = false;
    expected.fired = false;
    return fired;
}

static void dump(unsigned index, const struct trapframe *tf)
{
    unsigned ec  = (unsigned)ESR_EC(tf->esr);
    unsigned iss = (unsigned)ESR_ISS(tf->esr);

    kputs("\nPANIC: ");
    kputs(ec_name(ec));
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

    if (expected.armed && !expected.fired && synchronous) {
        expected.fired     = true;
        expected.info.esr  = tf->esr;
        expected.info.far  = tf->far;
        expected.info.elr  = tf->elr;
        tf->elr += 4;
        return;
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
