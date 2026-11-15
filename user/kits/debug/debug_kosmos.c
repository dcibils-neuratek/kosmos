/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Debug Kit's Lua door** (`docs/debug.md`):
 * `use("/Kosmos/Kits/debug")`.
 *
 *   debug.name(path, address)  -> name, offset | nil, why
 *   debug.forget()             -- the image's table given back
 *
 * The image is read through the Storage Kit, as any of this program's
 * files are. The last image asked about is kept open - about 4 MB for
 * JetSki - so a second address in it is a search and one small read,
 * until `forget`.
 *
 * Names are demangled where this image links a demangler: every C++ image
 * has libc++abi's `__cxa_demangle`, and the reference to it is weak, so
 * an image without one (the system's) gives the name as the file spells
 * it rather than carrying a second demangler.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "kosmos.h"
#include "storage.h"
#include "debug.h"

extern char *__cxa_demangle(const char *mangled, char *out, size_t *length,
                            int *status) __attribute__((weak));

static struct debug_image *open_image;
static char open_path[STORAGE_PATH_MAX];

static long storage_reader(void *ctx, uint64_t offset, void *into, size_t length)
{
    return storage_read(ctx, offset, into, length);
}

static int l_name(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    uint64_t addr = (uint64_t)luaL_checkinteger(L, 2), offset = 0;
    char name[2048];
    const char *why = NULL;

    if (strlen(path) >= sizeof open_path) {
        lua_pushnil(L);
        lua_pushliteral(L, "the path is too long");
        return 2;
    }
    if (!open_image || strcmp(open_path, path)) {
        debug_close(open_image);
        open_image = NULL;
        strcpy(open_path, path);
        open_image = debug_open(storage_reader, open_path, &why);
        if (!open_image) {
            open_path[0] = 0;
            lua_pushnil(L);
            lua_pushstring(L, why ? why : "no symbols");
            return 2;
        }
    }
    if (debug_name(open_image, addr, name, sizeof name, &offset)) {
        lua_pushnil(L);
        lua_pushliteral(L, "no function holds that address");
        return 2;
    }
    if (__cxa_demangle && name[0] == '_' && name[1] == 'Z') {
        int status = -1;
        char *words = __cxa_demangle(name, NULL, NULL, &status);
        if (words && status == 0) {
            lua_pushstring(L, words);
            free(words);
            lua_pushinteger(L, (lua_Integer)offset);
            return 2;
        }
        free(words);
    }
    lua_pushstring(L, name);
    lua_pushinteger(L, (lua_Integer)offset);
    return 2;
}

static int l_forget(lua_State *L)
{
    (void)L;
    debug_close(open_image);
    open_image = NULL;
    open_path[0] = 0;
    return 0;
}

void kosmos_debug_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_name);
    lua_setfield(L, -2, "name");
    lua_pushcfunction(L, l_forget);
    lua_setfield(L, -2, "forget");
}
