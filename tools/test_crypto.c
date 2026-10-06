/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Crypto Kit's primitives against the vectors in their own
 * specifications (`user/kits/crypto/crypto.c`).
 *
 * **`crypto.c` said this test existed from the day it arrived, and it did
 * not.** Its opening comment named a `cryptotest` in `make test` on 5
 * September; nothing by that name was ever written, and the file sat in
 * every image with no caller and no check (`testing.md` 18.291). Written
 * when DES joined it for VNC, and it is the discipline that file asks for:
 * a cipher with a wrong counter keeps working and is not secure, so running
 * proves nothing and only the vectors do.
 *
 * Where each comes from, cut out of the RFC's text by a script rather than
 * retyped, and each also checked against an implementation that is not
 * this one - Python's `hashlib` and `hmac`, OpenSSL's ChaCha20, Poly1305
 * and DES - the day it was written:
 *
 *   SHA-256       FIPS 180-4's examples: "abc", the two-block message, a
 *                 million "a" fed in pieces of seven, and nothing at all
 *   HMAC-SHA-256  RFC 4231 test cases 1, 2 and 6 (a key longer than a block)
 *   ChaCha20      RFC 8439 2.4.2, the sunscreen text over two blocks
 *   Poly1305      RFC 8439 2.5.2
 *   X25519        RFC 7748 5.2's two, and 6.1's exchange between Alice and Bob
 *   DES           FIPS 46's worked example (key 133457799BBCDFF1), a VNC
 *                 challenge answered under "Kosmos" as OpenSSL answers it,
 *                 and 1,024 blocks under four keys, as OpenSSL ciphers them
 *
 * And what SMB 2/3 needs (`docs/sharing.md`, N1), the kit's and BearSSL's:
 *
 *   MD4           RFC 1320 A.5, all seven
 *   NTLM          MS-NLMP 4.2.2.1.2's NTOWFv1 and 4.2.4.1.1's NTOWFv2, and
 *                 the NT hash and NTOWFv2 of the sign-in in Microsoft's
 *                 "the anatomy of signing and cryptographic keys"
 *   HMAC-MD5      RFC 2202 cases 1 and 2 (BearSSL's)
 *   AES-CMAC      RFC 4493 section 4's four examples
 *   SP 800-108    that blog's SMB 3.0 and 3.1.1 key derivations, both
 *                 channels, and two blocks against the kit's own HMAC
 *   AES-128-CCM   RFC 3610 packet vector 1 (BearSSL's), on each AES
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <bearssl.h>

#include "crypto.h"

static int failures, checks;

static void same(const char *what, const uint8_t *got, const uint8_t *want,
                 size_t bytes)
{
    checks++;

    if (memcmp(got, want, bytes) != 0) {
        size_t i;

        failures++;
        printf("FAIL: %s\n  got  ", what);
        for (i = 0; i < bytes; i++) printf("%02x", got[i]);
        printf("\n  want ");
        for (i = 0; i < bytes; i++) printf("%02x", want[i]);
        printf("\n");
    }
}

static void unhex(const char *hex, uint8_t *out)
{
    while (hex[0] && hex[1]) {
        unsigned v;

        sscanf(hex, "%2x", &v);
        *out++ = (uint8_t)v;
        hex += 2;
    }
}

/*------------------------------------------------------------------------
 * The vectors, from the RFCs' own text.
 *----------------------------------------------------------------------*/

static const uint8_t chacha_key[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static const uint8_t chacha_nonce[12] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t chacha_plain[114] = {
    0x4c, 0x61, 0x64, 0x69, 0x65, 0x73, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x47,
    0x65, 0x6e, 0x74, 0x6c, 0x65, 0x6d, 0x65, 0x6e, 0x20, 0x6f, 0x66, 0x20,
    0x74, 0x68, 0x65, 0x20, 0x63, 0x6c, 0x61, 0x73, 0x73, 0x20, 0x6f, 0x66,
    0x20, 0x27, 0x39, 0x39, 0x3a, 0x20, 0x49, 0x66, 0x20, 0x49, 0x20, 0x63,
    0x6f, 0x75, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x66, 0x65, 0x72, 0x20, 0x79,
    0x6f, 0x75, 0x20, 0x6f, 0x6e, 0x6c, 0x79, 0x20, 0x6f, 0x6e, 0x65, 0x20,
    0x74, 0x69, 0x70, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x74, 0x68, 0x65, 0x20,
    0x66, 0x75, 0x74, 0x75, 0x72, 0x65, 0x2c, 0x20, 0x73, 0x75, 0x6e, 0x73,
    0x63, 0x72, 0x65, 0x65, 0x6e, 0x20, 0x77, 0x6f, 0x75, 0x6c, 0x64, 0x20,
    0x62, 0x65, 0x20, 0x69, 0x74, 0x2e,
};

static const uint8_t chacha_cipher[114] = {
    0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28,
    0xdd, 0x0d, 0x69, 0x81, 0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2,
    0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b, 0xf9, 0x1b, 0x65, 0xc5,
    0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
    0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35,
    0x9f, 0x08, 0x61, 0xd8, 0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61,
    0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e, 0x52, 0xbc, 0x51, 0x4d,
    0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
    0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed,
    0xf2, 0x78, 0x5e, 0x42, 0x87, 0x4d,
};

static const uint8_t poly_key[32] = {
    0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52, 0xfe,
    0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d, 0xb2, 0xfd,
    0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b,
};

static const uint8_t poly_msg[34] = {
    0x43, 0x72, 0x79, 0x70, 0x74, 0x6f, 0x67, 0x72, 0x61, 0x70, 0x68, 0x69,
    0x63, 0x20, 0x46, 0x6f, 0x72, 0x75, 0x6d, 0x20, 0x52, 0x65, 0x73, 0x65,
    0x61, 0x72, 0x63, 0x68, 0x20, 0x47, 0x72, 0x6f, 0x75, 0x70,
};

static const uint8_t poly_tag[16] = {
    0xa8, 0x06, 0x1d, 0xc1, 0x30, 0x51, 0x36, 0xc6, 0xc2, 0x2b, 0x8b, 0xaf,
    0x0c, 0x01, 0x27, 0xa9,
};

static const uint8_t x_scalar0[32] = {
    0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15, 0x4b,
    0x82, 0x46, 0x5e, 0xdd, 0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
    0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4,
};

static const uint8_t x_u0[32] = {
    0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1, 0xa4,
    0x24, 0xb1, 0x5f, 0x7c, 0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
    0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c,
};

static const uint8_t x_out0[32] = {
    0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90, 0x8e, 0x94, 0xea, 0x4d,
    0xf2, 0x8d, 0x08, 0x4f, 0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
    0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52,
};

static const uint8_t x_scalar1[32] = {
    0x4b, 0x66, 0xe9, 0xd4, 0xd1, 0xb4, 0x67, 0x3c, 0x5a, 0xd2, 0x26, 0x91,
    0x95, 0x7d, 0x6a, 0xf5, 0xc1, 0x1b, 0x64, 0x21, 0xe0, 0xea, 0x01, 0xd4,
    0x2c, 0xa4, 0x16, 0x9e, 0x79, 0x18, 0xba, 0x0d,
};

static const uint8_t x_u1[32] = {
    0xe5, 0x21, 0x0f, 0x12, 0x78, 0x68, 0x11, 0xd3, 0xf4, 0xb7, 0x95, 0x9d,
    0x05, 0x38, 0xae, 0x2c, 0x31, 0xdb, 0xe7, 0x10, 0x6f, 0xc0, 0x3c, 0x3e,
    0xfc, 0x4c, 0xd5, 0x49, 0xc7, 0x15, 0xa4, 0x93,
};

static const uint8_t x_out1[32] = {
    0x95, 0xcb, 0xde, 0x94, 0x76, 0xe8, 0x90, 0x7d, 0x7a, 0xad, 0xe4, 0x5c,
    0xb4, 0xb8, 0x73, 0xf8, 0x8b, 0x59, 0x5a, 0x68, 0x79, 0x9f, 0xa1, 0x52,
    0xe6, 0xf8, 0xf7, 0x64, 0x7a, 0xac, 0x79, 0x57,
};

static const uint8_t alice_private[32] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72,
    0x51, 0xb2, 0x66, 0x45, 0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
    0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};

static const uint8_t alice_public[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d, 0xdc,
    0xb4, 0x3e, 0xf7, 0x5a, 0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
    0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
};

static const uint8_t bob_private[32] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b,
    0x83, 0x80, 0x0e, 0xe6, 0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
    0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
};

static const uint8_t bob_public[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2,
    0xec, 0xe4, 0x35, 0x37, 0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
    0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
};

static const uint8_t shared[32] = {
    0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1, 0x72, 0x8e, 0x3b, 0xf4,
    0x80, 0x35, 0x0f, 0x25, 0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
    0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
};

static const uint8_t hmac1_key[20] = {
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
};

static const uint8_t hmac1_data[8] = {
    0x48, 0x69, 0x20, 0x54, 0x68, 0x65, 0x72, 0x65,
};

static const uint8_t hmac1_mac[32] = {
    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53, 0x5c, 0xa8, 0xaf, 0xce,
    0xaf, 0x0b, 0xf1, 0x2b, 0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
    0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
};

static const uint8_t hmac2_key[4] = {
    0x4a, 0x65, 0x66, 0x65,
};

static const uint8_t hmac2_data[28] = {
    0x77, 0x68, 0x61, 0x74, 0x20, 0x64, 0x6f, 0x20, 0x79, 0x61, 0x20, 0x77,
    0x61, 0x6e, 0x74, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x6e, 0x6f, 0x74, 0x68,
    0x69, 0x6e, 0x67, 0x3f,
};

static const uint8_t hmac2_mac[32] = {
    0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26,
    0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
    0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
};

static const uint8_t hmac6_key[131] = {
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
};

static const uint8_t hmac6_data[54] = {
    0x54, 0x65, 0x73, 0x74, 0x20, 0x55, 0x73, 0x69, 0x6e, 0x67, 0x20, 0x4c,
    0x61, 0x72, 0x67, 0x65, 0x72, 0x20, 0x54, 0x68, 0x61, 0x6e, 0x20, 0x42,
    0x6c, 0x6f, 0x63, 0x6b, 0x2d, 0x53, 0x69, 0x7a, 0x65, 0x20, 0x4b, 0x65,
    0x79, 0x20, 0x2d, 0x20, 0x48, 0x61, 0x73, 0x68, 0x20, 0x4b, 0x65, 0x79,
    0x20, 0x46, 0x69, 0x72, 0x73, 0x74,
};

static const uint8_t hmac6_mac[32] = {
    0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f, 0x0d, 0x8a, 0x26, 0xaa,
    0xcb, 0xf5, 0xb7, 0x7f, 0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28, 0xc5, 0x14,
    0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54,
};

/*------------------------------------------------------------------------
 * The checks.
 *----------------------------------------------------------------------*/

static void check_sha256(void)
{
    static const struct { const char *text, *digest; } fips[] = {
        { "abc",
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        { "",
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
    };
    uint8_t got[32], want[32];
    struct sha256 s;
    static uint8_t chunk[7];
    unsigned i, fed;

    for (i = 0; i < sizeof(fips) / sizeof(fips[0]); i++) {
        sha256(fips[i].text, strlen(fips[i].text), got);
        unhex(fips[i].digest, want);
        same(fips[i].text[0] ? fips[i].text : "sha256 of nothing", got, want, 32);
    }

    /* A million "a", seven bytes at a time: every way a piece can straddle
     * a 64-byte block is taken somewhere along it. */
    memset(chunk, 'a', sizeof(chunk));
    sha256_init(&s);

    for (fed = 0; fed + 7 <= 1000000; fed += 7) {
        sha256_update(&s, chunk, 7);
    }

    sha256_update(&s, chunk, 1000000 - fed);
    sha256_final(&s, got);
    unhex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", want);
    same("sha256 of a million a, in pieces of seven", got, want, 32);
}

static void check_hmac(void)
{
    uint8_t got[32];

    hmac_sha256(hmac1_key, sizeof(hmac1_key), hmac1_data, sizeof(hmac1_data), got);
    same("hmac-sha256, RFC 4231 case 1", got, hmac1_mac, 32);
    hmac_sha256(hmac2_key, sizeof(hmac2_key), hmac2_data, sizeof(hmac2_data), got);
    same("hmac-sha256, RFC 4231 case 2", got, hmac2_mac, 32);
    hmac_sha256(hmac6_key, sizeof(hmac6_key), hmac6_data, sizeof(hmac6_data), got);
    same("hmac-sha256, RFC 4231 case 6 (a key longer than a block)", got, hmac6_mac, 32);
}

static void check_chacha_poly(void)
{
    uint8_t got[sizeof(chacha_plain)];
    uint8_t tag[16];

    chacha20(chacha_key, 1, chacha_nonce, chacha_plain, got, sizeof(chacha_plain));
    same("chacha20, RFC 8439 2.4.2", got, chacha_cipher, sizeof(chacha_cipher));

    poly1305(poly_key, poly_msg, sizeof(poly_msg), tag);
    same("poly1305, RFC 8439 2.5.2", tag, poly_tag, 16);
}

static void check_x25519(void)
{
    uint8_t got[32];

    x25519(got, x_scalar0, x_u0);
    same("x25519, RFC 7748 5.2 first", got, x_out0, 32);
    x25519(got, x_scalar1, x_u1);
    same("x25519, RFC 7748 5.2 second", got, x_out1, 32);

    x25519_base(got, alice_private);
    same("x25519 base, Alice's public key (RFC 7748 6.1)", got, alice_public, 32);
    x25519_base(got, bob_private);
    same("x25519 base, Bob's public key", got, bob_public, 32);
    x25519(got, alice_private, bob_public);
    same("x25519, the secret as Alice has it", got, shared, 32);
    x25519(got, bob_private, alice_public);
    same("x25519, the secret as Bob has it", got, shared, 32);
}

static void check_des(void)
{
    static const uint8_t key[8] = { 0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1 };
    static const uint8_t plain[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    static const uint8_t cipher[8] = { 0x85, 0xe8, 0x13, 0x54, 0x0f, 0x0a, 0xb4, 0x05 };
    /* "Kosmos", each byte's bits reversed as VNC reverses them. */
    static const uint8_t vnc_key[8] = { 0xd2, 0xf6, 0xce, 0xb6, 0xf6, 0xce, 0x00, 0x00 };
    static const uint8_t vnc_answer[16] = {
        0x1f, 0x7e, 0xba, 0x60, 0xf2, 0x20, 0xa3, 0x11,
        0x43, 0x99, 0xf8, 0x6f, 0x38, 0xd2, 0xf6, 0xc1,
    };
    uint8_t got[16], challenge[16];
    unsigned i;

    des_encrypt(key, plain, got);
    same("des, FIPS 46's example", got, cipher, 8);

    for (i = 0; i < 16; i++) challenge[i] = (uint8_t)i;

    des_encrypt(vnc_key, challenge, got);
    des_encrypt(vnc_key, challenge + 8, got + 8);
    same("des, a VNC challenge answered under \"Kosmos\"", got, vnc_answer, 16);

    /*
     * **And every entry of every table.** The two above use a sliver of
     * the S-boxes - an entry is read only when its six bits come round -
     * and a wrong digit in one passed both of them. Four keys, 256 blocks
     * each, and the SHA-256 of all 8 KB of what came out, as OpenSSL's DES
     * gives it: some sixteen thousand lookups a box, so no entry is missed,
     * and a key schedule that is wrong for one key of four shows too.
     */
    {
        struct sha256 all;
        uint8_t k[8], b[8], c[8], digest[32], want[32];
        unsigned n, j;

        sha256_init(&all);

        for (n = 0; n < 4; n++) {
            for (j = 0; j < 8; j++) k[j] = (uint8_t)(n * 37 + j * 11 + 1);

            for (j = 0; j < 256; j++) {
                uint64_t v = (uint64_t)j * 0x9E3779B97F4A7C15ull + n;
                unsigned at;

                for (at = 0; at < 8; at++) b[at] = (uint8_t)(v >> (56 - 8 * at));

                des_encrypt(k, b, c);
                sha256_update(&all, c, 8);
            }
        }

        sha256_final(&all, digest);
        unhex("a3d4b10928dddc341e31b06db3c2907f8408c5d93467f08124b88d049b01afd8", want);
        same("des, four keys by 256 blocks, as OpenSSL ciphers them", digest, want, 32);
    }
}

/*
 * The generator: from a seed of 0..31, 64 bytes and then 16, as OpenSSL's
 * ChaCha20 keystream has them - the first request is keystream bytes 32 to
 * 96 under the seed, and the second, bytes 32 to 48 under the key the first
 * left, which was that keystream's first 32.
 */
static void check_drbg(void)
{
    uint8_t seed[32], got[64], want[64];
    struct drbg d;
    unsigned i;

    for (i = 0; i < 32; i++) seed[i] = (uint8_t)i;

    drbg_seed(&d, seed);
    drbg_generate(&d, got, 64);
    unhex("2b23cce7a26023ab3f0eef693ac87f64258235eab1f7a32dc22762a0485b410c"
          "18b84231ade6a6d113615c61af434e27f8b1f3f5e1ad5b5cecf8fc122a35755c", want);
    same("the generator's first 64 bytes, as OpenSSL's keystream", got, want, 64);

    drbg_generate(&d, got, 16);
    unhex("2d41a59c90e41a8e7a4dccaa1c460699", want);
    same("its next 16, under the key the first left", got, want, 16);
}

/*------------------------------------------------------------------------
 * What SMB 2/3 needs (`docs/sharing.md`, N1): MD4, AES-CMAC and the
 * SP 800-108 KDF written in the kit, and the BearSSL pieces SMB's sign-in
 * and sealing stand on, composed as SMB composes them. Each vector below
 * was cut out of its source's text by a script, not retyped.
 *----------------------------------------------------------------------*/

static void hmac_md5(const void *key, size_t key_bytes,
                     const void *data, size_t bytes, uint8_t out[16])
{
    br_hmac_key_context kc;
    br_hmac_context hc;

    br_hmac_key_init(&kc, &br_md5_vtable, key, key_bytes);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, data, bytes);
    br_hmac_out(&hc, out);
}

/* ASCII to UTF-16LE, as NTLM wants every string it hashes. */
static size_t utf16(const char *s, uint8_t *out)
{
    size_t n = 0;

    while (*s) {
        out[n++] = (uint8_t)*s++;
        out[n++] = 0;
    }
    return n;
}

/* RFC 1320 A.5, all seven. */
static void check_md4(void)
{
    static const char *const in[7] = {
        "", "a", "abc", "message digest", "abcdefghijklmnopqrstuvwxyz",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        ("1234567890123456789012345678901234567890"
         "1234567890123456789012345678901234567890"),
    };
    static const char *const want_hex[7] = {
        "31d6cfe0d16ae931b73c59d7e0c089c0",
        "bde52cb31de33e46245e05fbdbd6fb24",
        "a448017aaf21d8525fc10ae87aa6729d",
        "d9130a8164549fe818874806e1c7014b",
        "d79e1c308aa5bbcdeea8ed63df412da9",
        "043f8582f241db351ce627e153e7f0e4",
        "e33b4ddc9c38f2199c3e7b164fcc0536",
    };
    uint8_t got[16], want[16];
    char what[64];
    unsigned i;

    for (i = 0; i < 7; i++) {
        crypto_md4(in[i], strlen(in[i]), got);
        unhex(want_hex[i], want);
        snprintf(what, sizeof what, "md4, RFC 1320 A.5 vector %u", i + 1);
        same(what, got, want, 16);
    }
}

/*
 * NTLM's sign-in, as far as it is this kit's: the NT hash is MD4 of the
 * password in UTF-16 (NTOWFv1), and NTOWFv2 is HMAC-MD5 under it of the
 * user's name upper-cased and the domain, in UTF-16 - MD4 written here,
 * HMAC and MD5 BearSSL's. The rest of NTLMv2 is libsmb2's.
 *
 *   MS-NLMP 4.2.2.1.2 and 4.2.4.1.1, user "User", domain "Domain",
 *   password "Password":
 *     NTOWFv1 a4 f4 9c 40 65 10 bd ca b6 82 4e e7 c3 0f d8 52
 *     NTOWFv2 0c 86 8a 40 3b fd 7a 93 a3 00 1e f2 2e f0 2e 3f
 *   Microsoft's Open Specifications blog, "SMB 2 and SMB 3 security in
 *   Windows 10: the anatomy of signing and cryptographic keys", the NTLMv2
 *   SessionKey example: "Password01!", ADMINISTRATOR, SUT311:
 *     NtHash 7C4FE5EADA682714A036E39378362BAB
 *     NTOWFv2 AEE3959B44A815F1EB28C9511B4F533B
 */
static void check_ntlm(void)
{
    uint8_t pass[64], who[64], nt[16], got[16], want[16];
    size_t p, w;

    p = utf16("Password", pass);
    crypto_md4(pass, p, nt);
    unhex("a4f49c406510bdcab6824ee7c30fd852", want);
    same("NTOWFv1, MS-NLMP 4.2.2.1.2", nt, want, 16);

    w = utf16("USERDomain", who);
    hmac_md5(nt, 16, who, w, got);
    unhex("0c868a403bfd7a93a3001ef22ef02e3f", want);
    same("NTOWFv2, MS-NLMP 4.2.4.1.1 (MD4, then BearSSL's HMAC-MD5)", got, want, 16);

    p = utf16("Password01!", pass);
    crypto_md4(pass, p, nt);
    unhex("7C4FE5EADA682714A036E39378362BAB", want);
    same("the NT hash of Microsoft's SMB 3.1.1 sign-in", nt, want, 16);

    w = utf16("ADMINISTRATORSUT311", who);
    hmac_md5(nt, 16, who, w, got);
    unhex("AEE3959B44A815F1EB28C9511B4F533B", want);
    same("its NTOWFv2", got, want, 16);
}

/* RFC 2202's first two, so a broken HMAC-MD5 is named as that rather than
 * as NTLM. */
static void check_hmac_md5(void)
{
    uint8_t key[16], got[16], want[16];

    memset(key, 0x0b, 16);
    hmac_md5(key, 16, "Hi There", 8, got);
    unhex("9294727a3638bb1c13f48ef8158bfc9d", want);
    same("hmac-md5 (BearSSL's), RFC 2202 case 1", got, want, 16);

    hmac_md5("Jefe", 4, "what do ya want for nothing?", 28, got);
    unhex("750c783e6ab0b503eaa86e310a5db738", want);
    same("hmac-md5 (BearSSL's), RFC 2202 case 2", got, want, 16);
}

/* RFC 4493 4, all four: an empty message, one whole block, a partial last
 * block, and four whole blocks. The subkeys are not exposed, so a wrong K1
 * fails examples 2 and 4 and a wrong K2 fails 1 and 3. */
static void check_cmac(void)
{
    static const char msg_hex[] =
        "6bc1bee22e409f96e93d7e117393172a"
        "ae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52ef"
        "f69f2445df4f9b17ad2b417be66c3710";
    static const struct { size_t len; const char *mac; } ex[4] = {
        {  0, "bb1d6929e95937287fa37d129b756746" },
        { 16, "070a16b46b4d4144f79bdd9dd04a287c" },
        { 40, "dfa66747de9ae63030ca32611497c827" },
        { 64, "51f0bebf7e3b9d92fc49741779363cfe" },
    };
    uint8_t key[16], msg[64], got[16], want[16];
    char what[64];
    unsigned i;

    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex(msg_hex, msg);

    for (i = 0; i < 4; i++) {
        crypto_aes_cmac(key, 16, msg, ex[i].len, got);
        unhex(ex[i].mac, want);
        snprintf(what, sizeof what, "aes-cmac, RFC 4493 example %u (len %u)",
                 i + 1, (unsigned)ex[i].len);
        same(what, got, want, 16);
    }

    /*
     * And in pieces (step N4), as an SMB reply arrives: each example cut at
     * every place it can be cut in two, and the longest also fed a byte at
     * a time and in pieces of 15, 16 and 17 - the boundaries where holding
     * back the block that may be last is right or wrong.
     */
    for (i = 0; i < 4; i++) {
        size_t cut;
        int wrong = 0;

        unhex(ex[i].mac, want);

        for (cut = 0; cut <= ex[i].len; cut++) {
            struct crypto_cmac c;

            crypto_cmac_init(&c, key, 16);
            crypto_cmac_update(&c, msg, cut);
            crypto_cmac_update(&c, msg + cut, ex[i].len - cut);
            crypto_cmac_final(&c, got);
            wrong += memcmp(got, want, 16) != 0;
        }

        checks++;
        if (wrong) {
            failures++;
            printf("FAIL: aes-cmac in two pieces, RFC 4493 example %u: %d of "
                   "%u cuts wrong\n", i + 1, wrong, (unsigned)ex[i].len + 1);
        }
    }

    {
        static const size_t steps[] = { 1, 15, 16, 17 };

        unhex(ex[3].mac, want);

        for (i = 0; i < 4; i++) {
            struct crypto_cmac c;
            size_t at;

            crypto_cmac_init(&c, key, 16);
            for (at = 0; at < 64; at += steps[i])
                crypto_cmac_update(&c, msg + at, 64 - at < steps[i] ? 64 - at : steps[i]);
            crypto_cmac_final(&c, got);
            snprintf(what, sizeof what, "aes-cmac in pieces of %u, RFC 4493 example 4",
                     (unsigned)steps[i]);
            same(what, got, want, 16);
        }
    }
}

/*
 * SMB 3's keys, from Microsoft's Open Specifications blog, "SMB 2 and SMB 3
 * security in Windows 10: the anatomy of signing and cryptographic keys",
 * Appendix: Key derivation examples - both channels of each, so two
 * session keys under each set of labels. Labels and contexts as MS-SMB2
 * 3.2.5.3.1 gives them, terminating NULs included.
 *
 *   3.0, first channel, SessionKey 0x7CD451825D0450D235424E44BA6E78CC:
 *     SigningKey     0x0B7E9C5CAC36C0F6EA9AB275298CEDCE
 *     EncryptionKey  0xFAD27796665B313EBB578F388632B4F7
 *     DecryptionKey  0xB0F0427F7CEB416D1D9DCC0CD4F99447
 *     ApplicationKey 0xBB23A4575AA26C721AF525AF15A87B4F
 *   3.0, second channel, SessionKey 0x4E01A2B313BCF660CC250BEF021AEDE6:
 *     SigningKey     0xBA1A17DBBFEC349BCA105563D598952F
 *   3.1.1, first channel, SessionKey 0x270E1BA896585EEB7AF3472D3B4C75A7,
 *   preauthIntegrityHashValue 0DD13628...BCE3C6C01:
 *     SigningKey     0x73FE7A9A77BEF0BDE49C650D8CCB5F76
 *     EncryptionKey  0x629BCBC54422A0F572B97F45989B6073
 *     DecryptionKey  0xE2AF0DCEFAC68DA71A0DFBD0D1350D74
 *     ApplicationKey 0x6D7AD7954E9EC61E907B4D473DC178FF
 *   3.1.1, second channel, SessionKey 0x84B9DBB730116A8FA6E9889555C265F9,
 *   preauthIntegrityHashValue EA3BF912...389026F6C:
 *     SigningKey     0xC962BCA1A9DD1697B030644199705431
 *
 * "EncryptionKey" there is the client's: the 3.0 one is "ServerIn " and
 * the 3.1.1 one "SMBC2SCipherKey".
 */
static void kdf_check(const char *what, const char *session_hex,
                      const char *label, size_t label_bytes,
                      const void *context, size_t context_bytes,
                      const char *want_hex)
{
    uint8_t ki[16], got[16], want[16];

    unhex(session_hex, ki);
    unhex(want_hex, want);
    crypto_kdf_ctr_hmac_sha256(ki, 16, label, label_bytes,
                               context, context_bytes, got, 16);
    same(what, got, want, 16);
}

#define LIT(s) s, sizeof(s)            /* the bytes and the NUL */

static void check_kdf(void)
{
    static const char s30a[] = "7CD451825D0450D235424E44BA6E78CC";
    static const char s30b[] = "4E01A2B313BCF660CC250BEF021AEDE6";
    static const char s311a[] = "270E1BA896585EEB7AF3472D3B4C75A7";
    static const char s311b[] = "84B9DBB730116A8FA6E9889555C265F9";
    uint8_t ha[64], hb[64], big[48];

    unhex("0DD13628CC3ED218EF9DF9772D436D0887AB9814BFAE63A80AA845F36909DB79"
          "28622DDDAD522D9751640A459762C5A9D6BB084CBB3CE6BDADEF5D5BCE3C6C01", ha);
    unhex("EA3BF912B11CBFEC5B1889E8209614218687F82FA5294521AD3063425E49E88A"
          "10BD022124CE25123BC9111F52D9566BA88BF46344E6063DC5E3FF0389026F6C", hb);

    kdf_check("smb 3.0 SigningKey", s30a, LIT("SMB2AESCMAC"), LIT("SmbSign"),
              "0B7E9C5CAC36C0F6EA9AB275298CEDCE");
    kdf_check("smb 3.0 EncryptionKey (client)", s30a, LIT("SMB2AESCCM"),
              LIT("ServerIn "), "FAD27796665B313EBB578F388632B4F7");
    kdf_check("smb 3.0 DecryptionKey (client)", s30a, LIT("SMB2AESCCM"),
              LIT("ServerOut"), "B0F0427F7CEB416D1D9DCC0CD4F99447");
    kdf_check("smb 3.0 ApplicationKey", s30a, LIT("SMB2APP"), LIT("SmbRpc"),
              "BB23A4575AA26C721AF525AF15A87B4F");
    kdf_check("smb 3.0 SigningKey, second channel", s30b, LIT("SMB2AESCMAC"),
              LIT("SmbSign"), "BA1A17DBBFEC349BCA105563D598952F");

    kdf_check("smb 3.1.1 SigningKey", s311a, LIT("SMBSigningKey"), ha, 64,
              "73FE7A9A77BEF0BDE49C650D8CCB5F76");
    kdf_check("smb 3.1.1 EncryptionKey (client)", s311a, LIT("SMBC2SCipherKey"),
              ha, 64, "629BCBC54422A0F572B97F45989B6073");
    kdf_check("smb 3.1.1 DecryptionKey (client)", s311a, LIT("SMBS2CCipherKey"),
              ha, 64, "E2AF0DCEFAC68DA71A0DFBD0D1350D74");
    kdf_check("smb 3.1.1 ApplicationKey", s311a, LIT("SMBAppKey"), ha, 64,
              "6D7AD7954E9EC61E907B4D473DC178FF");
    kdf_check("smb 3.1.1 SigningKey, second channel", s311b, LIT("SMBSigningKey"),
              hb, 64, "C962BCA1A9DD1697B030644199705431");

    /*
     * More than one block, which no SMB vector asks for: 48 bytes are
     * HMAC(Ki, [1] || Label || 0 || Context || [384]) and the first 16 of
     * the same with [2] - held to the kit's own HMAC-SHA256 in `crypto.c`,
     * which is not BearSSL's, so neither can agree with a mistake of the
     * other's.
     */
    {
        static const uint8_t tail[] = "SMB2AESCMAC\0\0SmbSign\0\0\0\x01\x80";
        uint8_t ki[16], in[4 + sizeof(tail) - 1], want[64];

        unhex(s30a, ki);
        crypto_kdf_ctr_hmac_sha256(ki, 16, LIT("SMB2AESCMAC"), LIT("SmbSign"), big, 48);

        memcpy(in + 4, tail, sizeof(tail) - 1);
        in[0] = in[1] = in[2] = 0;
        in[3] = 1;
        hmac_sha256(ki, 16, in, sizeof in, want);
        in[3] = 2;
        hmac_sha256(ki, 16, in, sizeof in, want + 32);
        same("the KDF over two blocks, L = 384, against the kit's own HMAC",
             big, want, 48);
    }
}

/*
 * AES-128-CCM, BearSSL's, over the kit's choice of AES: RFC 3610's packet
 * vector 1 (13-byte nonce, 8 bytes of header, an 8-byte tag), sealed,
 * opened, and refused with one bit of it changed - through `aes_ct64`,
 * and through AES-NI as well where the processor has it.
 */
static void ccm_with(const char *name, const br_block_ctrcbc_class *vt)
{
    br_aes_gen_ctrcbc_keys aes;
    br_ccm_context cc;
    uint8_t key[16], nonce[13], packet[31], want[39], tag[8];
    char what[96];
    int bad;

    unhex("C0C1C2C3C4C5C6C7C8C9CACBCCCDCECF", key);
    unhex("00000003020100A0A1A2A3A4A5", nonce);
    unhex("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E", packet);
    unhex("0001020304050607588C979A61C663D2F066D0C2C0F989806D5F6B61DAC38417"
          "E8D12CFDF926E0", want);

    vt->init(&aes.vtable, key, 16);
    br_ccm_init(&cc, &aes.vtable);
    br_ccm_reset(&cc, nonce, 13, 8, 23, 8);
    br_ccm_aad_inject(&cc, packet, 8);
    br_ccm_flip(&cc);
    br_ccm_run(&cc, 1, packet + 8, 23);
    br_ccm_get_tag(&cc, tag);
    snprintf(what, sizeof what, "aes-128-ccm sealed, RFC 3610 vector 1 (%s)", name);
    same(what, packet + 8, want + 8, 23);
    snprintf(what, sizeof what, "aes-128-ccm tag, RFC 3610 vector 1 (%s)", name);
    same(what, tag, want + 31, 8);

    br_ccm_reset(&cc, nonce, 13, 8, 23, 8);
    br_ccm_aad_inject(&cc, want, 8);
    br_ccm_flip(&cc);
    br_ccm_run(&cc, 0, packet + 8, 23);
    checks++;
    if (!br_ccm_check_tag(&cc, want + 31)) {
        failures++;
        printf("FAIL: aes-128-ccm refused its own vector (%s)\n", name);
    }
    unhex("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E", want);
    snprintf(what, sizeof what, "aes-128-ccm opened (%s)", name);
    same(what, packet + 8, want + 8, 23);

    /* One bit of the header changed: the tag must not check. */
    unhex("0001020304050607588C979A61C663D2F066D0C2C0F989806D5F6B61DAC38417"
          "E8D12CFDF926E0", want);
    want[3] ^= 0x10;
    br_ccm_reset(&cc, nonce, 13, 8, 23, 8);
    br_ccm_aad_inject(&cc, want, 8);
    br_ccm_flip(&cc);
    br_ccm_run(&cc, 0, want + 8, 23);
    bad = br_ccm_check_tag(&cc, want + 31);
    checks++;
    if (bad) {
        failures++;
        printf("FAIL: aes-128-ccm accepted a changed header (%s)\n", name);
    }
}

static const char *aes_used;

static void check_ccm(void)
{
    const br_block_ctrcbc_class *ni = br_aes_x86ni_ctrcbc_get_vtable();

    ccm_with("aes_ct64", &br_aes_ct64_ctrcbc_vtable);
    if (ni != NULL) ccm_with("aes_x86ni", ni);

    aes_used = crypto_aes_ctrcbc() == ni ? "AES-NI" : "aes_ct64";
}

int main(void)
{
    check_sha256();
    check_hmac();
    check_chacha_poly();
    check_x25519();
    check_des();
    check_drbg();
    check_md4();
    check_hmac_md5();
    check_ntlm();
    check_cmac();
    check_kdf();
    check_ccm();

    if (failures) {
        printf("FAIL: %d of %d checks on the Crypto Kit's primitives\n",
               failures, checks);
        return 1;
    }

    printf("PASS: %d checks on the Crypto Kit's primitives against their "
           "specifications' vectors (SHA-256, HMAC-SHA-256, ChaCha20, "
           "Poly1305, X25519, DES, the generator, and SMB's MD4, NTLM, "
           "HMAC-MD5, AES-CMAC, SP 800-108 KDF and AES-CCM - AES by %s)\n",
           checks, aes_used);
    return 0;
}
