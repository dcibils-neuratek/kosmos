/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `SDL.h` as far as Chocolate Quake's `host.c` needs it: the timer subsystem
 * started and stopped, and SDL itself shut down on the way out.
 *
 * There is nothing to start. Time on Kosmos is the counter, read by
 * `Sys_FloatTime` in `user/lib/quake_kosmos.c`, so starting the timer
 * succeeds and stopping it does nothing.
 */
#ifndef KOSMOS_QUAKE_SDL_H
#define KOSMOS_QUAKE_SDL_H

#include "SDL_stdinc.h"

#define SDL_INIT_TIMER 0x00000001u

static inline int SDL_Init(Uint32 flags)
{
    (void)flags;
    return 0;
}

static inline void SDL_QuitSubSystem(Uint32 flags)
{
    (void)flags;
}

static inline void SDL_Quit(void)
{
}

static inline const char *SDL_GetError(void)
{
    return "there is no SDL on Kosmos";
}

#endif
