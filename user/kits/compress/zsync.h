/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A piece of a zlib stream, deflated and flushed whole** - what
 * `compress.zstream`'s `deflate` is (`deflate.c`), and VNC's ZRLE stands on.
 *
 * miniz's `tdefl_compress` takes the input, gives what it can into the
 * buffer it is handed, and keeps the rest - output not yet given, and input
 * not yet compressed in its lookahead - for the next call. **A piece is not
 * flushed until both are empty.** The first version stopped at the first
 * call that gave less than a buffer, which is when the output already made
 * is out but not the lookahead behind it: a band of the M700's screen that
 * deflated to more than 16 KB lost its last 207 bytes, and TigerVNC hung up
 * on it (14 October 2026, `testing.md` 18.635).
 *
 * Pure C over miniz, so `tools/test_zsync.c` holds it on the Mac.
 */

#ifndef KOSMOS_ZSYNC_H
#define KOSMOS_ZSYNC_H

#include <stddef.h>
#include <stdint.h>

#include "miniz.h"

/* `bytes` from `in` deflated onto the stream `d` and sync-flushed, every
 * byte made handed to `emit` in order. TDEFL_STATUS_OKAY, or miniz's
 * refusal. */
tdefl_status zsync_deflate(tdefl_compressor *d, const uint8_t *in, size_t bytes,
                           void (*emit)(void *ctx, const uint8_t *out, size_t n),
                           void *ctx);

#endif
