/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A program's kernel, checked before it runs (`intelgpu_kernel.h` has what
 * and why; `docs/compute.md` C2).
 */

#include <stdbool.h>
#include <stddef.h>

#include "gpuproto.h"
#include "intelgpu_kernel.h"

/* A run of an instruction's 128 bits, `lo` to `hi`, inclusive. */
static uint32_t bits(const uint32_t *w, unsigned lo, unsigned hi)
{
    uint32_t v = 0;
    unsigned b;

    for (b = hi + 1u; b-- > lo;) {
        v = (v << 1) | ((w[b / 32u] >> (b % 32u)) & 1u);
    }

    return v;
}

#define FILE_ARF        0u
#define FILE_GRF        1u
#define FILE_IMM        3u

#define OP_NOP          126u
#define OP_JMPI         32u
#define OP_SEND         49u
#define OP_SENDC        50u

#define SFID_TS         7u      /* the thread spawner: a thread's end */
#define SFID_CONST      9u      /* the data port's constant cache, read only */
#define SFID_DC0        10u     /* the data cache */
#define SFID_DC1        12u     /* the data cache's second port */

#define ARF_IP          0xa0u

/* The instructions a kernel may use, by their operands' layout. */
enum shape { NONE, TWO, THREE, ALONE, BRANCH, SEND };

static enum shape shape_of(uint32_t op)
{
    switch (op) {
    case 1: case 2: case 4: case 5: case 6: case 7:   /* mov sel not and or xor */
    case 8: case 9: case 12: case 16: case 17:        /* shr shl asr cmp cmpn */
    case 23: case 25: case 56:                        /* bfrev bfi1 math */
    case 64: case 65: case 66: case 67: case 68:      /* add mul avg frc rndu */
    case 69: case 70: case 71: case 72: case 73:      /* rndd rnde rndz mac mach */
    case 74: case 75: case 76: case 77: case 78:      /* lzd fbh fbl cbit addc */
    case 79: case 84: case 85: case 86: case 87:      /* subb dp4 dph dp3 dp2 */
    case 89: case 90:                                 /* line pln */
        return TWO;
    case 18: case 24: case 26: case 91: case 92: case 93:  /* csel bfe bfi2 mad lrp madm */
        return THREE;
    case OP_NOP:
        return ALONE;
    case OP_JMPI:
        return BRANCH;
    case OP_SEND:
    case OP_SENDC:
        return SEND;
    default:
        return NONE;    /* movi, smov, wait, sends, and every other branch */
    }
}

/*
 * An architecture register a kernel may name, by its number's high
 * nibble: null, the accumulators and the flags; reading, the timestamp
 * too. Not a0 - nothing here addresses through it - nor the state, control
 * or instruction-pointer registers.
 */
static bool arf_allowed(uint32_t nr, bool writing)
{
    switch (nr >> 4) {
    case 0x0:           /* null */
    case 0x2:           /* acc0, acc1 */
    case 0x3:           /* f0, f1 */
        return true;
    case 0xc:           /* tm0 */
        return !writing;
    default:
        return false;
    }
}

/* A register operand: its file, then its number, at `nr_lo`. */
static uint32_t operand(const uint32_t *w, uint32_t file, unsigned nr_lo, bool writing)
{
    if (file == FILE_GRF) {
        return GPU_KERNEL_OK;
    }

    if (file == FILE_ARF && arf_allowed(bits(w, nr_lo, nr_lo + 7u), writing)) {
        return GPU_KERNEL_OK;
    }

    return GPU_KERNEL_REGISTER;     /* file 2 does not exist on generation 9 */
}

/* An align1 or align16 instruction's destination and up to two sources. */
static uint32_t two_source(const uint32_t *w)
{
    uint32_t dst_file = bits(w, 35, 36), s0 = bits(w, 41, 42), s1 = bits(w, 89, 90);
    uint32_t why;

    if (bits(w, 63, 63) != 0) {
        return GPU_KERNEL_INDIRECT;
    }

    if (dst_file == FILE_IMM) {
        return GPU_KERNEL_REGISTER;
    }

    if ((why = operand(w, dst_file, 53, true)) != GPU_KERNEL_OK) {
        return why;
    }

    if (s0 == FILE_IMM) {
        return GPU_KERNEL_OK;       /* the immediate fills the rest */
    }

    if (bits(w, 79, 79) != 0) {
        return GPU_KERNEL_INDIRECT;
    }

    if ((why = operand(w, s0, 69, false)) != GPU_KERNEL_OK) {
        return why;
    }

    if (s1 == FILE_IMM) {
        return GPU_KERNEL_OK;
    }

    if (bits(w, 111, 111) != 0) {
        return GPU_KERNEL_INDIRECT;
    }

    return operand(w, s1, 101, false);
}

/*
 * A message to a unit. Only the thread's end to the spawner, and the data
 * port's messages through a binding-table entry - each type listed, so a
 * scratch (bit 18 of the data cache's), 64-bit or stateless message is
 * not among them - and a table entry below the stateless and shared-local
 * ones. The descriptor must be the instruction's own number.
 */
static uint32_t send(const uint32_t *w, struct igpu_kernel_verdict *v)
{
    uint32_t sfid = bits(w, 24, 27), desc = bits(w, 96, 126), eot = bits(w, 127, 127);
    uint32_t type = (desc >> 14) & 0x1fu, bti = desc & 0xffu;
    uint32_t dst_file = bits(w, 35, 36);
    bool ok;

    if (bits(w, 63, 63) != 0 || bits(w, 79, 79) != 0) {
        return GPU_KERNEL_INDIRECT;
    }

    if (bits(w, 41, 42) != FILE_GRF || dst_file == FILE_IMM
        || operand(w, dst_file, 53, true) != GPU_KERNEL_OK) {
        return GPU_KERNEL_REGISTER;
    }

    if (bits(w, 89, 90) != FILE_IMM) {
        return GPU_KERNEL_DESC;
    }

    if (sfid == SFID_TS) {
        if (!eot) {
            return GPU_KERNEL_MESSAGE;  /* the spawner is only told a thread has ended */
        }

        if (bits(w, 16, 19) != 0 || bits(w, 69, 76) < 112u) {
            return GPU_KERNEL_EOT;      /* unpredicated, and from r112-r127 as the PRM asks */
        }

        return GPU_KERNEL_OK;
    }

    if (eot) {
        return GPU_KERNEL_EOT;
    }

    switch (sfid) {
    case SFID_DC0:      /* elk_eu_defines.h, GFX7_DATAPORT_DC_*: all but 9, none scratch */
        ok = type <= 13u && type != 9u;
        break;
    case SFID_DC1:      /* HSW_DATAPORT_DC_PORT1_*: untyped, media block, typed; no A64 */
        ok = type == 1u || type == 2u || type == 4u || type == 5u
          || type == 6u || type == 9u || type == 10u || type == 13u;
        break;
    case SFID_CONST:    /* block and scattered reads */
        ok = type <= 3u;
        break;
    default:
        return GPU_KERNEL_UNIT;
    }

    if (!ok || bti >= GPU_RUN_BUFFERS) {
        return GPU_KERNEL_MESSAGE;
    }

    if ((int32_t)bti > v->bti_max) {
        v->bti_max = (int32_t)bti;
    }

    return GPU_KERNEL_OK;
}

/*
 * `jmpi`: by an immediate, to an instruction of the kernel. The bspec's
 * rule - "unlike other flow control instructions, the offset used by JMPI
 * is relative to the incremented instruction pointer" - so from the
 * instruction after it.
 */
static uint32_t branch(const uint32_t *w, uint32_t at, uint32_t bytes)
{
    int64_t target;

    if (bits(w, 21, 23) != 0 || bits(w, 89, 90) != FILE_IMM
        || bits(w, 35, 36) != FILE_ARF || bits(w, 53, 60) != ARF_IP) {
        return GPU_KERNEL_BRANCH;
    }

    target = (int64_t)at * 16 + 16 + (int64_t)(int32_t)bits(w, 96, 127);

    if (target < 0 || target >= (int64_t)bytes || (target & 15) != 0) {
        return GPU_KERNEL_BRANCH;
    }

    return GPU_KERNEL_OK;
}

uint32_t igpu_kernel_check(const uint32_t *words, uint32_t bytes,
                           struct igpu_kernel_verdict *out)
{
    uint32_t n = bytes / 16u, i, why = GPU_KERNEL_OK;

    out->at = 0;
    out->bti_max = -1;

    if (bytes == 0 || bytes % 16u != 0 || bytes > IGPU_KERNEL_MAX_BYTES) {
        return out->why = GPU_KERNEL_SIZE;
    }

    for (i = 0; i < n && why == GPU_KERNEL_OK; i++) {
        const uint32_t *w = words + i * 4u;
        uint32_t op = bits(w, 0, 6);

        out->at = i;

        if (bits(w, 29, 29) != 0) {
            why = GPU_KERNEL_COMPACT;
        } else if (bits(w, 30, 30) != 0) {
            why = GPU_KERNEL_DEBUG;
        } else {
            switch (shape_of(op)) {
            case TWO:    why = two_source(w); break;
            case THREE:  why = GPU_KERNEL_OK; break;    /* every operand a GRF, directly */
            case ALONE:  why = GPU_KERNEL_OK; break;
            case BRANCH: why = branch(w, i, bytes); break;
            case SEND:   why = send(w, out); break;
            default:     why = GPU_KERNEL_OPCODE; break;
            }
        }
    }

    if (why == GPU_KERNEL_OK) {
        const uint32_t *last = words + (n - 1u) * 4u;
        uint32_t op = bits(last, 0, 6);

        out->at = n - 1u;

        if ((op != OP_SEND && op != OP_SENDC) || bits(last, 24, 27) != SFID_TS
            || bits(last, 127, 127) == 0) {
            why = GPU_KERNEL_END;   /* or the threads would run on past the kernel */
        }
    }

    if (why == GPU_KERNEL_OK) {
        out->at = 0;
    }

    return out->why = why;
}
