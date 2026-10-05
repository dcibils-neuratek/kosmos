/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * MD4 (RFC 1320), for one thing: NTLM's password hash, the NT hash, which
 * is MD4 of the password in UTF-16LE (MS-NLMP 3.3.1, `NTOWFv1`). SMB's
 * sign-in stands on it (`docs/sharing.md`, N1).
 *
 * **MD4 is broken** as a hash and nothing here may use it as one. It is
 * written because a protocol that is not ours fixes it, and BearSSL - which
 * supplies every other primitive SMB needs - has none.
 *
 * One shot, because its one caller hashes a password and nothing larger.
 * Plain C rather than vector lanes: a password is one or two 64-byte
 * blocks and each round depends on the one before, so there is no loop
 * here a vector unit could take. Held to all seven of RFC 1320's A.5
 * vectors and to MS-NLMP's own NTOWFv1 example (`tools/test_crypto.c`).
 */

#include "crypto.h"

static uint32_t rol(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#define F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))

/* RFC 1320 3.4: three rounds of sixteen, the second adding 5A827999 and
 * the third 6ED9EBA1, each over the words in its own order. */
static void md4_block(uint32_t h[4], const uint8_t block[64])
{
    static const uint8_t r2[16] = { 0, 4, 8, 12, 1, 5, 9, 13,
                                    2, 6, 10, 14, 3, 7, 11, 15 };
    static const uint8_t r3[16] = { 0, 8, 4, 12, 2, 10, 6, 14,
                                    1, 9, 5, 13, 3, 11, 7, 15 };
    static const uint8_t s1[4] = { 3, 7, 11, 19 };
    static const uint8_t s2[4] = { 3, 5, 9, 13 };
    static const uint8_t s3[4] = { 3, 9, 11, 15 };
    uint32_t x[16], a = h[0], b = h[1], c = h[2], d = h[3], t;
    unsigned i;

    for (i = 0; i < 16; i++) x[i] = le32(block + 4 * i);

    /* Each step computes a new value for one of a, b, c, d in turn; the
     * rotation of the four names is done by moving the values instead. */
    for (i = 0; i < 16; i++) {
        t = rol(a + F(b, c, d) + x[i], s1[i & 3]);
        a = d; d = c; c = b; b = t;
    }
    for (i = 0; i < 16; i++) {
        t = rol(a + G(b, c, d) + x[r2[i]] + 0x5A827999u, s2[i & 3]);
        a = d; d = c; c = b; b = t;
    }
    for (i = 0; i < 16; i++) {
        t = rol(a + H(b, c, d) + x[r3[i]] + 0x6ED9EBA1u, s3[i & 3]);
        a = d; d = c; c = b; b = t;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

void crypto_md4(const void *data, size_t bytes, uint8_t out[16])
{
    uint32_t h[4] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u };
    const uint8_t *p = data;
    uint8_t last[128];
    uint64_t bits = (uint64_t)bytes * 8;
    size_t rest, tail, i;

    while (bytes >= 64) {
        md4_block(h, p);
        p += 64;
        bytes -= 64;
    }

    /* RFC 1320 3.1-3.2: a 1 bit, zeros to 56 bytes mod 64, and the length
     * in bits as 64 bits little-endian - one block or two. */
    rest = bytes;
    for (i = 0; i < rest; i++) last[i] = p[i];
    last[rest] = 0x80;
    tail = rest < 56 ? 64 : 128;
    for (i = rest + 1; i < tail - 8; i++) last[i] = 0;
    for (i = 0; i < 8; i++) last[tail - 8 + i] = (uint8_t)(bits >> (8 * i));

    md4_block(h, last);
    if (tail == 128) md4_block(h, last + 64);

    for (i = 0; i < 4; i++) {
        out[4 * i]     = (uint8_t)h[i];
        out[4 * i + 1] = (uint8_t)(h[i] >> 8);
        out[4 * i + 2] = (uint8_t)(h[i] >> 16);
        out[4 * i + 3] = (uint8_t)(h[i] >> 24);
    }
}
