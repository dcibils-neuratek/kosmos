/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The scanner for a PDF content stream: `sys.pdf_scan`.
 *
 * **Why this is in C, with the number that put it here.** A page of the
 * document this was written against is 13,884 bytes of content stream and
 * 3,657 tokens, and reading it in Lua took 527 ms - 56% of the second a
 * page cost. That is 144 microseconds a token, and it is not the parser
 * being clever: it is `string.sub` called once per byte, each call
 * allocating a one-character string for the collector to take away again.
 *
 * `design.md` 6 draws the line at loops over bytes and this is the
 * definition of one. `CLAUDE.md` says nothing moves to C without a profile
 * that justifies it, and `bench/` has the profile: `pdfbench` prints it,
 * phase by phase, and will say if this ever stops being worth it.
 *
 * **What stays in Lua, and why that is not a compromise.** This says where
 * one token ends and the next begins. It does not know what `Tj` means, or
 * that a text matrix exists, or that a page has lines on it. The
 * interpreter above it walks these tokens and makes every decision, and it
 * is the part that will change as more of PDF is supported - so it is the
 * part that has to stay reloadable. A scanner for a syntax frozen in 1993
 * has nothing to reload.
 *
 * **Batched, because a crossing is the other cost.** `gfx.md` 19.11 puts a
 * Lua/C crossing at about two thousand pixels of work; one per token would
 * trade a slow loop for a slow boundary. So a call scans many tokens and
 * returns them in two parallel arrays - what each one is, and what it says
 * - which is also why there is no table per token to allocate.
 */

#include <stddef.h>
#include <stdint.h>

#include "lua.h"
#include "lauxlib.h"

/* What a token is. The interpreter switches on these. */
#define TOK_NUMBER      1
#define TOK_NAME        2
#define TOK_STRING      3
#define TOK_OPERATOR    4
#define TOK_ARRAY_OPEN  5
#define TOK_ARRAY_CLOSE 6
#define TOK_DICT_OPEN   7
#define TOK_DICT_CLOSE  8

static int is_space(int c)
{
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static int is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
           c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Whitespace and comments; a comment runs to the end of its line. */
static size_t skip_space(const unsigned char *p, size_t at, size_t len)
{
    while (at < len) {
        if (is_space(p[at])) {
            at++;
        } else if (p[at] == '%') {
            while (at < len && p[at] != '\n' && p[at] != '\r') {
                at++;
            }
        } else {
            break;
        }
    }

    return at;
}

/*
 * A literal string, with its escapes resolved and its parentheses counted.
 * Built straight into a Lua buffer, so a string of any length costs one
 * allocation at the end rather than one per piece.
 */
static size_t literal(lua_State *L, const unsigned char *p,
                      size_t at, size_t len)
{
    luaL_Buffer b;
    int depth = 1;

    luaL_buffinit(L, &b);

    while (at < len && depth > 0) {
        unsigned char c = p[at++];

        if (c == '\\' && at < len) {
            unsigned char e = p[at++];

            switch (e) {
            case 'n': luaL_addchar(&b, '\n'); break;
            case 'r': luaL_addchar(&b, '\r'); break;
            case 't': luaL_addchar(&b, '\t'); break;
            case 'b': luaL_addchar(&b, '\b'); break;
            case 'f': luaL_addchar(&b, '\f'); break;
            case '\r':
                if (at < len && p[at] == '\n') at++;
                break;
            case '\n':
                break;
            default:
                if (e >= '0' && e <= '7') {
                    int value = e - '0';
                    int digits = 1;
                    while (at < len && digits < 3 &&
                           p[at] >= '0' && p[at] <= '7') {
                        value = value * 8 + (p[at++] - '0');
                        digits++;
                    }
                    luaL_addchar(&b, (char)(value & 0xff));
                } else {
                    luaL_addchar(&b, (char)e);
                }
            }
        } else if (c == '(') {
            depth++;
            luaL_addchar(&b, '(');
        } else if (c == ')') {
            depth--;
            if (depth > 0) luaL_addchar(&b, ')');
        } else {
            luaL_addchar(&b, (char)c);
        }
    }

    luaL_pushresult(&b);
    return at;
}

/* A hex string. An odd digit count pads with a zero, which real files rely on. */
static size_t hex_string(lua_State *L, const unsigned char *p,
                         size_t at, size_t len)
{
    luaL_Buffer b;
    int high = -1;

    luaL_buffinit(L, &b);

    while (at < len && p[at] != '>') {
        int v = hex_value(p[at++]);

        if (v < 0) {
            continue;
        }

        if (high < 0) {
            high = v;
        } else {
            luaL_addchar(&b, (char)((high << 4) | v));
            high = -1;
        }
    }

    if (at < len) at++;                       /* the '>' */
    if (high >= 0) luaL_addchar(&b, (char)(high << 4));

    luaL_pushresult(&b);
    return at;
}

/*
 * `sys.pdf_scan(address, length, offset, max) -> kinds, values, offset`
 *
 * Scans up to `max` tokens from `offset` and returns them as two arrays of
 * the same length - a kind per token and a value per token - plus where to
 * carry on from. An empty pair of arrays means the stream is finished.
 *
 * The address comes from `sys.memory_map`, so the bytes are read where the
 * inflater put them and no copy of the stream reaches the Lua heap.
 */
static int l_pdf_scan(lua_State *L)
{
    const unsigned char *p   = (const unsigned char *)(uintptr_t)
                               luaL_checkinteger(L, 1);
    size_t   len   = (size_t)luaL_checkinteger(L, 2);
    size_t   at    = (size_t)luaL_checkinteger(L, 3);
    lua_Integer max = luaL_optinteger(L, 4, 512);

    lua_Integer produced = 0;

    if (p == NULL) {
        return luaL_error(L, "pdf_scan: no region");
    }

    if (max <= 0 || max > 4096) {
        max = 512;
    }

    lua_newtable(L);                          /* kinds  */
    lua_newtable(L);                          /* values */

    while (produced < max) {
        int kind;
        unsigned char c;

        at = skip_space(p, at, len);
        if (at >= len) break;

        c = p[at];

        if (c == '[') {
            at++; kind = TOK_ARRAY_OPEN;  lua_pushboolean(L, 1);
        } else if (c == ']') {
            at++; kind = TOK_ARRAY_CLOSE; lua_pushboolean(L, 1);
        } else if (c == '<' && at + 1 < len && p[at + 1] == '<') {
            at += 2; kind = TOK_DICT_OPEN; lua_pushboolean(L, 1);
        } else if (c == '>' && at + 1 < len && p[at + 1] == '>') {
            at += 2; kind = TOK_DICT_CLOSE; lua_pushboolean(L, 1);
        } else if (c == '<') {
            at = hex_string(L, p, at + 1, len);
            kind = TOK_STRING;
        } else if (c == '(') {
            at = literal(L, p, at + 1, len);
            kind = TOK_STRING;
        } else if (c == '/') {
            size_t start = ++at;
            while (at < len && !is_space(p[at]) && !is_delim(p[at])) at++;
            lua_pushlstring(L, (const char *)p + start, at - start);
            kind = TOK_NAME;
        } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            /*
             * Converted here rather than handed back as text. `lua_stringtonumber`
             * on a copy would be an allocation per number, and a content
             * stream is mostly numbers - six of them before every `cm`.
             */
            /*
             * Every digit into an integer, then one division by a power of
             * ten. Not the obvious loop that multiplies a running scale by
             * 0.1 per digit: 0.1 is not representable, so that accumulates
             * error and `-2.25` comes out a few units in the last place away
             * from what `tonumber("-2.25")` gives. It prints the same and
             * compares unequal, which is the worst way for a number to be
             * wrong - and a content stream is mostly fractions like
             * `.23999999`, six of them before every `cm`.
             *
             * An integer mantissa is exact up to 2^53, a power of ten is
             * exact up to 10^22, and one division of one by the other is
             * correctly rounded. That is the same answer strtod gives.
             */
            static const double POW10[] = {
                1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
            };

            double value = 0.0;
            int    sign = 1, dot = 0, places = 0;

            if (c == '+' || c == '-') {
                if (c == '-') sign = -1;
                at++;
            }

            while (at < len) {
                if (p[at] >= '0' && p[at] <= '9') {
                    value = value * 10.0 + (p[at] - '0');
                    if (dot && places < 22) places++;
                    at++;
                } else if (p[at] == '.' && !dot) {
                    dot = 1;
                    at++;
                } else {
                    break;
                }
            }

            if (places > 0) value /= POW10[places];

            lua_pushnumber(L, (lua_Number)(sign * value));
            kind = TOK_NUMBER;
        } else if (is_delim(c)) {
            at++;                             /* stray ) or } - skip it */
            continue;
        } else {
            size_t start = at;
            while (at < len && !is_space(p[at]) && !is_delim(p[at])) at++;

            if (at == start) { at++; continue; }

            lua_pushlstring(L, (const char *)p + start, at - start);
            kind = TOK_OPERATOR;
        }

        produced++;
        lua_rawseti(L, -2, produced);         /* values[n] = the value */
        lua_pushinteger(L, kind);
        lua_rawseti(L, -3, produced);         /* kinds[n]  = the kind  */
    }

    lua_pushinteger(L, (lua_Integer)at);
    return 3;
}

/*
 * The PDF kit: `use("/kits/pdf")`.
 *
 * The scanner and the names for what it returns, and deliberately nothing
 * else: the object layer and the content interpreter above it are Lua, in
 * `/lib/pdf.lua` and `/lib/pdfpage.lua`, because they are where the
 * decisions live and where the changes will be. This half is a scanner for a
 * syntax that was frozen in 1993 and has nothing to reload.
 */
void kosmos_pdf_kit(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, l_pdf_scan);
    lua_setfield(L, -2, "scan");

    lua_pushinteger(L, TOK_NUMBER);      lua_setfield(L, -2, "NUMBER");
    lua_pushinteger(L, TOK_NAME);        lua_setfield(L, -2, "NAME");
    lua_pushinteger(L, TOK_STRING);      lua_setfield(L, -2, "STRING");
    lua_pushinteger(L, TOK_OPERATOR);    lua_setfield(L, -2, "OPERATOR");
    lua_pushinteger(L, TOK_ARRAY_OPEN);  lua_setfield(L, -2, "ARRAY_OPEN");
    lua_pushinteger(L, TOK_ARRAY_CLOSE); lua_setfield(L, -2, "ARRAY_CLOSE");
    lua_pushinteger(L, TOK_DICT_OPEN);   lua_setfield(L, -2, "DICT_OPEN");
    lua_pushinteger(L, TOK_DICT_CLOSE);  lua_setfield(L, -2, "DICT_CLOSE");
}
