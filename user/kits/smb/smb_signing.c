/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * SMB's signatures, in place of libsmb2's `smb2-signing.c` (`docs/sharing.md`
 * step N4, *Signed and sealed*).
 *
 * **Why this file exists.** libsmb2's AES-CMAC is a loop that keys AES again
 * for every sixteen bytes - its block function takes the key, not a keyed
 * context - and signs a message by first copying all of it into one buffer
 * it allocates. Measured, natively on this Mac's cores and in the machine
 * (`testing.md` 18.409): 34.6 ms a megabyte on an ARM core's constant-time
 * AES where the kit's CMAC, keyed once, takes 14.3; 4.6 ms against 0.8
 * with AES-NI; and in the machine under emulation what signing added to a
 * megabyte read fell from about 330 ms to 140 on x86-64, and from 130-200
 * to 20-80 on ARM. A megabyte READ is checked once on arrival, so that
 * loop was most of what reading a signed share cost.
 *
 * **How it replaces it without an edit.** The Makefile leaves
 * `smb2-signing.c` out of the build by name (`LIBSMB2_LEFT_OUT`), as it
 * leaves out libsmb2's AES, and this file supplies every symbol that one
 * defines, with the same signatures: `smb3_aes_cmac_128`,
 * `smb2_calc_signature`, `smb2_pdu_add_signature` and
 * `smb2_pdu_check_signature`. Where a signature is *checked* is unchanged
 * and still libsmb2's - `socket.c` and `libsmb2.c` call
 * `smb2_calc_signature` over what arrived and compare - so this decides how
 * a signature is computed, never whether one is looked at.
 *
 * What it does differently, and nothing else:
 *
 *   - 3.x's AES-CMAC is the Crypto Kit's, keyed once a message, over the
 *     message's vectors where they lie - a READ's data is the caller's
 *     region, read into straight from the network - with no copy;
 *   - 2.x's HMAC-SHA256 is BearSSL's directly, which is what libsmb2's
 *     `hmacReset` already was here (`smb_crypto.c`).
 *
 * Held by `tools/test_smbsign.c` to RFC 4493's four examples and to
 * libsmb2's own file, compiled beside it as the reference, on messages cut
 * into vectors every way the sizes allow; and by a real signed session at
 * each dialect, with one byte changed on the way, in `run_share.py --part 3`.
 */

#include <string.h>

#include <bearssl.h>

#include "config.h"
#include "compat.h"
#include "smb2-signing.h"

#include "crypto.h"

/* Not in any header of libsmb2's, and called by nothing now; supplied
 * because `smb2-signing.c` defined it. RFC 4493's examples hold it. */
void smb3_aes_cmac_128(uint8_t key[SMB2_KEY_SIZE], uint8_t *msg,
                       uint64_t msg_len, uint8_t mac[SMB2_KEY_SIZE]);

void smb3_aes_cmac_128(uint8_t key[SMB2_KEY_SIZE], uint8_t *msg,
                       uint64_t msg_len, uint8_t mac[SMB2_KEY_SIZE])
{
    crypto_aes_cmac(key, SMB2_KEY_SIZE, msg, (size_t)msg_len, mac);
}

/*
 * The signature of a message given as vectors, the first its 64-byte
 * header, whose signature field is cleared first - as MS-SMB2 3.1.4.1 says,
 * and as libsmb2's did. `signature` may be that very field (a check of what
 * arrived passes it), so it is written only once the sum is done.
 */
int smb2_calc_signature(struct smb2_context *smb2, uint8_t *signature,
                        struct smb2_iovec *iov, size_t niov)
{
    uint8_t sum[32];
    size_t i;

    memset(iov[0].buf + 48, 0, SMB2_SIGNATURE_SIZE);

    if (smb2->dialect > SMB2_VERSION_0210) {
        struct crypto_cmac cmac;

        crypto_cmac_init(&cmac, smb2->signing_key, SMB2_KEY_SIZE);

        for (i = 0; i < niov; i++) {
            crypto_cmac_update(&cmac, iov[i].buf, iov[i].len);
        }

        crypto_cmac_final(&cmac, sum);
    } else {
        br_hmac_key_context kc;
        br_hmac_context hc;

        br_hmac_key_init(&kc, &br_sha256_vtable, smb2->signing_key, SMB2_KEY_SIZE);
        br_hmac_init(&hc, &kc, 0);

        for (i = 0; i < niov; i++) {
            br_hmac_update(&hc, iov[i].buf, iov[i].len);
        }

        (void)br_hmac_out(&hc, sum);
    }

    memcpy(signature, sum, SMB2_SIGNATURE_SIZE);
    return 0;
}

/*
 * A message about to go, signed: libsmb2's rules for which ones, as its
 * file had them - not the first legs of SESSION_SETUP, nothing before there
 * is a session, and a refusal when there is a session and no key.
 */
int smb2_pdu_add_signature(struct smb2_context *smb2, struct smb2_pdu *pdu)
{
    struct smb2_header *hdr = &pdu->header;
    struct smb2_iovec *iov;

    if (hdr->command == SMB2_SESSION_SETUP) {
        /* The first SESSION_SETUP answer that succeeded is the first
         * message signed. */
        if (hdr->status != 0 || !(hdr->flags & SMB2_FLAGS_SERVER_TO_REDIR)) {
            return 0;
        }
    }

    if (pdu->out.niov < 2) {
        smb2_set_error(smb2, "Too few vectors to sign");
        return -1;
    }

    if (pdu->out.iov[0].len != SMB2_HEADER_SIZE) {
        smb2_set_error(smb2, "First vector is not same size as smb2 header");
        return -1;
    }

    if (smb2->session_id == 0) {
        return 0;
    }

    if (smb2->session_key_size == 0) {
        return -1;
    }

    /* The flag is part of what is signed, so it is set first. */
    iov = &pdu->out.iov[0];
    hdr->flags |= SMB2_FLAGS_SIGNED;
    smb2_set_uint32(iov, 16, hdr->flags);

    if (smb2_calc_signature(smb2, hdr->signature, iov, (size_t)pdu->out.niov) < 0) {
        return -1;
    }

    memcpy(iov->buf + 48, hdr->signature, SMB2_SIGNATURE_SIZE);
    return 0;
}

/* libsmb2's is this too: what arrives is checked where it is read
 * (`socket.c`, `libsmb2.c`), and nothing calls this. */
int smb2_pdu_check_signature(struct smb2_context *smb2, struct smb2_pdu *pdu)
{
    (void)smb2;
    (void)pdu;
    return 0;
}
