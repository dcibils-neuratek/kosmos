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

/*
 * **In pieces** (step N4): a message that arrives as several buffers - an
 * SMB reply is its header, its fixed part and the data that libsmb2 read
 * straight into a caller's region, three vectors - is signed where each
 * piece lies, never gathered into one. CMAC's one subtlety is that the
 * last block is treated differently from the rest and is not known to be
 * last until the message ends, so up to a whole block is held back: what
 * has arrived beyond it goes through the chain, and `final` does the rest.
 */
_Static_assert(sizeof(br_aes_gen_ctrcbc_keys) <= sizeof(((struct crypto_cmac *)0)->aes),
               "BearSSL's keyed AES fits in struct crypto_cmac");

static const br_block_ctrcbc_class **keyed(struct crypto_cmac *c)
{
    return &((br_aes_gen_ctrcbc_keys *)(void *)c->aes)->vtable;
}

void crypto_cmac_init(struct crypto_cmac *c, const void *key, size_t key_bytes)
{
    static const uint8_t zero[16];
    br_aes_gen_ctrcbc_keys *aes = (br_aes_gen_ctrcbc_keys *)(void *)c->aes;
    const br_block_ctrcbc_class *vt = crypto_aes_ctrcbc();
    unsigned i;

    vt->init(&aes->vtable, key, key_bytes);

    /* RFC 4493 2.3: L = AES(K, 0), which is a CBC-MAC of one zero block
     * from a zero chain; K1 = 2L and K2 = 4L are made from it at the end. */
    for (i = 0; i < 16; i++) c->l[i] = c->mac[i] = 0;
    vt->mac(&aes->vtable, c->l, zero, 16);
    c->held_bytes = 0;
}

void crypto_cmac_update(struct crypto_cmac *c, const void *data, size_t bytes)
{
    const br_block_ctrcbc_class **vt = keyed(c);
    const uint8_t *m = data;

    while (bytes > 0) {
        size_t take;

        /* A whole block held, and more coming: it was not the last. */
        if (c->held_bytes == 16) {
            (*vt)->mac(vt, c->mac, c->held, 16);
            c->held_bytes = 0;
        }

        /* Nothing held: every whole block but the one that may be last
         * goes through the chain where it lies. */
        if (c->held_bytes == 0 && bytes > 16) {
            size_t whole = (bytes - 1) / 16 * 16;

            (*vt)->mac(vt, c->mac, m, whole);
            m += whole;
            bytes -= whole;
        }

        take = 16 - c->held_bytes;
        if (take > bytes) take = bytes;

        for (size_t i = 0; i < take; i++) c->held[c->held_bytes + i] = m[i];
        c->held_bytes += (unsigned)take;
        m += take;
        bytes -= take;
    }
}

void crypto_cmac_final(struct crypto_cmac *c, uint8_t out[16])
{
    const br_block_ctrcbc_class **vt = keyed(c);
    uint8_t k[16], last[16];
    unsigned i, rest = c->held_bytes;

    /* 2.4: the last block XORed with K1 when it is whole, and padded 10*
     * and XORed with K2 when it is not - an empty message is one padded
     * block. */
    dbl(k, c->l);
    if (rest < 16) {
        uint8_t k1[16];

        for (i = 0; i < 16; i++) k1[i] = k[i];
        dbl(k, k1);
    }

    for (i = 0; i < 16; i++) {
        uint8_t b = i < rest ? c->held[i] : (i == rest ? 0x80 : 0);

        last[i] = b ^ k[i];
    }
    (*vt)->mac(vt, c->mac, last, 16);

    for (i = 0; i < 16; i++) out[i] = c->mac[i];
}

void crypto_aes_cmac(const void *key, size_t key_bytes,
                     const void *data, size_t bytes, uint8_t out[16])
{
    struct crypto_cmac c;

    crypto_cmac_init(&c, key, key_bytes);
    crypto_cmac_update(&c, data, bytes);
    crypto_cmac_final(&c, out);
}
