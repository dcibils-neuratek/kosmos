/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The song, read out of Groove's tables (`synth_lua.h`).
 *
 * Everything the engine will touch is made here, on the caller's thread:
 * the clips, the notes, the sections, their lanes, the targets and a ramp
 * and a hold for each - so handing the result to the audio thread is a
 * pointer, and the thread never allocates.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "synth_lua.h"

/* PulseMusic's defaults, `presets.lua`, in `synth_param_names`' order. */
static const double PARAM_DEFAULTS[SYNTH_PARAMS] = {
    1, 1, 0, 8, 0.5, 1, 0, 0, 1,
    1, 2000, 0.2, 0.3, 0.001, 0.2, 0.2, 0, 0,
    0.002, 0.3, 0.7, 0.15, 2, 0, 0, 0.35,
};

static const double FX_DEFAULTS[SYNTH_FX] = { 2, 0.45, 0.4, 0.75, 0.4, 0.16, 0.8, 0.8 };

/* A number field of the table on top, or `def` - kept inside `lo` to `hi`. */
static double field(lua_State *L, const char *key, double def, double lo, double hi)
{
    double v = def;

    lua_getfield(L, -1, key);

    if (lua_type(L, -1) == LUA_TNUMBER) v = lua_tonumber(L, -1);

    lua_pop(L, 1);

    if (!(v >= lo)) v = lo;            /* NaN too */
    if (v > hi) v = hi;

    return v;
}

static bool flag(lua_State *L, const char *key)
{
    lua_getfield(L, -1, key);

    bool on = lua_toboolean(L, -1);

    lua_pop(L, 1);
    return on;
}

/* The table at `key` of the table on top, pushed; false (and nothing
 * pushed) when it is not a table. */
static bool enter(lua_State *L, const char *key)
{
    lua_getfield(L, -1, key);

    if (lua_type(L, -1) == LUA_TTABLE) return true;

    lua_pop(L, 1);
    return false;
}

static bool enter_index(lua_State *L, lua_Integer i)
{
    lua_geti(L, -1, i);

    if (lua_type(L, -1) == LUA_TTABLE) return true;

    lua_pop(L, 1);
    return false;
}

static bool read_clip(lua_State *L, struct synth_clip *clip, bool drum)
{
    int len = (int)field(L, "len", 16, 1, SYNTH_STEPS);

    clip->present = true;
    clip->len = len;

    if (drum) {
        if (!enter(L, "steps")) return true;

        for (int r = 0; r < SYNTH_ROWS; r++) {
            if (!enter_index(L, r + 1)) continue;

            for (int s = 0; s < SYNTH_STEPS; s++) {
                lua_geti(L, -1, s + 1);

                double vel = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0;

                lua_pop(L, 1);
                clip->steps[r][s] = (float)((vel > 0 && vel <= 2) ? vel : 0);
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
        return true;
    }

    if (!enter(L, "notes")) return true;

    lua_Integer count = (lua_Integer)lua_rawlen(L, -1);

    if (count > 0) {
        clip->notes = calloc((size_t)count, sizeof *clip->notes);

        if (clip->notes == NULL) {
            lua_pop(L, 1);
            return false;
        }

        for (lua_Integer i = 1; i <= count; i++) {
            if (!enter_index(L, i)) continue;

            struct synth_note *n = &clip->notes[clip->notes_count++];

            n->step = (int)field(L, "step", 0, 0, SYNTH_STEPS - 1);
            n->pitch = (int)field(L, "pitch", 60, 0, 127);
            n->len = (int)field(L, "len", 1, 1, 4096);
            n->vel = field(L, "vel", 0.8, 0, 2);
            lua_pop(L, 1);
        }
    }

    lua_pop(L, 1);
    return true;
}

static bool read_track(lua_State *L, struct synth_track *tr)
{
    lua_getfield(L, -1, "type");
    tr->drum = lua_type(L, -1) == LUA_TSTRING && strcmp(lua_tostring(L, -1), "drum") == 0;
    lua_pop(L, 1);

    tr->vol = field(L, "vol", 0.7, 0, 1);
    tr->pan = field(L, "pan", 0, -1, 1);
    tr->send_a = field(L, "sendA", 0, 0, 1);
    tr->send_b = field(L, "sendB", 0, 0, 1);
    tr->duck = field(L, "duck", 0, 0, 1);
    tr->mute = flag(L, "mute");
    tr->solo = flag(L, "solo");
    tr->kit = (int)field(L, "kit", 1, 1, SYNTH_KITS) - 1;

    bool params = enter(L, "params");

    for (int i = 0; i < SYNTH_PARAMS; i++) {
        double *at = synth_param_at(&tr->params, i);

        *at = PARAM_DEFAULTS[i];

        if (params) *at = field(L, synth_param_names[i], PARAM_DEFAULTS[i], -1e6, 1e6);
    }

    if (params) lua_pop(L, 1);

    /* Brought into the ranges that keep the sound finite. */
    if (tr->params.cut < 20) tr->params.cut = 20;
    if (tr->params.aD < 0.001) tr->params.aD = 0.001;
    if (tr->params.aR < 0.001) tr->params.aR = 0.001;
    if (tr->params.fD < 0.001) tr->params.fD = 0.001;

    bool rows = enter(L, "rows");

    for (int r = 0; r < SYNTH_ROWS; r++) {
        struct synth_row *row = &tr->rows[r];

        *row = (struct synth_row){ 0, 1, 0.5, 1 };

        if (rows && enter_index(L, r + 1)) {
            row->tune = field(L, "tune", 0, -24, 24);
            row->decay = field(L, "decay", 1, 0.05, 10);
            row->tone = field(L, "tone", 0.5, 0, 1);
            row->level = field(L, "level", 1, 0, 4);
            lua_pop(L, 1);
        }
    }

    if (rows) lua_pop(L, 1);

    if (enter(L, "clips")) {
        for (int c = 0; c < SYNTH_SCENES; c++) {
            if (!enter_index(L, c + 1)) continue;

            bool ok = read_clip(L, &tr->clips[c], tr->drum);

            lua_pop(L, 1);

            if (!ok) {
                lua_pop(L, 1);
                return false;
            }
        }

        lua_pop(L, 1);
    }

    return true;
}

int synth_song_target(const struct synth_song *song, const char *name)
{
    for (int i = 0; i < song->targets_count; i++) {
        if (strcmp(song->targets[i].name, name) == 0) return i;
    }

    return -1;
}

/*
 * A target by name, added if it is new and names something; its number, or
 * -1. `room` is how many the array was made for.
 */
static int target_of(struct synth_song *song, const char *name, int room)
{
    int at = synth_song_target(song, name);

    if (at >= 0) return at;

    if (song->targets_count >= room || strlen(name) >= sizeof song->targets[0].name) {
        return -1;
    }

    struct synth_target t;

    if (!synth_target_parse(name, &t)) return -1;

    memcpy(t.name, name, strlen(name) + 1);
    song->targets[song->targets_count] = t;
    return song->targets_count++;
}

/* How many targets the song could name: every key of every lane, and of
 * `autoBase` - an upper bound, counted before anything is allocated. */
static int count_targets(lua_State *L, int song_index)
{
    int n = 0;

    lua_getfield(L, song_index, "autoBase");

    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushnil(L);

        while (lua_next(L, -2)) {
            n++;
            lua_pop(L, 1);
        }
    }

    lua_pop(L, 1);

    lua_getfield(L, song_index, "chain");

    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_Integer count = (lua_Integer)lua_rawlen(L, -1);

        for (lua_Integer i = 1; i <= count; i++) {
            if (!enter_index(L, i)) continue;

            if (enter(L, "auto")) {
                lua_pushnil(L);

                while (lua_next(L, -2)) {
                    n++;
                    lua_pop(L, 1);
                }

                lua_pop(L, 1);
            }

            lua_pop(L, 1);
        }
    }

    lua_pop(L, 1);
    return n;
}

static bool read_chain(lua_State *L, struct synth_song *song, int room)
{
    if (!enter(L, "chain")) return true;

    lua_Integer count = (lua_Integer)lua_rawlen(L, -1);

    if (count > 0) {
        song->sections = calloc((size_t)count, sizeof *song->sections);

        if (song->sections == NULL) {
            lua_pop(L, 1);
            return false;
        }
    }

    for (lua_Integer i = 1; i <= count; i++) {
        if (!enter_index(L, i)) continue;

        struct synth_section *sec = &song->sections[song->sections_count++];

        sec->scene = (int)field(L, "scene", 1, 1, SYNTH_SCENES);
        sec->bars = (int)field(L, "bars", 1, 1, 64);

        if (enter(L, "auto")) {
            int lanes = 0;

            lua_pushnil(L);

            while (lua_next(L, -2)) {
                lanes++;
                lua_pop(L, 1);
            }

            sec->lanes = calloc((size_t)(lanes ? lanes : 1), sizeof *sec->lanes);

            if (sec->lanes == NULL) {
                lua_pop(L, 3);              /* auto, section, chain */
                return false;
            }

            lua_pushnil(L);

            while (lua_next(L, -2)) {
                int target = (lua_type(L, -2) == LUA_TSTRING)
                             ? target_of(song, lua_tostring(L, -2), room) : -1;

                if (target >= 0 && lua_type(L, -1) == LUA_TTABLE) {
                    struct synth_lane *lane = &sec->lanes[sec->lanes_count];

                    lane->target = target;
                    lane->count = sec->bars * 16;
                    lane->values = malloc((size_t)lane->count * sizeof *lane->values);

                    if (lane->values == NULL) {
                        lua_pop(L, 5);      /* value, key, auto, section, chain */
                        return false;
                    }

                    for (int s = 0; s < lane->count; s++) {
                        lua_geti(L, -1, s + 1);

                        lane->values[s] = lua_type(L, -1) == LUA_TNUMBER
                                          ? fmin(1, fmax(0, lua_tonumber(L, -1))) : -1;
                        lua_pop(L, 1);
                    }

                    sec->lanes_count++;
                }

                lua_pop(L, 1);
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    lua_pop(L, 1);
    return true;
}

struct synth_song *synth_song_from_lua(lua_State *L, int index)
{
    index = lua_absindex(L, index);

    if (lua_type(L, index) != LUA_TTABLE) {
        lua_pushliteral(L, "a song is a table");
        return NULL;
    }

    struct synth_song *song = calloc(1, sizeof *song);
    int room = count_targets(L, index);

    if (song == NULL) {
        lua_pushliteral(L, "no memory for a song");
        return NULL;
    }

    song->targets = calloc((size_t)(room ? room : 1), sizeof *song->targets);

    if (song->targets == NULL) {
        free(song);
        lua_pushliteral(L, "no memory for a song");
        return NULL;
    }

    lua_pushvalue(L, index);

    song->bpm = field(L, "bpm", 128, 20, 400);
    song->swing = field(L, "swing", 0, 0, 0.6);
    song->master = field(L, "master", 0.8, 0, 1.5);

    bool fx = enter(L, "fx");

    for (int i = 0; i < SYNTH_FX; i++) {
        song->fx[i] = fx ? field(L, synth_fx_names[i], FX_DEFAULTS[i], -1e6, 1e6)
                         : FX_DEFAULTS[i];
    }

    if (fx) lua_pop(L, 1);

    if (song->fx[FX_DUCKREL] < 0.001) song->fx[FX_DUCKREL] = 0.001;

    bool ok = true;

    if (enter(L, "tracks")) {
        for (int t = 0; t < SYNTH_TRACKS && ok; t++) {
            if (!enter_index(L, t + 1)) continue;

            ok = read_track(L, &song->tracks[t]);
            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    /* The resting values first, so a target named only there is known. */
    if (ok && enter(L, "autoBase")) {
        lua_pushnil(L);

        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TNUMBER) {
                int t = target_of(song, lua_tostring(L, -2), room);

                if (t >= 0) {
                    song->targets[t].has_base = true;
                    song->targets[t].base = fmin(1, fmax(0, lua_tonumber(L, -1)));
                }
            }

            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    if (ok) ok = read_chain(L, song, room);

    lua_pop(L, 1);

    if (ok) {
        int n = song->targets_count ? song->targets_count : 1;

        song->ramps = calloc((size_t)n, sizeof *song->ramps);
        song->held = calloc((size_t)n, sizeof *song->held);
        ok = song->ramps != NULL && song->held != NULL;
    }

    if (!ok) {
        synth_song_free(song);
        lua_pushliteral(L, "no memory for a song");
        return NULL;
    }

    return song;
}
