/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Synth Kit's engine on this machine (`roadmap.md` 6zh, Groove).
 *
 *   build/host/test_synth tools/test_synth.lua
 *
 * A Lua interpreter with one table more, `synth`, over the kit's pure half -
 * the sound, the engine and the song read from Lua tables - and no thread:
 * the script renders when it asks to, and asks what came out. What a song
 * should sound like is the script's to say (`tools/test_synth.lua`); this is
 * only the hands.
 *
 *   synth.reset()                  a fresh engine: its noise from the start,
 *                                  its delay and reverb empty
 *   synth.song(t)                  the song, from Groove's tables
 *   synth.play() synth.stop()      and the rest of the transport:
 *   synth.mode(song) synth.launch_clip(t, s) synth.launch_scene(s)
 *   synth.note_on(t, p, v) synth.note_off(t, p)
 *   synth.render(frames)           into the capture, from its start
 *   synth.onset(from, level)       the first frame at or past `from` louder
 *                                  than `level`, or nil
 *   synth.peak(from, to)           the loudest frame between
 *   synth.crossings(from, to)      rising zero crossings of the left side
 *   synth.sum()                    every sample added, for sameness
 *   synth.state()                  playing, step, chain position, finished
 *   synth.dump(path)               the capture as doubles, left and right
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lualib.h"
#include "synth_lua.h"

#define CAPTURE (SYNTH_RATE * 60)       /* a minute is plenty for a check */

static struct synth_engine *engine;
static double *cap_l, *cap_r;
static long captured;

static int l_song(lua_State *L)
{
    struct synth_song *song = synth_song_from_lua(L, 1);

    if (song == NULL) return lua_error(L);

    synth_song_free(synth_engine_set_song(engine, song, false));
    captured = 0;
    return 0;
}

static int l_reset(lua_State *L)
{
    (void)L;
    synth_song_free(synth_engine_set_song(engine, NULL, false));
    synth_engine_init(engine, 1);
    captured = 0;
    return 0;
}

static int l_play(lua_State *L) { (void)L; synth_engine_play(engine); return 0; }
static int l_stop(lua_State *L) { (void)L; synth_engine_stop(engine); return 0; }

static int l_mode(lua_State *L)
{
    synth_engine_mode(engine, lua_toboolean(L, 1));
    return 0;
}

static int l_launch_clip(lua_State *L)
{
    synth_engine_launch_clip(engine, (int)luaL_checkinteger(L, 1) - 1,
                             (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_launch_scene(lua_State *L)
{
    synth_engine_launch_scene(engine, (int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_note_on(lua_State *L)
{
    synth_engine_note_on(engine, (int)luaL_checkinteger(L, 1) - 1,
                         (int)luaL_checkinteger(L, 2), luaL_optnumber(L, 3, 0.8));
    return 0;
}

static int l_note_off(lua_State *L)
{
    synth_engine_note_off(engine, (int)luaL_checkinteger(L, 1) - 1,
                          (int)luaL_checkinteger(L, 2));
    return 0;
}

static int l_render(lua_State *L)
{
    long frames = (long)luaL_checkinteger(L, 1);

    while (frames > 0 && captured < CAPTURE) {
        int n = frames > SYNTH_BLOCK ? SYNTH_BLOCK : (int)frames;

        if (captured + n > CAPTURE) n = (int)(CAPTURE - captured);

        synth_engine_render(engine, cap_l + captured, cap_r + captured, n);
        captured += n;
        frames -= n;
    }

    lua_pushinteger(L, captured);
    return 1;
}

static long clamp_frame(lua_Integer i)
{
    if (i < 0) return 0;
    if (i > captured) return captured;
    return (long)i;
}

static int l_onset(lua_State *L)
{
    long from = clamp_frame(luaL_checkinteger(L, 1));
    double level = luaL_checknumber(L, 2);

    for (long i = from; i < captured; i++) {
        if (fabs(cap_l[i]) > level || fabs(cap_r[i]) > level) {
            lua_pushinteger(L, i);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

static int l_peak(lua_State *L)
{
    long from = clamp_frame(luaL_checkinteger(L, 1));
    long to = clamp_frame(luaL_checkinteger(L, 2));
    double peak = 0;

    for (long i = from; i < to; i++) {
        if (fabs(cap_l[i]) > peak) peak = fabs(cap_l[i]);
        if (fabs(cap_r[i]) > peak) peak = fabs(cap_r[i]);
    }

    lua_pushnumber(L, peak);
    return 1;
}

static int l_crossings(lua_State *L)
{
    long from = clamp_frame(luaL_checkinteger(L, 1));
    long to = clamp_frame(luaL_checkinteger(L, 2));
    int n = 0;

    for (long i = from + 1; i < to; i++) {
        if (cap_l[i - 1] < 0 && cap_l[i] >= 0) n++;
    }

    lua_pushinteger(L, n);
    return 1;
}

static int l_sum(lua_State *L)
{
    double sum = 0;

    for (long i = 0; i < captured; i++) sum += cap_l[i] * 3 + cap_r[i];

    lua_pushnumber(L, sum);
    return 1;
}

static int l_state(lua_State *L)
{
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, engine->playing);
    lua_setfield(L, -2, "playing");
    lua_pushinteger(L, engine->step);
    lua_setfield(L, -2, "step");
    lua_pushinteger(L, engine->chain_pos);
    lua_setfield(L, -2, "chain_pos");
    lua_pushboolean(L, engine->finished);
    lua_setfield(L, -2, "finished");
    return 1;
}

static int l_dump(lua_State *L)
{
    FILE *f = fopen(luaL_checkstring(L, 1), "wb");

    if (f == NULL) return luaL_error(L, "cannot write %s", lua_tostring(L, 1));

    for (long i = 0; i < captured; i++) {
        fwrite(&cap_l[i], sizeof(double), 1, f);
        fwrite(&cap_r[i], sizeof(double), 1, f);
    }

    fclose(f);
    return 0;
}

static const luaL_Reg FUNCS[] = {
    { "reset", l_reset }, { "song", l_song }, { "play", l_play }, { "stop", l_stop }, { "mode", l_mode },
    { "launch_clip", l_launch_clip }, { "launch_scene", l_launch_scene },
    { "note_on", l_note_on }, { "note_off", l_note_off }, { "render", l_render },
    { "onset", l_onset }, { "peak", l_peak }, { "crossings", l_crossings },
    { "sum", l_sum }, { "state", l_state }, { "dump", l_dump }, { NULL, NULL },
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_synth script.lua [args]\n");
        return 2;
    }

    engine = malloc(sizeof *engine);
    cap_l = malloc(CAPTURE * sizeof *cap_l);
    cap_r = malloc(CAPTURE * sizeof *cap_r);

    if (!engine || !cap_l || !cap_r) return 1;

    synth_engine_init(engine, 1);

    lua_State *L = luaL_newstate();

    luaL_openlibs(L);
    luaL_newlib(L, FUNCS);
    lua_setglobal(L, "synth");

    lua_createtable(L, argc, 0);

    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }

    lua_setglobal(L, "arg");

    if (luaL_dofile(L, argv[1]) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        return 1;
    }

    return 0;
}
