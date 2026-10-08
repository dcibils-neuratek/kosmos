/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Base64 for C, both ways (`base64_core.h`). Moved out of `base64.c`, the
 * Lua door, on 8 October so the Mail Kit's parts are undone by the same
 * code as `compress.unbase64` - one door, as the kit's own comment has it.
 */

#include "base64_core.h"

static const char digits[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t i, o = 0;

    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];

        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = digits[(v >> 6) & 63];
        out[o++] = digits[v & 63];
    }

    if (n - i == 1) {
        uint32_t v = (uint32_t)in[i] << 16;

        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8;

        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = digits[(v >> 6) & 63];
        out[o++] = '=';
    }

    return o;
}

size_t base64_decode(const char *in, size_t n, uint8_t *out, size_t room, int lines,
                     size_t *bad)
{
    size_t i, o = 0;
    uint32_t acc = 0;
    int bits = 0;

    *bad = n;

    for (i = 0; i < n; i++) {
        unsigned c = (unsigned char)in[i], v;

        if (c >= 'A' && c <= 'Z') {
            v = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            v = c - 'a' + 26;
        } else if (c >= '0' && c <= '9') {
            v = c - '0' + 52;
        } else if (c == '+') {
            v = 62;
        } else if (c == '/') {
            v = 63;
        } else if (c == '=') {
            break;
        } else if (lines && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            continue;
        } else {
            *bad = i;
            break;
        }

        acc = (acc << 6) | v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;

            if (o < room) out[o] = (uint8_t)((acc >> bits) & 0xff);
            o++;
        }
    }

    return o < room ? o : room;
}
