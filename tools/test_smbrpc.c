/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The share list over IPC$'s srvsvc pipe, on the Mac (`user/servers/smbrpc.c`,
 * `docs/sharing.md` N12d): a BIND for srvsvc in NDR accepted, the bind time
 * feature negotiation acknowledged with no features, another interface
 * refused; NetrShareEnum at levels 1 and 0, every share's name read back out
 * of the NDR; NetrShareGetInfo by a name in another case, and one not
 * shared answered "not found"; NetrServerGetInfo's name; an operation it does
 * not know a fault "out of range"; half a PDU waited for; something that is
 * not DCE/RPC refused; and an answer that would not fit refused, not cut.
 */

#include "smbrpc.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int checks, fails;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("  %s\n", what);
    }
}

static const uint8_t SRVSVC[16] = { 0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01,
                                    0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88 };
static const uint8_t NDR32[16] = { 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11,
                                   0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60 };
static const uint8_t FEATURES[16] = { 0x2c, 0x1c, 0xb7, 0x6c, 0x12, 0x98, 0x40, 0x45,
                                      0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t LSARPC[16] = { 0x78, 0x57, 0x34, 0x12, 0x34, 0x12, 0xcd, 0xab,
                                    0xef, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab };
static const uint8_t OTHER[16] = { 0x78, 0x57, 0x34, 0x12, 0x34, 0x12, 0xcd, 0xab,
                                   0xef, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab };

struct buf { uint8_t b[4096]; size_t n; };

static void u8(struct buf *x, unsigned v) { x->b[x->n++] = (uint8_t)v; }
static void u16(struct buf *x, unsigned v) { u8(x, v & 0xff); u8(x, v >> 8); }
static void u32(struct buf *x, uint32_t v) { u16(x, v & 0xffff); u16(x, v >> 16); }
static void bytes(struct buf *x, const uint8_t *p, size_t n) { memcpy(x->b + x->n, p, n); x->n += n; }
static void pad4(struct buf *x, size_t base) { while ((x->n - base) % 4) u8(x, 0); }

static void header(struct buf *x, unsigned ptype, uint32_t call)
{
    u8(x, 5); u8(x, 0); u8(x, ptype); u8(x, 3);
    u8(x, 0x10); u8(x, 0); u8(x, 0); u8(x, 0);
    u16(x, 0); u16(x, 0); u32(x, call);
}

static void length(struct buf *x)
{
    x->b[8] = (uint8_t)x->n;
    x->b[9] = (uint8_t)(x->n >> 8);
}

static void string(struct buf *x, const char *s, size_t base)
{
    size_t n = strlen(s) + 1, i;

    pad4(x, base);
    u32(x, (uint32_t)n); u32(x, 0); u32(x, (uint32_t)n);
    for (i = 0; i < n; i++) u16(x, (uint8_t)s[i]);
    pad4(x, base);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A conformant varying string out of the answer, as ASCII. */
static size_t take_string(const uint8_t *p, size_t at, size_t base, char *out, size_t room)
{
    uint32_t n, i;

    while ((at - base) % 4) at++;
    n = rd32(p + at + 8);
    at += 12;
    for (i = 0; i < n && i + 1 < room; i++) out[i] = (char)p[at + 2 * i];
    out[i < room ? i : room - 1] = '\0';
    at += 2u * n;
    while ((at - base) % 4) at++;
    return at;
}

static const struct smb_rpc_share SHARES[] = {
    { "Projects", SMB_RPC_DISK, "" },
    { "Music",    SMB_RPC_DISK, "" },
    { "IPC$",     SMB_RPC_IPC,  "IPC Service" },
};
static const struct smb_rpc_server SERVER = { SMB_RPC_SRVSVC, "KOSMOS-TEST", SHARES, 3, NULL, NULL };

static uint8_t out[8192];

static size_t ask(const struct buf *in, int *rc, size_t *in_used, size_t room)
{
    size_t used = 0;

    *rc = smb_rpc_answer(&SERVER, in->b, in->n, in_used, out, room, &used);
    return used;
}

/* A request: opnum, then its stub as `fill` writes it. */
static void request(struct buf *x, unsigned opnum)
{
    header(x, 0, 7);
    u32(x, 0); u16(x, 0); u16(x, opnum);
}

int main(void)
{
    struct buf x;
    size_t n, used, at, stub;
    int rc;
    char name[64];

    /* BIND: srvsvc in NDR, the feature negotiation, and a stranger. */
    memset(&x, 0, sizeof x);
    header(&x, 11, 1);
    u16(&x, 4280); u16(&x, 4280); u32(&x, 0);
    u8(&x, 3); u8(&x, 0); u16(&x, 0);
    u16(&x, 0); u8(&x, 1); u8(&x, 0); bytes(&x, SRVSVC, 16); u32(&x, 3); bytes(&x, NDR32, 16); u32(&x, 2);
    u16(&x, 1); u8(&x, 1); u8(&x, 0); bytes(&x, SRVSVC, 16); u32(&x, 3); bytes(&x, FEATURES, 16); u32(&x, 1);
    u16(&x, 2); u8(&x, 1); u8(&x, 0); bytes(&x, OTHER, 16); u32(&x, 1); bytes(&x, NDR32, 16); u32(&x, 2);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    check(rc == 0 && used == x.n && out[2] == 12 && rd16(out + 8) == n,
          "a BIND was not answered with a BIND_ACK of its own length");
    at = 16 + 8;
    at += 2 + rd16(out + at);           /* the secondary address */
    while (at % 4) at++;
    check(out[at] == 3, "the BIND_ACK did not answer three contexts");
    at += 4;
    check(rd16(out + at) == 0 && memcmp(out + at + 4, NDR32, 16) == 0,
          "srvsvc in NDR was not accepted");
    check(rd16(out + at + 24) == 3 && rd16(out + at + 26) == 0,
          "the bind time feature negotiation was not acknowledged with none");
    check(rd16(out + at + 48) == 2 && rd16(out + at + 50) == 1,
          "another interface was not refused as an abstract syntax not supported");

    /* NetrShareEnum, level 1: every share, with its type. */
    memset(&x, 0, sizeof x);
    request(&x, 15);
    stub = x.n;
    u32(&x, 0x20000); string(&x, "\\\\127.0.0.1", stub);
    u32(&x, 1); u32(&x, 1); u32(&x, 0x20004); u32(&x, 0); u32(&x, 0);
    u32(&x, 0xffffffff); u32(&x, 0);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    check(rc == 0 && out[2] == 2 && rd16(out + 8) == n && rd32(out + 16) == n - 24,
          "NetrShareEnum was not answered with a RESPONSE whose sizes agree");
    stub = 24;
    check(rd32(out + stub) == 1 && rd32(out + stub + 4) == 1 && rd32(out + stub + 12) == 3
          && rd32(out + stub + 20) == 3,
          "level 1's container did not hold three shares");
    check(rd32(out + stub + 28) == SMB_RPC_DISK && rd32(out + stub + 52) == SMB_RPC_IPC,
          "the shares' types were not disk, disk and IPC");
    at = stub + 24 + 3 * 12;
    {
        char seen[3][16];
        char comment[16];
        int i;

        for (i = 0; i < 3; i++) {
            at = take_string(out, at, stub, seen[i], sizeof seen[i]);
            at = take_string(out, at, stub, comment, sizeof comment);
        }

        check(strcmp(seen[0], "Projects") == 0 && strcmp(seen[1], "Music") == 0
              && strcmp(seen[2], "IPC$") == 0,
              "the names read back were not Projects, Music and IPC$");
        check(rd32(out + at) == 3 && rd32(out + at + 12) == 0,
              "TotalEntries and WERR_OK did not follow the strings");
    }

    /* Level 0: names alone. */
    memset(&x, 0, sizeof x);
    request(&x, 15);
    stub = x.n;
    u32(&x, 0);
    u32(&x, 0); u32(&x, 0); u32(&x, 0x20004); u32(&x, 0); u32(&x, 0);
    u32(&x, 0xffffffff); u32(&x, 0);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    at = 24 + 24 + 3 * 4;
    at = take_string(out, at, 24, name, sizeof name);
    check(rc == 0 && rd32(out + 24) == 0 && strcmp(name, "Projects") == 0,
          "level 0 did not begin with Projects' name");

    /* NetrShareGetInfo: a share by a name in another case, then one that is
     * not shared. */
    memset(&x, 0, sizeof x);
    request(&x, 16);
    stub = x.n;
    u32(&x, 0); string(&x, "music", stub); u32(&x, 1);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    at = take_string(out, 24 + 8 + 12, 24, name, sizeof name);
    check(rc == 0 && rd32(out + 24) == 1 && rd32(out + 24 + 4) != 0 && strcmp(name, "Music") == 0,
          "NetrShareGetInfo did not find music as Music");

    memset(&x, 0, sizeof x);
    request(&x, 16);
    stub = x.n;
    u32(&x, 0); string(&x, "Private", stub); u32(&x, 1);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    check(rc == 0 && rd32(out + 28) == 0 && rd32(out + 32) == 0x906,
          "a share not offered was not WERR_NET_NAME_NOT_FOUND");

    /* NetrServerGetInfo 101: the name. */
    memset(&x, 0, sizeof x);
    request(&x, 21);
    stub = x.n;
    u32(&x, 0); u32(&x, 101);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    at = take_string(out, 24 + 32, 24, name, sizeof name);
    check(rc == 0 && rd32(out + 24) == 101 && rd32(out + 32) == 500
          && strcmp(name, "KOSMOS-TEST") == 0,
          "NetrServerGetInfo 101 did not say this machine's name");

    /* An operation it does not know: a fault, out of range. */
    memset(&x, 0, sizeof x);
    request(&x, 40);
    u32(&x, 0);
    length(&x);
    n = ask(&x, &rc, &used, sizeof out);
    check(rc == 0 && out[2] == 3 && rd32(out + 24) == 0x1c010002,
          "an unknown operation was not a fault, operation out of range");

    /* Half a PDU: waited for. Not DCE/RPC: refused. */
    memset(&x, 0, sizeof x);
    request(&x, 15);
    u32(&x, 0);
    length(&x);
    x.n -= 2;
    n = ask(&x, &rc, &used, sizeof out);
    check(rc == 0 && used == 0 && n == 0, "half a PDU was not waited for");

    x.b[0] = 4;
    x.n += 2;
    (void)ask(&x, &rc, &used, sizeof out);
    check(rc == -1, "a PDU that is not DCE/RPC 5 was not refused");

    /* An answer with no room: refused, not cut. */
    memset(&x, 0, sizeof x);
    request(&x, 15);
    stub = x.n;
    u32(&x, 0); u32(&x, 1); u32(&x, 1); u32(&x, 0x20004); u32(&x, 0); u32(&x, 0);
    u32(&x, 0xffffffff); u32(&x, 0);
    length(&x);
    (void)ask(&x, &rc, &used, 100);
    check(rc == -1, "an answer too big for its room was not refused");

    /*
     * lsarpc, which the Mac's Finder opens after the share list to learn
     * who it signed in as (MS-LSAT).
     */
    {
        int unknown = -1;
        const struct smb_rpc_server lsa = { SMB_RPC_LSARPC, "KOSMOS-TEST", SHARES, 3,
                                            "diego", &unknown };
        size_t u;

        memset(&x, 0, sizeof x);
        header(&x, 11, 1);
        u16(&x, 4280); u16(&x, 4280); u32(&x, 0);
        u8(&x, 1); u8(&x, 0); u16(&x, 0);
        u16(&x, 0); u8(&x, 1); u8(&x, 0); bytes(&x, LSARPC, 16); u32(&x, 0); bytes(&x, NDR32, 16); u32(&x, 2);
        length(&x);
        u = 0;
        rc = smb_rpc_answer(&lsa, x.b, x.n, &used, out, sizeof out, &u);
        at = 16 + 8;
        check(rc == 0 && rd16(out + at) == 12 && memcmp(out + at + 2, "\\PIPE\\lsass", 12) == 0,
              "the lsarpc BIND_ACK did not name \\PIPE\\lsass");
        at += 2 + rd16(out + at);
        while (at % 4) at++;
        check(out[at] == 1 && rd16(out + at + 4) == 0, "lsarpc in NDR was not accepted");

        /* LsarOpenPolicy2: a handle, success. */
        memset(&x, 0, sizeof x);
        request(&x, 44);
        u32(&x, 0); u32(&x, 24); u32(&x, 0); u32(&x, 0); u32(&x, 0); u32(&x, 0); u32(&x, 0);
        u32(&x, 0x02000000);
        length(&x);
        u = 0;
        rc = smb_rpc_answer(&lsa, x.b, x.n, &used, out, sizeof out, &u);
        check(rc == 0 && out[2] == 2 && memcmp(out + 28, (uint8_t[16]){ 0 }, 16) != 0
              && rd32(out + 44) == 0, "LsarOpenPolicy2 did not answer a handle and success");

        /* LsarGetUserName: diego, and this machine. */
        memset(&x, 0, sizeof x);
        request(&x, 45);
        u32(&x, 0); u32(&x, 0); u32(&x, 0x20000); u32(&x, 0);
        length(&x);
        u = 0;
        rc = smb_rpc_answer(&lsa, x.b, x.n, &used, out, sizeof out, &u);
        {
            char user[16] = "", domain[16] = "";
            size_t i, n = rd32(out + 24 + 12 + 8);

            for (i = 0; i < n && i + 1 < sizeof user; i++) user[i] = (char)out[24 + 12 + 12 + 2 * i];
            at = 24 + 12 + 12 + 2 * n;
            while ((at - 24) % 4) at++;
            at += 8 + 8;                    /* DomainName's two pointers, its fields */
            n = rd32(out + at + 8);
            for (i = 0; i < n && i + 1 < sizeof domain; i++) domain[i] = (char)out[at + 12 + 2 * i];
            check(rc == 0 && strcmp(user, "diego") == 0 && strcmp(domain, "KOSMOS-TEST") == 0,
                  "LsarGetUserName did not say diego of KOSMOS-TEST");
        }

        /* LsarLookupNames: DIEGO a user, RID 1000; somebody else not mapped. */
        memset(&x, 0, sizeof x);
        request(&x, 14);
        stub = x.n;
        bytes(&x, (uint8_t[20]){ 0 }, 20);
        u32(&x, 2);
        u32(&x, 2);
        u16(&x, 10); u16(&x, 10); u32(&x, 0x20000);
        u16(&x, 18); u16(&x, 18); u32(&x, 0x20004);
        {
            const char *names[2] = { "DIEGO", "somebody" };
            int k;
            size_t i;

            for (k = 0; k < 2; k++) {
                size_t n2 = strlen(names[k]);

                pad4(&x, stub);
                u32(&x, (uint32_t)n2); u32(&x, 0); u32(&x, (uint32_t)n2);
                for (i = 0; i < n2; i++) u16(&x, (uint8_t)names[k][i]);
            }
        }
        pad4(&x, stub);
        u32(&x, 0); u32(&x, 0); u16(&x, 1); u16(&x, 0); u32(&x, 0);
        length(&x);
        u = 0;
        rc = smb_rpc_answer(&lsa, x.b, x.n, &used, out, sizeof out, &u);
        {
            size_t end = u;

            /* The tail: TranslatedSids' two entries, MappedCount, the status. */
            check(rc == 0 && rd32(out + end - 4) == 0x00000107u && rd32(out + end - 8) == 1
                  && rd16(out + end - 8 - 24) == 1 && rd32(out + end - 8 - 20) == 1000
                  && rd16(out + end - 8 - 12) == 8,
                  "LsarLookupNames did not map DIEGO to RID 1000 and leave somebody unknown");
        }

        /* Something lsarpc does not answer: a fault, and said. */
        memset(&x, 0, sizeof x);
        request(&x, 76);
        u32(&x, 0);
        length(&x);
        u = 0;
        rc = smb_rpc_answer(&lsa, x.b, x.n, &used, out, sizeof out, &u);
        check(rc == 0 && out[2] == 3 && unknown == 76,
              "an lsarpc operation not answered was not a fault, said as 76");
    }

    if (fails > 0) {
        printf("FAIL: %d of %d checks on the srvsvc pipe\n", fails, checks);
        return 1;
    }

    printf("PASS: %d checks on the srvsvc pipe (a BIND accepted for srvsvc in NDR, the "
           "feature negotiation acknowledged, a stranger refused; the share list at levels 1 "
           "and 0 read back; one share by name in any case, one not shared not found; this "
           "machine's name; an unknown operation a fault; half a PDU waited for, a wrong one "
           "refused, an answer too big refused; lsarpc's bind, LsarOpenPolicy2, LsarGetUserName "
           "and LsarLookupNames - diego RID 1000, somebody unknown - and an operation "
           "not answered said)\n", checks);
    return 0;
}
