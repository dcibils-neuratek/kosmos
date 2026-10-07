/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The C Kit: TinyCC inside Kosmos (`docs/tinycc.md`, step C3).
 *
 *   local tcc = use("/Kosmos/Kits/tcc")
 *   local r = tcc.build{
 *     sources  = { "/Home/Projects/Primes/primes.c" },
 *     includes = { "/Home/Developer/include" },
 *     link     = { "/Home/Developer/head.o", "/Home/Developer/runtime.o",
 *                  "/Home/Developer/libgcc.a" },     -- head first
 *     prelude  = "/Home/Developer/include/kosmos_lua.h",
 *     base     = 0x80000000,              -- this machine's when left out
 *     reader   = function(path) return address, size end,  -- or nil
 *   }
 *   -- r.ok; r.problems = { { file, line, severity, text }, ... };
 *   -- r.image = { cap, at, size }, a region the caller writes and frees
 *
 * **Compiling and linking are TinyCC's, in this process**, a kit being
 * code you run. Files come through `reader` (`shim.c`): the Lua side maps
 * each into a region, so a header or the 21 MB runtime reaches TinyCC
 * without passing through the interpreter, and only what is included is
 * read. The image TinyCC writes is memory here until the build is done;
 * then Kosmos's header is stamped into it (`stamp.c`) and it is copied into
 * a region of its own for the Lua side to write through the namespace.
 *
 * **Problems are TinyCC's own words**, taken one at a time from its error
 * callback as TinyCC composed them - "file:line: error: text" - and split
 * there, never parsed back out of printed output.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "lua.h"
#include "lauxlib.h"

#include "libtcc.h"
#include "stamp.h"

void tcck_begin(lua_State *L, int reader_index);
void tcck_end(void);
unsigned char *tcck_output(size_t *len);

void kosmos_tcc_kit(lua_State *L);

struct problems {
    lua_State *L;
    int table;          /* stack index of the problems table */
    int count;
    int errors;
};

/* "file:line: error: text", or "file:line: warning: text", or a sentence. */
static void on_problem(void *opaque, const char *msg)
{
    struct problems *p = opaque;
    lua_State *L = p->L;
    const char *colon = strchr(msg, ':');
    const char *severity = "error", *text = msg;
    long line = 0;

    lua_createtable(L, 0, 4);

    if (colon != NULL) {
        char *after;

        line = strtol(colon + 1, &after, 10);

        if (after != colon + 1 && *after == ':') {
            lua_pushlstring(L, msg, (size_t)(colon - msg));
            lua_setfield(L, -2, "file");
            text = after + 1;

            while (*text == ' ') {
                text++;
            }

            if (strncmp(text, "warning: ", 9) == 0) {
                severity = "warning";
                text += 9;
            } else if (strncmp(text, "error: ", 7) == 0) {
                text += 7;
            }
        } else {
            line = 0;
        }
    }

    if (strncmp(text, "warning: ", 9) == 0) {
        severity = "warning";
        text += 9;
    } else if (strncmp(text, "error: ", 7) == 0) {
        text += 7;
    }

    lua_pushinteger(L, line);
    lua_setfield(L, -2, "line");
    lua_pushstring(L, severity);
    lua_setfield(L, -2, "severity");
    lua_pushstring(L, text);
    lua_setfield(L, -2, "text");
    lua_rawseti(L, p->table, ++p->count);

    if (strcmp(severity, "error") == 0) {
        p->errors++;
    }
}

/* A string field of the request, or NULL. */
static const char *field(lua_State *L, int t, const char *name)
{
    const char *s;

    lua_getfield(L, t, name);
    s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return s;
}

/* Each string of a list field, handed to `add`. */
static int each(lua_State *L, int t, const char *name, TCCState *s,
                int (*add)(TCCState *, const char *))
{
    int i, n, failed = 0;

    lua_getfield(L, t, name);
    n = lua_istable(L, -1) ? (int)lua_rawlen(L, -1) : 0;

    for (i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);

        if (lua_isstring(L, -1) && add(s, lua_tostring(L, -1)) < 0) {
            failed = 1;
        }

        lua_pop(L, 1);
    }

    lua_pop(L, 1);
    return failed;
}

/* The image, stamped, copied into a region of its own: { cap, at, size }. */
static int hand_over(lua_State *L, unsigned char *bytes, size_t len, uint64_t base,
                     const char **why)
{
    unsigned long pages = (len + 4095u) / 4096u;
    long cap, at;

    *why = tcc_stamp(bytes, len, base);

    if (*why != NULL) {
        return 0;
    }

    cap = kosmos_mem_create(pages ? pages : 1);
    if (cap < 0) {
        *why = "no memory for the image";
        return 0;
    }

    at = kosmos_mem_map(cap);
    if (at < 0) {
        kosmos_cap_drop(cap);
        *why = "the image's region would not map";
        return 0;
    }

    memcpy((void *)(uintptr_t)at, bytes, len);

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, cap);
    lua_setfield(L, -2, "cap");
    lua_pushinteger(L, at);
    lua_setfield(L, -2, "at");
    lua_pushinteger(L, (lua_Integer)len);
    lua_setfield(L, -2, "size");
    return 1;
}

static int l_build(lua_State *L)
{
    struct problems p;
    TCCState *s;
    const char *prelude, *why = NULL;
    char opt[64];
    uint64_t base;
    unsigned char *image;
    size_t len = 0;
    int ok;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "reader");
    luaL_argcheck(L, lua_isfunction(L, -1), 1, "a reader is wanted");
    /* Where an image starts: this machine's, as the kit was built for it,
     * unless the caller says otherwise. */
    lua_getfield(L, 1, "base");
    base = (uint64_t)luaL_optinteger(L, -1, (lua_Integer)KOSMOS_USER_BASE);
    lua_pop(L, 1);                              /* the reader stays, at -1 */

    lua_createtable(L, 0, 4);                   /* the result */
    lua_newtable(L);                            /* its problems */
    p.L = L;
    p.table = lua_gettop(L);
    p.count = 0;
    p.errors = 0;

    tcck_begin(L, p.table - 2);

    s = tcc_new();
    if (s == NULL) {
        tcck_end();
        return luaL_error(L, "tcc: no memory for a compiler");
    }

    tcc_set_error_func(s, &p, on_problem);
    tcc_set_options(s, "-nostdinc -nostdlib -static");
    snprintf(opt, sizeof opt, "-Wl,-Ttext=0x%llx", (unsigned long long)base);
    tcc_set_options(s, opt);
    tcc_set_output_type(s, TCC_OUTPUT_EXE);
    tcc_define_symbol(s, "KOSMOS_USER", "1");

    /* Where the image is mapped, as GCC's builds say it (`kosmos.h`): the
     * base this image is linked at. */
    {
        char at[32];

        snprintf(at, sizeof at, "0x%llx", (unsigned long long)base);
        tcc_define_symbol(s, "KOSMOS_USER_BASE", at);
    }

    each(L, 1, "includes", s, tcc_add_include_path);

    prelude = field(L, 1, "prelude");
    if (prelude != NULL) {
        char inc[600];

        snprintf(inc, sizeof inc, "-include %s", prelude);
        tcc_set_options(s, inc);
    }

    /* The header slot first, so `_start` lands at the base plus sixteen;
     * then the project's sources; then the runtime and libgcc. */
    lua_getfield(L, 1, "link");
    lua_rawgeti(L, -1, 1);
    ok = lua_isstring(L, -1) && tcc_add_file(s, lua_tostring(L, -1)) >= 0;
    lua_pop(L, 2);

    ok = ok && !each(L, 1, "sources", s, tcc_add_file);

    if (ok) {
        int i, n;

        lua_getfield(L, 1, "link");
        n = (int)lua_rawlen(L, -1);

        for (i = 2; i <= n && ok; i++) {
            lua_rawgeti(L, -1, i);
            ok = tcc_add_file(s, lua_tostring(L, -1)) >= 0;
            lua_pop(L, 1);
        }

        lua_pop(L, 1);
    }

    ok = ok && p.errors == 0 && tcc_output_file(s, "image.elf") >= 0 && p.errors == 0;
    tcc_delete(s);

    image = tcck_output(&len);
    tcck_end();

    if (ok && image != NULL && hand_over(L, image, len, base, &why)) {
        lua_setfield(L, -3, "image");
    } else {
        ok = 0;

        if (why != NULL) {
            on_problem(&p, why);
        }
    }

    free(image);

    lua_setfield(L, -2, "problems");            /* result.problems */
    lua_pushboolean(L, ok);
    lua_setfield(L, -2, "ok");
    return 1;
}

/* `tcc.release(image)`: the region `build` made, unmapped and let go. */
static int l_release(lua_State *L)
{
    lua_Integer cap, at, size;

    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "cap");
    lua_getfield(L, 1, "at");
    lua_getfield(L, 1, "size");
    cap = luaL_checkinteger(L, -3);
    at = luaL_checkinteger(L, -2);
    size = luaL_checkinteger(L, -1);

    kosmos_share_unmap((unsigned long)at, ((unsigned long)size + 4095u) / 4096u);
    kosmos_cap_drop((long)cap);
    return 0;
}

/*
 * `tcc.protostamp(at, size)`: the protocol stamp a runtime carries behind
 * its marker (`tools/protostamp.py`), or nil - so a pack from another build
 * is refused before it is linked, and its 21 MB are searched here rather
 * than in a Lua string.
 */
static int l_protostamp(lua_State *L)
{
    static const char mark[] = "KOSMOS-PROTOSTAMP:";
    const unsigned char *at = (const unsigned char *)(uintptr_t)luaL_checkinteger(L, 1);
    size_t size = (size_t)luaL_checkinteger(L, 2);
    size_t n = sizeof mark - 1, i;

    for (i = 0; size >= n + 16 && i <= size - n - 16; i++) {
        if (at[i] == 'K' && memcmp(at + i, mark, n) == 0) {
            lua_pushlstring(L, (const char *)at + i + n, 16);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

void kosmos_tcc_kit(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_build);
    lua_setfield(L, -2, "build");
    lua_pushcfunction(L, l_release);
    lua_setfield(L, -2, "release");
    lua_pushcfunction(L, l_protostamp);
    lua_setfield(L, -2, "protostamp");
    lua_pushstring(L, "0.9.28rc");      /* runtime/upstream/tinycc/VERSION */
    lua_setfield(L, -2, "version");
}
