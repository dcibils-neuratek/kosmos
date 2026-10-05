/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The key-derivation function of NIST SP 800-108, in counter mode with
 * HMAC-SHA256 as its pseudorandom function - the one SMB 3 derives every
 * signing, sealing and application key with (MS-SMB2 3.1.4.2), over
 * BearSSL's HMAC and SHA-256.
 *
 * Block i of the output is
 *
 *     HMAC-SHA256(Ki, [i]_32 || Label || 0x00 || Context || [L]_32)
 *
 * with i counting from 1 and L the output's length in bits, both 32-bit
 * big-endian. The label and context are passed with whatever terminator
 * the caller's protocol puts in them: SMB's labels carry their own NUL
 * ("SMB2AESCMAC\0"), and the 0x00 above is SP 800-108's separator after it.
 *
 * Plain C: the loop is one HMAC per 32 bytes of key, and SMB asks for 16.
 * Held to Microsoft's published SMB 3.0 and 3.1.1 derivations
 * (`tools/test_crypto.c`).
 */

#include <bearssl.h>

#include "crypto.h"

static void be32(uint8_t out[4], uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

void crypto_kdf_ctr_hmac_sha256(const void *key, size_t key_bytes,
                                const void *label, size_t label_bytes,
                                const void *context, size_t context_bytes,
                                uint8_t *out, size_t out_bytes)
{
    static const uint8_t separator = 0;
    br_hmac_key_context kc;
    br_hmac_context hc;
    uint8_t counter[4], length[4], block[32];
    uint32_t i;
    size_t done = 0, n, j;

    br_hmac_key_init(&kc, &br_sha256_vtable, key, key_bytes);
    be32(length, (uint32_t)(out_bytes * 8));

    for (i = 1; done < out_bytes; i++) {
        be32(counter, i);
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, counter, 4);
        br_hmac_update(&hc, label, label_bytes);
        br_hmac_update(&hc, &separator, 1);
        br_hmac_update(&hc, context, context_bytes);
        br_hmac_update(&hc, length, 4);
        br_hmac_out(&hc, block);

        n = out_bytes - done < 32 ? out_bytes - done : 32;
        for (j = 0; j < n; j++) out[done + j] = block[j];
        done += n;
    }
}
