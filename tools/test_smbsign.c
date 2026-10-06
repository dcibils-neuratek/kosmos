/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * SMB's signatures as Kosmos computes them, held to libsmb2's own
 * (`docs/sharing.md` step N4; `user/kits/smb/smb_signing.c`).
 *
 * `smb_signing.c` replaces libsmb2's `smb2-signing.c` in the machine's build,
 * so the reference here is that very file, compiled beside it with its
 * names given a `ref_` prefix (on the compile line - nothing is edited) and
 * standing on libsmb2's own AES and SHA-256, not on the kit's or BearSSL's.
 * Two implementations that share no code agree, or this fails:
 *
 *   - RFC 4493 section 4's four examples, through `smb3_aes_cmac_128` -
 *     both files' - and against the RFC's own MACs;
 *   - `smb2_calc_signature` at every dialect libsmb2 speaks - HMAC-SHA256
 *     to 2.1, AES-CMAC after - over messages of a header and a body of many
 *     lengths, cut into vectors every way that matters to a CMAC that holds
 *     back the block which may be last: a body in one vector, in two at
 *     every cut near a block's edge, a byte at a time, and a megabyte READ
 *     as libsmb2 lays one out (header, fixed part, data);
 *   - `smb2_pdu_add_signature`: the flag set, the signature written into
 *     the header's own bytes and its structure, and the messages that are
 *     not signed - a SESSION_SETUP still going, no session yet - left alone.
 *
 * Natively, where the kit's AES is `aes_ct64`, and through Rosetta as
 * `test_smbsign_x86`, where it is AES-NI. And what a signed megabyte costs
 * each way, printed: not a check - a number of this Mac's, for `testing.md`.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <bearssl.h>

#include "config.h"
#include "compat.h"
#include "smb2-signing.h"

#include "crypto.h"

void smb3_aes_cmac_128(uint8_t key[16], uint8_t *msg, uint64_t msg_len, uint8_t mac[16]);
void ref_smb3_aes_cmac_128(uint8_t key[16], uint8_t *msg, uint64_t msg_len, uint8_t mac[16]);
int ref_smb2_calc_signature(struct smb2_context *smb2, uint8_t *signature,
                            struct smb2_iovec *iov, size_t niov);
int ref_smb2_pdu_add_signature(struct smb2_context *smb2, struct smb2_pdu *pdu);

static int failures, checks;

/* What both files call of the rest of libsmb2, as `pdu.c` and `init.c`
 * have them. */
void smb2_set_error(struct smb2_context *smb2, const char *error_string, ...)
{
    (void)smb2;
    (void)error_string;
}

int smb2_set_uint32(struct smb2_iovec *iov, int offset, uint32_t value)
{
    if (offset + sizeof(uint32_t) > iov->len) {
        return -1;
    }

    iov->buf[offset] = (uint8_t)value;
    iov->buf[offset + 1] = (uint8_t)(value >> 8);
    iov->buf[offset + 2] = (uint8_t)(value >> 16);
    iov->buf[offset + 3] = (uint8_t)(value >> 24);
    return 0;
}

static void same(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    checks++;

    if (memcmp(got, want, n) != 0) {
        failures++;
        printf("FAIL: %s\n", what);
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

/* A small generator, so every run cuts the same messages the same way. */
static uint64_t seed = 0x9e3779b97f4a7c15u;

static uint8_t next(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return (uint8_t)(seed >> 32);
}

static void check_rfc4493(void)
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
    uint8_t key[16], msg[64], got[16], ref[16], want[16];
    char what[96];
    unsigned i;

    unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
    unhex(msg_hex, msg);

    for (i = 0; i < 4; i++) {
        unhex(ex[i].mac, want);
        smb3_aes_cmac_128(key, msg, ex[i].len, got);
        ref_smb3_aes_cmac_128(key, msg, ex[i].len, ref);
        snprintf(what, sizeof what, "smb3_aes_cmac_128, RFC 4493 example %u", i + 1);
        same(what, got, want, 16);
        snprintf(what, sizeof what, "libsmb2's own, RFC 4493 example %u", i + 1);
        same(what, ref, want, 16);
    }
}

/*
 * One message - a 64-byte header and `body` bytes - signed by both, its
 * body given as the vectors `cuts` says (each a length; what is left over
 * is a last vector). Each side gets its own copy, since both clear the
 * header's signature field and the reference copies the message whole.
 */
static int signed_alike(struct smb2_context *smb2, const uint8_t *message,
                        size_t body, const size_t *cuts, size_t ncuts)
{
    struct smb2_iovec a[40], b[40];
    uint8_t *ma = malloc(64 + body), *mb = malloc(64 + body);
    uint8_t sa[16], sb[16];
    size_t n = 0, at = 64, i;
    int alike;

    memcpy(ma, message, 64 + body);
    memcpy(mb, message, 64 + body);
    a[n].buf = ma; b[n].buf = mb; a[n].len = b[n].len = 64; n++;

    for (i = 0; i < ncuts && at < 64 + body; i++) {
        size_t len = cuts[i] < 64 + body - at ? cuts[i] : 64 + body - at;

        a[n].buf = ma + at; b[n].buf = mb + at; a[n].len = b[n].len = len;
        at += len;
        n++;
    }

    if (at < 64 + body) {
        a[n].buf = ma + at; b[n].buf = mb + at; a[n].len = b[n].len = 64 + body - at;
        n++;
    }

    smb2_calc_signature(smb2, sa, a, n);
    ref_smb2_calc_signature(smb2, sb, b, n);
    alike = memcmp(sa, sb, 16) == 0 && memcmp(ma, mb, 64 + body) == 0;
    free(ma);
    free(mb);
    return alike;
}

static void check_signatures(void)
{
    static const uint16_t dialects[] = { SMB2_VERSION_0202, SMB2_VERSION_0210,
                                         SMB2_VERSION_0300, SMB2_VERSION_0302,
                                         SMB2_VERSION_0311 };
    static const size_t bodies[] = { 0, 1, 15, 16, 17, 31, 32, 33, 48, 100, 4096, 65536 + 7 };
    static struct smb2_context smb2;
    size_t big = (1u << 20) + 16;
    uint8_t *message = malloc(64 + big);
    unsigned d, k, wrong;
    size_t i, cut;
    char what[128];

    for (i = 0; i < 64 + big; i++) message[i] = next();

    for (d = 0; d < sizeof dialects / sizeof dialects[0]; d++) {
        smb2.dialect = dialects[d];
        for (i = 0; i < SMB2_KEY_SIZE; i++) smb2.signing_key[i] = next();

        /* Every body length in one vector, and in two at every cut. */
        wrong = 0;
        for (k = 0; k < sizeof bodies / sizeof bodies[0]; k++) {
            size_t body = bodies[k];

            wrong += !signed_alike(&smb2, message, body, NULL, 0);

            for (cut = 0; cut <= body && cut <= 70; cut++) {
                wrong += !signed_alike(&smb2, message, body, &cut, 1);
            }
        }

        checks++;
        if (wrong) {
            failures++;
            printf("FAIL: smb2_calc_signature at 0x%04x: %u of the messages "
                   "in one or two vectors signed differently\n", dialects[d], wrong);
        }

        /* A byte at a time, and in pieces of 15 and 17, crossing every edge. */
        {
            size_t ones[38], fifteens[38], seventeens[38];

            for (i = 0; i < 38; i++) { ones[i] = 1; fifteens[i] = 15; seventeens[i] = 17; }
            snprintf(what, sizeof what, "smb2_calc_signature at 0x%04x, a body "
                     "of 100 in pieces of 1, 15 and 17", dialects[d]);
            checks++;
            if (!signed_alike(&smb2, message, 37, ones, 38)
                || !signed_alike(&smb2, message, 100, fifteens, 38)
                || !signed_alike(&smb2, message, 100, seventeens, 38)) {
                failures++;
                printf("FAIL: %s\n", what);
            }
        }

        /* A megabyte READ as libsmb2 lays one out: the header, the reply's
         * fixed 16 bytes, and the data in the caller's buffer. */
        {
            size_t fixed = 16;

            snprintf(what, sizeof what, "smb2_calc_signature at 0x%04x, a "
                     "megabyte READ in three vectors", dialects[d]);
            checks++;
            if (!signed_alike(&smb2, message, big, &fixed, 1)) {
                failures++;
                printf("FAIL: %s\n", what);
            }
        }
    }

    free(message);
}

/* A message to go, as `pdu.c` would hand it to be signed. */
static void make_pdu(struct smb2_pdu *pdu, uint8_t *header, uint8_t *body,
                     uint16_t command, uint32_t status, uint32_t flags)
{
    memset(pdu, 0, sizeof(*pdu));
    pdu->header.command = command;
    pdu->header.status = status;
    pdu->header.flags = flags;
    pdu->out.niov = 2;
    pdu->out.iov[0].buf = header;
    pdu->out.iov[0].len = SMB2_HEADER_SIZE;
    pdu->out.iov[1].buf = body;
    pdu->out.iov[1].len = 40;
}

static void check_add(void)
{
    static const struct { uint16_t command; uint32_t status, flags; uint64_t session;
                          const char *what; } cases[] = {
        { SMB2_READ,          0, 0, 0x1122334455667788u, "a READ in a session" },
        { SMB2_TREE_CONNECT,  0, 0, 0x1122334455667788u, "a TREE_CONNECT" },
        { SMB2_SESSION_SETUP, 0, 0, 0x1122334455667788u, "a SESSION_SETUP asked" },
        { SMB2_SESSION_SETUP, 0, SMB2_FLAGS_SERVER_TO_REDIR, 0x1122334455667788u,
          "a SESSION_SETUP answered" },
        { SMB2_READ,          0, 0, 0, "a READ with no session yet" },
    };
    static const uint16_t dialects[] = { SMB2_VERSION_0210, SMB2_VERSION_0311 };
    static struct smb2_context smb2;
    static uint8_t key_held[16];
    unsigned c, d, i;

    for (d = 0; d < 2; d++) {
        smb2.dialect = dialects[d];
        smb2.session_key = key_held;
        smb2.session_key_size = 16;
        for (i = 0; i < SMB2_KEY_SIZE; i++) smb2.signing_key[i] = next();

        for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            struct smb2_pdu pa, pb;
            uint8_t ha[64], hb[64], ba[40], bb[40];
            int ra, rb;
            char what[128];

            for (i = 0; i < 64; i++) ha[i] = hb[i] = next();
            for (i = 0; i < 40; i++) ba[i] = bb[i] = next();
            smb2.session_id = cases[c].session;
            make_pdu(&pa, ha, ba, cases[c].command, cases[c].status, cases[c].flags);
            make_pdu(&pb, hb, bb, cases[c].command, cases[c].status, cases[c].flags);

            ra = smb2_pdu_add_signature(&smb2, &pa);
            rb = ref_smb2_pdu_add_signature(&smb2, &pb);

            snprintf(what, sizeof what, "smb2_pdu_add_signature at 0x%04x, %s",
                     dialects[d], cases[c].what);
            checks++;
            if (ra != rb || memcmp(ha, hb, 64) != 0 || memcmp(ba, bb, 40) != 0
                || pa.header.flags != pb.header.flags
                || memcmp(pa.header.signature, pb.header.signature, 16) != 0) {
                failures++;
                printf("FAIL: %s\n", what);
            }
        }
    }
}

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* What signing a megabyte READ costs here, for the record - not a check. */
static void cost(void)
{
    static struct smb2_context smb2;
    size_t mb = 1u << 20;
    uint8_t *m = calloc(1, 64 + 16 + mb), sig[16];
    struct smb2_iovec iov[3] = { { m, 64, NULL }, { m + 64, 16, NULL },
                                 { m + 80, mb, NULL } };
    uint16_t dialect[2] = { SMB2_VERSION_0210, SMB2_VERSION_0311 };
    double ms[2];
    int d, i, reps = 8;

    for (d = 0; d < 2; d++) {
        double t = now();

        smb2.dialect = dialect[d];
        for (i = 0; i < reps; i++) smb2_calc_signature(&smb2, sig, iov, 3);
        ms[d] = (now() - t) * 1000 / reps;
    }

    printf("  a signed megabyte here: %.2f ms with HMAC-SHA256 (2.x), %.2f ms "
           "with AES-CMAC (3.x)\n", ms[0], ms[1]);
    free(m);
}

int main(void)
{
    const char *aes = br_aes_x86ni_ctrcbc_get_vtable() != NULL ? "AES-NI" : "aes_ct64";

    check_rfc4493();
    check_signatures();
    check_add();

    if (failures) {
        printf("FAIL: %d of %d checks on SMB's signatures against libsmb2's own\n",
               failures, checks);
        return 1;
    }

    printf("PASS: %d checks on SMB's signatures, Kosmos's against libsmb2's own "
           "(RFC 4493; every dialect, messages in vectors cut every way; "
           "signing a message to go) - AES by %s\n", checks, aes);
    cost();
    return 0;
}
