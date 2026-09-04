/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Console Kit: the console's wire format, in the language that defines
 * it.
 *
 * `/dev/console` is the one protocol in this system with **two
 * implementations**, and that is what this kit exists for. Usually a server
 * is the only thing that answers its own protocol, so the shape can live in
 * the server's C and nobody else needs it. Not here: a terminal window
 * mounts *itself* as its child's `/dev/console`, so a program running in a
 * terminal prints to an application and cannot tell. That is the namespace
 * working exactly as intended - and it means the console ABI has an
 * application on one end of it.
 *
 * The alternative was `string.pack` in `terminal.lua` against a format
 * string copied from `conproto.h`, which is what the namespace does for
 * `/dev`, `/bin` and `/app`. It works, and it puts a second copy of the
 * layout in a second language, where nothing but an `assert` on the total
 * size notices when the two drift.
 *
 * So the layout is compiled once, here, against the header itself. The
 * namespace encodes requests with it, the console server and any terminal
 * decode them with it, and there is one definition rather than a definition
 * and a description of it.
 *
 * **A kit rather than a global**, so the rule the rest of the system runs on
 * still holds: `use("/kits/console")` comes through the namespace, and a
 * program that was not given `use` has no kits.
 */

#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "conproto.h"

/*
 * Reading a field out of a Lua table, with a default.
 *
 * `luaL_optinteger` on a field would need the value on the stack first, and
 * every one of these is the same four lines otherwise.
 */
static uint32_t field_u32(lua_State *L, int idx, const char *name,
                          uint32_t fallback)
{
    uint32_t out = fallback;

    lua_getfield(L, idx, name);

    if (lua_isnumber(L, -1)) {
        lua_Integer v = lua_tointeger(L, -1);

        out = (v < 0) ? 0u : (uint32_t)v;
    } else if (lua_isboolean(L, -1)) {
        out = lua_toboolean(L, -1) ? 1u : 0u;
    }

    lua_pop(L, 1);
    return out;
}

/*
 * Copying a string into a fixed field, truncated rather than refused.
 *
 * Truncation is the right answer for `write`, which is a stream: the caller
 * splits what does not fit and sends the rest, and `ns.write` already does.
 * Returning the length written is what lets it.
 */
static uint32_t copy_text(char *dst, size_t cap, const char *src, size_t n)
{
    if (n > cap) {
        n = cap;
    }

    memcpy(dst, src, n);
    return (uint32_t)n;
}

/*------------------------------------------------------------------------
 * The client's half: a request out, a reply in.
 *----------------------------------------------------------------------*/

static int l_encode_request(lua_State *L)
{
    struct con_request req;
    const char *text = NULL;
    size_t n = 0;

    luaL_checktype(L, 1, LUA_TTABLE);
    memset(&req, 0, sizeof(req));

    req.op    = field_u32(L, 1, "op", 0);
    req.ticks = field_u32(L, 1, "ticks", 0);

    lua_getfield(L, 1, "text");

    if (lua_isstring(L, -1)) {
        text = lua_tolstring(L, -1, &n);
        req.length = copy_text(req.text, CON_TEXT_MAX, text, n);
    }

    lua_pop(L, 1);

    lua_pushlstring(L, (const char *)&req, sizeof(req));

    /* How much of the string was taken, so a caller splitting a long write
     * knows where to carry on from. */
    lua_pushinteger(L, (lua_Integer)req.length);
    return 2;
}

static int l_decode_reply(lua_State *L)
{
    size_t n = 0;
    const char *bytes = luaL_checklstring(L, 1, &n);
    struct con_reply rep;
    uint32_t i;

    if (n < sizeof(rep)) {
        lua_pushnil(L);
        lua_pushliteral(L, "a console reply of the wrong size");
        return 2;
    }

    memcpy(&rep, bytes, sizeof(rep));

    lua_createtable(L, 0, 8);

    lua_pushinteger(L, (lua_Integer)rep.error);
    lua_setfield(L, -2, "error");

    lua_pushboolean(L, rep.seen != 0);
    lua_setfield(L, -2, "seen");

    /* Clamped on the way out as well as on the way in. The struct arrived
     * from another process, and a length longer than the field it describes
     * is exactly the thing a declared shape is here to stop. */
    lua_pushlstring(L, rep.line,
                    (rep.length > CON_TEXT_MAX) ? CON_TEXT_MAX : rep.length);
    lua_setfield(L, -2, "line");

    /* The bytes typed, as an array of numbers - which is what the line
     * editor and the window manager both already expect. */
    lua_createtable(L, (int)((rep.nkeys > CON_KEYS_MAX)
                             ? CON_KEYS_MAX : rep.nkeys), 0);

    for (i = 0; i < rep.nkeys && i < CON_KEYS_MAX; i++) {
        lua_pushinteger(L, (lua_Integer)rep.keys[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "keys");

    /* And the transitions, which are a different question from the bytes:
     * `keys` is what they meant, this is which key moved. */
    lua_createtable(L, (int)((rep.nevents > CON_EVENTS_MAX)
                             ? CON_EVENTS_MAX : rep.nevents), 0);

    for (i = 0; i < rep.nevents && i < CON_EVENTS_MAX; i++) {
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, (lua_Integer)rep.events[i].code);
        lua_setfield(L, -2, "code");
        lua_pushboolean(L, rep.events[i].down != 0);
        lua_setfield(L, -2, "down");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    lua_setfield(L, -2, "events");

    lua_createtable(L, 0, 8);
    lua_pushinteger(L, (lua_Integer)rep.x);       lua_setfield(L, -2, "x");
    lua_pushinteger(L, (lua_Integer)rep.y);       lua_setfield(L, -2, "y");
    lua_pushinteger(L, (lua_Integer)rep.min_x);   lua_setfield(L, -2, "min_x");
    lua_pushinteger(L, (lua_Integer)rep.max_x);   lua_setfield(L, -2, "max_x");
    lua_pushinteger(L, (lua_Integer)rep.min_y);   lua_setfield(L, -2, "min_y");
    lua_pushinteger(L, (lua_Integer)rep.max_y);   lua_setfield(L, -2, "max_y");
    lua_pushinteger(L, (lua_Integer)rep.buttons); lua_setfield(L, -2, "buttons");
    lua_pushboolean(L, rep.moved != 0);           lua_setfield(L, -2, "moved");
    lua_setfield(L, -2, "pointer");

    lua_createtable(L, 0, 4);
    lua_pushinteger(L, (lua_Integer)rep.bytes);      lua_setfield(L, -2, "bytes");
    lua_pushinteger(L, (lua_Integer)rep.lines);      lua_setfield(L, -2, "lines");
    lua_pushinteger(L, (lua_Integer)rep.interrupts); lua_setfield(L, -2, "interrupts");
    lua_pushinteger(L, (lua_Integer)rep.reloads);    lua_setfield(L, -2, "reloads");
    lua_setfield(L, -2, "stat");

    return 1;
}

/*------------------------------------------------------------------------
 * The server's half: a request in, a reply out.
 *
 * This is the side a terminal uses, and it is why this is a kit at all.
 *----------------------------------------------------------------------*/

static int l_decode_request(lua_State *L)
{
    size_t n = 0;
    const char *bytes = luaL_checklstring(L, 1, &n);
    struct con_request req;

    if (n < sizeof(req)) {
        lua_pushnil(L);
        lua_pushliteral(L, "a console request of the wrong size");
        return 2;
    }

    memcpy(&req, bytes, sizeof(req));

    lua_createtable(L, 0, 3);

    lua_pushinteger(L, (lua_Integer)req.op);
    lua_setfield(L, -2, "op");

    lua_pushinteger(L, (lua_Integer)req.ticks);
    lua_setfield(L, -2, "ticks");

    /* Clamped: `length` is a number another process chose. */
    lua_pushlstring(L, req.text,
                    (req.length > CON_TEXT_MAX) ? CON_TEXT_MAX : req.length);
    lua_setfield(L, -2, "text");

    return 1;
}

static int l_encode_reply(lua_State *L)
{
    struct con_reply rep;

    luaL_checktype(L, 1, LUA_TTABLE);
    memset(&rep, 0, sizeof(rep));

    rep.error = field_u32(L, 1, "error", CON_OK);
    rep.seen  = field_u32(L, 1, "seen", 0);

    lua_getfield(L, 1, "line");

    if (lua_isstring(L, -1)) {
        size_t n = 0;
        const char *s = lua_tolstring(L, -1, &n);

        rep.length = copy_text(rep.line, CON_TEXT_MAX, s, n);
    }

    lua_pop(L, 1);

    /* The bytes typed. An absent or empty table is none, which is what a
     * terminal answering `keys` says. */
    lua_getfield(L, 1, "keys");

    if (lua_istable(L, -1)) {
        lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
        lua_Integer i;

        for (i = 1; i <= n && rep.nkeys < CON_KEYS_MAX; i++) {
            lua_rawgeti(L, -1, i);

            if (lua_isnumber(L, -1)) {
                rep.keys[rep.nkeys++] = (uint8_t)lua_tointeger(L, -1);
            }

            lua_pop(L, 1);
        }
    }

    lua_pop(L, 1);

    lua_getfield(L, 1, "events");

    if (lua_istable(L, -1)) {
        lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
        lua_Integer i;

        for (i = 1; i <= n && rep.nevents < CON_EVENTS_MAX; i++) {
            lua_rawgeti(L, -1, i);

            if (lua_istable(L, -1)) {
                struct con_key *k = &rep.events[rep.nevents++];

                k->code = field_u32(L, -1, "code", 0);
                k->down = field_u32(L, -1, "down", 0);
            }

            lua_pop(L, 1);
        }
    }

    lua_pop(L, 1);

    lua_getfield(L, 1, "pointer");

    if (lua_istable(L, -1)) {
        rep.x       = field_u32(L, -1, "x", 0);
        rep.y       = field_u32(L, -1, "y", 0);
        rep.min_x   = field_u32(L, -1, "min_x", 0);
        rep.max_x   = field_u32(L, -1, "max_x", 0);
        rep.min_y   = field_u32(L, -1, "min_y", 0);
        rep.max_y   = field_u32(L, -1, "max_y", 0);
        rep.buttons = field_u32(L, -1, "buttons", 0);
        rep.moved   = field_u32(L, -1, "moved", 0);
    }

    lua_pop(L, 1);

    lua_getfield(L, 1, "stat");

    if (lua_istable(L, -1)) {
        rep.bytes      = field_u32(L, -1, "bytes", 0);
        rep.lines      = field_u32(L, -1, "lines", 0);
        rep.interrupts = field_u32(L, -1, "interrupts", 0);
        rep.reloads    = field_u32(L, -1, "reloads", 0);
    }

    lua_pop(L, 1);

    lua_pushlstring(L, (const char *)&rep, sizeof(rep));
    return 1;
}

/*
 * The sentence for a number.
 *
 * The protocol carries the number and this composes the sentence, which is
 * `CLAUDE.md`'s division: a server says which of a fixed set of things went
 * wrong, and whoever puts it in front of a person decides how to say it.
 * Here rather than in each caller so that two callers do not word the same
 * failure differently.
 */
static int l_message(lua_State *L)
{
    lua_Integer code = luaL_checkinteger(L, 1);
    const char *s;

    switch (code) {
    case CON_OK:             s = "no error"; break;
    case CON_ERR_BUSY:       s = "the console already has a reader"; break;
    case CON_ERR_NO_POINTER: s = "this machine has no pointer"; break;
    case CON_ERR_BAD_OP:     s = "the console did not understand that"; break;
    case CON_ERR_NO_READER:  s = "this cannot be read from"; break;
    default:                 s = "console error"; break;
    }

    lua_pushstring(L, s);
    return 1;
}

static void set_int(lua_State *L, const char *name, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

void kosmos_console_kit(lua_State *L)
{
    static const luaL_Reg api[] = {
        { "encode_request", l_encode_request },
        { "decode_request", l_decode_request },
        { "encode_reply",   l_encode_reply },
        { "decode_reply",   l_decode_reply },
        { "message",        l_message },
        { NULL, NULL }
    };

    luaL_newlib(L, api);

    /* The operations, by the names the namespace already uses for them, so
     * a caller writes `con.WRITE` rather than remembering that it is 1. */
    set_int(L, "WRITE",   CON_OP_WRITE);
    set_int(L, "READ",    CON_OP_READ);
    set_int(L, "KEYS",    CON_OP_KEYS);
    set_int(L, "WAIT",    CON_OP_WAIT);
    set_int(L, "POINTER", CON_OP_POINTER);
    set_int(L, "POLL",    CON_OP_POLL);
    set_int(L, "STAT",    CON_OP_STAT);

    set_int(L, "OK",             CON_OK);
    set_int(L, "ERR_BUSY",       CON_ERR_BUSY);
    set_int(L, "ERR_NO_POINTER", CON_ERR_NO_POINTER);
    set_int(L, "ERR_BAD_OP",     CON_ERR_BAD_OP);
    set_int(L, "ERR_NO_READER",  CON_ERR_NO_READER);

    /* What a `write` may carry, so a caller can split to it rather than
     * discovering the limit by having its text truncated. */
    set_int(L, "TEXT_MAX", CON_TEXT_MAX);
}
