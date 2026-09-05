/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The cryptography SSH needs, written here and checked against published
 * vectors.
 *
 * **This is the one place in the system where a bug is silent.** Everything
 * else fails loudly: a stack that gets a sequence number wrong stops
 * working, a filesystem that gets an offset wrong reads the wrong bytes and
 * somebody notices. A cipher that gets a counter wrong keeps working
 * perfectly and is not secure, and a signature check with a subtly wrong
 * field arithmetic accepts signatures it should refuse. Nothing about
 * *running* tells you.
 *
 * So the discipline here is different from the rest of the project, and it
 * is the only discipline that helps: **every primitive is checked against
 * the test vectors in its own specification**, byte for byte, and the check
 * is in `make test`. `cryptotest` is that, and it is not optional in the way
 * a benchmark is - a change here that passes the suite is a change that
 * still computes SHA-256, and a change that does not is a change that would
 * otherwise have been discovered by somebody's connection being readable.
 *
 * **Written from knowledge of the specifications** - FIPS 180-4, RFC 2104,
 * RFC 8439, RFC 7748, RFC 8032 - which is exactly why the vectors matter
 * more here than anywhere else in the tree.
 *
 * What is deliberately *not* here: anything constant-time beyond the obvious.
 * The field arithmetic below avoids branching on secret data, which is the
 * part that matters most, but this has not been audited for timing and this
 * machine has no cache-timing story at all. That is written down rather than
 * implied: this is a learning system on an emulator, and it should not be
 * the thing between you and somebody who wants your keys.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crypto.h"

/*------------------------------------------------------------------------
 * SHA-256. FIPS 180-4.
 *
 * The hash everything else here is built on: the exchange hash, the key
 * derivation, and HMAC. Sixty-four rounds over a sixteen-word schedule that
 * is extended to sixty-four, with eight working variables.
 *----------------------------------------------------------------------*/

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16)
             | ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }

    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);

        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void sha256_init(struct sha256 *s)
{
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;

    s->length = 0;
    s->held = 0;
}

void sha256_update(struct sha256 *s, const void *data, size_t bytes)
{
    const uint8_t *p = data;

    s->length += bytes;

    while (bytes > 0) {
        size_t take = 64u - s->held;

        if (take > bytes) {
            take = bytes;
        }

        memcpy(s->buffer + s->held, p, take);
        s->held += take;
        p += take;
        bytes -= take;

        if (s->held == 64u) {
            sha256_block(s, s->buffer);
            s->held = 0;
        }
    }
}

/*
 * The padding, and the one thing about it that catches people.
 *
 * A single 0x80 byte, then zeroes, then the length **in bits** as a 64-bit
 * big-endian number - and the length is of the message, not of the padded
 * block. A implementation that pads to a multiple of 64 and then finds it
 * has no room for the length has to emit *another* whole block, which is
 * the case that is easy to leave out and only shows on messages of exactly
 * the wrong length.
 */
void sha256_final(struct sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->length * 8u;
    uint8_t tail[8];
    unsigned i;
    static const uint8_t one = 0x80u;

    sha256_update(s, &one, 1);

    while (s->held != 56u) {
        static const uint8_t zero = 0;

        sha256_update(s, &zero, 1);
    }

    for (i = 0; i < 8; i++) {
        tail[i] = (uint8_t)(bits >> (56u - i * 8u));
    }

    /* Not through `update`, because that would count these eight bytes in
     * the length that is being written. */
    memcpy(s->buffer + 56, tail, 8);
    sha256_block(s, s->buffer);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void sha256(const void *data, size_t bytes, uint8_t out[32])
{
    struct sha256 s;

    sha256_init(&s);
    sha256_update(&s, data, bytes);
    sha256_final(&s, out);
}

/*------------------------------------------------------------------------
 * HMAC-SHA256. RFC 2104.
 *
 * Two hashes with the key padded to the block size and exclusive-ored with
 * two different constants. The subtlety is the key that is *longer* than a
 * block: it is hashed first, and an implementation that truncated instead
 * would agree with itself and with nobody else.
 *----------------------------------------------------------------------*/

void hmac_sha256(const void *key, size_t key_bytes,
                 const void *data, size_t bytes, uint8_t out[32])
{
    uint8_t block[64];
    uint8_t inner[32];
    struct sha256 s;
    unsigned i;

    memset(block, 0, sizeof(block));

    if (key_bytes > sizeof(block)) {
        sha256(key, key_bytes, block);
    } else {
        memcpy(block, key, key_bytes);
    }

    for (i = 0; i < sizeof(block); i++) {
        block[i] ^= 0x36u;
    }

    sha256_init(&s);
    sha256_update(&s, block, sizeof(block));
    sha256_update(&s, data, bytes);
    sha256_final(&s, inner);

    for (i = 0; i < sizeof(block); i++) {
        block[i] ^= 0x36u ^ 0x5cu;
    }

    sha256_init(&s);
    sha256_update(&s, block, sizeof(block));
    sha256_update(&s, inner, sizeof(inner));
    sha256_final(&s, out);
}

/*------------------------------------------------------------------------
 * ChaCha20. RFC 8439.
 *
 * Twenty rounds - ten column rounds and ten diagonal ones, interleaved -
 * over a sixteen-word state made of a constant, the key, a counter and a
 * nonce. The output is the state added to its own starting value, which is
 * what makes it a permutation rather than something invertible.
 *
 * **The counter is the whole security argument.** ChaCha20 is a stream
 * cipher: the keystream is exclusive-ored with the plaintext, so using the
 * same key, nonce and counter twice gives two ciphertexts whose difference
 * is the difference of the plaintexts. That failure is completely silent -
 * both messages encrypt, both decrypt, and anybody who has seen both can
 * read them. It is the reason SSH's ChaCha20 construction numbers every
 * packet.
 *----------------------------------------------------------------------*/

static uint32_t rol(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32u - n));
}

#define QR(a, b, c, d)                                  \
    do {                                                \
        a += b; d ^= a; d = rol(d, 16);                 \
        c += d; b ^= c; b = rol(b, 12);                 \
        a += b; d ^= a; d = rol(d, 8);                  \
        c += d; b ^= c; b = rol(b, 7);                  \
    } while (0)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64])
{
    /* "expand 32-byte k", which is what those four words spell. A constant
     * rather than a nothing-up-my-sleeve number, and it is in the
     * specification exactly like this. */
    static const uint32_t C[4] = {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u
    };

    uint32_t s[16], x[16];
    unsigned i;

    s[0] = C[0]; s[1] = C[1]; s[2] = C[2]; s[3] = C[3];

    for (i = 0; i < 8; i++) {
        s[4 + i] = le32(key + i * 4);
    }

    s[12] = counter;

    for (i = 0; i < 3; i++) {
        s[13 + i] = le32(nonce + i * 4);
    }

    memcpy(x, s, sizeof(x));

    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);

        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }

    for (i = 0; i < 16; i++) {
        uint32_t v = x[i] + s[i];

        out[i * 4]     = (uint8_t)v;
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

void chacha20(const uint8_t key[32], uint32_t counter,
              const uint8_t nonce[12], const uint8_t *in, uint8_t *out,
              size_t bytes)
{
    uint8_t block[64];
    size_t at = 0;

    while (at < bytes) {
        size_t take = bytes - at;
        size_t i;

        if (take > sizeof(block)) {
            take = sizeof(block);
        }

        chacha20_block(key, counter, nonce, block);
        counter++;

        for (i = 0; i < take; i++) {
            out[at + i] = in[at + i] ^ block[i];
        }

        at += take;
    }
}

/*------------------------------------------------------------------------
 * Poly1305. RFC 8439.
 *
 * A one-time authenticator: the message is read as a sequence of 130-bit
 * numbers, accumulated modulo 2^130 - 5 multiplied by `r`, and `s` is added
 * at the end.
 *
 * **One-time is not advice.** The key is a *pair* and using the same one for
 * two messages lets anybody who has both recover `r` and forge anything
 * afterwards. That is why the ChaCha20-Poly1305 construction derives a fresh
 * pair from the cipher for every packet, and why this takes the key as an
 * argument rather than holding one.
 *
 * Twenty-six-bit limbs, five of them, so that a product of two limbs fits in
 * 64 bits with room for the carries. The alternative is 128-bit arithmetic,
 * which this compiler has and which would make the reduction below harder to
 * check against the specification rather than easier.
 */

struct poly1305 {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    uint8_t  buffer[16];
    size_t   held;
};

static void poly1305_init(struct poly1305 *p, const uint8_t key[32])
{
    /* `r` is clamped: some bits are cleared so that the products below
     * cannot overflow the limb arithmetic. The constants are the
     * specification's and are not a choice. */
    uint32_t t0 = le32(key);
    uint32_t t1 = le32(key + 4);
    uint32_t t2 = le32(key + 8);
    uint32_t t3 = le32(key + 12);
    unsigned i;

    p->r[0] = t0 & 0x3ffffffu;
    p->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03u;
    p->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ffu;
    p->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fffu;
    p->r[4] = (t3 >> 8) & 0x00fffffu;

    for (i = 0; i < 5; i++) {
        p->h[i] = 0;
    }

    for (i = 0; i < 4; i++) {
        p->pad[i] = le32(key + 16 + i * 4);
    }

    p->held = 0;
}

static void poly1305_block(struct poly1305 *p, const uint8_t *m, uint32_t high)
{
    uint32_t r0 = p->r[0], r1 = p->r[1], r2 = p->r[2];
    uint32_t r3 = p->r[3], r4 = p->r[4];
    uint32_t s1 = r1 * 5u, s2 = r2 * 5u, s3 = r3 * 5u, s4 = r4 * 5u;
    uint32_t t0 = le32(m), t1 = le32(m + 4);
    uint32_t t2 = le32(m + 8), t3 = le32(m + 12);
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    p->h[0] += t0 & 0x3ffffffu;
    p->h[1] += ((t0 >> 26) | (t1 << 6)) & 0x3ffffffu;
    p->h[2] += ((t1 >> 20) | (t2 << 12)) & 0x3ffffffu;
    p->h[3] += ((t2 >> 14) | (t3 << 18)) & 0x3ffffffu;
    p->h[4] += (t3 >> 8) | high;

    d0 = (uint64_t)p->h[0] * r0 + (uint64_t)p->h[1] * s4
       + (uint64_t)p->h[2] * s3 + (uint64_t)p->h[3] * s2
       + (uint64_t)p->h[4] * s1;
    d1 = (uint64_t)p->h[0] * r1 + (uint64_t)p->h[1] * r0
       + (uint64_t)p->h[2] * s4 + (uint64_t)p->h[3] * s3
       + (uint64_t)p->h[4] * s2;
    d2 = (uint64_t)p->h[0] * r2 + (uint64_t)p->h[1] * r1
       + (uint64_t)p->h[2] * r0 + (uint64_t)p->h[3] * s4
       + (uint64_t)p->h[4] * s3;
    d3 = (uint64_t)p->h[0] * r3 + (uint64_t)p->h[1] * r2
       + (uint64_t)p->h[2] * r1 + (uint64_t)p->h[3] * r0
       + (uint64_t)p->h[4] * s4;
    d4 = (uint64_t)p->h[0] * r4 + (uint64_t)p->h[1] * r3
       + (uint64_t)p->h[2] * r2 + (uint64_t)p->h[3] * r1
       + (uint64_t)p->h[4] * r0;

    c = (uint32_t)(d0 >> 26); p->h[0] = (uint32_t)d0 & 0x3ffffffu;
    d1 += c; c = (uint32_t)(d1 >> 26); p->h[1] = (uint32_t)d1 & 0x3ffffffu;
    d2 += c; c = (uint32_t)(d2 >> 26); p->h[2] = (uint32_t)d2 & 0x3ffffffu;
    d3 += c; c = (uint32_t)(d3 >> 26); p->h[3] = (uint32_t)d3 & 0x3ffffffu;
    d4 += c; c = (uint32_t)(d4 >> 26); p->h[4] = (uint32_t)d4 & 0x3ffffffu;

    /* 2^130 = 5 in this field, so the carry out of the top limb comes back
     * in at the bottom multiplied by five. That single line is the whole of
     * the modular reduction. */
    p->h[0] += c * 5u;
    c = p->h[0] >> 26; p->h[0] &= 0x3ffffffu;
    p->h[1] += c;
}

static void poly1305_update(struct poly1305 *p, const uint8_t *m, size_t bytes)
{
    while (bytes > 0) {
        size_t take = 16u - p->held;

        if (take > bytes) {
            take = bytes;
        }

        memcpy(p->buffer + p->held, m, take);
        p->held += take;
        m += take;
        bytes -= take;

        if (p->held == 16u) {
            /* The high bit is the 129th, set on every full block: it is what
             * makes a message of zero bytes different from one of sixteen
             * zeroes. */
            poly1305_block(p, p->buffer, 1u << 24);
            p->held = 0;
        }
    }
}

static void poly1305_final(struct poly1305 *p, uint8_t out[16])
{
    uint32_t g[5];
    uint32_t c, mask;
    uint64_t f;
    unsigned i;

    if (p->held > 0) {
        p->buffer[p->held] = 1;

        for (i = (unsigned)p->held + 1; i < 16; i++) {
            p->buffer[i] = 0;
        }

        poly1305_block(p, p->buffer, 0);
    }

    c = p->h[1] >> 26; p->h[1] &= 0x3ffffffu;
    p->h[2] += c; c = p->h[2] >> 26; p->h[2] &= 0x3ffffffu;
    p->h[3] += c; c = p->h[3] >> 26; p->h[3] &= 0x3ffffffu;
    p->h[4] += c; c = p->h[4] >> 26; p->h[4] &= 0x3ffffffu;
    p->h[0] += c * 5u; c = p->h[0] >> 26; p->h[0] &= 0x3ffffffu;
    p->h[1] += c;

    /* h - p, computed unconditionally and selected with a mask rather than a
     * branch: whether the accumulator was above the prime is secret. */
    g[0] = p->h[0] + 5u; c = g[0] >> 26; g[0] &= 0x3ffffffu;
    g[1] = p->h[1] + c;  c = g[1] >> 26; g[1] &= 0x3ffffffu;
    g[2] = p->h[2] + c;  c = g[2] >> 26; g[2] &= 0x3ffffffu;
    g[3] = p->h[3] + c;  c = g[3] >> 26; g[3] &= 0x3ffffffu;
    g[4] = p->h[4] + c - (1u << 26);

    mask = (g[4] >> 31) - 1u;

    for (i = 0; i < 5; i++) {
        g[i] &= mask;
    }

    mask = ~mask;

    for (i = 0; i < 5; i++) {
        p->h[i] = (p->h[i] & mask) | g[i];
    }

    p->h[0] = (p->h[0] | (p->h[1] << 26)) & 0xffffffffu;
    p->h[1] = ((p->h[1] >> 6) | (p->h[2] << 20)) & 0xffffffffu;
    p->h[2] = ((p->h[2] >> 12) | (p->h[3] << 14)) & 0xffffffffu;
    p->h[3] = ((p->h[3] >> 18) | (p->h[4] << 8)) & 0xffffffffu;

    f = (uint64_t)p->h[0] + p->pad[0]; p->h[0] = (uint32_t)f;
    f = (uint64_t)p->h[1] + p->pad[1] + (f >> 32); p->h[1] = (uint32_t)f;
    f = (uint64_t)p->h[2] + p->pad[2] + (f >> 32); p->h[2] = (uint32_t)f;
    f = (uint64_t)p->h[3] + p->pad[3] + (f >> 32); p->h[3] = (uint32_t)f;

    for (i = 0; i < 4; i++) {
        out[i * 4]     = (uint8_t)p->h[i];
        out[i * 4 + 1] = (uint8_t)(p->h[i] >> 8);
        out[i * 4 + 2] = (uint8_t)(p->h[i] >> 16);
        out[i * 4 + 3] = (uint8_t)(p->h[i] >> 24);
    }
}

void poly1305(const uint8_t key[32], const void *data, size_t bytes,
              uint8_t out[16])
{
    struct poly1305 p;

    poly1305_init(&p, key);
    poly1305_update(&p, data, bytes);
    poly1305_final(&p, out);
}

/*------------------------------------------------------------------------
 * X25519. RFC 7748.
 *
 * The key exchange: each side has a secret scalar, sends the public point
 * that scalar times the base point gives, and multiplies what it receives by
 * its own scalar. Both arrive at the same point, and an observer who saw
 * both public values cannot compute it.
 *
 * **The Montgomery ladder, which is the shape rather than an optimisation.**
 * A conditional swap driven by the bit being processed means both branches
 * of every step run every time, so the *time* the loop takes does not depend
 * on the secret. The obvious double-and-add would be shorter, faster and
 * would leak the scalar to anybody who could measure it - which is the whole
 * reason this algorithm is written the way it is everywhere.
 *
 * Ten limbs of 25 and 26 bits alternately, so that products fit in 64 bits
 * with room for carries. `2^255 - 19` is the field, and the 19 is why a
 * carry out of the top comes back in at the bottom multiplied by nineteen -
 * the same trick Poly1305's five is.
 *----------------------------------------------------------------------*/

typedef int64_t fe[10];

static void fe_0(fe h)  { memset(h, 0, sizeof(fe)); }
static void fe_1(fe h)  { fe_0(h); h[0] = 1; }
static void fe_copy(fe h, const fe f) { memcpy(h, f, sizeof(fe)); }

static void fe_add(fe h, const fe f, const fe g)
{
    unsigned i;

    for (i = 0; i < 10; i++) {
        h[i] = f[i] + g[i];
    }
}

static void fe_sub(fe h, const fe f, const fe g)
{
    unsigned i;

    for (i = 0; i < 10; i++) {
        h[i] = f[i] - g[i];
    }
}

/*
 * Swap `f` and `g` when `b` is one, in constant time.
 *
 * The mask is 0 or all-ones and the exchange is three exclusive-ors, so the
 * same instructions run either way. An `if` here would be the leak the
 * ladder exists to avoid, in the one place it is most obvious.
 */
static void fe_cswap(fe f, fe g, unsigned b)
{
    int64_t mask = -(int64_t)b;
    unsigned i;

    for (i = 0; i < 10; i++) {
        int64_t x = (f[i] ^ g[i]) & mask;

        f[i] ^= x;
        g[i] ^= x;
    }
}

static void fe_carry(fe h)
{
    int64_t c;
    unsigned i;

    /* Two passes: the first spreads the carries, the second catches what
     * the reduction of the top limb put back into the bottom. One pass is
     * the mistake that shows only on values near the prime. */
    for (i = 0; i < 2; i++) {
        c = (h[0] + (1 << 25)) >> 26; h[1] += c; h[0] -= c * (1 << 26);
        c = (h[1] + (1 << 24)) >> 25; h[2] += c; h[1] -= c * (1 << 25);
        c = (h[2] + (1 << 25)) >> 26; h[3] += c; h[2] -= c * (1 << 26);
        c = (h[3] + (1 << 24)) >> 25; h[4] += c; h[3] -= c * (1 << 25);
        c = (h[4] + (1 << 25)) >> 26; h[5] += c; h[4] -= c * (1 << 26);
        c = (h[5] + (1 << 24)) >> 25; h[6] += c; h[5] -= c * (1 << 25);
        c = (h[6] + (1 << 25)) >> 26; h[7] += c; h[6] -= c * (1 << 26);
        c = (h[7] + (1 << 24)) >> 25; h[8] += c; h[7] -= c * (1 << 25);
        c = (h[8] + (1 << 25)) >> 26; h[9] += c; h[8] -= c * (1 << 26);
        c = (h[9] + (1 << 24)) >> 25; h[0] += c * 19; h[9] -= c * (1 << 25);
    }
}

static void fe_mul(fe h, const fe f, const fe g)
{
    int64_t t[19];
    unsigned i, j;

    for (i = 0; i < 19; i++) {
        t[i] = 0;
    }

    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            /* The odd-indexed limbs are 25 bits, so a product of two of them
             * is short by one bit and is doubled to line up. That single
             * factor of two is the part of this that is easy to get wrong
             * and impossible to notice without a vector. */
            int64_t v = f[i] * g[j];

            if ((i & 1) != 0 && (j & 1) != 0) {
                v *= 2;
            }

            t[i + j] += v;
        }
    }

    for (i = 0; i < 9; i++) {
        h[i] = t[i] + 19 * t[i + 10];
    }

    h[9] = t[9];

    fe_carry(h);
}

static void fe_sq(fe h, const fe f)
{
    fe_mul(h, f, f);
}

static void fe_mul121666(fe h, const fe f)
{
    unsigned i;

    for (i = 0; i < 10; i++) {
        h[i] = f[i] * 121666;
    }

    fe_carry(h);
}

/*
 * The inverse, by Fermat: x^(p-2) is x^-1 in a field of order p.
 *
 * Two hundred and fifty-four squarings and a handful of multiplications,
 * driven by the bit pattern of p-2 - which for 2^255-19 is all ones except
 * for a gap. Written as the standard addition chain rather than a loop over
 * the bits, because the chain is what every implementation uses and is what
 * a reader can compare against one.
 */
static void fe_invert(fe out, const fe z)
{
    fe t0, t1, t2, t3;
    int i;

    fe_sq(t0, z);
    fe_sq(t1, t0); fe_sq(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_sq(t2, t0);
    fe_mul(t1, t1, t2);
    fe_sq(t2, t1);
    for (i = 1; i < 5; i++) { fe_sq(t2, t2); }
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 10; i++) { fe_sq(t2, t2); }
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 20; i++) { fe_sq(t3, t3); }
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 10; i++) { fe_sq(t2, t2); }
    fe_mul(t1, t2, t1);
    fe_sq(t2, t1);
    for (i = 1; i < 50; i++) { fe_sq(t2, t2); }
    fe_mul(t2, t2, t1);
    fe_sq(t3, t2);
    for (i = 1; i < 100; i++) { fe_sq(t3, t3); }
    fe_mul(t2, t3, t2);
    fe_sq(t2, t2);
    for (i = 1; i < 50; i++) { fe_sq(t2, t2); }
    fe_mul(t1, t2, t1);
    fe_sq(t1, t1);
    for (i = 1; i < 5; i++) { fe_sq(t1, t1); }
    fe_mul(out, t1, t0);
}

/*
 * Bytes to limbs, and back.
 *
 * Written as a bit loop rather than as hand-rolled word shifts. The shifts
 * are what every reference implementation uses and they are faster; the
 * first version here got one of them wrong in a way the compiler warned
 * about and a reader would not have caught, which is the whole argument for
 * the slow obvious form in a file where a bug is silent. This runs once per
 * key exchange.
 */
static void fe_from_bytes(fe h, const uint8_t in[32])
{
    uint8_t t[32];
    unsigned i, bitpos = 0;

    memcpy(t, in, 32);

    /* The top bit is ignored, which RFC 7748 requires: a peer that sets it
     * is sending a value with two representations, and masking is what
     * every implementation agrees to do about it. */
    t[31] &= 127u;

    for (i = 0; i < 10; i++) {
        unsigned bits = (i & 1u) ? 25u : 26u;
        int64_t v = 0;
        unsigned k;

        for (k = 0; k < bits; k++) {
            unsigned bit = bitpos + k;

            if (((t[bit / 8] >> (bit % 8)) & 1u) != 0) {
                v |= (int64_t)1 << k;
            }
        }

        h[i] = v;
        bitpos += bits;
    }
}

static void fe_to_bytes(uint8_t out[32], const fe f)
{
    /* 2^255 - 19, little endian. */
    static const uint8_t P[32] = {
        0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f
    };

    fe t;
    uint8_t buf[32];
    uint8_t less[32];
    unsigned i, pass, bitpos = 0;
    int borrow = 0;
    uint8_t mask;

    fe_copy(t, f);

    for (i = 0; i < 3; i++) {
        fe_carry(t);
    }

    /*
     * Every limb non-negative. `fe_carry` centres them, so some may be
     * negative; borrowing from the next one up - and from limb 0 with a
     * factor of nineteen when it is the top - is what makes them positive
     * without changing the value. Twice, because a borrow into limb 0 can
     * make *it* negative.
     */
    for (pass = 0; pass < 3; pass++) {
        for (i = 0; i < 10; i++) {
            unsigned bits = (i & 1u) ? 25u : 26u;

            while (t[i] < 0) {
                t[i] += (int64_t)1 << bits;

                if (i == 9) {
                    t[0] -= 19;
                } else {
                    t[i + 1] -= 1;
                }
            }
        }
    }

    memset(buf, 0, sizeof(buf));

    for (i = 0; i < 10; i++) {
        unsigned bits = (i & 1u) ? 25u : 26u;
        uint64_t v = (uint64_t)t[i];
        unsigned k;

        for (k = 0; k < bits; k++) {
            if (((v >> k) & 1u) != 0) {
                unsigned bit = bitpos + k;

                buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
            }
        }

        bitpos += bits;
    }

    /*
     * The value may still be at or above the prime, so subtract it - and
     * choose whether to keep the result with a mask rather than a branch,
     * because whether it was above is derived from the secret.
     */
    for (i = 0; i < 32; i++) {
        int v = (int)buf[i] - (int)P[i] - borrow;

        borrow = (v < 0) ? 1 : 0;
        less[i] = (uint8_t)(v & 0xff);
    }

    /* No borrow means the subtraction was legitimate: take it. */
    mask = (uint8_t)(borrow - 1);

    for (i = 0; i < 32; i++) {
        out[i] = (uint8_t)((less[i] & mask) | (buf[i] & (uint8_t)~mask));
    }
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t e[32];
    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    unsigned swap = 0;
    int pos;

    memcpy(e, scalar, 32);

    /*
     * Clamping, and it is not optional.
     *
     * The bottom three bits are cleared so the scalar is a multiple of the
     * cofactor - which stops a small-subgroup attack - and the top two are
     * set so the ladder always takes the same number of steps and the value
     * is large enough. RFC 7748 requires exactly this and an implementation
     * that skips it interoperates until it does not.
     */
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe_from_bytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    for (pos = 254; pos >= 0; pos--) {
        unsigned b = (unsigned)((e[pos / 8] >> (pos & 7)) & 1u);
        fe a, aa, bb, bcopy, da, cb, e2;

        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        fe_sub(a, x2, z2);
        fe_add(bcopy, x2, z2);
        fe_sub(tmp0, x3, z3);
        fe_add(tmp1, x3, z3);
        fe_mul(da, tmp0, bcopy);
        fe_mul(cb, tmp1, a);
        fe_sq(aa, a);
        fe_sq(bb, bcopy);

        fe_add(tmp0, da, cb);
        fe_sub(tmp1, da, cb);
        fe_sq(x3, tmp0);
        fe_sq(tmp0, tmp1);
        fe_mul(z3, tmp0, x1);

        fe_mul(x2, aa, bb);
        fe_sub(e2, bb, aa);
        fe_mul121666(tmp0, e2);
        fe_add(tmp0, tmp0, aa);
        fe_mul(z2, e2, tmp0);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32])
{
    static const uint8_t nine[32] = { 9 };

    x25519(out, scalar, nine);
}
