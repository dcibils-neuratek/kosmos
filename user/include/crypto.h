/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_CRYPTO_H
#define KOSMOS_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * The primitives SSH needs.
 *
 * `crypto.c` says why the discipline here is different from the rest of the
 * tree: this is the one place where a bug is silent, so every one of these
 * is checked against the vectors in its own specification and the check is
 * in `make test`.
 */

struct sha256 {
    uint32_t h[8];
    uint64_t length;                /* bytes fed in, for the padding */
    uint8_t  buffer[64];
    unsigned held;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t bytes);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void sha256(const void *data, size_t bytes, uint8_t out[32]);

void hmac_sha256(const void *key, size_t key_bytes,
                 const void *data, size_t bytes, uint8_t out[32]);

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);
void chacha20(const uint8_t key[32], uint32_t counter,
              const uint8_t nonce[12], const uint8_t *in, uint8_t *out,
              size_t bytes);

void poly1305(const uint8_t key[32], const void *data, size_t bytes,
              uint8_t out[16]);

void x25519(uint8_t out[32], const uint8_t scalar[32],
            const uint8_t point[32]);
void x25519_base(uint8_t out[32], const uint8_t scalar[32]);

#endif /* KOSMOS_CRYPTO_H */
