/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Quake, on this machine.
 *
 * The half of the port that is ours. `runtime/upstream/quake/` is Chocolate
 * Quake, byte for byte and GPL; this file is the platform under it, written
 * in place of the SDL one upstream ships, and the one place the two
 * vocabularies meet. `runtime/upstream/quake/README.kosmos.md` is the
 * account, including why the whole thing is behind `make QUAKE=1`.
 *
 * What the engine asks for, and what it gets:
 *
 * - **`Sys_*`**: files through this libc's `fopen`, which reads the pak the
 *   Lua side provided and refuses everything else; the counter for time; and
 *   `exit` for errors and for quitting, which lands back in `quake.start` or
 *   `quake.frame` rather than ending the process with the reason unread.
 * - **`VID_*`**: 320 by 240 palette indices, which `quake.frame` spreads into
 *   the window's surface at twice the size, through the palette.
 * - **`IN_*` and `Sys_SendKeyEvents`**: the keys and mouse movement the Lua
 *   side queued since the last frame.
 * - **`SNDDMA_*`, `BGMusic_*`, `S_Codec*`**: silence, for now. The engine
 *   runs its sound code, finds no device, and says so.
 * - **A network driver table with only Loopback in it**, which is all a game
 *   on one machine uses.
 * - **A stack of its own** for the engine, a megabyte with a guard page under
 *   it, because one renderer frame is most of a process's stack here.
 *
 * None of it blocks. `quake.start` runs `Host_Init` and returns, and
 * `quake.frame` runs one `Host_Frame` and returns, so the Lua side owns the
 * window and the loop - which is what lets the window be closed.
 */

#include "quakedef.h"
#include "client.h"
#include "d_local.h"
#include "host.h"
#include "input.h"
#include "keys.h"
#include "net.h"
#include "net/src/net_loop.h"
#include "render.h"
#include "sound.h"
#include "sys.h"
#include "vid.h"
#include "zone.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kosmos.h>

#include "lua.h"
#include "lauxlib.h"

/* From `gfx.c`, the one file allowed to know how a surface is laid out. */
uint32_t *kosmos_surface_pixels(lua_State *L, int index,
                                unsigned *w, unsigned *h, unsigned *pitch);

/* The ring `printf` spills into for a process without the console. */
size_t kosmos_spill_drain(char *out, size_t max);

/* Defined in `menu.c` and declared by nothing public. */
void M_Menu_Options_f(void);

/*
 * Quake's memory, in one block. Upstream's SDL build asks for 256 MB; a
 * process here may map 48 in all, the pak takes eighteen of them, and the
 * shareware episode runs in sixteen, which is what WinQuake asked for.
 */
#define HUNK_BYTES (16 * 1024 * 1024)

/* The size Quake renders at, and how much bigger the window shows it. */
#define QUAKE_W 320
#define QUAKE_H 240
#define SCALE   2

/*--------------------------------------------------------------------------
 * Time.
 *------------------------------------------------------------------------*/

static unsigned long started_at;
static unsigned long counter_hz = 62500000ul;

double Sys_FloatTime(void)
{
    return (double)(kosmos_ticks() - started_at) / (double)counter_hz;
}

/*--------------------------------------------------------------------------
 * Files.
 *
 * Handles over `FILE`s, the way upstream's `sys.c` keeps them. Reading works
 * for what `kosmos_provide` named; opening for writing is refused and says
 * so with -1, which `COM_WriteFile` checks. A handle that was never opened is
 * ignored rather than dereferenced, because `COM_CopyFile` does not check.
 *------------------------------------------------------------------------*/

qboolean isDedicated;

#define MAX_HANDLES 10

static FILE *handles[MAX_HANDLES];

static FILE *handle(i32 h)
{
    return (h > 0 && h < MAX_HANDLES) ? handles[h] : NULL;
}

i32 Sys_FileOpenRead(char *path, i32 *hndl)
{
    i32 i;
    FILE *f;
    long end;

    for (i = 1; i < MAX_HANDLES && handles[i] != NULL; i++) {
    }

    if (i == MAX_HANDLES) {
        Sys_Error("Sys_FileOpenRead: out of handles");
    }

    f = fopen(path, "rb");

    if (f == NULL) {
        *hndl = -1;
        return -1;
    }

    fseek(f, 0, SEEK_END);
    end = ftell(f);
    fseek(f, 0, SEEK_SET);

    handles[i] = f;
    *hndl = i;

    return (i32)end;
}

i32 Sys_FileOpenWrite(char *path)
{
    (void)path;

    return -1;
}

void Sys_FileClose(i32 h)
{
    FILE *f = handle(h);

    if (f != NULL) {
        fclose(f);
        handles[h] = NULL;
    }
}

void Sys_FileSeek(i32 h, i32 position)
{
    FILE *f = handle(h);

    if (f != NULL) {
        fseek(f, position, SEEK_SET);
    }
}

size_t Sys_FileRead(i32 h, void *dest, i32 count)
{
    FILE *f = handle(h);

    return (f != NULL && count > 0) ? fread(dest, 1, (size_t)count, f) : 0;
}

size_t Sys_FileWrite(i32 h, void *data, i32 count)
{
    (void)h; (void)data; (void)count;

    return 0;
}

i32 Sys_FileTime(char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        return -1;
    }

    fclose(f);

    return 1;
}

void Sys_mkdir(char *path)
{
    (void)path;
}

/*--------------------------------------------------------------------------
 * Talking, failing and stopping.
 *------------------------------------------------------------------------*/

void Sys_Printf(char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/*
 * The error first, then the shutdown upstream does, then `exit` - which lands
 * in whichever of `quake.start` and `quake.frame` is running. Once only: a
 * shutdown that fails calls this again, and a second shutdown would be worse.
 */
void Sys_Error(char *error, ...)
{
    static int in_error;
    va_list ap;
    char text[1024];

    va_start(ap, error);
    vsnprintf(text, sizeof text, error, ap);
    va_end(ap);

    printf("Error: %s\n", text);

    if (!in_error) {
        in_error = 1;
        Host_Shutdown();
    }

    exit(1);
}

void Sys_Quit(void)
{
    Host_Shutdown();
    exit(0);
}

char *Sys_ConsoleInput(void)
{
    return NULL;
}

void Sys_HighFPPrecision(void)
{
}

void Sys_LowFPPrecision(void)
{
}

quakeparms_t *Sys_Init(i32 argc, char *argv[])
{
    static quakeparms_t parms;

    parms.memsize = HUNK_BYTES;
    parms.membase = malloc((size_t)parms.memsize);
    parms.basedir = ".";
    parms.cachedir = NULL;

    if (parms.membase == NULL) {
        Sys_Error("no room for a %d MB hunk", HUNK_BYTES >> 20);
    }

    COM_InitArgv(argc, argv);
    parms.argc = com_argc;
    parms.argv = com_argv;

    return &parms;
}

/*--------------------------------------------------------------------------
 * Keys.
 *
 * Transitions queued by `quake.key` and handed to `Key_Event` when the engine
 * asks, once a frame. Dropping the oldest when the queue is full, because a
 * lost release is a key held down for ever.
 *------------------------------------------------------------------------*/

#define KEYQ 64

static struct {
    i32 key;
    qboolean down;
} keyq[KEYQ];

static unsigned keyq_head, keyq_tail;

/* How many times this frame the engine has asked with nothing queued. */
static int empty_asks;

void Sys_SendKeyEvents(void)
{
    if (keyq_head == keyq_tail) {
        /*
         * A question asked inside a frame. `SCR_ModalMessage` ("start a new
         * game?") and `Con_NotifyBox` loop here until a key arrives - and on
         * this machine keys arrive from Lua, between frames, so that loop
         * would never end and the window would stop answering. The third
         * empty ask in one frame is that loop; it is answered no, which is
         * the answer that changes nothing, and said once.
         */
        if (++empty_asks > 2) {
            static int said;

            if (!said) {
                said = 1;
                printf("quake: a question asked mid-frame was answered no - "
                       "this port cannot wait for a key inside a frame\n");
            }

            Key_Event('n', true);
            Key_Event('n', false);
        }

        return;
    }

    while (keyq_tail != keyq_head) {
        i32 key = keyq[keyq_tail].key;
        qboolean down = keyq[keyq_tail].down;

        keyq_tail = (keyq_tail + 1) % KEYQ;
        Key_Event(key, down);
    }
}

/*--------------------------------------------------------------------------
 * The mouse.
 *
 * Movement the Lua side reported since the last frame, turned into looking
 * and moving the way id's own drivers do: sideways turns you unless strafing,
 * and up and down pitches your view with mouse-look held and walks you
 * forward and back without it.
 *------------------------------------------------------------------------*/

extern cvar_t sensitivity, m_yaw, m_pitch, m_side, m_forward, lookstrafe;

static int mouse_dx, mouse_dy;

void IN_Init(void)
{
}

void IN_Shutdown(void)
{
}

void IN_Move(usercmd_t *cmd)
{
    float mx = (float)mouse_dx * sensitivity.value;
    float my = (float)mouse_dy * sensitivity.value;
    int strafing = (in_strafe.state & 1) != 0;
    int looking = (in_mlook.state & 1) != 0;

    mouse_dx = 0;
    mouse_dy = 0;

    if (strafing || (lookstrafe.value != 0 && looking)) {
        cmd->sidemove += m_side.value * mx;
    } else {
        cl.viewangles[YAW] -= m_yaw.value * mx;
    }

    if (looking) {
        V_StopPitchDrift();
    }

    if (looking && !strafing) {
        cl.viewangles[PITCH] += m_pitch.value * my;

        if (cl.viewangles[PITCH] > 80) {
            cl.viewangles[PITCH] = 80;
        }

        if (cl.viewangles[PITCH] < -70) {
            cl.viewangles[PITCH] = -70;
        }
    } else if (strafing && noclip_anglehack) {
        cmd->upmove -= m_forward.value * my;
    } else {
        cmd->forwardmove -= m_forward.value * my;
    }
}

/*--------------------------------------------------------------------------
 * The screen.
 *
 * Palette indices in `screen`, the z-buffer and the surface cache on the
 * hunk the way upstream allocates them, and 256 colours ready to spread.
 *------------------------------------------------------------------------*/

viddef_t vid;

static byte     screen[QUAKE_W * QUAKE_H];
static uint32_t colours[256];
static qboolean vid_ready;
static qboolean drawn;
static i32      high_mark;

void VID_SetPalette(const byte *palette)
{
    int i;

    for (i = 0; i < 256; i++) {
        /* Six bits a channel, as the VGA controller kept them and as
         * Chocolate Quake keeps the look. */
        uint32_t r = palette[i * 3] & ~3u;
        uint32_t g = palette[i * 3 + 1] & ~3u;
        uint32_t b = palette[i * 3 + 2] & ~3u;

        colours[i] = 0xff000000u | (r << 16) | (g << 8) | b;
    }
}

void VID_ShiftPalette(const byte *palette)
{
    VID_SetPalette(palette);
}

void VID_Init(const byte *palette)
{
    i32 cache = D_SurfaceCacheForRes(QUAKE_W, QUAKE_H);
    i32 zbytes = QUAKE_W * QUAKE_H * (i32)sizeof(*d_pzbuffer);

    vid.width = QUAKE_W;
    vid.height = QUAKE_H;
    vid.aspect = 1.0f;
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.buffer = screen;
    vid.recalc_refdef = 1;

    high_mark = Hunk_HighMark();
    d_pzbuffer = Hunk_HighAllocName(zbytes + cache, "video");

    if (d_pzbuffer == NULL) {
        Sys_Error("Not enough memory for video mode\n");
    }

    D_InitCaches((byte *)d_pzbuffer + zbytes, cache);

    VID_SetPalette(palette);
    vid_ready = true;
}

void VID_Shutdown(void)
{
    if (!vid_ready) {
        return;
    }

    D_FlushCaches();
    Hunk_FreeToHighMark(high_mark);
    d_pzbuffer = NULL;
    vid_ready = false;
}

/* A frame is in `screen`. `quake.frame` copies it out after `Host_Frame`. */
void VID_Update(vrect_t *rects)
{
    (void)rects;

    drawn = true;
}

void VID_LockBuffer(void)
{
}

void VID_UnlockBuffer(void)
{
}

void VID_HandlePause(qboolean pause)
{
    (void)pause;
}

qboolean VID_IsFullscreenMode(void)
{
    return false;
}

qboolean VID_IsWindowedMode(void)
{
    return true;
}

qboolean VID_WindowedMouse(void)
{
    return true;
}

void VID_ToggleMouseGrab(void)
{
}

/* The video options: there is nothing to choose, because the window is the
 * size the Lua side made it. Escape goes back. */
void VID_MenuDraw(void)
{
}

void VID_MenuKey(i32 key)
{
    if (key == K_ESCAPE) {
        M_Menu_Options_f();
    }
}

/* The disc that flashes while loading, which this build does not show. */
void D_BeginDirectRect(i32 x, i32 y, byte *pbitmap, i32 width, i32 height)
{
    (void)x; (void)y; (void)pbitmap; (void)width; (void)height;
}

void D_EndDirectRect(i32 x, i32 y, i32 width, i32 height)
{
    (void)x; (void)y; (void)width; (void)height;
}

/*--------------------------------------------------------------------------
 * Sound, music and the codecs music needs: none yet.
 *------------------------------------------------------------------------*/

qboolean SNDDMA_Init(dma_t *dma)
{
    (void)dma;

    return false;
}

i32 SNDDMA_GetDMAPos(void)
{
    return 0;
}

void SNDDMA_Shutdown(void)
{
}

void SNDDMA_LockBuffer(void)
{
}

void SNDDMA_Submit(void)
{
}

void SNDDMA_BlockSound(void)
{
}

void SNDDMA_UnblockSound(void)
{
}

i32 BGMusic_Init(void)
{
    return false;
}

void BGMusic_Shutdown(void)
{
}

void BGMusic_Play(byte track, qboolean looping)
{
    (void)track; (void)looping;
}

void BGMusic_Stop(void)
{
}

void BGMusic_Pause(void)
{
}

void BGMusic_Resume(void)
{
}

void BGMusic_Update(void)
{
}

void S_CodecInit(void);
void S_CodecShutdown(void);

void S_CodecInit(void)
{
}

void S_CodecShutdown(void)
{
}

/*--------------------------------------------------------------------------
 * The network: Loopback, which is how a single-player game talks to itself.
 *------------------------------------------------------------------------*/

net_driver_t net_drivers[MAX_NET_DRIVERS] = {
    {
        .name                     = "Loopback",
        .initialized              = false,
        .Init                     = Loop_Init,
        .Listen                   = Loop_Listen,
        .SearchForHosts           = Loop_SearchForHosts,
        .Connect                  = Loop_Connect,
        .CheckNewConnections      = Loop_CheckNewConnections,
        .QGetMessage              = Loop_GetMessage,
        .QSendMessage             = Loop_SendMessage,
        .SendUnreliableMessage    = Loop_SendUnreliableMessage,
        .CanSendMessage           = Loop_CanSendMessage,
        .CanSendUnreliableMessage = Loop_CanSendUnreliableMessage,
        .Close                    = Loop_Close,
        .Shutdown                 = Loop_Shutdown,
    },
};

i32 net_numdrivers = 1;

/*--------------------------------------------------------------------------
 * The Lua side of it.
 *------------------------------------------------------------------------*/

static int running;

static char  arg0[] = "quake";
static char *quake_argv[] = { arg0, NULL };

/*
 * The engine's own stack.
 *
 * Quake's renderer keeps its edge and surface lists on the stack -
 * `R_EdgeDrawing` is a 205 KB frame with `R_RenderWorld`'s 80 KB inside it -
 * and a process's stack here is 256 KB, which the first frame ran off onto
 * the guard page. Raising that would cost every process the memory, each as
 * one contiguous run.
 *
 * So the engine runs on a megabyte of ordinary mapped pages, entered through
 * `kosmos_call_on_stack`. One page more is mapped and the lowest unmapped
 * again: an address here is never reused, so it stays a hole for good, and an
 * overflow faults on it as it would on the process stack's guard, instead of
 * writing into whatever was mapped before.
 */
#define ENGINE_STACK_PAGES 256

static void *engine_stack_top;

static int make_engine_stack(void)
{
    long base;

    if (engine_stack_top != NULL) {
        return 0;
    }

    base = kosmos_map(ENGINE_STACK_PAGES + 1);

    if (base < 0 || kosmos_unmap((unsigned long)base, 1) < 0) {
        return -1;
    }

    engine_stack_top = (void *)(uintptr_t)((unsigned long)base
                       + (ENGINE_STACK_PAGES + 1) * KOSMOS_PAGE_SIZE);

    return 0;
}

static void engine_init(void *unused)
{
    (void)unused;

    Host_Init(Sys_Init(1, quake_argv));
}

static void engine_frame(void *seconds)
{
    Host_Frame((float)*(double *)seconds);
}

/*
 * quake.start(address, length) - where the pak is, and how much of it.
 *
 * An address and not a string, for the reason `doom.start` gives: eighteen
 * megabytes cannot be a Lua value here. The libc is told the region is
 * `pak0.pak`, which is the name `COM_AddGameDirectory` asks `fopen` for, and
 * the caller keeps the region for as long as the process lives.
 */
static int l_start(lua_State *L)
{
    unsigned long at = (unsigned long)luaL_checkinteger(L, 1);
    size_t len = (size_t)luaL_checkinteger(L, 2);
    struct sysinfo info;

    if (running) {
        return luaL_error(L, "quake is already running in this process");
    }

    if (at == 0 || len < 12) {
        return luaL_error(L, "that is not a pak: %d bytes at %d",
                          (int)len, (int)at);
    }

    if (kosmos_provide("pak0.pak", (const void *)at, len) != 0) {
        return luaL_error(L, "the libc would not take the pak");
    }

    started_at = kosmos_ticks();

    if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) {
        counter_hz = info.counter_hz;
    }

    if (make_engine_stack() != 0) {
        return luaL_error(L, "no room for the engine's %d KB stack",
                          ENGINE_STACK_PAGES * 4);
    }

    if (kosmos_exit_arm() != 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "quake stopped during startup");
        return 2;
    }

    kosmos_call_on_stack(engine_init, NULL, engine_stack_top);

    kosmos_exit_disarm();
    running = 1;

    lua_pushboolean(L, 1);
    return 1;
}

/*
 * The frame Quake left in `screen`, twice the size, through the palette.
 *
 * Row by row through the surface's own pitch (`gfx.md` 19.3), and each
 * source row written once and copied to the row under it.
 */
static void spread(uint32_t *dst, unsigned w, unsigned h, unsigned pitch)
{
    unsigned rows = (h < QUAKE_H * SCALE) ? h : QUAKE_H * SCALE;
    unsigned cols = (w < QUAKE_W * SCALE) ? w : QUAKE_W * SCALE;
    unsigned y;

    for (y = 0; y < rows; y += SCALE) {
        const byte *src = screen + (y / SCALE) * QUAKE_W;
        uint32_t *out = (uint32_t *)((uint8_t *)dst + (size_t)y * pitch);
        unsigned x;

        for (x = 0; x < cols; x++) {
            out[x] = colours[src[x / SCALE]];
        }

        if (y + 1 < rows) {
            memcpy((uint8_t *)dst + (size_t)(y + 1) * pitch, out,
                   (size_t)cols * sizeof *out);
        }
    }
}

/*
 * quake.frame(surface, seconds) - one `Host_Frame`, and the picture if it
 * drew one.
 *
 * Returns `true, drew` while the game runs, and `false, why` once `exit` has
 * landed - "quit" for the menu's quit, anything else for an error, whose
 * words are in the log.
 */
static int l_frame(lua_State *L)
{
    unsigned w = 0, h = 0, pitch = 0;
    uint32_t *dst = kosmos_surface_pixels(L, 1, &w, &h, &pitch);
    double seconds = luaL_checknumber(L, 2);
    int landed;

    if (!running) {
        return luaL_error(L, "quake has not been started");
    }

    drawn = false;
    empty_asks = 0;

    landed = kosmos_exit_arm();

    if (landed != 0) {
        running = 0;
        lua_pushboolean(L, 0);
        lua_pushstring(L, (landed == 1) ? "quit"
                                        : "stopped with an error, above");
        return 2;
    }

    kosmos_call_on_stack(engine_frame, &seconds, engine_stack_top);

    kosmos_exit_disarm();

    if (drawn && dst != NULL) {
        spread(dst, w, h, pitch);
    }

    lua_pushboolean(L, 1);
    lua_pushboolean(L, drawn);
    return 2;
}

/* quake.key(key, down) - one transition, in Quake's own key numbering. */
static int l_key(lua_State *L)
{
    unsigned next = (keyq_head + 1) % KEYQ;

    keyq[keyq_head].key = (i32)luaL_checkinteger(L, 1);
    keyq[keyq_head].down = lua_toboolean(L, 2) ? true : false;
    keyq_head = next;

    if (keyq_head == keyq_tail) {
        keyq_tail = (keyq_tail + 1) % KEYQ;
    }

    return 0;
}

/* quake.mouse(dx, dy) - movement since the last report, added up. */
static int l_mouse(lua_State *L)
{
    mouse_dx += (int)luaL_checkinteger(L, 1);
    mouse_dy += (int)luaL_checkinteger(L, 2);

    return 0;
}

/* quake.log() - whatever Quake printed since the last call, or nil. */
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

static const luaL_Reg quake_lib[] = {
    { "start", l_start },
    { "frame", l_frame },
    { "key",   l_key },
    { "mouse", l_mouse },
    { "log",   l_log },
    { NULL, NULL },
};

/* The window's size rides along, so the Lua side does not carry the number. */
void kosmos_quake_open(lua_State *L)
{
    luaL_newlib(L, quake_lib);

    lua_pushinteger(L, QUAKE_W * SCALE);
    lua_setfield(L, -2, "width");

    lua_pushinteger(L, QUAKE_H * SCALE);
    lua_setfield(L, -2, "height");

    lua_setglobal(L, "quake");
}
