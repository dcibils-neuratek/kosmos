/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Base64 for C (`base64_core.c`): the Compression Kit's, for the Lua door in
 * `base64.c` and for any C that wants it - the Mail Kit first, whose parts
 * are base64 broken into lines of 76 (`docs/mail.md` M1).
 */

#ifndef KOSMOS_BASE64_CORE_H
#define KOSMOS_BASE64_CORE_H

#include <stddef.h>
#include <stdint.h>

/* `n` bytes as base64 into `out`, which has room for `(n + 2) / 3 * 4`;
 * how many characters were written. */
size_t base64_encode(const uint8_t *in, size_t n, char *out);

/*
 * Base64 text back to bytes, into `out` of `room` (`n / 4 * 3 + 3` always
 * suffices). Stops at the first `=`. With `lines`, spaces, tabs and line
 * ends are skipped, as a message's base64 has them; without, anything not
 * base64 stops it. How many bytes came out; `*bad` is the offset of a
 * character refused, or `n` when there was none.
 */
size_t base64_decode(const char *in, size_t n, uint8_t *out, size_t room, int lines,
                     size_t *bad);

#endif
