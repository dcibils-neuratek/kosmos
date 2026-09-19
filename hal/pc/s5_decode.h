/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The one number turning a PC off needs from its AML, found without
 * running any.
 *
 * Entering S5 is two writes to the FADT's PM1a control block: the sleep
 * type, then the same with SLP_EN. The type is the machine's, and it is
 * written in the DSDT as `\_S5`, a package whose first element is what PM1a
 * takes. q35's is 0 and the ThinkPad T14's is 7 - so the constant this
 * board used to write, QEMU's 0, put the T14 into S0, where it already was,
 * and then halted with the last frame on the screen.
 *
 * **No AML interpreter, and this is not one.** `\_S5` is a `Name`, not a
 * method: its bytes are a fixed shape the specification's grammar spells out
 * (ACPI 6.5, 20.2.5.1 and 20.2.5.4) - NameOp 08h, the name, PackageOp 12h,
 * a PkgLength, a count, then each element as ZeroOp 00h, OneOp 01h,
 * BytePrefix 0Ah and a byte, or WordPrefix 0Bh and two. A firmware that
 * computed its sleep type in a method would not be found, and would be
 * refused rather than guessed at. Split out so `tools/test_s5decode.c` can
 * ask it the awkward questions on the host.
 */
#ifndef KOSMOS_HAL_PC_S5_DECODE_H
#define KOSMOS_HAL_PC_S5_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * PM1a's S5 sleep type from a DSDT's bytes. False when there is no `_S5_`
 * of that shape, or its first element is not a number SLP_TYP's three bits
 * can hold.
 */
bool s5_decode(const uint8_t *aml, size_t length, unsigned *slp_typ);

#endif
