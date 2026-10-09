/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The Mail Kit** - `use("/Kosmos/Kits/mail")` (`docs/mail.md` M1) - a
 * message read, for Mail and for anything else that is handed one: a
 * `.eml` opened in Tracker, a message attached to a message.
 *
 *   local mail = use("/Kosmos/Kits/mail")
 *   local m, why = mail.parse(at, bytes)  a message in a region, which the
 *                                         caller keeps while `m` is used
 *   local m, why = mail.parse(text)       or in a string, which `m` keeps
 *   m:header(name[, id])                  the field, unfolded, its encoded
 *                                         words decoded, UTF-8; nil for none
 *   m:addresses(name)                     { { name =, address = }, ... }
 *   m:date()                              seconds since 1970, UTC; nil
 *   m:parts()                             { { id =, parent =, type =,
 *                                         charset =, encoding =, name =,
 *                                         disposition =, cid =, multipart =,
 *                                         bytes =, bound = }, ... }
 *   m:part_into(id, at, cap)              bytes: the part's content at `at`,
 *                                         its transfer encoding undone and a
 *                                         text part in UTF-8; nil and the
 *                                         room it needs when `cap` is less
 *   m:preview([bytes])                    the first words of its text
 *
 *   mail.build(t, at, cap)                a message written (M6) into a
 *                                         region: its length, or nil and the
 *                                         room it needs. `t` = { from =, to =,
 *                                         cc =, subject =, text =, date =,
 *                                         zone =, id =, in_reply_to =,
 *                                         references =, attachments = };
 *                                         from, and each of to and cc's, an
 *                                         address as { name =, address = }
 *                                         or as text; each attachment
 *                                         { name =, type =, at =, bytes = },
 *                                         its bytes in a region the caller
 *                                         keeps while this runs (M7)
 *   mail.encode_words(text)               a header's words, encoded if need be
 *
 * A part's `id` is its place in `parts()`, 1 for the message itself. **Its
 * content never passes through a Lua string here**: it goes into a region,
 * and from there to the Web Kit, to a file, or - text for a page of words -
 * to `sys.region_read`. The byte work is `mime.c` and `charset.c`; this is
 * only the door.
 *
 * Addresses come from `regions.make`, as `deflate_into`'s do, and a wrong
 * one faults this process and nothing else.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "mime.h"

#define MESSAGE "kosmos.mail.message"

static struct mime_msg *check_msg(lua_State *L)
{
    return luaL_checkudata(L, 1, MESSAGE);
}

/* A part's id from Lua, 1 for the message, as an index; an error if none. */
static size_t check_part(lua_State *L, const struct mime_msg *m, int arg, lua_Integer dflt)
{
    lua_Integer id = luaL_optinteger(L, arg, dflt);

    luaL_argcheck(L, id >= 1 && (size_t)id <= m->nparts, arg, "no such part");
    return (size_t)(id - 1);
}

/*
 * `mail.parse(at, bytes)` or `mail.parse(text)`. A string is kept in the
 * message's user value, so it lives as long as the message pointing into it.
 */
static int l_parse(lua_State *L)
{
    const uint8_t *buf;
    size_t len;
    struct mime_msg *m;

    if (lua_type(L, 1) == LUA_TSTRING) {
        buf = (const uint8_t *)lua_tolstring(L, 1, &len);
    } else {
        buf = (const uint8_t *)(uintptr_t)luaL_checkinteger(L, 1);
        len = (size_t)luaL_checkinteger(L, 2);

        if (buf == NULL && len > 0) return luaL_error(L, "parse: needs a mapped region");
    }

    m = lua_newuserdatauv(L, sizeof *m, 1);

    if (mime_parse(m, buf, len) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, "that is not a message: it has no header");
        return 2;
    }

    if (lua_type(L, 1) == LUA_TSTRING) {
        lua_pushvalue(L, 1);
        lua_setiuservalue(L, -2, 1);
    }

    luaL_setmetatable(L, MESSAGE);
    return 1;
}

static int l_header(lua_State *L)
{
    struct mime_msg *m = check_msg(L);
    const char *name = luaL_checkstring(L, 2);
    size_t p = check_part(L, m, 3, 1);
    char out[2048];
    long n = mime_header(m, p, name, out, sizeof out);

    if (n < 0) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, out, (size_t)n);
    }

    return 1;
}

static int l_addresses(lua_State *L)
{
    struct mime_msg *m = check_msg(L);
    const char *name = luaL_checkstring(L, 2);
    static char value[8192];
    static struct mime_address a[64];
    size_t n;

    lua_newtable(L);

    if (mime_header(m, 0, name, value, sizeof value) < 0) return 1;

    n = mime_addresses(value, a, sizeof a / sizeof a[0]);

    for (size_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, a[i].name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, a[i].address);
        lua_setfield(L, -2, "address");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    return 1;
}

static int l_date(lua_State *L)
{
    struct mime_msg *m = check_msg(L);
    char value[256];
    int ok = 0;
    int64_t t;

    if (mime_header(m, 0, "date", value, sizeof value) < 0) {
        lua_pushnil(L);
        return 1;
    }

    t = mime_date(value, &ok);

    if (ok) {
        lua_pushinteger(L, (lua_Integer)t);
    } else {
        lua_pushnil(L);
    }

    return 1;
}

static void set_string(lua_State *L, const char *key, const char *value)
{
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
}

static int l_parts(lua_State *L)
{
    struct mime_msg *m = check_msg(L);

    lua_createtable(L, (int)m->nparts, 0);

    for (size_t i = 0; i < m->nparts; i++) {
        const struct mime_part *p = &m->part[i];

        lua_createtable(L, 0, 11);
        lua_pushinteger(L, (lua_Integer)i + 1);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, p->parent < 0 ? 0 : (lua_Integer)p->parent + 1);
        lua_setfield(L, -2, "parent");
        set_string(L, "type", p->type);
        set_string(L, "charset", p->charset);
        set_string(L, "encoding", p->encoding);
        set_string(L, "name", p->name);
        set_string(L, "disposition", p->disposition);
        set_string(L, "cid", p->cid);
        lua_pushboolean(L, p->multipart);
        lua_setfield(L, -2, "multipart");
        lua_pushinteger(L, (lua_Integer)p->body_len);
        lua_setfield(L, -2, "bytes");
        lua_pushinteger(L, (lua_Integer)mime_part_bound(m, i));
        lua_setfield(L, -2, "bound");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }

    return 1;
}

static int l_part_into(lua_State *L)
{
    struct mime_msg *m = check_msg(L);
    size_t p = check_part(L, m, 2, 1);
    uint8_t *at = (uint8_t *)(uintptr_t)luaL_checkinteger(L, 3);
    size_t cap = (size_t)luaL_checkinteger(L, 4);
    size_t bound = mime_part_bound(m, p);

    if (at == NULL) return luaL_error(L, "part_into: needs a mapped region");

    /* Short of the bound, a text part would come back cut: say how much. */
    if (cap < bound) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)bound);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)mime_part_bytes(m, p, at, cap));
    return 1;
}

static int l_preview(lua_State *L)
{
    struct mime_msg *m = check_msg(L);
    lua_Integer want = luaL_optinteger(L, 2, 200);
    char out[1024];
    size_t room = want < 1 ? 1 : (size_t)want + 1;
    size_t n;

    if (room > sizeof out) room = sizeof out;

    n = mime_preview(m, out, room);
    lua_pushlstring(L, out, n);
    return 1;
}

/*
 * Addresses from Lua: a list of { name =, address = } or strings, each
 * string read as a field's value is ("Lena <lena@example.com>, bob@x"), so
 * what a person typed goes in as typed.
 */
static size_t take_addresses(lua_State *L, int t, const char *key, struct mime_address *a, size_t most)
{
    size_t n = 0;

    if (lua_getfield(L, t, key) == LUA_TNIL) {
        lua_pop(L, 1);
        return 0;
    }

    if (lua_type(L, -1) == LUA_TSTRING) {
        n = mime_addresses(lua_tostring(L, -1), a, most);
        lua_pop(L, 1);
        return n;
    }

    luaL_argcheck(L, lua_istable(L, -1), 1, "an address list is a table");

    /* One address, { name =, address = }, rather than a list of them. */
    if (lua_getfield(L, -1, "address") == LUA_TSTRING && most > 0) {
        memset(&a[0], 0, sizeof a[0]);
        strncpy(a[0].address, lua_tostring(L, -1), sizeof a[0].address - 1);
        lua_getfield(L, -2, "name");
        strncpy(a[0].name, luaL_optstring(L, -1, ""), sizeof a[0].name - 1);
        lua_pop(L, 3);
        return a[0].address[0] ? 1 : 0;
    }

    lua_pop(L, 1);

    for (lua_Integer i = 1; n < most; i++) {
        int kind = lua_rawgeti(L, -1, i);

        if (kind == LUA_TNIL) {
            lua_pop(L, 1);
            break;
        }

        if (kind == LUA_TSTRING) {
            n += mime_addresses(lua_tostring(L, -1), a + n, most - n);
        } else if (kind == LUA_TTABLE) {
            memset(&a[n], 0, sizeof a[n]);
            lua_getfield(L, -1, "name");
            strncpy(a[n].name, luaL_optstring(L, -1, ""), sizeof a[n].name - 1);
            lua_pop(L, 1);
            lua_getfield(L, -1, "address");
            strncpy(a[n].address, luaL_checkstring(L, -1), sizeof a[n].address - 1);
            lua_pop(L, 1);
            if (a[n].address[0]) n++;
        }

        lua_pop(L, 1);
    }

    lua_pop(L, 1);
    return n;
}

static const char *opt_field(lua_State *L, int t, const char *key, size_t *len)
{
    const char *s;

    lua_getfield(L, t, key);
    s = luaL_optlstring(L, -1, "", len);
    lua_pop(L, 1);          /* `t` holds the string, so it outlives the pop */
    return s;
}

static int l_build(lua_State *L)
{
    static struct mime_address to[64], cc[64], from[1];
    struct mail_draft d;
    uint8_t *at = (uint8_t *)(uintptr_t)luaL_checkinteger(L, 2);
    size_t cap = (size_t)luaL_checkinteger(L, 3);
    size_t n;

    luaL_checktype(L, 1, LUA_TTABLE);
    if (at == NULL) return luaL_error(L, "build: needs a mapped region");

    memset(&d, 0, sizeof d);
    luaL_argcheck(L, take_addresses(L, 1, "from", from, 1) == 1, 1, "a message needs whom it is from");

    d.from_name = from[0].name;
    d.from_address = from[0].address;
    d.nto = take_addresses(L, 1, "to", to, 64);
    d.ncc = take_addresses(L, 1, "cc", cc, 64);
    d.to = to;
    d.cc = cc;
    d.subject = opt_field(L, 1, "subject", NULL);
    d.text = (const uint8_t *)opt_field(L, 1, "text", &d.text_len);
    d.message_id = opt_field(L, 1, "id", NULL);
    d.in_reply_to = opt_field(L, 1, "in_reply_to", NULL);
    d.references = opt_field(L, 1, "references", NULL);
    luaL_argcheck(L, d.message_id[0], 1, "a message needs its id");

    /* Files: up to 32, each in a region whose address and size are given. */
    {
        static struct mail_attachment att[32];
        size_t n = 0;

        if (lua_getfield(L, 1, "attachments") == LUA_TTABLE) {
            for (lua_Integer i = 1; n < 32; i++) {
                if (lua_rawgeti(L, -1, i) != LUA_TTABLE) {
                    lua_pop(L, 1);
                    break;
                }

                lua_getfield(L, -1, "name");
                att[n].name = luaL_optstring(L, -1, "");
                lua_getfield(L, -2, "type");
                att[n].type = luaL_optstring(L, -1, "");
                lua_getfield(L, -3, "at");
                att[n].bytes = (const uint8_t *)(uintptr_t)luaL_checkinteger(L, -1);
                lua_getfield(L, -4, "bytes");
                att[n].len = (size_t)luaL_checkinteger(L, -1);
                lua_pop(L, 4);

                /* The strings stay alive: `t` holds the tables that hold them. */
                if (att[n].bytes == NULL && att[n].len > 0) return luaL_error(L, "build: a file needs a mapped region");

                n++;
                lua_pop(L, 1);
            }
        }

        lua_pop(L, 1);
        d.att = att;
        d.natt = n;
    }

    lua_getfield(L, 1, "date");
    d.date = (int64_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "zone");
    d.zone = (int)luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);

    n = mail_build(&d, at, cap);

    if (n > cap) {
        lua_pushnil(L);
        lua_pushinteger(L, (lua_Integer)n);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

static int l_encode_words(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    char out[2048];
    size_t n = mail_encode_words(text, out, sizeof out);

    lua_pushlstring(L, out, n < sizeof out ? n : sizeof out);
    return 1;
}

void kosmos_mail_kit(lua_State *L)
{
    static const luaL_Reg methods[] = {
        { "header",    l_header },
        { "addresses", l_addresses },
        { "date",      l_date },
        { "parts",     l_parts },
        { "part_into", l_part_into },
        { "preview",   l_preview },
        { NULL, NULL },
    };

    if (luaL_newmetatable(L, MESSAGE)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
    }

    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, l_parse);
    lua_setfield(L, -2, "parse");
    lua_pushcfunction(L, l_build);
    lua_setfield(L, -2, "build");
    lua_pushcfunction(L, l_encode_words);
    lua_setfield(L, -2, "encode_words");
}
