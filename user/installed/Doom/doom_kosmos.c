/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Doom, on this machine.
 *
 * The half of the port that is ours. `runtime/upstream/doom/` is id's source, byte for
 * byte and GPLv2; this file is the platform underneath it and is the only
 * place the two vocabularies meet. The two are linked into `doom.elf`, an
 * application's own image in `/Home/Apps/Doom` (`make apps`), and no system
 * image carries either; `runtime/upstream/doom/README.md` says what that
 * makes of an image's licence.
 *
 *--------------------------------------------------------------------------
 * What doomgeneric asks for, and what it gets.
 *
 * Six functions - `DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`,
 * `DG_GetKey`, `DG_SetWindowTitle` - plus a `wad_file_class_t` for reading
 * the WAD. That is the whole contract, which is why doomgeneric is the port
 * to start from rather than Chocolate Doom directly.
 *
 * None of them block, and that is the design. `doomgeneric_Create` runs
 * Doom's setup and returns, and `doomgeneric_Tick` runs one frame and
 * returns, so the *Lua* side owns the loop: it owns the window, it decides
 * when a frame happens, and it can stop. A port that owned its own loop
 * would be an application that cannot be closed, which on this desktop
 * means a window the compositor keeps drawing for ever.
 *
 *--------------------------------------------------------------------------
 * The WAD does not come through `fopen`.
 *
 * `stdio.c` in this system is a set of functions that fail, and deliberately:
 * there is no global tree, so there is no path to open. What there is is a
 * namespace, reached from Lua.
 *
 * So the division is the one `pdf.lua` already uses and `CLAUDE.md`
 * describes - Lua does the I/O, C does the loops. Lua reads the WAD through
 * the ordinary filesystem and hands the bytes here; `wad_file_t` has a
 * `mapped` pointer for exactly this case, so Doom reads it where it lies and
 * never copies it. Four megabytes, read once.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <kosmos.h>

#include "lua.h"
#include "lauxlib.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "w_file.h"

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

/* A surface's pixels, from `gfx.c`, which is the only file allowed to know
 * how a surface is laid out: `kosmos_surface_pixels`. */
#include "kits/gfx/gfx_draw.h"

#include "audioring.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

/*--------------------------------------------------------------------------
 * The WAD, in memory.
 *------------------------------------------------------------------------*/

static const unsigned char *wad_bytes;
static size_t               wad_len;

static wad_file_t wad_handle;

/* Defined below, and referred to above: an implementation has to put its
 * own class into the handle it returns. */
extern wad_file_class_t stdc_wad_file;

static wad_file_t *kosmos_OpenFile(char *path)
{
    (void)path;

    /*
     * The path is ignored, and that is not a shortcut.
     *
     * There is exactly one WAD and the Lua side already chose it, by name,
     * through a namespace this process was handed. Honouring a path here
     * would mean this file resolving one - which is the ambient authority
     * the whole system is built to refuse. Doom asks for "the IWAD"; it
     * gets the one it was given.
     */
    if (wad_bytes == NULL) {
        return NULL;
    }

    /*
     * The implementation sets this, not `W_OpenFile`.
     *
     * It said `NULL` here with a comment saying the caller would fill it in,
     * which was a guess and was wrong: `W_OpenFile` only walks the list of
     * classes and takes the first that returns non-NULL - every one of
     * upstream's own implementations sets its own `file_class` on the way
     * out. `W_Read` then loads it and calls through it, so a NULL here is
     * `ldr x4, [x4, #16]` on address zero, which is exactly the fault it
     * produced.
     */
    wad_handle.file_class = &stdc_wad_file;
    wad_handle.mapped = (unsigned char *)wad_bytes;
    wad_handle.length = (unsigned int)wad_len;

    return &wad_handle;
}

static void kosmos_CloseFile(wad_file_t *file)
{
    (void)file;

    /* Nothing to close. The bytes belong to Lua and outlive this. */
}

static size_t kosmos_Read(wad_file_t *file, unsigned int offset,
                          void *buffer, size_t buffer_len)
{
    size_t available;

    (void)file;

    if (wad_bytes == NULL || offset >= wad_len) {
        return 0;
    }

    /*
     * Clamped rather than trusted. `offset` and `buffer_len` come out of the
     * WAD's own directory, which is a file on a disk - so they are input,
     * and a truncated or hostile WAD that asks for a lump past the end must
     * get a short read rather than a walk off the end of the buffer.
     */
    available = wad_len - offset;

    if (buffer_len > available) {
        buffer_len = available;
    }

    memcpy(buffer, wad_bytes + offset, buffer_len);

    return buffer_len;
}

/*
 * The name `w_file.c` expects. It has a table of implementations - Win32,
 * mmap, stdio - and picks the first that opens the file. Ours is the only
 * one compiled, under the name the table refers to.
 */
wad_file_class_t stdc_wad_file = {
    kosmos_OpenFile,
    kosmos_CloseFile,
    kosmos_Read,
};

/*--------------------------------------------------------------------------
 * The six.
 *------------------------------------------------------------------------*/

static unsigned long started_at;
static unsigned long counter_hz = 62500000ul;
static unsigned long tick_hz = 250ul;       /* the scheduler's, which a sleep counts in */

void DG_Init(void)
{
    struct sysinfo info;

    started_at = kosmos_ticks();

    if (kosmos_sysinfo(&info) == 0) {
        if (info.counter_hz != 0) {
            counter_hz = info.counter_hz;
        }

        if (info.tick_hz != 0) {
            tick_hz = info.tick_hz;
        }
    }
}

/*
 * Doom has finished a frame.
 *
 * Nothing is drawn here: the pixels are in `DG_ScreenBuffer` and the Lua
 * side copies them into its window when it asks for the frame. Drawing from
 * inside Doom would mean this file holding a window handle and deciding when
 * to send it, which is the loop the Lua side owns.
 */
void DG_DrawFrame(void)
{
}

/*
 * **A sleep, and it was a spin.**
 *
 * This is what Doom calls to hold itself to 35 tics a second: `TryRunTics`
 * asks for a millisecond at a time until the next tic is due. It yielded
 * until the counter passed the deadline, on the reasoning that a yield
 * hands the processor to whoever needs it - and on the M700's eight
 * processors, with room on them, a yield comes straight back. A profile of
 * the desktop found 84% of Doom's samples on the way back from one: about
 * 29% of a processor spent waiting (`roadmap.md`, FOUND on 29 September).
 *
 * The kernel sleeps in scheduler ticks, four milliseconds at 250 a second,
 * and a millisecond is less than one - so a wait is a tick at least, and
 * Doom looks at its clock again when it ends. A tic is 28.6 ms and may now
 * start up to a tick late; the game's speed is kept by the clock
 * (`DG_GetTicksMs`), not by this, and `run_doom.py` holds the frames a
 * second to what they were.
 */
void DG_SleepMs(uint32_t ms)
{
    unsigned long ticks = ((unsigned long)ms * tick_hz + 999ul) / 1000ul;

    kosmos_sleep(ticks != 0 ? ticks : 1ul);
}

uint32_t DG_GetTicksMs(void)
{
    unsigned long since = kosmos_ticks() - started_at;

    return (uint32_t)(since / (counter_hz / 1000ul));
}

/*
 * Keys, as transitions.
 *
 * A ring of press-and-release events, filled by the Lua side and drained
 * here. Doom wants transitions rather than state - it tracks which keys are
 * down itself, and needs to be told each time one changes - so this is a
 * queue and not the bitmap the HAL keeps.
 *
 * Dropping the oldest when it is full rather than the newest: a lost press
 * is a step you did not take, and a lost *release* is a key stuck down for
 * ever, which is worse than either.
 */
#define KEYQ 32

static struct {
    unsigned char key;
    unsigned char down;
} keyq[KEYQ];

static unsigned keyq_head, keyq_tail;

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (keyq_head == keyq_tail) {
        return 0;
    }

    *pressed = keyq[keyq_tail].down;
    *key = keyq[keyq_tail].key;
    keyq_tail = (keyq_tail + 1) % KEYQ;

    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;

    /* The window is the Lua side's and it named it already. */
}

/*--------------------------------------------------------------------------
 * The Lua side of it.
 *------------------------------------------------------------------------*/

static int running;

/*
 * doom.start(address, length) - where the WAD is, and how much of it.
 *
 * An address rather than a string, and that is not an optimisation.
 *
 * `USER_HEAP_PAGES` is 512, so the Lua heap is two megabytes and the
 * smallest real IWAD is four. A WAD simply cannot be a Lua value here. What
 * it can be is a *region*: pages taken from the page allocator, outside the
 * heap, which `fs.read_into` fills from the disk server a window at a time,
 * through a scratch region `sys.region_copy` moves each window out of,
 * without the bytes ever passing through Lua at all.
 *
 * So the Lua side maps a region and hands over the address, and Doom reads
 * it where it lies through `wad_file_t.mapped`. Four megabytes, read once and
 * never copied again - and the limit that forced it turned out to be pointing
 * at the better design, which is usually what a limit like that is doing.
 *
 * The caller must keep the region alive for as long as Doom runs. It does:
 * the application holds the capability for its own lifetime, and when it
 * exits the process goes with it.
 */
static int l_start(lua_State *L)
{
    unsigned long at = (unsigned long)luaL_checkinteger(L, 1);
    size_t len = (size_t)luaL_checkinteger(L, 2);
    /*
     * `-iwad doom1.wad`, and both halves matter.
     *
     * Without it `D_FindIWAD` searches directories for a WAD, using an
     * `opendir` this system does not have, and ends at `I_Error("Game mode
     * indeterminate")`. With it, Doom takes the name it was given.
     *
     * The *name* is not decoration either: `IdentifyIWADByName` decides
     * which game this is - shareware Doom, Ultimate, Doom II - from the
     * filename alone, so `doom1.wad` is what makes the episode menus right.
     * That is Doom's convention and this is Doom.
     */
    static char arg0[] = "doom";
    static char arg1[] = "-iwad";
    static char arg2[] = "doom1.wad";
    static char *argv[] = { arg0, arg1, arg2, NULL };

    if (running) {
        return luaL_error(L, "doom is already running in this process");
    }

    if (at == 0 || len < 12) {
        return luaL_error(L, "that is not a WAD: %d bytes at %d",
                          (int)len, (int)at);
    }

    wad_bytes = (const unsigned char *)at;
    wad_len = len;

    /*
     * And the libc is told what that name means here.
     *
     * `D_FindWADByName` checks the file exists before believing in it, and
     * it checks with `fopen`. So the same bytes are reachable two ways: as
     * a `wad_file_t.mapped` for the reading Doom does at speed, and as a
     * `FILE` for the one question it asks about existence. Both point at the
     * same region and neither copies it.
     */
    if (kosmos_provide("doom1.wad", (const void *)at, len) != 0) {
        return luaL_error(L, "the libc would not take the WAD");
    }

    running = 1;

    /*
     * This runs Doom's whole setup - reading the WAD's directory, building
     * its tables, and one frame - and returns. It does not loop: that is
     * what makes doomgeneric the port to start from, and what lets the Lua
     * side own the frame loop and be able to stop.
     */
    /*
     * Armed, because Doom exits when it is unhappy.
     *
     * `I_Error` prints what is wrong and calls `exit`, and without a landing
     * place that is a panic - the process dies with the explanation still in
     * the spill ring and nobody left to drain it. With one, control comes
     * back here, `l_start` returns false, and the Lua side prints what Doom
     * actually said.
     */
    kosmos_exit_arm();

    if (setjmp(kosmos_exit_to) != 0) {
        running = 0;

        lua_pushboolean(L, 0);
        lua_pushstring(L, "doom stopped during startup");

        return 2;
    }

    doomgeneric_Create(3, argv);

    kosmos_exit_disarm();

    lua_pushboolean(L, 1);

    return 1;
}

/* doom.frame(surface) - one tick, then the pixels into that surface. */
static int l_frame(lua_State *L)
{
    unsigned w = 0, h = 0, pitch = 0;
    uint32_t *dst = kosmos_surface_pixels(L, 1, &w, &h, &pitch);
    unsigned y, rows, cols;

    if (!running) {
        return luaL_error(L, "doom has not been started");
    }

    doomgeneric_Tick();

    if (DG_ScreenBuffer == NULL) {
        return 0;
    }

    rows = (h < DOOMGENERIC_RESY) ? h : DOOMGENERIC_RESY;
    cols = (w < DOOMGENERIC_RESX) ? w : DOOMGENERIC_RESX;

    /*
     * Row by row through the surface's own pitch.
     *
     * `gfx.md` 19.3: the pitch is almost never `width * 4`, and the fact
     * that it happens to be for 640 pixels is not a reason to rely on it -
     * a window of another width is one resize away. The alpha is forced
     * opaque because Doom writes none and a surface is 0xAARRGGBB.
     */
    for (y = 0; y < rows; y++) {
        const uint32_t *src = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;
        uint32_t *out = (uint32_t *)((uint8_t *)dst + (size_t)y * pitch);
        unsigned x;

        for (x = 0; x < cols; x++) {
            out[x] = src[x] | 0xff000000u;
        }
    }

    return 0;
}

/* doom.key(code, down) - one transition, in Doom's own key numbering. */
static int l_key(lua_State *L)
{
    unsigned next = (keyq_head + 1) % KEYQ;

    keyq[keyq_head].key = (unsigned char)luaL_checkinteger(L, 1);
    keyq[keyq_head].down = lua_toboolean(L, 2) ? 1 : 0;

    keyq_head = next;

    /* Full: the oldest goes, so a release is never the one that is lost. */
    if (keyq_head == keyq_tail) {
        keyq_tail = (keyq_tail + 1) % KEYQ;
    }

    return 0;
}

/*
 * doom.log() - whatever Doom printed since the last call, or nil.
 *
 * Doom talks: which IWAD it found, how many lumps, what it could not load,
 * and `I_Error`'s last words. All of it goes through `printf`, and `printf`
 * is refused for a process that does not own the console - so it lands in
 * the ring in `stdio.c` and this is the spoon. The Lua side calls it each
 * frame and prints what comes out, which puts Doom's account of itself on
 * the same console as everything else.
 */
size_t kosmos_spill_drain(char *out, size_t max);

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

/*--------------------------------------------------------------------------
 * Sound (Diego, 11 October 2026: "doom and quake need sound support").
 *
 * **Doom's sound module, ours**: doomgeneric names `DG_sound_module` when
 * `FEATURE_SOUND` is on and leaves it to the platform, and this is it. It
 * claims the Sound Blaster, which is what Doom's settings choose unless
 * told otherwise.
 *
 * Doom's effects are lumps in the WAD - DMX's format: three, the rate, the
 * length, sixteen bytes DMX skipped, eight-bit unsigned samples, sixteen
 * more - and Doom asks for one on a channel with a volume from 0 to 127 and a
 * separation from 0 (left) to 254 (right). Each channel steps through its
 * effect at its own rate against the device's, in 16.16, and the channels
 * are summed into stereo frames that go straight into the audio server's ring
 * (`audio_ring_feed`) - no Lua string at either end, as the Super Nintendo's
 * sound does.
 *
 * **Mixed when Doom updates its sound**, once a game tic, as far ahead as
 * `AHEAD` periods: a tic is 28.6 ms and a period here about 6, so six
 * periods ahead is a tic and a little, enough that one late tic is not a
 * gap, and no more, so a shot is heard within about 35 ms of being fired.
 *
 * Music is a later step: Doom's is MUS, a score, and it wants a synthesiser
 * to be heard (`roadmap.md`, Doom and Quake with sound).
 *------------------------------------------------------------------------*/

#define CHANNELS   16
#define AHEAD      6u              /* periods mixed ahead of the speaker */
#define MIX_MAX    2048u           /* frames mixed at a time */

struct voice {
    const uint8_t *data;           /* the samples, 8-bit unsigned */
    uint32_t       length;         /* how many */
    uint32_t       at;             /* where, in 16.16 */
    uint32_t       step;           /* how far a device frame moves it, 16.16 */
    int            left, right;    /* 0 to 127 each */
    boolean        playing;
};

static struct voice        voices[CHANNELS];
static struct audio_ring  *sound_ring;
static uint32_t            sound_rate;
static uint32_t            sound_partial;
static int16_t             mixed[MIX_MAX * 2u];

/* Two numbers libsamplerate would read, which an SDL build defines. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static snddevice_t claimed[] = { SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
                                 SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS,
                                 SNDDEVICE_AWE32 };

static boolean sfx_prefixed;

static boolean dg_sound_init(boolean use_sfx_prefix)
{
    sfx_prefixed = use_sfx_prefix;
    memset(voices, 0, sizeof voices);
    return true;
}

static void dg_sound_shutdown(void)
{
    sound_ring = NULL;
}

static int dg_sound_lump(sfxinfo_t *sfx)
{
    char name[16];

    if (sfx->link != NULL) {
        sfx = sfx->link;
    }

    snprintf(name, sizeof name, sfx_prefixed ? "ds%s" : "%s", sfx->name);
    return W_GetNumForName(name);
}

static void dg_sound_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= CHANNELS) {
        return;
    }

    if (vol < 0) vol = 0;
    if (vol > 127) vol = 127;
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;

    voices[channel].left = (vol * (254 - sep)) / 254;
    voices[channel].right = (vol * sep) / 254;
}

static int dg_sound_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= CHANNELS || sound_rate == 0) {
        return -1;
    }

    int lump = sfx->lumpnum;
    unsigned lumplen = W_LumpLength(lump);
    const uint8_t *data = W_CacheLumpNum(lump, PU_STATIC);

    /* DMX's header, held to what Chocolate Doom holds it to. */
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00) {
        return -1;
    }

    uint32_t rate = (uint32_t)(data[3] << 8 | data[2]);
    uint32_t length = (uint32_t)data[7] << 24 | (uint32_t)data[6] << 16
                      | (uint32_t)data[5] << 8 | data[4];

    if (length > lumplen - 8 || length <= 48 || rate == 0) {
        return -1;
    }

    struct voice *v = &voices[channel];

    v->data = data + 24;           /* past the header and DMX's sixteen */
    v->length = length - 32;       /* and its sixteen at the end */
    v->at = 0;
    v->step = (uint32_t)(((uint64_t)rate << 16) / sound_rate);
    v->playing = true;
    dg_sound_params(channel, vol, sep);

    return channel;
}

static void dg_sound_stop(int channel)
{
    if (channel >= 0 && channel < CHANNELS) {
        voices[channel].playing = false;
    }
}

static boolean dg_sound_playing(int channel)
{
    return channel >= 0 && channel < CHANNELS && voices[channel].playing;
}

static void dg_sound_cache(sfxinfo_t *sounds, int num)
{
    (void)sounds;
    (void)num;
}

/* The channels summed into `frames` stereo frames. */
static void mix(unsigned frames)
{
    for (unsigned f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;

        for (int c = 0; c < CHANNELS; c++) {
            struct voice *v = &voices[c];

            if (!v->playing) {
                continue;
            }

            uint32_t i = v->at >> 16;

            if (i >= v->length) {
                v->playing = false;
                continue;
            }

            int32_t sample = ((int32_t)v->data[i] - 128) << 8;

            l += sample * v->left / 127;
            r += sample * v->right / 127;
            v->at += v->step;
        }

        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;

        mixed[2 * f] = (int16_t)l;
        mixed[2 * f + 1] = (int16_t)r;
    }
}

/* Once a tic: the ring kept `AHEAD` periods ahead of the speaker. */
static void dg_sound_update(void)
{
    struct audio_ring *r = sound_ring;

    if (r == NULL) {
        return;
    }

    uint32_t period = r->period_bytes / 4u;
    uint32_t queued = audio_ring_ready(r) * period + sound_partial;
    uint32_t want = AHEAD * period;
    uint32_t room = audio_ring_room(r, sound_partial);

    while (queued < want && room > 0) {
        unsigned n = want - queued;

        if (n > room) n = room;
        if (n > MIX_MAX) n = MIX_MAX;

        mix(n);
        audio_ring_feed(r, &sound_partial, mixed, n);
        queued += n;
        room -= n;
    }
}

sound_module_t DG_sound_module = {
    claimed, sizeof claimed / sizeof claimed[0],
    dg_sound_init, dg_sound_shutdown, dg_sound_lump, dg_sound_update,
    dg_sound_params, dg_sound_start, dg_sound_stop, dg_sound_playing,
    dg_sound_cache,
};

/* No music yet: a module that takes every call and plays nothing. */
static boolean dg_music_init(void) { return true; }
static void dg_music_nothing(void) { }
static void dg_music_volume(int volume) { (void)volume; }
static void *dg_music_register(void *data, int len) { (void)data; (void)len; return NULL; }
static void dg_music_unregister(void *handle) { (void)handle; }
static void dg_music_play(void *handle, boolean looping) { (void)handle; (void)looping; }
static boolean dg_music_playing(void) { return false; }

music_module_t DG_music_module = {
    NULL, 0,
    dg_music_init, dg_music_nothing, dg_music_volume, dg_music_nothing,
    dg_music_nothing, dg_music_register, dg_music_unregister, dg_music_play,
    dg_music_nothing, dg_music_playing, dg_music_nothing,
};

/*
 * doom.sound(ring, rate) - where the sound goes, and how fast the device
 * plays it: the ring `audio.open` made, by address, and
 * `audio.format().rate`. Before it, Doom is silent.
 */
static int l_sound(lua_State *L)
{
    struct audio_ring *r = (struct audio_ring *)(uintptr_t)luaL_checkinteger(L, 1);
    lua_Integer rate = luaL_checkinteger(L, 2);

    if (!audio_ring_valid(r) || r->period_bytes % 4u != 0 || rate <= 0) {
        return luaL_error(L, "that is not a stereo sound ring");
    }

    sound_ring = r;
    sound_rate = (uint32_t)rate;
    sound_partial = 0;
    return 0;
}

static const luaL_Reg doom_lib[] = {
    { "sound", l_sound },
    { "start", l_start },
    { "log",   l_log },
    { "frame", l_frame },
    { "key",   l_key },
    { NULL, NULL },
};

/*
 * The table `doom.lua` reaches as `use("doom.elf")` - the image it runs in,
 * by its file - and the size Doom renders at, so the Lua side can make a
 * window that fits without either of them carrying the other's number.
 *
 * Not a global, which it was: a global named `doom` hid `doom.lua`, in
 * `/Home/Apps/Doom`, from the prompt. `snes_kosmos.c` has the longer account.
 */
void kosmos_doom_kit(lua_State *L)
{
    luaL_newlib(L, doom_lib);

    lua_pushinteger(L, DOOMGENERIC_RESX);
    lua_setfield(L, -2, "width");

    lua_pushinteger(L, DOOMGENERIC_RESY);
    lua_setfield(L, -2, "height");
}
