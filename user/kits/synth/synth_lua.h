/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A song as the Synth Kit plays it, made from the song as Groove keeps it:
 * Lua tables in PulseMusic's shape (`roadmap.md` 6zh).
 *
 *   song.bpm, song.swing, song.master, song.fx.dTime ...
 *   song.tracks[1..8] = { type = "drum" | "synth", vol, pan, sendA, sendB,
 *                         duck, mute, solo, kit, params = { w1, ... },
 *                         rows[1..8] = { tune, decay, tone, level },
 *                         clips[1..8] = { len, steps[row][step] = vel }
 *                                     | { len, notes = { {step, pitch,
 *                                                         len, vel}, ... } } }
 *   song.chain[i] = { scene, bars, auto = { ["t3.p.cut"] = { [step] = n } } }
 *   song.autoBase = { ["t3.p.cut"] = n }
 *
 * A field that is missing is PulseMusic's default for it, as `E.setSong`
 * fills them; a value out of its range is brought into it, since the audio
 * thread must not be handed a bpm of zero. Pure Lua C API - no `sys` - so
 * `tools/test_synth.c` builds songs this way on the Mac.
 */
#ifndef KOSMOS_SYNTH_LUA_H
#define KOSMOS_SYNTH_LUA_H

#include "lua.h"
#include "synth_engine.h"

/* The song at `index`, allocated, or NULL with the reason on the stack. */
struct synth_song *synth_song_from_lua(lua_State *L, int index);

/* The number of the target named `name` in `song`, or -1. */
int synth_song_target(const struct synth_song *song, const char *name);

#endif
