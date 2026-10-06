/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * AES-CCM (RFC 3610, NIST SP 800-38C), sealed and opened through one door:
 * SMB 3's sealing and the keyring's file (`docs/keyring.md`, K1).
 *
 * **Nothing here is a cipher either.** CCM is BearSSL's (`br_ccm_*`), over
 * the kit's choice of AES (`crypto_aes_ctrcbc()`, in `cmac.c`): AES-NI
 * where the processor has it, constant-time `aes_ct64` everywhere else.
 * What this file adds is the shape every caller wanted and each had written
 * for itself - the SMB Kit's `ccm_start` and `tools/test_crypto.c` - so a
 * third, the keyring, does not write it again:
 *
 *   - a key of 16 or 32 bytes, anything else refused;
 *   - BearSSL's limits on the nonce (7 to 13 bytes) and the tag (4 to 16,
 *     even) refused rather than ignored, since `br_ccm_reset` says no by
 *     its answer and a caller that does not look seals with nothing;
 *   - and an open whose tag does not match **zeroes what it decrypted**
 *     before answering, so a forged message's bytes are never left in the
 *     caller's buffer looking like a message.
 *
 * The tag is compared by `br_ccm_check_tag`, in constant time.
 */

#include <bearssl.h>
#include <string.h>

#include "crypto.h"

static int start(br_ccm_context *ccm, br_aes_gen_ctrcbc_keys *aes,
                 const void *key, size_t key_bytes,
                 const uint8_t *nonce, size_t nonce_bytes,
                 const void *aad, size_t aad_bytes,
                 size_t bytes, size_t tag_bytes)
{
    const br_block_ctrcbc_class *vt = crypto_aes_ctrcbc();

    if (key_bytes != 16 && key_bytes != 32)
        return -1;

    vt->init(&aes->vtable, key, key_bytes);
    br_ccm_init(ccm, &aes->vtable);

    if (!br_ccm_reset(ccm, nonce, nonce_bytes, aad_bytes, bytes, tag_bytes))
        return -1;

    br_ccm_aad_inject(ccm, aad, aad_bytes);
    br_ccm_flip(ccm);
    return 0;
}

int crypto_aes_ccm_seal(const void *key, size_t key_bytes,
                        const uint8_t *nonce, size_t nonce_bytes,
                        const void *aad, size_t aad_bytes,
                        void *data, size_t bytes,
                        uint8_t *tag, size_t tag_bytes)
{
    br_aes_gen_ctrcbc_keys aes;
    br_ccm_context ccm;

    if (start(&ccm, &aes, key, key_bytes, nonce, nonce_bytes,
              aad, aad_bytes, bytes, tag_bytes) != 0)
        return -1;

    br_ccm_run(&ccm, 1, data, bytes);
    (void)br_ccm_get_tag(&ccm, tag);
    return 0;
}

int crypto_aes_ccm_open(const void *key, size_t key_bytes,
                        const uint8_t *nonce, size_t nonce_bytes,
                        const void *aad, size_t aad_bytes,
                        void *data, size_t bytes,
                        const uint8_t *tag, size_t tag_bytes)
{
    br_aes_gen_ctrcbc_keys aes;
    br_ccm_context ccm;

    if (start(&ccm, &aes, key, key_bytes, nonce, nonce_bytes,
              aad, aad_bytes, bytes, tag_bytes) != 0)
        return -1;

    br_ccm_run(&ccm, 0, data, bytes);

    if (!br_ccm_check_tag(&ccm, tag)) {
        memset(data, 0, bytes);
        return -1;
    }

    return 0;
}
