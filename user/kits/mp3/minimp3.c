/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **minimp3's implementation, once** - for the MP3 Kit (`mp3_kosmos.c`) and
 * the Playback Kit's thread (`docs/playback.md`), which both decode with it.
 * It was compiled inside `mp3_kosmos.c`; a second kit including it there too
 * would have been two copies of a decoder in one image. The header, vendored
 * as shipped, is in `runtime/upstream/minimp3/`.
 */
#undef MINIMP3_FLOAT_OUTPUT             /* sixteen-bit, what the device takes */
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
