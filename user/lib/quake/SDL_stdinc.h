/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The part of SDL's `SDL_stdinc.h` that Chocolate Quake's engine uses, and
 * nothing more.
 *
 * Chocolate Quake is written against SDL2, and its engine - not only the
 * platform layer Kosmos replaces - reaches SDL for two things: the fixed
 * width integer names `common.h` builds `u8` to `i64` on, and libc with
 * `SDL_` in front of it. Kosmos has no SDL. This is that surface, measured
 * across the engine files rather than guessed: eight types, two macros and
 * fourteen functions that are libc under another name.
 *
 * The functions are macros for the libc name, not wrappers, so a function
 * this libc lacks fails the link under its own name. The same reason
 * `user/lib/litexl/SDL.h` exists: the vendored tree is not edited.
 */
#ifndef KOSMOS_QUAKE_SDL_STDINC_H
#define KOSMOS_QUAKE_SDL_STDINC_H

/*
 * And the standard headers the real one includes, because the engine leans
 * on them arriving this way: `common.h` names `FILE` and includes nothing
 * that declares it, which failed all 77 engine files before these were here.
 * This is SDL 2.26's list as far as this libc has it; `wchar.h` is the one it does not have, and nothing in the engine is wide.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <float.h>

typedef uint8_t  Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef uint64_t Uint64;
typedef int8_t   Sint8;
typedef int16_t  Sint16;
typedef int32_t  Sint32;
typedef int64_t  Sint64;

#define SDL_min(x, y)      (((x) < (y)) ? (x) : (y))
#define SDL_clamp(x, a, b) (((x) < (a)) ? (a) : (((x) > (b)) ? (b) : (x)))

#define SDL_malloc      malloc
#define SDL_calloc      calloc
#define SDL_free        free
#define SDL_strtol      strtol

#define SDL_memset      memset
#define SDL_memmove     memmove
#define SDL_memcpy      memcpy
#define SDL_memcmp      memcmp

#define SDL_strlen      strlen
#define SDL_strchr      strchr
#define SDL_strrchr     strrchr
#define SDL_strstr      strstr
#define SDL_strcmp      strcmp
#define SDL_strncmp     strncmp
#define SDL_strncasecmp strncasecmp

#endif
