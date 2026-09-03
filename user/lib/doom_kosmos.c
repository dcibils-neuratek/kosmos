/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Doom, on this machine.
 *
 * The half of the port that is ours. `user/doom/` is id's source, byte for
 * byte and GPLv2; this file is the platform underneath it and is the only
 * place the two vocabularies meet. See `user/doom/README.md` for why the
 * whole thing is behind `make DOOM=1`.
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

#include <kosmos.h>

#include "lua.h"
#include "lauxlib.h"

#include "../doom/doomgeneric.h"
#include "../doom/doomkeys.h"
#include "../doom/w_file.h"

void doomgeneric_Create(int argc, char **argv);
void doomgeneric_Tick(void);

/*
 * A surface's pixels, from `gfx.c`, which is the only file allowed to know
 * how a surface is laid out. Declared here the way `gfx.c` declares
 * `kosmos_png_open`: one line, next to the thing that needs it.
 */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

/*--------------------------------------------------------------------------
 * The WAD, in memory.
 *------------------------------------------------------------------------*/

static const unsigned char *wad_bytes;
static size_t               wad_len;

static wad_file_t wad_handle;

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

    wad_handle.file_class = NULL;       /* filled in by W_OpenFile's caller */
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

void DG_Init(void)
{
    struct sysinfo info;

    started_at = kosmos_ticks();

    if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) {
        counter_hz = info.counter_hz;
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

void DG_SleepMs(uint32_t ms)
{
    unsigned long until = kosmos_ticks() + (counter_hz / 1000ul) * ms;

    /*
     * Yielding rather than spinning. This is what Doom calls to hold itself
     * to 35 tics a second, and a busy wait here would be a busy wait on a
     * desktop that has a compositor to run - `sched_prio.c`'s bands are
     * about exactly this.
     */
    while (kosmos_ticks() < until) {
        kosmos_yield();
    }
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

/* doom.start(wad) - hands over the bytes and runs Doom's setup. */
static int l_start(lua_State *L)
{
    size_t len;
    const char *bytes = luaL_checklstring(L, 1, &len);
    static char arg0[] = "doom";
    static char *argv[] = { arg0, NULL };

    if (running) {
        return luaL_error(L, "doom is already running in this process");
    }

    /*
     * The string is pinned in the registry for the life of the process.
     *
     * `wad_bytes` points into a Lua string and Doom keeps that pointer -
     * `wad_file_t.mapped` is the whole reason this is fast. A collected
     * string would be a dangling pointer inside id's code, which is the one
     * place a bug here would be impossible to read. Pinned, it cannot be.
     */
    lua_pushvalue(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "kosmos.doom.wad");

    wad_bytes = (const unsigned char *)bytes;
    wad_len = len;

    running = 1;

    doomgeneric_Create(1, argv);

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

static const luaL_Reg doom_lib[] = {
    { "start", l_start },
    { "frame", l_frame },
    { "key",   l_key },
    { NULL, NULL },
};

/*
 * The size Doom renders at, so the Lua side can make a window that fits
 * without either of them carrying the other's number.
 */
void kosmos_doom_open(lua_State *L)
{
    luaL_newlib(L, doom_lib);

    lua_pushinteger(L, DOOMGENERIC_RESX);
    lua_setfield(L, -2, "width");

    lua_pushinteger(L, DOOMGENERIC_RESY);
    lua_setfield(L, -2, "height");

    lua_setglobal(L, "doom");
}
