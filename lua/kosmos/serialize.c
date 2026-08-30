/*
 * Lua values, over a message.
 *
 * This is where `design.md` §1's thesis stops being an argument and becomes
 * code. The protocol between servers is the data model of the userland
 * language: a client sends a table, a server receives a table, and neither
 * writes a line of marshalling because this is the only marshalling there is.
 * There is no IDL, no generated stub, and no second type system to keep in
 * step with the first.
 *
 * The kernel does not participate. It copies bytes and has no opinion about
 * their shape, which is exactly what makes the thesis affordable: a protocol
 * the middle has to understand is a protocol the middle constrains.
 *
 * The format is deliberately dull. A type byte, then a payload, recursively.
 * Fixed-width integers rather than varints, because the wire is a copy inside
 * one machine and the bytes saved would buy nothing against the branches
 * spent saving them.
 *
 * It is not a general Lua serialiser and must not become one:
 *
 *   - **Functions, userdata and coroutines are refused.** Not because they
 *     are hard, but because a value that means something only inside the
 *     state that made it cannot cross to a state that did not. `design.md`
 *     §16.8's replicants send *source* and state, and that is the reason.
 *   - **Depth is bounded**, which also bounds cycles. A table containing
 *     itself is refused rather than recursed into, and the refusal is an
 *     error rather than a stack overflow.
 *   - **Metatables do not travel.** What arrives is data. A metatable is
 *     behaviour, and behaviour belongs to the state that defines it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#include "serialize.h"

/*
 * The type byte. Booleans get one each rather than a shared tag and a
 * payload, because it is the same byte either way and one fewer thing to
 * read.
 */
#define T_NIL       0
#define T_FALSE     1
#define T_TRUE      2
#define T_INT       3
#define T_FLOAT     4
#define T_STRING    5
#define T_TABLE     6
#define T_END       7   /* closes a table */

/*
 * How deep a value may nest.
 *
 * This is what stands in for cycle detection. A table that contains itself
 * recurses until it hits this and is refused, rather than running the kernel
 * stack out. Tracking visited tables would be more precise and would need a
 * set, which would need allocation, inside a path that must not fail for
 * want of memory.
 */
#define MAX_DEPTH   16

struct writer {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     overflow;
};

struct reader {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           underflow;
};

static void put(struct writer *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }

    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void put_byte(struct writer *w, uint8_t b)
{
    put(w, &b, 1);
}

static bool take(struct reader *r, void *out, size_t n)
{
    if (r->pos + n > r->len) {
        r->underflow = true;
        return false;
    }

    memcpy(out, r->buf + r->pos, n);
    r->pos += n;
    return true;
}

static int pack_value(lua_State *L, int index, struct writer *w, int depth);

static int pack_table(lua_State *L, int index, struct writer *w, int depth)
{
    /* Absolute, because the traversal below pushes and pops and a relative
     * index would move under it. */
    int t = lua_absindex(L, index);

    /*
     * Room for this level's key and value before taking any.
     *
     * Lua guarantees a C function only LUA_MINSTACK slots, which is twenty.
     * Each level of nesting holds two, so a value sixteen deep needs more
     * than that, and without asking, `lua_next` writes past the end of the
     * stack. It does not fail: it corrupts, and the crash arrives later
     * somewhere unrelated, which is exactly how this was found - a cyclic
     * table faulted on a garbage address rather than being refused at the
     * depth limit.
     */
    if (!lua_checkstack(L, 4)) {
        return SERIALIZE_ERR_DEPTH;
    }

    put_byte(w, T_TABLE);

    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        /* key at -2, value at -1 */
        int rc = pack_value(L, -2, w, depth + 1);

        if (rc != SERIALIZE_OK) {
            lua_pop(L, 2);
            return rc;
        }

        rc = pack_value(L, -1, w, depth + 1);
        if (rc != SERIALIZE_OK) {
            lua_pop(L, 2);
            return rc;
        }

        lua_pop(L, 1);      /* the value; the key stays for lua_next */
    }

    put_byte(w, T_END);
    return SERIALIZE_OK;
}

static int pack_value(lua_State *L, int index, struct writer *w, int depth)
{
    if (depth > MAX_DEPTH) {
        return SERIALIZE_ERR_DEPTH;
    }

    switch (lua_type(L, index)) {
    case LUA_TNIL:
        put_byte(w, T_NIL);
        break;

    case LUA_TBOOLEAN:
        put_byte(w, lua_toboolean(L, index) ? T_TRUE : T_FALSE);
        break;

    case LUA_TNUMBER:
        /*
         * Lua 5.4 distinguishes integers from floats, and the distinction
         * has to survive the crossing. A server that gets 1.0 where the
         * client sent 1 will index a table with the wrong key and find
         * nothing, which looks like a missing entry rather than a type
         * error.
         */
        if (lua_isinteger(L, index)) {
            lua_Integer n = lua_tointeger(L, index);
            int64_t v = (int64_t)n;
            put_byte(w, T_INT);
            put(w, &v, sizeof(v));
        } else {
            lua_Number n = lua_tonumber(L, index);
            double v = (double)n;
            put_byte(w, T_FLOAT);
            put(w, &v, sizeof(v));
        }
        break;

    case LUA_TSTRING: {
        size_t len;
        const char *str = lua_tolstring(L, index, &len);
        uint32_t n;

        if (len > 0xffffffffUL) {
            return SERIALIZE_ERR_TOO_BIG;
        }

        n = (uint32_t)len;
        put_byte(w, T_STRING);
        put(w, &n, sizeof(n));
        /* Length-prefixed, so a string containing a zero byte survives.
         * Lua strings are byte strings and truncating at a NUL would lose
         * data silently. */
        put(w, str, len);
        break;
    }

    case LUA_TTABLE:
        return pack_table(L, index, w, depth);

    default:
        /*
         * A function, a userdata, a coroutine, or a light userdata. None of
         * them mean anything in a state that did not create them, and
         * pretending otherwise is how a boundary stops being one.
         */
        return SERIALIZE_ERR_TYPE;
    }

    return (w->overflow) ? SERIALIZE_ERR_TOO_BIG : SERIALIZE_OK;
}

int serialize_pack(lua_State *L, int index, struct message *m)
{
    struct writer w = { m->data, MSG_BYTES, 0, false };
    int rc;

    m->length = 0;

    rc = pack_value(L, index, &w, 0);

    if (rc != SERIALIZE_OK) {
        return rc;
    }

    if (w.overflow) {
        return SERIALIZE_ERR_TOO_BIG;
    }

    m->length = (uint32_t)w.len;
    return SERIALIZE_OK;
}

static int unpack_value(lua_State *L, struct reader *r, int depth)
{
    uint8_t type;

    if (depth > MAX_DEPTH) {
        return SERIALIZE_ERR_DEPTH;
    }

    if (!take(r, &type, 1)) {
        return SERIALIZE_ERR_MALFORMED;
    }

    switch (type) {
    case T_NIL:
        lua_pushnil(L);
        break;

    case T_FALSE:
        lua_pushboolean(L, 0);
        break;

    case T_TRUE:
        lua_pushboolean(L, 1);
        break;

    case T_INT: {
        int64_t v;
        if (!take(r, &v, sizeof(v))) {
            return SERIALIZE_ERR_MALFORMED;
        }
        lua_pushinteger(L, (lua_Integer)v);
        break;
    }

    case T_FLOAT: {
        double v;
        if (!take(r, &v, sizeof(v))) {
            return SERIALIZE_ERR_MALFORMED;
        }
        lua_pushnumber(L, (lua_Number)v);
        break;
    }

    case T_STRING: {
        uint32_t n;

        if (!take(r, &n, sizeof(n))) {
            return SERIALIZE_ERR_MALFORMED;
        }

        /*
         * Checked against what is actually left rather than against the
         * buffer size. A length field is the first thing an attacker
         * changes, and this is the code that will one day read bytes that
         * came from somewhere untrusted.
         */
        if (r->pos + n > r->len) {
            return SERIALIZE_ERR_MALFORMED;
        }

        lua_pushlstring(L, (const char *)(r->buf + r->pos), n);
        r->pos += n;
        break;
    }

    case T_TABLE: {
        /* The same reason as in pack_table: each level of nesting holds a
         * key and a value while it builds. */
        if (!lua_checkstack(L, 4)) {
            return SERIALIZE_ERR_DEPTH;
        }

        lua_newtable(L);

        for (;;) {
            int rc;

            if (r->pos >= r->len) {
                return SERIALIZE_ERR_MALFORMED;     /* no T_END */
            }

            if (r->buf[r->pos] == T_END) {
                r->pos++;
                break;
            }

            rc = unpack_value(L, r, depth + 1);     /* key */
            if (rc != SERIALIZE_OK) {
                return rc;
            }

            rc = unpack_value(L, r, depth + 1);     /* value */
            if (rc != SERIALIZE_OK) {
                return rc;
            }

            /*
             * A nil key would raise from inside lua_settable, which is an
             * error thrown across a C boundary that did not ask for one.
             * Malformed input is a return value here.
             */
            if (lua_isnil(L, -2)) {
                return SERIALIZE_ERR_MALFORMED;
            }

            lua_settable(L, -3);
        }
        break;
    }

    default:
        return SERIALIZE_ERR_MALFORMED;
    }

    return SERIALIZE_OK;
}

int serialize_unpack(lua_State *L, const struct message *m)
{
    struct reader r = { m->data, m->length, 0, false };
    int top = lua_gettop(L);
    int rc;

    if (m->length > MSG_BYTES) {
        return SERIALIZE_ERR_MALFORMED;
    }

    rc = unpack_value(L, &r, 0);

    if (rc != SERIALIZE_OK) {
        /* Whatever was half-built is dropped, so a failed unpack leaves the
         * stack exactly as it was found. */
        lua_settop(L, top);
        return rc;
    }

    return SERIALIZE_OK;
}

const char *serialize_error(int rc)
{
    switch (rc) {
    case SERIALIZE_OK:            return "ok";
    case SERIALIZE_ERR_TOO_BIG:   return "value does not fit in a message";
    case SERIALIZE_ERR_DEPTH:     return "value nests too deeply, or is cyclic";
    case SERIALIZE_ERR_TYPE:      return "that type cannot cross a boundary";
    case SERIALIZE_ERR_MALFORMED: return "the message is not a value";
    default:                      return "unknown serialisation error";
    }
}
