/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The door to `webkit2test.cpp` (`docs/w7.md` W7a): a kit in no image but
 * `webkit2test.elf`, reached as `use("webkit2test.elf")`.
 * `load(html, surface, ms, "kill"?)` returns what happened, a table of
 * lines, the last `painted` when the page's picture is in the surface.
 */

#include "lua.h"
#include "lauxlib.h"

#include <string.h>

#include "webkit2test.h"
#include "kits/gfx/gfx_draw.h"      /* kosmos_surface_pixels */
#include "kits/kitenv.h"            /* kit_capability */

/* `posix_program.h`: where this process's standard error goes, and so
 * the web and network processes' it starts. */
void posix_console(long capability);
long posix_console_capability(void);

static void add_line(const char *line, void *ctx)
{
    lua_State *L = ctx;

    lua_pushstring(L, line);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
}

static int l_load(lua_State *L)
{
    const char *html = luaL_checkstring(L, 1);
    unsigned w, h, pitch;
    uint32_t *pixels = kosmos_surface_pixels(L, 2, &w, &h, &pitch);
    lua_Integer ms = luaL_checkinteger(L, 3);
    const char *mode = luaL_optstring(L, 4, "load");

    luaL_argcheck(L, ms > 0 && ms < 600000, 3, "milliseconds");
    luaL_argcheck(L, !strcmp(mode, "load") || !strcmp(mode, "kill"), 4, "\"load\" or \"kill\"");
    if (posix_console_capability() < 0) {
        posix_console(kit_capability(L, "/Devices/console"));
    }
    lua_newtable(L);
    (void)webkit2test_load(html, pixels, w, h, pitch, (unsigned)ms,
                           strcmp(mode, "kill") ? WEBKIT2TEST_LOAD : WEBKIT2TEST_KILL, add_line, L);
    return 1;
}

void kosmos_webkit2test_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_load);
    lua_setfield(L, -2, "load");
}
