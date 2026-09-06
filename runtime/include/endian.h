/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef ENDIAN_H
#define ENDIAN_H

/*
 * Which end the bytes start at, which for this system is a settled
 * question and not a portable one.
 *
 * **Kosmos is little-endian on both its targets and has decided to stay
 * that way.** AArch64 *can* be big-endian through `SCTLR_EL1.EE` and this
 * kernel never sets it; x86-64 has no such switch. `docs/hal.md` records
 * the audit: every `string.pack` in the on-disk format carries an explicit
 * `<`, and the network stack builds big-endian by construction with byte
 * shifts rather than by relying on the host - so nothing above this line
 * depends on the answer.
 *
 * The file exists because a vendored library asks. musl's `libm.h` includes
 * it to decide the shape of a `long double`, which is genuinely
 * byte-order-dependent on the 80-bit format. Written out rather than taken
 * from a toolchain for the reason this directory exists at all: the third
 * time a build broke because one compiler's vendor bundled a header and the
 * other's did not was the time to stop.
 *
 * `__BYTE_ORDER` and friends are the glibc spellings, which is what
 * portable code tests, so they are what a vendored library finds.
 */

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#define __BYTE_ORDER    __LITTLE_ENDIAN

#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define PDP_ENDIAN      __PDP_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER

#endif /* ENDIAN_H */
