/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A piece of a zlib stream, deflated and flushed whole (`zsync.h`).
 */

#include "zsync.h"

tdefl_status zsync_deflate(tdefl_compressor *d, const uint8_t *in, size_t bytes,
                           void (*emit)(void *ctx, const uint8_t *out, size_t n),
                           void *ctx)
{
    static uint8_t chunk[16384];
    size_t left = bytes;

    /*
     * Until the input is all taken, the output made is all given, and the
     * lookahead - input taken and not yet compressed - is empty too: a
     * call that gives less than a buffer has drained what was made, not
     * necessarily what is still behind it.
     */
    for (;;) {
        size_t in_size = left, out_size = sizeof chunk;
        tdefl_status st = tdefl_compress(d, in, &in_size, chunk, &out_size, TDEFL_SYNC_FLUSH);

        if (st != TDEFL_STATUS_OKAY) {
            return st;
        }

        in += in_size;
        left -= in_size;

        if (out_size > 0) {
            emit(ctx, chunk, out_size);
        }

        if (left == 0 && out_size < sizeof chunk && d->m_lookahead_size == 0
            && d->m_output_flush_remaining == 0) {
            return TDEFL_STATUS_OKAY;
        }
    }
}
