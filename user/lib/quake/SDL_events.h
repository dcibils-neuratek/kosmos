/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * `SDL_Event`, named and never defined.
 *
 * `input.h` declares the SDL input handlers with a pointer to one, and those
 * handlers are exactly the files the Kosmos build replaces. An incomplete
 * type is enough for a prototype and turns any attempt to use one into a
 * compile error, which is the honest answer to "is there an SDL event here".
 */
#ifndef KOSMOS_QUAKE_SDL_EVENTS_H
#define KOSMOS_QUAKE_SDL_EVENTS_H

#include "SDL_stdinc.h"

typedef union SDL_Event SDL_Event;

#endif
