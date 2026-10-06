/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * libsmb2's cryptography, as the Crypto Kit's (`docs/sharing.md`, *What the
 * Kosmos port patches*, item 4; `CLAUDE.md`, *Encryption is C, all of it*).
 *
 * libsmb2 carries its own: tiny-AES (table-driven, so its timing depends on
 * the key), an AES-128-CCM, MD4, MD5, HMAC-MD5, and RFC 6234's SHA family
 * and HMAC. **None of those files is in Kosmos's build**
 * (`LIBSMB2_SRCS` in the Makefile leaves them out), and this file gives the
 * names libsmb2's other files call to the kit's primitives and BearSSL's -
 * the ones `tools/test_crypto.c` holds to their specifications' vectors
 * (step N1). A second AES in the tree would be a defect (`CLAUDE.md`, the
 * premise), and this is how there is not one without editing a vendored
 * file: the library is linked against these instead of its own.
 *
 * What each name is, and what it now stands on:
 *
 *   aes128ccm_*          SMB 3's sealing: BearSSL's CCM over
 *                        `crypto_aes_ctrcbc()` - AES-NI where the processor
 *                        has it, constant-time `aes_ct64` elsewhere.
 *   MD4*                 the NT hash: `crypto_md4`.
 *   smb2_hmac_md5        NTLMv2's proof: BearSSL's HMAC over its MD5.
 *   hmac* (SHA-256)      SMB 2's signature and SMB 3's key derivation:
 *                        BearSSL's HMAC over its SHA-256.
 *   USHA* (SHA-512)      3.1.1's preauthentication hash: BearSSL's SHA-512.
 *
 * libsmb2's context structures (`MD4_CTX`, `HMACContext`, `USHAContext`)
 * are what its callers allocate, so BearSSL's state is kept inside them;
 * that each fits is checked when this compiles, not trusted.
 *
 * **And signing is `smb_signing.c`'s** (step N4): libsmb2's `smb2-signing.c`
 * leaves the build too, and with it the one caller of the single block of
 * AES this file gave as `AES128_ECB_encrypt` - its CMAC, which keyed AES
 * again for every block, and is now the kit's, keyed once.
 */

#include <stdlib.h>
#include <string.h>

#include <bearssl.h>

#include "config.h"
#include "compat.h"
#include "aes128ccm.h"
#include "md4.h"
#include "hmac-md5.h"
#include "sha.h"

#include "crypto.h"

/*------------------------------------------------------------------------
 * AES-128-CCM, SMB 3's sealing.
 *
 * AES-128-CCM, as libsmb2 calls it: the message `p` encrypted (or decrypted)
 * in place, the tag `m` of `mlen` bytes written (or checked). Decryption
 * answers 0 when the tag matches, which is what libsmb2's own did (its
 * `memcmp`).
 */
static void ccm_start(br_ccm_context *ccm, br_aes_gen_ctrcbc_keys *aes,
                      const unsigned char *key, const unsigned char *nonce,
                      size_t nlen, const unsigned char *aad, size_t alen,
                      size_t plen, size_t mlen)
{
    const br_block_ctrcbc_class *vt = crypto_aes_ctrcbc();

    vt->init(&aes->vtable, key, 16);
    br_ccm_init(ccm, &aes->vtable);
    (void)br_ccm_reset(ccm, nonce, nlen, alen, plen, mlen);
    br_ccm_aad_inject(ccm, aad, alen);
    br_ccm_flip(ccm);
}

void aes128ccm_encrypt(unsigned char *key,
                       unsigned char *nonce, size_t nlen,
                       unsigned char *aad, size_t alen,
                       unsigned char *p, size_t plen,
                       unsigned char *m, size_t mlen)
{
    br_aes_gen_ctrcbc_keys aes;
    br_ccm_context ccm;

    ccm_start(&ccm, &aes, key, nonce, nlen, aad, alen, plen, mlen);
    br_ccm_run(&ccm, 1, p, plen);
    (void)br_ccm_get_tag(&ccm, m);
}

int aes128ccm_decrypt(unsigned char *key,
                      unsigned char *nonce, size_t nlen,
                      unsigned char *aad, size_t alen,
                      unsigned char *p, size_t plen,
                      unsigned char *m, size_t mlen)
{
    br_aes_gen_ctrcbc_keys aes;
    br_ccm_context ccm;

    ccm_start(&ccm, &aes, key, nonce, nlen, aad, alen, plen, mlen);
    br_ccm_run(&ccm, 0, p, plen);

    return br_ccm_check_tag(&ccm, m) ? 0 : -1;
}

/*------------------------------------------------------------------------
 * MD4, for the NT hash.
 *
 * The kit's MD4 is one call over the whole of what is hashed, and libsmb2's
 * one use of `MD4_CTX` is exactly that: `NTOWFv1` calls Init, one Update
 * over the password in UTF-16, and Final, three lines together. So the
 * context holds where the bytes are until Final hashes them. A second
 * Update would be a library that changed under this file, and stops the
 * process rather than hashing half a password. smbfs hands libsmb2 the NT
 * hash it made itself ("ntlm:" and the hex, `smbfs.c`), so in Kosmos even
 * this is not reached.
 *----------------------------------------------------------------------*/

struct md4_held {
    const unsigned char *data;
    unsigned int         bytes;
    unsigned int         updates;
};

_Static_assert(sizeof(struct md4_held) <= sizeof(MD4_CTX),
               "what MD4Update holds fits in libsmb2's MD4_CTX");

void MD4Init(MD4_CTX *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
}

void MD4Update(MD4_CTX *ctx, unsigned char *data, unsigned int bytes)
{
    struct md4_held held;

    memcpy(&held, ctx, sizeof(held));

    if (held.updates++ != 0) {
        abort();
    }

    held.data = data;
    held.bytes = bytes;
    memcpy(ctx, &held, sizeof(held));
}

void MD4Final(unsigned char out[16], MD4_CTX *ctx)
{
    struct md4_held held;

    memcpy(&held, ctx, sizeof(held));
    crypto_md4(held.data, held.bytes, out);
    memset(ctx, 0, sizeof(*ctx));
}

/*------------------------------------------------------------------------
 * HMAC-MD5, for NTLMv2.
 *----------------------------------------------------------------------*/

void smb2_hmac_md5(unsigned char *text, int text_len, unsigned char *key,
                   unsigned int key_len, unsigned char *digest)
{
    br_hmac_key_context kc;
    br_hmac_context hc;

    br_hmac_key_init(&kc, &br_md5_vtable, key, key_len);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, text, (size_t)text_len);
    (void)br_hmac_out(&hc, digest);
}

/*------------------------------------------------------------------------
 * HMAC-SHA256 and SHA-512, in libsmb2's RFC 6234 shapes.
 *
 * Only the two libsmb2 asks for: `hmacReset` with SHA256 and `USHAReset`
 * with SHA512. Any other is refused by RFC 6234's own convention - a
 * nonzero answer, `shaBadParam` - and libsmb2 does not ask.
 *----------------------------------------------------------------------*/

_Static_assert(sizeof(br_hmac_context) <= sizeof(HMACContext),
               "BearSSL's HMAC state fits in libsmb2's HMACContext");
_Static_assert(sizeof(br_sha512_context) <= sizeof(USHAContext),
               "BearSSL's SHA-512 state fits in libsmb2's USHAContext");

int hmacReset(HMACContext *ctx, enum SHAversion which,
              const unsigned char *key, size_t key_len)
{
    br_hmac_key_context kc;
    br_hmac_context hc;

    if (which != SHA256) {
        return shaBadParam;
    }

    br_hmac_key_init(&kc, &br_sha256_vtable, key, key_len);
    br_hmac_init(&hc, &kc, 0);
    memcpy(ctx, &hc, sizeof(hc));
    return shaSuccess;
}

int hmacInput(HMACContext *ctx, const unsigned char *text, size_t text_len)
{
    br_hmac_update((br_hmac_context *)(void *)ctx, text, text_len);
    return shaSuccess;
}

int hmacResult(HMACContext *ctx, uint8_t *digest)
{
    (void)br_hmac_out((br_hmac_context *)(void *)ctx, digest);
    return shaSuccess;
}

int USHAReset(USHAContext *ctx, SHAversion which)
{
    if (which != SHA512) {
        return shaBadParam;
    }

    br_sha512_init((br_sha512_context *)(void *)ctx);
    return shaSuccess;
}

int USHAInput(USHAContext *ctx, const uint8_t *bytes, size_t count)
{
    br_sha512_update((br_sha512_context *)(void *)ctx, bytes, count);
    return shaSuccess;
}

int USHAResult(USHAContext *ctx, uint8_t digest[USHAMaxHashSize])
{
    br_sha512_out((br_sha512_context *)(void *)ctx, digest);
    return shaSuccess;
}
