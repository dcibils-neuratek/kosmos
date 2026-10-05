/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * AES-CMAC (RFC 4493, NIST SP 800-38B), SMB 3.0 to 3.1.1's signature, over
 * BearSSL's AES - and which AES that is, chosen in one place.
 *
 * **Nothing here is a cipher.** The block encryption is BearSSL's
 * (`runtime/upstream/bearssl/`, already in every image for TLS): AES-NI
 * where the processor has it, `aes_ct64` - constant-time, bit-sliced -
 * everywhere else, and never the table-driven `aes_big`, whose timing
 * depends on the key. What is written here is CMAC's own part: two subkeys
 * by doubling in GF(2^128), and the last block's padding. The rest is a
 * CBC-MAC, which BearSSL's CTR+CBC-MAC class already computes over data it
 * does not modify - so a message is signed where it lies, never copied.
 *
 * Plain C: the loop over the message is BearSSL's, and what is left is
 * three 16-byte blocks.
 */

#include <bearssl.h>

#include "crypto.h"

const struct br_block_ctrcbc_class_ *crypto_aes_ctrcbc(void)
{
    const br_block_ctrcbc_class *ni = br_aes_x86ni_ctrcbc_get_vtable();

    return ni != NULL ? ni : &br_aes_ct64_ctrcbc_vtable;
}

/* Multiplication by x in GF(2^128) with RFC 4493's 0x87, in constant time:
 * whether the top bit was set decides the reduction by a mask, not a
 * branch, because the input is derived from the key. */
static void dbl(uint8_t out[16], const uint8_t in[16])
{
    uint8_t carry = (uint8_t)(-(in[0] >> 7)) & 0x87;
    unsigned i;

    for (i = 0; i < 15; i++)
        out[i] = (uint8_t)(in[i] << 1 | in[i + 1] >> 7);
    out[15] = (uint8_t)(in[15] << 1) ^ carry;
}

void crypto_aes_cmac(const void *key, size_t key_bytes,
                     const void *data, size_t bytes, uint8_t out[16])
{
    static const uint8_t zero[16];
    br_aes_gen_ctrcbc_keys aes;
    const br_block_ctrcbc_class *vt = crypto_aes_ctrcbc();
    const uint8_t *m = data;
    uint8_t l[16] = { 0 }, k[16], last[16], mac[16] = { 0 };
    size_t whole, rest, i;

    vt->init(&aes.vtable, key, key_bytes);

    /* RFC 4493 2.3: L = AES(K, 0), which is a CBC-MAC of one zero block
     * from a zero chain; K1 = 2L, K2 = 4L. */
    vt->mac(&aes.vtable, l, zero, 16);

    /* 2.4: every block but the last through the chain as it is; the last
     * XORed with K1 when it is whole, and padded 10* and XORed with K2 when
     * it is not - an empty message is one padded block. */
    rest = bytes % 16;
    if (bytes > 0 && rest == 0) rest = 16;
    whole = bytes - rest;

    vt->mac(&aes.vtable, mac, m, whole);

    dbl(k, l);
    if (rest < 16) {
        uint8_t k1[16];

        for (i = 0; i < 16; i++) k1[i] = k[i];
        dbl(k, k1);
    }

    for (i = 0; i < 16; i++) {
        uint8_t b = i < rest ? m[whole + i] : (i == rest ? 0x80 : 0);

        last[i] = b ^ k[i];
    }
    vt->mac(&aes.vtable, mac, last, 16);

    for (i = 0; i < 16; i++) out[i] = mac[i];
}
