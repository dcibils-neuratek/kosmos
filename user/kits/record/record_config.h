/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_RECORD_CONFIG_H
#define KOSMOS_RECORD_CONFIG_H

/*
 * **How the two vendored headers are built**, included first by every file
 * that includes either - because these change the encoder's structures, and
 * two files that disagreed about them would disagree about memory.
 *
 * Every line is a build step rather than an edit: `runtime/upstream` holds
 * what lieff released, byte for byte.
 */

/* One thread - a process records one camera - and plain H.264, no SVC. */
#define H264E_MAX_THREADS 0
#define H264E_SVC_API     0

/*
 * **Little-endian, said outright.** Its endianness test knows Linux and
 * Apple and nothing else, and stops with "platform not supported" anywhere
 * else - and its Apple branch reads `BYTE_ORDER`, which strict C11 does not
 * define, so both sides of `__BYTE_ORDER == __BIG_ENDIAN` were undefined,
 * equal, and the Mac took itself for big-endian: the first run of
 * `tools/test_record.c` wrote its parameter sets a word backwards, `16 00
 * 42 67` for `67 42 00 16`. Every machine this is built for is
 * little-endian, which is what the ARMCC branch says, and the Mac's test
 * builds what the guests build. Nothing else in either header reads the
 * name, and neither does anything of Kosmos's.
 */
#if !defined(__ARMCC_VERSION)
#define __ARMCC_VERSION 1
#endif

/*
 * **And on Apple's own compiler for arm64** it takes a branch that calls
 * `vtbl2q_u8`, which is not an intrinsic: the lookup it means - eight bytes
 * of index into a 32-byte table - is `vqtbl2_u8`. Only the Mac's copy of
 * `tools/test_record.c` reaches it; the guests' GCC takes the `vtbl4_u8`
 * branch, the same lookup over the same 32 bytes.
 */
#if defined(__APPLE__) && defined(__aarch64__)
#define vtbl2q_u8 vqtbl2_u8
#endif

#endif /* KOSMOS_RECORD_CONFIG_H */
