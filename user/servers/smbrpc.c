/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The share list, over `IPC$`'s `srvsvc` pipe (`docs/sharing.md` N12d).
 *
 * The Mac's Finder signs in and then, before it mounts anything - even a
 * share it was given by name - opens `IPC$` and asks, by DCE/RPC over a
 * named pipe, what this machine shares (MS-SRVS NetrShareEnum). Refused, it
 * says "There was a problem connecting" and stops. So `smbd` answers the
 * pipe, with this: a BIND accepted for srvsvc in NDR, and three of its
 * operations -
 *
 *   15 NetrShareEnum      levels 0 and 1: every share, IPC$ among them
 *   16 NetrShareGetInfo   levels 0 and 1: one, by name
 *   21 NetrServerGetInfo  level 101: this machine, by its NetBIOS name
 *
 * - and `lsarpc`, asked next by Finder, which wants to know who it signed
 * in as (MS-LSAT):
 *
 *    0 LsarClose          the handle let go
 *    6, 44 LsarOpenPolicy(2)  a handle
 *   45 LsarGetUserName    the account, and this machine as its domain
 *   14 LsarLookupNames    the account's SID - this machine's domain SID and
 *                         RID 1000 - which the Mac maps owners by; any other
 *                         name not mapped
 *
 * - and anything else a fault, "operation out of range". libsmb2 has a
 * client's half of this (`libdcerpc`) and no server's.
 *
 * Bytes in and bytes out and nothing else, so the Mac holds it to its
 * cases (`tools/test_smbrpc.c`). Little-endian NDR (the data
 * representation every client asks for), one fragment a PDU each way: a
 * request too big for one is answered with a fault, and an answer too big
 * for the room it is given is refused rather than cut. Sizes and names from
 * C706 (DCE 1.1 RPC) chapters 12 and 14 and MS-RPCE 2.2.2.
 */

#include <stdbool.h>
#include <string.h>

#include "smbrpc.h"

#define PTYPE_REQUEST   0u
#define PTYPE_RESPONSE  2u
#define PTYPE_FAULT     3u
#define PTYPE_BIND     11u
#define PTYPE_BIND_ACK 12u

#define PFC_FIRST_FRAG  0x01u
#define PFC_LAST_FRAG   0x02u

#define HEADER         16u
#define MAX_FRAG     4280u      /* what Windows offers, and every client takes */

#define NCA_OP_RANGE    0x1c010002u     /* nca_s_op_rng_error */
#define NCA_PROTO       0x1c01000bu     /* nca_proto_error */

#define WERR_UNKNOWN_LEVEL      0x0000007cu
#define WERR_NET_NAME_NOT_FOUND 0x00000906u

/* The interface and the transfer syntax, as their UUIDs go on the wire. */
static const uint8_t SRVSVC[16] = { 0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01,
                                    0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88 };
static const uint8_t LSARPC[16] = { 0x78, 0x57, 0x34, 0x12, 0x34, 0x12, 0xcd, 0xab,
                                    0xef, 0x00, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab };
static const uint8_t NDR32[16] = { 0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11,
                                   0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60 };

/* Bind time feature negotiation (MS-RPCE 3.3.1.5.3): a "transfer syntax"
 * 6cb71c2c-9812-4540-xxxx-xxxxxxxxxxxx whose last eight bytes are the
 * features a client offers. Not a syntax: answered "negotiate ack", with
 * the features this accepts - none. */
static const uint8_t FEATURES[8] = { 0x2c, 0x1c, 0xb7, 0x6c, 0x12, 0x98, 0x40, 0x45 };

/*
 * ------------------------------------------------------------------------
 * Reading and writing, bounded.
 * ------------------------------------------------------------------------
 */

struct reader {
    const uint8_t *p;
    size_t at, len;
    bool bad;
};

static uint32_t get(struct reader *r, unsigned bytes)
{
    uint32_t v = 0;
    unsigned i;

    if (r->bad || r->at + bytes > r->len) {
        r->bad = true;
        return 0;
    }

    for (i = 0; i < bytes; i++) {
        v |= (uint32_t)r->p[r->at + i] << (8 * i);
    }

    r->at += bytes;
    return v;
}

static void align_r(struct reader *r, unsigned to, size_t base)
{
    while ((r->at - base) % to != 0 && r->at < r->len) {
        r->at++;
    }
}

struct writer {
    uint8_t *p;
    size_t at, room;
    bool full;
};

static void put(struct writer *w, uint32_t v, unsigned bytes)
{
    unsigned i;

    if (w->full || w->at + bytes > w->room) {
        w->full = true;
        return;
    }

    for (i = 0; i < bytes; i++) {
        w->p[w->at + i] = (uint8_t)(v >> (8 * i));
    }

    w->at += bytes;
}

static void put_bytes(struct writer *w, const uint8_t *b, size_t n)
{
    if (w->full || w->at + n > w->room) {
        w->full = true;
        return;
    }

    memcpy(w->p + w->at, b, n);
    w->at += n;
}

static void align_w(struct writer *w, unsigned to, size_t base)
{
    while ((w->at - base) % to != 0) {
        put(w, 0, 1);
    }
}

/* UTF-8 to UTF-16 code units: how many, and each in turn into `w` when it
 * is given. A sequence that is not UTF-8 becomes U+FFFD. */
static size_t utf16(const char *s, struct writer *w)
{
    const uint8_t *u = (const uint8_t *)s;
    size_t n = 0;

    while (*u != 0) {
        uint32_t c = *u++;
        unsigned more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;

        if (c >= 0x80 && more == 0) {
            c = 0xfffd;
        } else if (more > 0) {
            c &= 0x3fu >> more;

            while (more-- > 0) {
                if ((*u & 0xc0) != 0x80) {
                    c = 0xfffd;
                    break;
                }
                c = (c << 6) | (*u++ & 0x3f);
            }
        }

        if (c >= 0x10000) {
            if (w != NULL) {
                put(w, 0xd800 + ((c - 0x10000) >> 10), 2);
                put(w, 0xdc00 + ((c - 0x10000) & 0x3ff), 2);
            }
            n += 2;
        } else {
            if (w != NULL) {
                put(w, c, 2);
            }
            n++;
        }
    }

    return n;
}

/* A conformant varying string, with its zero: max, offset, actual, then the
 * units, aligned to four after. */
static void put_string(struct writer *w, const char *s, size_t base)
{
    size_t n = utf16(s, NULL) + 1;

    align_w(w, 4, base);
    put(w, (uint32_t)n, 4);
    put(w, 0, 4);
    put(w, (uint32_t)n, 4);
    (void)utf16(s, w);
    put(w, 0, 2);
    align_w(w, 4, base);
}

/* One read back, to compare a share's name: true when it is `name`, without
 * regard to ASCII case. Moves past it either way. */
static bool read_string_is(struct reader *r, const char *name, size_t base)
{
    uint32_t max, actual, i;
    bool same = true;
    const uint8_t *u = (const uint8_t *)name;

    align_r(r, 4, base);
    max = get(r, 4);
    (void)get(r, 4);
    actual = get(r, 4);

    if (r->bad || actual > max || actual > 4096) {
        r->bad = true;
        return false;
    }

    for (i = 0; i < actual; i++) {
        uint32_t c = get(r, 2);
        uint32_t want = *u;

        if (c == 0 && i + 1 == actual) {
            same = same && *u == 0;
            break;
        }

        if (c >= 'A' && c <= 'Z') c += 32;
        if (want >= 'A' && want <= 'Z') want += 32;

        if (*u == 0 || c != want) {
            same = false;
        } else {
            u++;
        }
    }

    align_r(r, 4, base);
    return same && !r->bad;
}

static void skip_string(struct reader *r, size_t base)
{
    uint32_t actual;

    align_r(r, 4, base);
    (void)get(r, 4);
    (void)get(r, 4);
    actual = get(r, 4);

    if (actual > 4096) {
        r->bad = true;
        return;
    }

    r->at += (size_t)actual * 2;
    align_r(r, 4, base);
}

/*
 * ------------------------------------------------------------------------
 * PDUs.
 * ------------------------------------------------------------------------
 */

static void header(struct writer *w, unsigned ptype, uint32_t call_id)
{
    put(w, 5, 1);                       /* rpc_vers */
    put(w, 0, 1);                       /* rpc_vers_minor */
    put(w, ptype, 1);
    put(w, PFC_FIRST_FRAG | PFC_LAST_FRAG, 1);
    put(w, 0x10, 1);                    /* little-endian, ASCII, IEEE */
    put(w, 0, 1);
    put(w, 0, 1);
    put(w, 0, 1);
    put(w, 0, 2);                       /* frag_length, filled in after */
    put(w, 0, 2);                       /* auth_length */
    put(w, call_id, 4);
}

static void finish(struct writer *w, size_t start)
{
    size_t len = w->at - start;

    if (!w->full) {
        w->p[start + 8] = (uint8_t)len;
        w->p[start + 9] = (uint8_t)(len >> 8);
    }
}

static void fault(struct writer *w, uint32_t call_id, uint32_t status)
{
    size_t start = w->at;

    header(w, PTYPE_FAULT, call_id);
    put(w, 32, 4);                      /* alloc_hint */
    put(w, 0, 2);                       /* p_cont_id */
    put(w, 0, 1);                       /* cancel_count */
    put(w, 0, 1);
    put(w, status, 4);
    put(w, 0, 4);
    finish(w, start);
}

/* BIND: srvsvc in NDR accepted; any other context refused, as "proposed
 * transfer syntaxes not supported" or "abstract syntax not supported". */
static void bind_ack(const struct smb_rpc_server *server, struct reader *r,
                     struct writer *w, uint32_t call_id)
{
    bool lsa = server->pipe == SMB_RPC_LSARPC;
    const char *ADDRESS = lsa ? "\\PIPE\\lsass" : "\\PIPE\\srvsvc";
    size_t address_bytes = strlen(ADDRESS) + 1;
    size_t start = w->at, n, i;
    uint32_t max_xmit = get(r, 2), max_recv = get(r, 2), group = get(r, 4);

    n = get(r, 1);
    (void)get(r, 3);

    header(w, PTYPE_BIND_ACK, call_id);
    put(w, max_xmit < MAX_FRAG ? max_xmit : MAX_FRAG, 2);
    put(w, max_recv < MAX_FRAG ? max_recv : MAX_FRAG, 2);
    put(w, group != 0 ? group : 0x53f0u, 4);
    put(w, (uint32_t)address_bytes, 2);
    put_bytes(w, (const uint8_t *)ADDRESS, address_bytes);
    align_w(w, 4, start);
    put(w, (uint32_t)n, 1);
    put(w, 0, 3);

    for (i = 0; i < n && !r->bad; i++) {
        uint8_t abstract[16];
        unsigned ts, t;
        bool ours, ndr = false, features = false;

        (void)get(r, 2);                /* p_cont_id */
        ts = get(r, 1);
        (void)get(r, 1);

        if (r->at + 20 > r->len) {
            r->bad = true;
            break;
        }

        memcpy(abstract, r->p + r->at, 16);
        r->at += 16;
        {
            uint32_t version = get(r, 4);   /* read whatever the interface */

            ours = lsa ? memcmp(abstract, LSARPC, 16) == 0 && version == 0u
                       : memcmp(abstract, SRVSVC, 16) == 0 && version == 3u;
        }

        for (t = 0; t < ts && !r->bad; t++) {
            if (r->at + 20 > r->len) {
                r->bad = true;
                break;
            }

            if (memcmp(r->p + r->at, NDR32, 16) == 0) {
                r->at += 16;
                ndr = ndr || get(r, 4) == 2u;
            } else {
                features = features || memcmp(r->p + r->at, FEATURES, 8) == 0;
                r->at += 20;
            }
        }

        if (ours && features && !ndr) {
            put(w, 3, 2);               /* negotiate ack */
            put(w, 0, 2);               /* the features accepted: none */
            put_bytes(w, (const uint8_t[16]){ 0 }, 16);
            put(w, 0, 4);
        } else if (ours && ndr) {
            put(w, 0, 2);               /* acceptance */
            put(w, 0, 2);
            put_bytes(w, NDR32, 16);
            put(w, 2, 4);
        } else {
            put(w, 2, 2);               /* provider rejection */
            put(w, ours ? 2u : 1u, 2);  /* transfer syntaxes / abstract syntax */
            put_bytes(w, (const uint8_t[16]){ 0 }, 16);
            put(w, 0, 4);
        }
    }

    finish(w, start);
}

/* A share's SHARE_INFO_0 or _1, its pointers first, and its strings
 * deferred until every entry's pointers are out (NDR's order for an array
 * of structures that point). */
static void share_pointers(struct writer *w, const struct smb_rpc_share *s, uint32_t level,
                           uint32_t *referent)
{
    put(w, (*referent)++ * 4 + 0x20000u, 4);
    if (level == 1) {
        put(w, s->type, 4);
        put(w, (*referent)++ * 4 + 0x20000u, 4);
    }
}

static void share_strings(struct writer *w, const struct smb_rpc_share *s, uint32_t level,
                          size_t base)
{
    put_string(w, s->name, base);
    if (level == 1) {
        put_string(w, s->comment != NULL ? s->comment : "", base);
    }
}

/* NetrShareEnum (15): the level asked, and every share at it. */
static void share_enum(const struct smb_rpc_server *server, struct reader *r,
                       struct writer *w, size_t base)
{
    uint32_t level, referent = 1;
    size_t i;

    if (get(r, 4) != 0) {               /* ServerName, unique */
        skip_string(r, base);
    }

    align_r(r, 4, base);
    level = get(r, 4);

    put(w, level, 4);                   /* InfoStruct->Level */
    put(w, level, 4);                   /* the union's switch */

    if (level != 0 && level != 1) {
        put(w, 0, 4);                   /* no container */
        put(w, 0, 4);                   /* TotalEntries */
        put(w, 0, 4);                   /* ResumeHandle, none */
        put(w, WERR_UNKNOWN_LEVEL, 4);
        return;
    }

    put(w, referent++ * 4 + 0x20000u, 4);       /* the container */
    put(w, (uint32_t)server->count, 4);         /* EntriesRead */

    if (server->count == 0) {
        put(w, 0, 4);
    } else {
        put(w, referent++ * 4 + 0x20000u, 4);   /* the array */
        put(w, (uint32_t)server->count, 4);     /* its conformance */

        for (i = 0; i < server->count; i++) {
            share_pointers(w, &server->shares[i], level, &referent);
        }

        for (i = 0; i < server->count; i++) {
            share_strings(w, &server->shares[i], level, base);
        }
    }

    align_w(w, 4, base);
    put(w, (uint32_t)server->count, 4);         /* TotalEntries */
    put(w, referent++ * 4 + 0x20000u, 4);       /* ResumeHandle */
    put(w, 0, 4);
    put(w, 0, 4);                               /* WERR_OK */
}

/* NetrShareGetInfo (16): one share, by its name. */
static void share_get(const struct smb_rpc_server *server, struct reader *r,
                      struct writer *w, size_t base)
{
    const struct smb_rpc_share *found = NULL;
    uint32_t level, referent = 1;
    size_t i, at;

    if (get(r, 4) != 0) {               /* ServerName, unique */
        skip_string(r, base);
    }

    at = r->at;                         /* NetName, [ref]: inline */

    for (i = 0; i < server->count && found == NULL; i++) {
        r->at = at;
        if (read_string_is(r, server->shares[i].name, base)) {
            found = &server->shares[i];
        }
    }

    if (found == NULL) {
        r->at = at;
        skip_string(r, base);
    }

    align_r(r, 4, base);
    level = get(r, 4);

    put(w, level, 4);                   /* the union's switch */

    if (found == NULL || (level != 0 && level != 1)) {
        put(w, 0, 4);
        put(w, found == NULL ? WERR_NET_NAME_NOT_FOUND : WERR_UNKNOWN_LEVEL, 4);
        return;
    }

    put(w, referent++ * 4 + 0x20000u, 4);
    share_pointers(w, found, level, &referent);
    share_strings(w, found, level, base);
    put(w, 0, 4);                       /* WERR_OK */
}

/* NetrServerGetInfo (21): level 101, this machine as a server. */
static void server_get(const struct smb_rpc_server *server, struct reader *r,
                       struct writer *w, size_t base)
{
    uint32_t level;

    if (get(r, 4) != 0) {
        skip_string(r, base);
    }

    align_r(r, 4, base);
    level = get(r, 4);

    put(w, level, 4);

    if (level != 101) {
        put(w, 0, 4);
        put(w, WERR_UNKNOWN_LEVEL, 4);
        return;
    }

    put(w, 0x20004u, 4);                /* the info */
    put(w, 500, 4);                     /* platform: NT */
    put(w, 0x20008u, 4);                /* its name */
    put(w, 10, 4);                      /* version 10.0 */
    put(w, 0, 4);
    put(w, 0x00009003u, 4);             /* workstation, server, NT */
    put(w, 0x2000cu, 4);                /* its comment */
    put_string(w, server->hostname != NULL ? server->hostname : "KOSMOS", base);
    put_string(w, "Kosmos", base);
    put(w, 0, 4);                       /* WERR_OK */
}

/* An RPC_UNICODE_STRING's own fields, its buffer deferred. */
static void put_unicode(struct writer *w, const char *s, uint32_t referent)
{
    uint32_t bytes = (uint32_t)utf16(s, NULL) * 2;

    put(w, bytes, 2);                   /* Length */
    put(w, bytes, 2);                   /* MaximumLength */
    put(w, referent, 4);                /* Buffer */
}

static void put_unicode_buffer(struct writer *w, const char *s, size_t base)
{
    uint32_t units = (uint32_t)utf16(s, NULL);

    align_w(w, 4, base);
    put(w, units, 4);
    put(w, 0, 4);
    put(w, units, 4);
    (void)utf16(s, w);
    align_w(w, 4, base);
}

/* An RPC_UNICODE_STRING in a request: its fields, and its buffer if it has
 * one - read past. */
static void skip_unicode(struct reader *r, size_t base)
{
    uint32_t buffer;

    (void)get(r, 2);
    (void)get(r, 2);
    buffer = get(r, 4);

    if (buffer != 0) {
        skip_string(r, base);
    }
}

/* LsarGetUserName (45): the account, and this machine as its authority. */
static void lsa_user(const struct smb_rpc_server *server, struct reader *r,
                     struct writer *w, size_t base)
{
    uint32_t inner, domain;
    const char *user = server->user != NULL ? server->user : "";
    const char *host = server->hostname != NULL ? server->hostname : "KOSMOS";

    if (get(r, 4) != 0) {               /* SystemName, unique */
        skip_string(r, base);
    }

    inner = get(r, 4);                  /* *UserName: the string's pointer */
    if (inner != 0) {
        skip_unicode(r, base);
    }

    domain = get(r, 4);                 /* DomainName, unique */
    if (domain != 0 && get(r, 4) != 0) {
        skip_unicode(r, base);
    }

    put(w, 0x20000u, 4);                /* *UserName */
    put_unicode(w, user, 0x20004u);
    put_unicode_buffer(w, user, base);

    if (domain != 0) {
        put(w, 0x20008u, 4);            /* DomainName */
        put(w, 0x2000cu, 4);            /* *DomainName */
        put_unicode(w, host, 0x20010u);
        put_unicode_buffer(w, host, base);
    } else {
        put(w, 0, 4);
    }

    put(w, 0, 4);                       /* STATUS_SUCCESS */
}

/* LsarOpenPolicy(2) (6, 44): a handle - this keeps nothing behind it, so
 * one handle serves every caller; LsarClose (0) a handle zeroed. */
static void lsa_handle(struct writer *w, bool open)
{
    static const uint8_t HANDLE[16] = { 'K', 'o', 's', 'm', 'o', 's', '-', 'L',
                                        'S', 'A', '-', 'p', 'o', 'l', 'c', 'y' };

    put(w, 0, 4);                       /* attributes */
    put_bytes(w, open ? HANDLE : (const uint8_t[16]){ 0 }, 16);
    put(w, 0, 4);                       /* STATUS_SUCCESS */
}

/*
 * This machine's domain SID: S-1-5-21 and three numbers, fixed, since a
 * machine with one account and no domain needs only one that does not
 * change; the account is its RID 1000, as Windows numbers the first user.
 */
static const uint32_t DOMAIN_SUB[4] = { 21, 0x4b6f736d, 0x6f735368, 0x61726573 };
#define ACCOUNT_RID 1000u

static void put_sid(struct writer *w)
{
    static const uint8_t NT_AUTHORITY[6] = { 0, 0, 0, 0, 0, 5 };
    size_t i;

    put(w, 4, 4);                       /* the SubAuthority array's conformance */
    put(w, 1, 1);                       /* Revision */
    put(w, 4, 1);                       /* SubAuthorityCount */
    put_bytes(w, NT_AUTHORITY, 6);
    for (i = 0; i < 4; i++) {
        put(w, DOMAIN_SUB[i], 4);
    }
}

/* A name in a request, as ASCII, at most `room` - 1 of it. */
static void read_name(struct reader *r, char *out, size_t room, size_t base)
{
    uint32_t actual, i, n = 0;

    align_r(r, 4, base);
    (void)get(r, 4);
    (void)get(r, 4);
    actual = get(r, 4);

    if (actual > 1024) {
        r->bad = true;
        return;
    }

    for (i = 0; i < actual; i++) {
        uint32_t c = get(r, 2);

        if (n + 1 < room) {
            out[n++] = c < 0x80 ? (char)c : '?';
        }
    }

    out[n] = '\0';
    align_r(r, 4, base);
}

static bool same_account(const char *asked, const char *user)
{
    const char *slash = strrchr(asked, '\\');
    const char *a = slash != NULL ? slash + 1 : asked;

    if (user == NULL || user[0] == '\0') {
        return false;
    }

    while (*a != '\0' && *user != '\0') {
        char x = *a++, y = *user++;

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) {
            return false;
        }
    }

    return *a == *user;
}

/* LsarLookupNames (14): each name a SID, or "unknown". */
static void lsa_lookup_names(const struct smb_rpc_server *server, struct reader *r,
                             struct writer *w, size_t base)
{
    const char *host = server->hostname != NULL ? server->hostname : "KOSMOS";
    uint32_t count, i, mapped = 0;
    bool known[32];

    r->at += 20;                        /* PolicyHandle */
    count = get(r, 4);

    if (count > 32) {
        r->bad = true;
        return;
    }

    (void)get(r, 4);                    /* Names' conformance */
    for (i = 0; i < count; i++) {
        (void)get(r, 2);
        (void)get(r, 2);
        (void)get(r, 4);
    }

    for (i = 0; i < count && !r->bad; i++) {
        char name[128];

        read_name(r, name, sizeof name, base);
        known[i] = same_account(name, server->user);
        mapped += known[i] ? 1u : 0u;
    }

    /* ReferencedDomains: this machine, its name and its SID. */
    put(w, 0x20000u, 4);
    put(w, 1, 4);                       /* Entries */
    put(w, 0x20004u, 4);                /* Domains */
    put(w, 32, 4);                      /* MaxEntries */
    put(w, 1, 4);                       /* the array's conformance */
    put_unicode(w, host, 0x20008u);
    put(w, 0x2000cu, 4);                /* Sid */
    put_unicode_buffer(w, host, base);
    put_sid(w);

    /* TranslatedSids. */
    align_w(w, 4, base);
    put(w, count, 4);
    put(w, count > 0 ? 0x20010u : 0u, 4);
    if (count > 0) {
        put(w, count, 4);
        for (i = 0; i < count; i++) {
            put(w, known[i] ? 1u : 8u, 2);     /* SidTypeUser, or SidTypeUnknown */
            put(w, 0, 2);
            put(w, known[i] ? ACCOUNT_RID : 0u, 4);
            put(w, known[i] ? 0u : 0xffffffffu, 4);
        }
    }

    put(w, mapped, 4);                  /* MappedCount */
    put(w, mapped == count ? 0x00000000u              /* STATUS_SUCCESS */
         : mapped > 0      ? 0x00000107u              /* STATUS_SOME_NOT_MAPPED */
                           : 0xc0000073u, 4);         /* STATUS_NONE_MAPPED */
}

static bool answers(const struct smb_rpc_server *server, uint32_t opnum)
{
    if (server->pipe == SMB_RPC_LSARPC) {
        return opnum == 0 || opnum == 6 || opnum == 14 || opnum == 44 || opnum == 45;
    }

    return opnum == 15 || opnum == 16 || opnum == 21;
}

static void request(const struct smb_rpc_server *server, struct reader *r,
                    struct writer *w, uint32_t call_id, unsigned flags)
{
    size_t start = w->at, stub;
    uint32_t context, opnum;

    (void)get(r, 4);                    /* alloc_hint */
    context = get(r, 2);
    opnum = get(r, 2);

    if (r->bad || (flags & (PFC_FIRST_FRAG | PFC_LAST_FRAG))
                  != (PFC_FIRST_FRAG | PFC_LAST_FRAG)
        || !answers(server, opnum)) {
        if (!r->bad && !answers(server, opnum) && server->unknown != NULL) {
            *server->unknown = (int)opnum;
        }
        fault(w, call_id, r->bad || answers(server, opnum) ? NCA_PROTO : NCA_OP_RANGE);
        return;
    }

    header(w, PTYPE_RESPONSE, call_id);
    put(w, 0, 4);                       /* alloc_hint, filled in after */
    put(w, context, 2);
    put(w, 0, 1);
    put(w, 0, 1);
    stub = w->at;

    {
        size_t base = r->at;

        if (server->pipe == SMB_RPC_LSARPC) {
            if (opnum == 45) {
                lsa_user(server, r, w, base);
            } else if (opnum == 14) {
                lsa_lookup_names(server, r, w, base);
            } else {
                lsa_handle(w, opnum != 0);
            }
        } else if (opnum == 15) {
            share_enum(server, r, w, base);
        } else if (opnum == 16) {
            share_get(server, r, w, base);
        } else {
            server_get(server, r, w, base);
        }
    }

    if (r->bad) {
        w->at = start;
        w->full = false;
        fault(w, call_id, NCA_PROTO);
        return;
    }

    if (!w->full) {
        size_t hint = w->at - stub;

        w->p[stub - 8] = (uint8_t)hint;
        w->p[stub - 7] = (uint8_t)(hint >> 8);
        w->p[stub - 6] = (uint8_t)(hint >> 16);
        w->p[stub - 5] = (uint8_t)(hint >> 24);
    }

    finish(w, start);
}

int smb_rpc_answer(const struct smb_rpc_server *server,
                   const uint8_t *in, size_t in_len, size_t *in_used,
                   uint8_t *out, size_t out_room, size_t *out_used)
{
    struct reader r = { in, 0, in_len, false };
    struct writer w = { out, *out_used, out_room, false };
    unsigned ptype, flags;
    uint32_t frag, call_id;

    *in_used = 0;

    if (in_len < HEADER) {
        return 0;
    }

    frag = (uint32_t)in[8] | (uint32_t)in[9] << 8;

    if (in[0] != 5 || in[1] != 0 || (in[4] & 0xf0) != 0x10 || frag < HEADER) {
        return -1;
    }

    if (frag > in_len) {
        return 0;                       /* not all of it yet */
    }

    r.len = frag;
    r.at = 2;
    ptype = get(&r, 1);
    flags = get(&r, 1);
    r.at = 12;
    call_id = get(&r, 4);
    *in_used = frag;

    if (ptype == PTYPE_BIND) {
        bind_ack(server, &r, &w, call_id);
    } else if (ptype == PTYPE_REQUEST) {
        request(server, &r, &w, call_id, flags);
    } else {
        fault(&w, call_id, NCA_PROTO);
    }

    if (w.full) {
        return -1;
    }

    *out_used = w.at;
    return 0;
}
