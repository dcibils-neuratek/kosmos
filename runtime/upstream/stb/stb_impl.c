/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * stb_truetype, instantiated once.
 *
 * The header is the library: including it with STB_TRUETYPE_IMPLEMENTATION
 * defined emits the code, and that must happen in exactly one translation
 * unit. Doing it here rather than inside gfx.c keeps five thousand lines of
 * somebody else's code out of a file we edit, and keeps its compile time
 * off every edit of ours.
 *
 * Vendored under `runtime/upstream/` beside `puff`, unmodified, with its
 * licence in the file - which is `CLAUDE.md`'s rule for anything vendored.
 * It is dual-licensed public domain / MIT; the notice is at the bottom of
 * the header.
 *
 * **Its own warning, which is worth repeating where somebody will read it:**
 * "NO SECURITY GUARANTEE -- DO NOT USE THIS ON UNTRUSTED FONT FILES ... an
 * attacker can use it to read arbitrary memory." The fonts this system
 * loads are vendored in its own image, so that is not a live risk today.
 * The mitigation for the day it is: this runs at EL0 in whichever process
 * asked to draw text, so a malicious font kills that process and nothing
 * else. That is the microkernel earning its keep rather than an excuse to
 * be careless - a font from a disk should still be treated as hostile.
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
