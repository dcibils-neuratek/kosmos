/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Super Nintendo, on this machine.
 *
 * The half of the port that is ours. `runtime/upstream/lakesnes/snes/` is
 * LakeSnes's core, byte for byte and MIT; this file stands where upstream's
 * SDL frontend was, and is the one place the two vocabularies meet.
 * `runtime/upstream/lakesnes/README.kosmos.md` is the account.
 *
 * **There was almost nothing to replace.** Doom needed six platform
 * functions and a WAD reader, Quake a platform layer of nine hundred lines
 * and a stack of its own. LakeSnes's core already *is* a library: it asks for
 * `malloc`, `memcpy`, `printf` and six functions from `math.h`, and offers
 * `snes_loadRom`, `snes_runFrame`, `snes_setPixels`, `snes_setSamples` and
 * `snes_setButtonState` - no files, no clock, no threads, and no `exit` to
 * land. Its largest stack frame is 544 bytes.
 *
 * So what is here is exactly what doom_kosmos.c's Lua half is, and nothing
 * else: a ROM handed in as an address, a frame into a surface and its sound
 * into a ring, a button, and the log. None of it blocks, and the Lua side
 * owns the loop - which window, which ROM, when a frame happens, and when to
 * stop.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "audioring.h"
#include "snes.h"
#include "snes_blit.h"

/* From `gfx.c`, the one file allowed to know how a surface is laid out. */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

/* The ring `printf` spills into for a process without the console. */
size_t kosmos_spill_drain(char *out, size_t max);

/*
 * What `snes_setPixels` writes: 512 by 480, four bytes a pixel.
 *
 * Twice the console's 256 by 240 both ways, because the core renders the
 * high-resolution modes and interlace at their real size and doubles the
 * rest. The layout is SDL's ARGB8888 - upstream's frontend hands its texture
 * straight in - which on a little-endian machine is a `uint32_t` of
 * 0xAARRGGBB, the same word a surface holds.
 */
#define PICTURE_W 512
#define PICTURE_H 480

/*
 * The biggest ROM worth trying: six megabytes is the largest cartridge made,
 * and a copier header is 512 bytes more. The limit is not the emulator's -
 * it is that `snes_loadRom` takes an `int` and copies the ROM twice, and a
 * process here may map 48 MB in all.
 */
#define ROM_MAX (8u * 1024u * 1024u)

static Snes    *machine;
static uint8_t *picture;
static bool     loaded;

/*--------------------------------------------------------------------------
 * Sound.
 *
 * The core hands back one console frame's worth of samples at whatever rate
 * it is asked for, and they go straight into the audio server's ring from
 * here - `CLAUDE.md`'s rule that a stream never travels as a message, and
 * `sys.pcm_into`'s way of keeping it. Frames are written into the slot at
 * `write`, which the server cannot be reading, because it never reads an
 * index this side has not published; and a slot is published only once it is
 * full. No Lua string exists at either end, so a minute of sound allocates
 * nothing and the collector has no reason to arrive inside a period.
 *------------------------------------------------------------------------*/

/*
 * The most device frames one console frame may ask for. 44100 at 50 Hz is
 * 882, so this is room for a device a little over twice that fast.
 */
#define SAMPLES_MAX 2048u

static struct audio_ring *ring;
static int16_t           *samples;
static double             per_frame;  /* device frames one console frame makes */
static double             owed;       /* the fraction carried to the next frame */
static uint32_t           partial;    /* frames already in the slot at `write` */

/*
 * The console's frame rate as this core keeps it: 60 or 50, exactly.
 *
 * A real NTSC console draws 60.0988 frames a second and a PAL one 50.007, and
 * those are not the numbers to use here. `apu.c` clocks the SPC700 at 32040 Hz
 * *per 60.0 Hz frame* - per 50.0 for PAL - so 60 and 50 are the rates at which
 * this core's sound comes out at its own pitch. Upstream's frontend asks for
 * 48000 / 60 samples a frame for the same reason.
 */
static double console_rate(void)
{
    return machine->palTiming ? 50.0 : 60.0;
}

/*
 * Frames into the ring, a slot at a time. Returns how many did not fit - none,
 * unless a frame was run that the ring had not asked for.
 *
 * `audio_ring_space` before every slot, and not only before the first: a full
 * ring's `write` slot *is* the slot at `read`, which the server may be mixing.
 */
static unsigned feed(const int16_t *in, unsigned frames)
{
    uint32_t period = ring->period_bytes / 4u;
    unsigned done = 0;

    while (done < frames && audio_ring_space(ring) > 0) {
        uint8_t *slot = audio_ring_slot(ring, ring->write);
        uint32_t take = period - partial;

        if (take > frames - done) {
            take = frames - done;
        }

        memcpy(slot + (size_t)partial * 4u, in + (size_t)done * 2u,
               (size_t)take * 4u);

        partial += take;
        done += take;

        if (partial == period) {
            audio_ring_publish(ring, ring->write + 1);
            partial = 0;
        }
    }

    return frames - done;
}

/*
 * snes.start(address, length) - where the ROM is, and how much of it.
 *
 * An address rather than a string, for the reason `doom.start` gives: a ROM
 * is megabytes and the Lua heap starts at two. The Lua side reads it into a
 * region and keeps the region; `snes_loadRom` copies it into the cartridge,
 * so nothing here holds on to the address afterwards.
 *
 * Returns `true, rate` - the console's frames a second, 60 or 50, which the
 * Lua side paces by when there is no sound to pace by - or `false, why`.
 */
static int l_start(lua_State *L)
{
    unsigned long at = (unsigned long)luaL_checkinteger(L, 1);
    size_t len = (size_t)luaL_checkinteger(L, 2);

    if (loaded) {
        return luaL_error(L, "a ROM is already running in this process");
    }

    /*
     * Upstream refuses anything under 32 KB itself. The upper bound is ours,
     * and it is checked here because the length becomes an `int` on the way
     * in, and a hostile or truncated size must not become a negative one.
     */
    if (at == 0 || len < 0x8000u || len > ROM_MAX) {
        return luaL_error(L, "that is not a ROM this can run: %d bytes",
                          (int)(len > ROM_MAX ? ROM_MAX + 1 : len));
    }

    /*
     * Allocated here rather than as statics, and that is about every other
     * process. The image is copied into all of them, `.bss` with it, so a
     * 900 KB picture declared at file scope would be 900 KB paid by the shell
     * and the Deskbar for a game neither of them runs.
     */
    if (machine == NULL) {
        machine = snes_init();
    }

    if (picture == NULL) {
        picture = malloc((size_t)PICTURE_W * PICTURE_H * 4u);
    }

    if (machine == NULL || picture == NULL) {
        return luaL_error(L, "no room for a Super Nintendo");
    }

    if (!snes_loadRom(machine, (const uint8_t *)(uintptr_t)at, (int)len)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "the emulator would not load it; its reason is above");
        return 2;
    }

    loaded = true;

    lua_pushboolean(L, 1);
    lua_pushnumber(L, (lua_Number)console_rate());
    return 2;
}

/*
 * snes.sound(ring, rate) - where the sound goes, and how fast the device
 * takes it.
 *
 * The ring is the one `audio.open` made and handed to the audio server, by
 * address; the rate is `audio.format().rate`, asked of the machine rather
 * than assumed. From here on every `snes.frame` puts its samples in the ring.
 */
static int l_sound(lua_State *L)
{
    struct audio_ring *r =
        (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 1);
    lua_Integer rate = luaL_checkinteger(L, 2);

    if (!loaded) {
        return luaL_error(L, "start a ROM before its sound");
    }

    if (!audio_ring_valid(r) || r->period_bytes % 4u != 0) {
        return luaL_argerror(L, 1, "not a ring of 16-bit stereo periods");
    }

    if (rate <= 0 || (double)rate / console_rate() > (double)SAMPLES_MAX) {
        return luaL_argerror(L, 2, "not a rate this can feed");
    }

    if (samples == NULL) {
        samples = malloc((size_t)SAMPLES_MAX * 2u * sizeof *samples);
    }

    if (samples == NULL) {
        return luaL_error(L, "no room for a frame of sound");
    }

    ring = r;
    per_frame = (double)rate / console_rate();
    owed = 0.0;
    partial = 0;

    return 0;
}

/*
 * snes.frame(surface) - one frame of the console, its sound, then its
 * picture. Returns how many frames of sound the ring had no room for.
 *
 * The sound is a whole number of device frames each time and the fraction is
 * carried, so 735 a frame at 44100 over 60 comes out exactly and a rate that
 * does not divide evenly neither drifts nor rounds.
 *
 * The picture goes row by row through the surface's own pitch (`gfx.md`
 * 19.3), which is why the core draws into `picture` rather than into the
 * surface: it assumes 2048 bytes a row, and a surface is a resize away from
 * not having them. The alpha is forced opaque, as Doom's is, because a
 * surface blends on it and nothing guarantees the core writes one.
 *
 * `snes_blit.c` does the copy, at the scale the optional second argument
 * asks for - 1, or 2 for `--scale 2` - and it is a file of its own so the
 * host can test it without a core.
 */
static int l_frame(lua_State *L)
{
    unsigned w = 0, h = 0, pitch = 0;
    uint32_t *dst = kosmos_surface_pixels(L, 1, &w, &h, &pitch);
    lua_Integer scale = luaL_optinteger(L, 2, 1);
    unsigned dropped = 0;

    if (!loaded) {
        return luaL_error(L, "no ROM has been started");
    }

    if (scale < 1 || scale > (lua_Integer)SNES_SCALE_MAX) {
        return luaL_error(L, "a scale of 1 to %d, not %d",
                          (int)SNES_SCALE_MAX, (int)scale);
    }

    snes_runFrame(machine);

    if (ring != NULL) {
        unsigned want;

        owed += per_frame;
        want = (unsigned)owed;
        owed -= (double)want;

        if (want > SAMPLES_MAX) {
            want = SAMPLES_MAX;
        }

        snes_setSamples(machine, samples, (int)want);
        dropped = feed(samples, want);
    }

    snes_setPixels(machine, picture);

    if (dst != NULL) {
        snes_blit(dst, w, h, pitch, (const uint32_t *)(void *)picture,
                  PICTURE_W, PICTURE_H, (unsigned)scale);
    }

    lua_pushinteger(L, (lua_Integer)dropped);
    return 1;
}

/*
 * snes.button(which, down) - one button on the first pad.
 *
 * State rather than transitions, unlike `doom.key`: the console reads the
 * pad once a frame, so what matters is whether a button is down when it
 * looks, not how it got there.
 *
 * The range is checked because the number becomes a shift of a 16-bit
 * register inside the core, and a shift past its width is undefined rather
 * than merely wrong.
 */
static int l_button(lua_State *L)
{
    lua_Integer which = luaL_checkinteger(L, 1);

    if (which < 0 || which > 11) {
        return luaL_argerror(L, 1, "a button is 0 to 11; see snes.buttons");
    }

    if (machine != NULL) {
        snes_setButtonState(machine, 1, (int)which, lua_toboolean(L, 2));
    }

    return 0;
}

/* snes.log() - whatever the core printed since the last call, or nil. */
static int l_log(lua_State *L)
{
    char buf[1024];
    size_t n = kosmos_spill_drain(buf, sizeof buf);

    if (n == 0) {
        return 0;
    }

    lua_pushlstring(L, buf, n);
    return 1;
}

static const luaL_Reg snes_lib[] = {
    { "start",  l_start },
    { "sound",  l_sound },
    { "frame",  l_frame },
    { "button", l_button },
    { "log",    l_log },
    { NULL, NULL },
};

/*
 * The buttons, by name, in the order the pad shifts them out: B first and R
 * last, which is the bit each one is in the register the core keeps. Named
 * here rather than in the Lua side, so that number lives next to the code it
 * belongs to.
 */
static const char *const button_names[12] = {
    "b", "y", "select", "start", "up", "down", "left", "right",
    "a", "x", "l", "r",
};

void kosmos_snes_open(lua_State *L)
{
    int i;

    luaL_newlib(L, snes_lib);

    lua_createtable(L, 0, 12);

    for (i = 0; i < 12; i++) {
        lua_pushinteger(L, i);
        lua_setfield(L, -2, button_names[i]);
    }

    lua_setfield(L, -2, "buttons");

    lua_pushinteger(L, PICTURE_W);
    lua_setfield(L, -2, "width");

    lua_pushinteger(L, PICTURE_H);
    lua_setfield(L, -2, "height");

    lua_setglobal(L, "snes");
}
