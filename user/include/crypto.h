/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_CRYPTO_H
#define KOSMOS_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

/*
 * The Crypto Kit's primitives (`user/kits/crypto/`).
 *
 * `crypto.c` says why the discipline here is different from the rest of the
 * tree: this is the one place where a bug is silent, so every one of these
 * is checked against the vectors in its own specification and the check is
 * in `make test` (`tools/test_crypto.c`).
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

/* One block of DES (FIPS 46-3), for VNC Authentication and nothing that has
 * to stay secret. */
void des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

/* A generator: ChaCha20 with fast key erasure, seeded from the hardware
 * (`SYS_ENTROPY`) by whoever holds one - `crypto.random` in a process. */
struct drbg {
    uint8_t key[32];
};

void drbg_seed(struct drbg *d, const uint8_t seed[32]);
void drbg_reseed(struct drbg *d, const uint8_t fresh[32]);
void drbg_generate(struct drbg *d, uint8_t *out, size_t bytes);

/*
 * What SMB 2/3 needs (`docs/sharing.md`, N1), and the one header for it.
 *
 * **Most of it is BearSSL's, called directly** (`runtime/upstream/bearssl/`,
 * in every image for TLS; `<bearssl.h>`), and not wrapped here, because a
 * wrapper would only rename it:
 *
 *   MD5, HMAC-MD5      `br_md5_vtable` with `br_hmac_key_init`, `br_hmac_init`,
 *                      `br_hmac_update`, `br_hmac_out` - NTLMv2's proof
 *   HMAC-SHA256        the same with `br_sha256_vtable` - SMB 2.x's signature
 *   SHA-512            `br_sha512_*` - 3.1.1's preauthentication hash
 *   AES-128-CCM        `br_ccm_*` over `crypto_aes_ctrcbc()` below - SMB 3's
 *                      sealing
 *   AES-128-GCM        `br_gcm_*` over `br_aes_x86ni_ctr_get_vtable()` or
 *                      `br_aes_ct64_ctr_vtable`, with `br_ghash_pclmul_get()`
 *                      or `br_ghash_ctmul64` - 3.1.1's other cipher, later
 *
 * What BearSSL does not have is written in the kit, on BearSSL's AES and
 * HMAC where it can be, and held to its specification's vectors in
 * `tools/test_crypto.c`.
 */

/* MD4 (RFC 1320), for the NT hash and nothing else: MD4 is broken. */
void crypto_md4(const void *data, size_t bytes, uint8_t out[16]);

/* BearSSL's AES as the CTR + CBC-MAC class `br_ccm_init` takes: AES-NI
 * where the processor has it, constant-time `aes_ct64` everywhere else, and
 * never the table-driven implementations. The one place that choice is made;
 * AES-CMAC below uses it too. */
struct br_block_ctrcbc_class_;
const struct br_block_ctrcbc_class_ *crypto_aes_ctrcbc(void);

/* AES-CMAC (RFC 4493), SMB 3's signature. A key of 16 or 32 bytes. */
void crypto_aes_cmac(const void *key, size_t key_bytes,
                     const void *data, size_t bytes, uint8_t out[16]);

/* The same, over a message in pieces, each signed where it lies (step N4:
 * an SMB reply is several buffers, one of them the caller's region). The
 * keyed AES is BearSSL's, held in `aes`, which `cmac.c` checks it fits. */
struct crypto_cmac {
    uint64_t aes[48];
    uint8_t  l[16];                 /* AES(K, 0), the subkeys' root */
    uint8_t  mac[16];               /* the chain so far */
    uint8_t  held[16];              /* the block that may be the last */
    unsigned held_bytes;
};

void crypto_cmac_init(struct crypto_cmac *c, const void *key, size_t key_bytes);
void crypto_cmac_update(struct crypto_cmac *c, const void *data, size_t bytes);
void crypto_cmac_final(struct crypto_cmac *c, uint8_t out[16]);

/* NIST SP 800-108's KDF in counter mode with HMAC-SHA256: SMB 3's keys.
 * The label and the context are taken as they are, terminators included -
 * SMB's carry their own NUL - and SP 800-108's 0x00 is put between them. */
void crypto_kdf_ctr_hmac_sha256(const void *key, size_t key_bytes,
                                const void *label, size_t label_bytes,
                                const void *context, size_t context_bytes,
                                uint8_t *out, size_t out_bytes);

#endif /* KOSMOS_CRYPTO_H */
