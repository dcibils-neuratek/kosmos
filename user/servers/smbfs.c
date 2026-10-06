/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * smbfs: the SMB client, one process for every share on every server
 * (`docs/sharing.md`, *`smbfs`, the client*; this is step N2, *smbfs
 * connects*).
 *
 * **A server in C**, a role of `init.elf` started at boot and idle until
 * somebody connects: what runs on behalf of another process does not get a
 * collector (`CLAUDE.md`, *Language split*). It speaks SMB 2 and 3 through
 * the SMB Kit - libsmb2 as vendored, on the network stack's ring
 * (`user/kits/smb/`) - and answers `shareproto.h` on its endpoint: PROBE,
 * CONNECT, STATUS and DISCONNECT. A share's files, `diskproto.h`, are step
 * N3.
 *
 * It is handed, at boot: its own endpoint; the stack's, as a client of it;
 * and the console's, so what it does is said in the log.
 *
 * **Two things to wait on, and one thread that waits.** Callers arrive on
 * the endpoint, and bytes arrive on each server's ring, and the kernel has
 * no single wait for both - nor is one proposed. So **each connection has a
 * waiter**, a thread (`kosmos_thread_start`, `threads.md` step 3) that
 * blocks in the stack's `NET_OP_POLL` on that one connection and, when
 * something happens, says so as a *call to smbfs's own endpoint*, a message
 * like any caller's. The main thread is the only one that touches libsmb2,
 * the tables and `malloc` - there is no futex yet and `malloc` has no lock,
 * and nothing here needs either. A waiter reads three words the main thread
 * writes - what to wait for, on which connection, and whether to stop -
 * with `__atomic` loads, and waits at most a second at a time, so a change
 * it did not hear about in an answer is seen by the next.
 *
 * **Nothing waits on smbfs.** PROBE and CONNECT are answered at once, and
 * the caller asks STATUS on its own clock; the main thread waits only in
 * `kosmos_receive`, until a caller, a waiter, or the next deadline. A server
 * that stops answering mid-negotiation is a deadline passing, and STATUS is
 * answered the whole time (the control in `tools/run_share.py`).
 *
 * **The password crosses once and is not kept.** CONNECT carries it; smbfs
 * makes the NT hash from it - MD4 of the password in UTF-16, the Crypto
 * Kit's - forgets the password, and hands libsmb2 the hash in the form it
 * takes for one ("ntlm:" and 32 hex digits). NTLMv2 needs the hash and only
 * the hash.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kosmos.h"
#include "netproto.h"
#include "shareproto.h"
#include "crypto.h"
#include "init/say.h"

#include "smb_kit.h"

/*
 * **How long a server has to answer**: connecting, negotiating, signing in
 * and connecting the share, all of it. A number in one place, as the design
 * asks; a quarter of a second is the bound for a question about names
 * (step N3), and this is the bound for a person waiting on a connect.
 */
#define ANSWER_SECONDS      10u

/* How long a waiter waits before looking again at what it should wait for. */
#define WAITER_SECONDS       1u

/* What a waiter says, in `flags`. */
#define SAID_RESOLVED        1u
#define SAID_EVENT           2u
#define SAID_LEAVING         3u

struct server {
    struct share_server pub;        /* what STATUS says of it */

    bool     probe;
    bool     forgotten;             /* DISCONNECT: freed once its waiter has left */
    bool     closing;               /* libsmb2's callbacks are no longer listened to */
    bool     settled;               /* a callback decided how it went */
    uint64_t since;                 /* counter ticks: when `pub.state` became so */
    uint64_t deadline;              /* counter ticks, while ASKING */

    char     host[SHARE_ADDRESS_MAX];
    uint32_t port;
    bool     by_name;               /* the waiter resolves `host` first */
    char     target[SHARE_ADDRESS_MAX]; /* what libsmb2 is given: numbers and port */
    char     hash[40];              /* "ntlm:" and the NT hash, in hex */

    struct smb2_context *smb2;

    /* The waiter. */
    long     thread;                /* its index, or -1 */
    long     poll_region;
    struct net_poll_entry *poll_set;
    uint64_t handle;                /* __atomic: the connection it waits on */
    uint32_t want;                  /* __atomic: NET_WANT_*; 0 is nothing yet */
    uint32_t stop;                  /* __atomic: its work is over */
    uint8_t  resolved[4];
};

static long endpoint = -1;
static long net = -1;
static long console = -1;
static uint64_t token;              /* a waiter's word (`SHARE_OP_WAITER`) */
static uint64_t counter_hz = 62500000u;
static uint64_t tick_hz = 250u;

static struct server **servers;     /* in the order they were asked for */
static unsigned server_count, server_room;

/*------------------------------------------------------------------------
 * Small things.
 *----------------------------------------------------------------------*/

/* How long a string is, looking no further than `room` bytes. */
static size_t bounded(const char *s, size_t room)
{
    size_t n = 0;

    while (n < room && s[n] != '\0') {
        n++;
    }

    return n;
}

static void copy(char *to, size_t room, const char *from)
{
    size_t n = bounded(from, room - 1);

    memcpy(to, from, n);
    to[n] = '\0';
}

/* A fixed field from a request, which may be full and unterminated. */
static void field(char *to, size_t room, const char *from, size_t from_room)
{
    size_t n = bounded(from, from_room);

    if (n > room - 1) {
        n = room - 1;
    }

    memcpy(to, from, n);
    to[n] = '\0';
}

static const char *named(const struct server *s)
{
    return s->pub.name[0] != '\0' ? s->pub.name : s->pub.address;
}

static void become(struct server *s, uint32_t state, const char *why)
{
    s->pub.state = state;
    s->since = kosmos_ticks();
    copy(s->pub.why, sizeof(s->pub.why), why != NULL ? why : "");

    if (state != SHARE_STATE_ASKING) {
        s->deadline = 0;
    }
}

static const char *dialect_text(uint16_t d)
{
    switch (d) {
    case 0x0202: return "2.0.2";
    case 0x0210: return "2.1";
    case 0x0300: return "3.0";
    case 0x0302: return "3.0.2";
    case 0x0311: return "3.1.1";
    default:     return "?";
    }
}

static void tell(const struct server *s)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, "smbfs: ");

    switch (s->pub.state) {
    case SHARE_STATE_ANSWERED:
    case SHARE_STATE_CONNECTED:
        say_text(&line, s->pub.address);
        say_text(&line, " answered - ");
        say_text(&line, s->pub.name[0] != '\0' ? s->pub.name : "a server");
        say_text(&line, ", SMB ");
        say_text(&line, dialect_text(s->pub.dialect));
        say_text(&line, s->pub.signing ? ", signed" : "");
        say_text(&line, s->pub.sealing ? ", sealed" : "");

        if (s->pub.state == SHARE_STATE_CONNECTED) {
            say_text(&line, "; ");
            say_text(&line, s->pub.share);
            say_text(&line, " as ");
            say_text(&line, s->pub.account);
        }
        break;
    default:
        say_text(&line, s->pub.why);       /* which names the server */
        break;
    }

    say_send(console, &line);
}

/*------------------------------------------------------------------------
 * The table, which grows and whose entries never move: a waiter holds the
 * address of its own.
 *----------------------------------------------------------------------*/

static struct server *server_find(const char *address)
{
    unsigned i;

    for (i = 0; i < server_count; i++) {
        if (!servers[i]->forgotten
            && strcmp(servers[i]->pub.address, address) == 0) {
            return servers[i];
        }
    }

    return NULL;
}

static bool server_known(const struct server *s)
{
    unsigned i;

    for (i = 0; i < server_count; i++) {
        if (servers[i] == s) {
            return true;
        }
    }

    return false;
}

static struct server *server_add(void)
{
    struct server *s;

    if (server_count == server_room) {
        unsigned more = server_room == 0 ? 8u : server_room * 2u;
        struct server **grown = realloc(servers, more * sizeof(*grown));

        if (grown == NULL) {
            return NULL;
        }

        servers = grown;
        server_room = more;
    }

    s = calloc(1, sizeof(*s));

    if (s == NULL) {
        return NULL;
    }

    s->thread = -1;
    s->poll_region = -1;
    servers[server_count++] = s;
    return s;
}

/* Gone from the table and from memory: only once nothing - no context, no
 * waiter - holds it. */
static void server_free(struct server *s)
{
    unsigned i;

    for (i = 0; i < server_count; i++) {
        if (servers[i] == s) {
            memmove(&servers[i], &servers[i + 1],
                    (server_count - i - 1) * sizeof(servers[0]));
            server_count--;
            break;
        }
    }

    if (s->poll_set != NULL
        && kosmos_share_unmap((unsigned long)(uintptr_t)s->poll_set, 1) == 0) {
        (void)kosmos_cap_drop(s->poll_region);
    }

    memset(s, 0, sizeof(*s));
    free(s);
}

/* libsmb2 let go of: its socket closed with it. Never from inside one of
 * its own callbacks - those only decide, and this runs after. */
static void server_close(struct server *s)
{
    if (s->smb2 != NULL) {
        /*
         * **libsmb2 would free this record.** `smb2_connect_async` keeps its
         * caller's `cb_data` as the context's `connect_data`, and
         * `smb2_destroy_context` frees whatever is there as the `struct
         * connect_data` its own `smb2_connect_share_async` puts there - so a
         * probe, which connects with the first and is its own `cb_data`,
         * had its record freed under it and listed empty. Found by
         * `run_share.py`'s probe; it is libsmb2's to fix, and until then
         * a record is taken back before the context goes.
         */
        if (s->smb2->connect_data == (void *)s) {
            s->smb2->connect_data = NULL;
        }

        s->closing = true;
        smb2_destroy_context(s->smb2);
        s->smb2 = NULL;
    }

    __atomic_store_n(&s->want, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s->stop, 1u, __ATOMIC_RELEASE);
}

/*------------------------------------------------------------------------
 * How it went, in words.
 *----------------------------------------------------------------------*/

static void refused_or_away(struct server *s, int status)
{
    char why[SHARE_WHY_MAX];
    uint32_t nt = s->smb2 != NULL ? (uint32_t)smb2_get_nterror(s->smb2) : 0;
    struct smb_link link;
    bool linked = s->smb2 != NULL && smb_kit_link(smb2_get_fd(s->smb2), &link);
    uint32_t state = SHARE_STATE_REFUSED;

    (void)status;

    if (nt == SMB2_STATUS_LOGON_FAILURE || nt == SMB2_STATUS_WRONG_PASSWORD
        || nt == SMB2_STATUS_WRONG_PASSWORD_CORE) {
        snprintf(why, sizeof(why), "%s refused the account %s: the name or "
                 "the password is wrong", named(s), s->pub.account);
    } else if (nt == SMB2_STATUS_ACCOUNT_DISABLED) {
        snprintf(why, sizeof(why), "%s will not let %s sign in", named(s),
                 s->pub.account);
    } else if (nt == SMB2_STATUS_BAD_NETWORK_NAME) {
        snprintf(why, sizeof(why), "%s has no share called %s", named(s),
                 s->pub.share);
    } else if (nt == SMB2_STATUS_ACCESS_DENIED) {
        snprintf(why, sizeof(why), "%s does not let %s open %s", named(s),
                 s->pub.account, s->pub.share);
    } else if (linked && !link.taken && link.received == 0) {
        /* Nothing on the far end took a byte: nobody there. */
        snprintf(why, sizeof(why), "nothing at %s took the connection",
                 s->pub.address);
        state = SHARE_STATE_AWAY;
    } else if (!linked && smb_kit_last_refusal() != NET_OK) {
        snprintf(why, sizeof(why), "%s could not be reached (the network "
                 "said %u)", s->pub.address, (unsigned)smb_kit_last_refusal());
        state = SHARE_STATE_AWAY;
    } else if (linked && link.received == 0) {
        /* It took the connection, heard NEGOTIATE in SMB 2's form, and hung
         * up without a word: a server of SMB 1, which this machine does not
         * speak (`docs/sharing.md`, *Not here, and why*). */
        snprintf(why, sizeof(why), "%s took the connection and hung up "
                 "without answering: it does not speak SMB 2 or 3",
                 s->pub.address);
    } else {
        const char *said = s->smb2 != NULL ? smb2_get_error(s->smb2) : "";

        snprintf(why, sizeof(why), "%s: %s", named(s),
                 said != NULL && said[0] != '\0' ? said : "it stopped answering");
    }

    become(s, state, why);
}

/*------------------------------------------------------------------------
 * libsmb2's callbacks: they decide, and the loop acts after.
 *----------------------------------------------------------------------*/

static void read_session(struct server *s)
{
    const char *domain = smb2_get_domain(s->smb2);

    s->pub.dialect = smb2_get_dialect(s->smb2);
    s->pub.signing = s->smb2->sign ? 1 : 0;
    s->pub.sealing = s->smb2->seal ? 1 : 0;

    /* The name the server gave in NTLM's challenge, which libsmb2 keeps as
     * the domain when it was given none - and smbfs gives none. */
    if (domain != NULL && domain[0] != '\0') {
        copy(s->pub.name, sizeof(s->pub.name), domain);
    }
}

static void connected(struct smb2_context *smb2, int status, void *data,
                      void *private_data)
{
    struct server *s = private_data;

    (void)smb2; (void)data;

    if (s->closing || s->settled) {
        return;
    }

    s->settled = true;

    if (status == 0) {
        read_session(s);
        become(s, SHARE_STATE_CONNECTED, NULL);
    } else {
        refused_or_away(s, status);
    }

    tell(s);
}

static void probe_negotiated(struct smb2_context *smb2, int status,
                             void *data, void *private_data)
{
    struct server *s = private_data;
    struct smb2_negotiate_reply *rep = data;

    (void)smb2;

    if (s->closing || s->settled) {
        return;
    }

    s->settled = true;

    if (status == 0 && rep != NULL) {
        s->pub.dialect = rep->dialect_revision;
        s->pub.signing =
            (rep->security_mode & SMB2_NEGOTIATE_SIGNING_REQUIRED) ? 1 : 0;
        become(s, SHARE_STATE_ANSWERED, NULL);
    } else {
        refused_or_away(s, status);
    }

    tell(s);
}

/* The connection is there: NEGOTIATE, as libsmb2's own connect asks it,
 * and nothing after it. */
static void probe_connected(struct smb2_context *smb2, int status,
                            void *data, void *private_data)
{
    struct server *s = private_data;
    struct smb2_negotiate_request req;
    struct smb2_pdu *pdu;

    (void)data;

    if (s->closing || s->settled) {
        return;
    }

    if (status != 0) {
        s->settled = true;
        refused_or_away(s, status);
        tell(s);
        return;
    }

    memset(&req, 0, sizeof(req));
    req.capabilities  = SMB2_GLOBAL_CAP_LARGE_MTU | SMB2_GLOBAL_CAP_ENCRYPTION;
    req.security_mode = SMB2_NEGOTIATE_SIGNING_ENABLED;
    req.dialect_count = 5;
    req.dialects[0]   = SMB2_VERSION_0202;
    req.dialects[1]   = SMB2_VERSION_0210;
    req.dialects[2]   = SMB2_VERSION_0300;
    req.dialects[3]   = SMB2_VERSION_0302;
    req.dialects[4]   = SMB2_VERSION_0311;
    memcpy(req.client_guid, smb2_get_client_guid(smb2), SMB2_GUID_SIZE);

    pdu = smb2_cmd_negotiate_async(smb2, &req, probe_negotiated, s);

    if (pdu == NULL) {
        s->settled = true;
        become(s, SHARE_STATE_REFUSED, "no memory for the question");
        tell(s);
        return;
    }

    smb2_queue_pdu(smb2, pdu);
}

/*------------------------------------------------------------------------
 * Driving libsmb2: what it has to send goes into the ring at once, and
 * what arrives is read when the waiter says so.
 *----------------------------------------------------------------------*/

/* After any call into libsmb2: what it queued, written while there is room;
 * a connection that ended, in words; a decision, acted on. */
static void settle(struct server *s, int serviced)
{
    int round;

    for (round = 0; serviced >= 0 && s->smb2 != NULL && round < 32; round++) {
        struct smb_link link;
        int events = smb2_which_events(s->smb2);

        if ((events & POLLOUT) == 0) {
            break;
        }

        if (smb_kit_link(smb2_get_fd(s->smb2), &link) && link.room == 0) {
            /* The ring is full: the waiter waits for room too. */
            __atomic_store_n(&s->want, NET_WANT_READ | NET_WANT_WRITE,
                             __ATOMIC_RELEASE);
            return;
        }

        serviced = smb2_service(s->smb2, POLLOUT);
    }

    if (serviced < 0 && !s->settled) {
        s->settled = true;
        refused_or_away(s, serviced);
        tell(s);
    }

    /* Over: a probe once it has its answer, anything once refused or away. */
    if (s->settled && s->pub.state != SHARE_STATE_CONNECTED) {
        server_close(s);
        return;
    }

    if (serviced < 0 && s->pub.state == SHARE_STATE_CONNECTED) {
        /* A connected server that hung up: away, until step N5 signs in
         * again by itself. */
        become(s, SHARE_STATE_AWAY, "the server closed the connection");
        tell(s);
        server_close(s);
        return;
    }

    if (s->smb2 != NULL) {
        struct smb_link link;

        if (smb_kit_link(smb2_get_fd(s->smb2), &link)) {
            __atomic_store_n(&s->handle, link.handle, __ATOMIC_RELEASE);
        }

        __atomic_store_n(&s->want, NET_WANT_READ, __ATOMIC_RELEASE);
    }
}

/* libsmb2 started on a server whose address is numbers. */
static void start_smb(struct server *s)
{
    int rc;

    s->smb2 = smb2_init_context();

    if (s->smb2 == NULL) {
        s->settled = true;
        become(s, SHARE_STATE_REFUSED, "no memory for the conversation");
        return;
    }

    smb2_set_security_mode(s->smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_version(s->smb2, SMB2_VERSION_ANY);
    smb2_set_workstation(s->smb2, "KOSMOS");

    if (s->probe) {
        rc = smb2_connect_async(s->smb2, s->target, probe_connected, s);
    } else {
        smb2_set_user(s->smb2, s->pub.account);
        smb2_set_password(s->smb2, s->hash);
        rc = smb2_connect_share_async(s->smb2, s->target, s->pub.share,
                                      s->pub.account, connected, s);
    }

    /* The hash is libsmb2's to keep while connected; this copy goes. */
    memset(s->hash, 0, sizeof(s->hash));

    if (rc < 0) {
        s->settled = true;
        refused_or_away(s, rc);
        tell(s);
        server_close(s);
        return;
    }

    /* The stack answered the connect at once (`NET_CONNECT_AT_ONCE`): what
     * libsmb2 writes now waits in the ring until the far end takes it. */
    settle(s, smb2_service(s->smb2, POLLOUT));
}

/*------------------------------------------------------------------------
 * The waiter: one a connection, and the only code here that blocks on the
 * network. It touches nothing but its own server's three words, its poll
 * region and its own stack.
 *----------------------------------------------------------------------*/

static uint32_t waiter_call(struct server *s, uint32_t said, uint32_t what,
                            const uint8_t address[4])
{
    struct message msg, out;
    struct disk_request req;
    struct disk_reply rep;

    memset(&req, 0, sizeof(req));
    req.op       = SHARE_OP_WAITER;
    req.offset   = token;
    req.bytes    = (uint64_t)(uintptr_t)s;
    req.flags    = said;
    req.reserved = what;

    if (address != NULL) {
        memcpy(req.u.data, address, 4);
        req.length = 4;
    }

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(req);
    memcpy(msg.data, &req, sizeof(req));

    if (kosmos_call(endpoint, &msg, &out) != 0 || out.length < sizeof(rep)) {
        return 1;                   /* nobody to tell: stop */
    }

    memcpy(&rep, out.data, sizeof(rep));
    return rep.error;
}

static void waiter_main(unsigned long arg)
{
    struct server *s = (struct server *)(uintptr_t)arg;

    if (s->by_name) {
        struct net_request req;
        struct net_reply rep;
        struct message msg, out;
        size_t len = strlen(s->host);
        uint32_t status = NET_ERR_NO_NAME;

        memset(&req, 0, sizeof(req));
        req.op = NET_OP_RESOLVE;
        req.length = (uint32_t)len;
        req.wait_ticks = (uint32_t)(5u * tick_hz);
        memcpy(req.payload, s->host, len);

        memset(&msg, 0, sizeof(msg));
        msg.length = sizeof(req);
        memcpy(msg.data, &req, sizeof(req));

        memset(&rep, 0, sizeof(rep));

        if (kosmos_call(net, &msg, &out) == 0 && out.length >= sizeof(rep)) {
            memcpy(&rep, out.data, sizeof(rep));
            status = rep.status;
        }

        if (waiter_call(s, SAID_RESOLVED, status, rep.address.byte) != 0) {
            (void)waiter_call(s, SAID_LEAVING, 0, NULL);
            kosmos_thread_exit(0);
            return;
        }
    }

    while (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE) == 0) {
        struct net_request req;
        struct net_reply rep;
        struct message msg, out;
        uint32_t want = __atomic_load_n(&s->want, __ATOMIC_ACQUIRE);
        uint64_t handle = __atomic_load_n(&s->handle, __ATOMIC_ACQUIRE);

        /* An entry that wants nothing holds the place: a wait for the
         * deadline and no more (`net.c`, `poll_ready`). */
        s->poll_set[0].handle = handle;
        s->poll_set[0].want   = handle != 0 ? want : 0;
        s->poll_set[0].got    = 0;

        memset(&req, 0, sizeof(req));
        req.op         = NET_OP_POLL;
        req.length     = 1;
        req.wait_ticks = (uint32_t)(WAITER_SECONDS * tick_hz);

        memset(&msg, 0, sizeof(msg));
        msg.length = sizeof(req);
        msg.cap_plus_one = (uint32_t)(s->poll_region + 1);
        memcpy(msg.data, &req, sizeof(req));

        if (kosmos_call(net, &msg, &out) != 0 || out.length < sizeof(rep)) {
            break;
        }

        memcpy(&rep, out.data, sizeof(rep));

        if (rep.status == NET_OK && rep.ready > 0 && s->poll_set[0].got != 0) {
            if (waiter_call(s, SAID_EVENT, s->poll_set[0].got, NULL) != 0) {
                break;
            }
        }
    }

    (void)waiter_call(s, SAID_LEAVING, 0, NULL);
    kosmos_thread_exit(0);
}

static bool waiter_start(struct server *s)
{
    long at;

    s->poll_region = kosmos_mem_create(1);

    if (s->poll_region < 0) {
        return false;
    }

    at = kosmos_mem_map(s->poll_region);

    if (at < 0) {
        (void)kosmos_cap_drop(s->poll_region);
        s->poll_region = -1;
        return false;
    }

    s->poll_set = (struct net_poll_entry *)(uintptr_t)at;
    s->thread = kosmos_thread_start(waiter_main, (unsigned long)(uintptr_t)s);

    if (s->thread < 0) {
        s->thread = -1;
        return false;
    }

    return true;
}

/* What a waiter said, and the answer that sends it on or ends it. */
static uint32_t from_waiter(struct server *s, uint32_t said, uint32_t what,
                            const char *data)
{
    if (said == SAID_LEAVING) {
        return 1;                   /* reaped once this answer has gone */
    }

    if (__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE) != 0 || s->forgotten) {
        return 1;
    }

    if (said == SAID_RESOLVED) {
        char why[SHARE_WHY_MAX];

        if (what != NET_OK) {
            snprintf(why, sizeof(why), "no address for %s (the network said %u)",
                     s->host, (unsigned)what);
            s->settled = true;
            become(s, SHARE_STATE_AWAY, why);
            tell(s);
            server_close(s);
            return 1;
        }

        memcpy(s->resolved, data, 4);
        snprintf(s->target, sizeof(s->target), "%u.%u.%u.%u:%u",
                 s->resolved[0], s->resolved[1], s->resolved[2],
                 s->resolved[3], (unsigned)s->port);
        start_smb(s);
        return __atomic_load_n(&s->stop, __ATOMIC_ACQUIRE) != 0 ? 1u : 0u;
    }

    if (said == SAID_EVENT && s->smb2 != NULL) {
        int events = 0;

        /* The end is read, not taken on trust: what arrived before a close
         * is still to be read, and the read that finds nothing after it is
         * what tells libsmb2 the connection is over. */
        if ((what & (NET_WANT_READ | NET_GOT_OVER)) != 0) {
            events |= POLLIN;
        }

        if ((what & NET_WANT_WRITE) != 0) {
            events |= POLLOUT;
        }

        settle(s, smb2_service(s->smb2, events));
    }

    return __atomic_load_n(&s->stop, __ATOMIC_ACQUIRE) != 0 ? 1u : 0u;
}

/*------------------------------------------------------------------------
 * Callers.
 *----------------------------------------------------------------------*/

static void refuse(struct disk_reply *rep, uint32_t error, const char *why)
{
    rep->error = error;
    copy(rep->u.data, sizeof(rep->u.data), why);
    rep->length = (uint32_t)strlen(rep->u.data);
}

/* "10.0.2.2:4450", "diego-mac", "nas:445": a host of letters, digits, dots
 * and dashes, and a port if there is one. */
static bool parse_address(const char *text, char *host, size_t room,
                          uint32_t *port, bool *by_name)
{
    const char *colon = strrchr(text, ':');
    size_t n = colon != NULL ? (size_t)(colon - text) : strlen(text);
    bool numbers = true;
    size_t i;

    if (n == 0 || n >= room) {
        return false;
    }

    for (i = 0; i < n; i++) {
        char c = text[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
            return false;
        }

        if (!((c >= '0' && c <= '9') || c == '.')) {
            numbers = false;
        }
    }

    memcpy(host, text, n);
    host[n] = '\0';
    *port = 445;

    if (colon != NULL) {
        char *end = NULL;
        long p = strtol(colon + 1, &end, 10);

        if (end == colon + 1 || *end != '\0' || p <= 0 || p > 65535) {
            return false;
        }

        *port = (uint32_t)p;
    }

    *by_name = !numbers;
    return true;
}

/* The NT hash: MD4 of the password in UTF-16, as "ntlm:" and hex. */
static bool nt_hash(const char *password, char out[40])
{
    static const char hex[] = "0123456789ABCDEF";
    struct smb2_utf16 *wide = smb2_utf8_to_utf16(password);
    uint8_t h[16];
    unsigned i;

    if (wide == NULL) {
        return false;
    }

    crypto_md4(wide->val, (size_t)wide->len * 2u, h);
    memset(wide->val, 0, (size_t)wide->len * 2u);
    free(wide);

    memcpy(out, "ntlm:", 5);

    for (i = 0; i < 16; i++) {
        out[5 + 2 * i]     = hex[h[i] >> 4];
        out[5 + 2 * i + 1] = hex[h[i] & 15];
    }

    out[37] = '\0';
    memset(h, 0, sizeof(h));
    return true;
}

static void ask_server(struct disk_request *req, struct disk_reply *rep,
                       bool probe)
{
    struct share_ask *ask = (struct share_ask *)(void *)req->u.data;
    char address[SHARE_ADDRESS_MAX];
    struct server *s, *was;
    char host[SHARE_ADDRESS_MAX];
    uint32_t port;
    bool by_name;

    field(address, sizeof(address), ask->address, sizeof(ask->address));

    if (!parse_address(address, host, sizeof(host), &port, &by_name)) {
        refuse(rep, SHARE_ERR_ADDRESS, "that is not an address: a name or four "
               "numbers, and a port after a colon if it is not 445");
        return;
    }

    if (!probe && ask->account[0] == '\0') {
        refuse(rep, SHARE_ERR_ACCOUNT, "an account is needed: guests are not "
               "let in");
        return;
    }

    if (!probe && ask->share[0] == '\0') {
        refuse(rep, SHARE_ERR_ADDRESS, "which share: smb://server/share");
        return;
    }

    was = server_find(address);

    if (was != NULL && was->pub.state == SHARE_STATE_CONNECTED) {
        refuse(rep, SHARE_ERR_ALREADY, "already connected to it");
        return;
    }

    if (was != NULL && was->pub.state == SHARE_STATE_ASKING) {
        refuse(rep, SHARE_ERR_ALREADY, "already asking it");
        return;
    }

    /* What was said of it before - refused, away, answered - is replaced. */
    if (was != NULL) {
        was->forgotten = true;
        server_close(was);

        if (was->thread < 0) {
            server_free(was);
        }
    }

    s = server_add();

    if (s == NULL) {
        refuse(rep, SHARE_ERR_NO_MEMORY, "no memory for another server");
        return;
    }

    copy(s->pub.address, sizeof(s->pub.address), address);
    copy(s->host, sizeof(s->host), host);
    s->port = port;
    s->by_name = by_name;
    s->probe = probe;
    s->pub.probe = probe ? 1u : 0u;

    if (!probe) {
        char password[SHARE_SECRET_MAX + 1];

        field(s->pub.share, sizeof(s->pub.share), ask->share, sizeof(ask->share));
        field(s->pub.account, sizeof(s->pub.account), ask->account,
              sizeof(ask->account));
        field(password, sizeof(password), ask->password, sizeof(ask->password));

        if (!nt_hash(password, s->hash)) {
            memset(password, 0, sizeof(password));
            server_free(s);
            refuse(rep, SHARE_ERR_NO_MEMORY, "no memory for the account");
            return;
        }

        memset(password, 0, sizeof(password));
    }

    become(s, SHARE_STATE_ASKING, NULL);
    s->deadline = s->since + ANSWER_SECONDS * counter_hz;

    if (!by_name) {
        copy(s->target, sizeof(s->target), address);

        if (strchr(s->target, ':') == NULL) {
            size_t n = strlen(s->target);

            snprintf(s->target + n, sizeof(s->target) - n, ":%u",
                     (unsigned)port);
        }

        start_smb(s);
    }

    /* The waiter, for the connection or for the name before it. */
    if ((s->smb2 != NULL || by_name) && !waiter_start(s)) {
        s->settled = true;
        become(s, SHARE_STATE_REFUSED, "no thread to wait for it with");
        server_close(s);
    }

    rep->error = DISK_OK;
}

static void status_page(const struct disk_request *req, struct disk_reply *rep)
{
    uint64_t now = kosmos_ticks();
    unsigned at = 0, i, n = 0;
    struct share_server *out = (struct share_server *)(void *)rep->u.data;

    for (i = 0; i < server_count && n < SHARE_PER_PAGE; i++) {
        const struct server *s = servers[i];

        if (s->forgotten) {
            continue;
        }

        if (at++ < req->offset) {
            continue;
        }

        out[n] = s->pub;
        out[n].in_state_ms = (now - s->since) * 1000u / counter_hz;
        n++;
    }

    rep->count = n;
    rep->length = (uint32_t)(n * sizeof(struct share_server));
    rep->offset = req->offset + n;

    for (; i < server_count; i++) {
        if (!servers[i]->forgotten) {
            rep->more = 1;
            break;
        }
    }
}

static void disconnect(struct disk_request *req, struct disk_reply *rep)
{
    struct share_ask *ask = (struct share_ask *)(void *)req->u.data;
    char address[SHARE_ADDRESS_MAX];
    struct server *s;

    field(address, sizeof(address), ask->address, sizeof(ask->address));
    s = server_find(address);

    if (s == NULL) {
        refuse(rep, SHARE_ERR_UNKNOWN, "nothing was asked of that server");
        return;
    }

    {
        struct say_line line;

        say_begin(&line);
        say_text(&line, "smbfs: ");
        say_text(&line, s->pub.address);
        say_text(&line, " let go");
        say_send(console, &line);
    }

    s->forgotten = true;
    server_close(s);

    if (s->thread < 0) {
        server_free(s);
    }

    rep->error = DISK_OK;
}

static void answer(struct message *msg, uint64_t sender)
{
    struct message out;
    struct disk_request req;
    struct disk_reply *rep = (struct disk_reply *)(void *)out.data;
    struct server *leaving = NULL;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rep);

    /* Exactly the shape, or nothing is read of it. */
    if (msg->length != sizeof(req)) {
        rep->error = DISK_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    memcpy(&req, msg->data, sizeof(req));
    memset(msg->data, 0, sizeof(req));          /* a password goes no further */

    switch (req.op) {
    case SHARE_OP_PROBE:
        ask_server(&req, rep, true);
        break;

    case SHARE_OP_CONNECT:
        ask_server(&req, rep, false);
        break;

    case SHARE_OP_STATUS:
        status_page(&req, rep);
        break;

    case SHARE_OP_DISCONNECT:
        disconnect(&req, rep);
        break;

    case SHARE_OP_WAITER: {
        struct server *s = (struct server *)(uintptr_t)req.bytes;

        if (req.offset != token || !server_known(s)) {
            rep->error = DISK_ERR_BAD_OP;
            break;
        }

        rep->error = from_waiter(s, req.flags, req.reserved, req.u.data);

        if (req.flags == SAID_LEAVING) {
            leaving = s;
        }
        break;
    }

    default:
        rep->error = DISK_ERR_BAD_OP;
        break;
    }

    memset(&req, 0, sizeof(req));
    (void)kosmos_reply(sender, &out);

    /* A waiter that has said it is going is waited for - it is past its
     * last call, so this is the moment it takes to leave - and what it
     * waited for is let go of if nobody wants it any more. */
    if (leaving != NULL) {
        (void)kosmos_thread_wait((unsigned long)leaving->thread);
        leaving->thread = -1;

        if (leaving->forgotten) {
            server_free(leaving);
        }
    }
}

/* Every server still asking whose time is up. */
static void deadlines(void)
{
    uint64_t now = kosmos_ticks();
    unsigned i;

    for (i = 0; i < server_count; i++) {
        struct server *s = servers[i];

        if (s->pub.state == SHARE_STATE_ASKING && s->deadline != 0
            && now >= s->deadline) {
            char why[SHARE_WHY_MAX];
            struct smb_link link;
            bool took = s->smb2 != NULL
                        && smb_kit_link(smb2_get_fd(s->smb2), &link) && link.taken;

            snprintf(why, sizeof(why), took
                     ? "%s took the connection and did not answer within %u seconds"
                     : "%s did not answer within %u seconds",
                     s->pub.address, ANSWER_SECONDS);
            s->settled = true;
            become(s, SHARE_STATE_AWAY, why);
            tell(s);
            server_close(s);
        }
    }
}

/* Scheduler ticks to the nearest deadline, or 0 for none. */
static unsigned long next_wait(void)
{
    uint64_t now = kosmos_ticks(), soonest = 0;
    unsigned i;

    for (i = 0; i < server_count; i++) {
        uint64_t d = servers[i]->deadline;

        if (servers[i]->pub.state == SHARE_STATE_ASKING && d != 0
            && (soonest == 0 || d < soonest)) {
            soonest = d;
        }
    }

    if (soonest == 0) {
        return 0;
    }

    if (soonest <= now) {
        return 1;
    }

    return (unsigned long)((soonest - now) * tick_hz / counter_hz) + 1u;
}

void smbfs_server(long endpoint_cap, long net_cap, long console_cap)
{
    struct sysinfo info;

    endpoint = endpoint_cap;
    net = net_cap;
    console = console_cap;

    memset(&info, 0, sizeof(info));

    if (kosmos_sysinfo(&info) == 0) {
        if (info.counter_hz != 0) counter_hz = info.counter_hz;
        if (info.tick_hz != 0) tick_hz = info.tick_hz;
    }

    if (kosmos_entropy(&token, sizeof(token)) != (long)sizeof(token)) {
        token = kosmos_ticks() ^ 0x534d4246u;   /* never reached: entropy answers */
    }

    smb_kit_start(net_cap);
    say(console, "smbfs: SMB 2 and 3, idle until a share is connected\n");

    for (;;) {
        struct message msg;
        uint64_t sender = 0;
        long got = kosmos_receive(endpoint, &msg, &sender, 0, next_wait());

        /*
         * Anything but a message is the deadline, as `net.c` reads it. The
         * kernel answers a receive that timed out with `IPC_NO_MESSAGE`
         * (-7), not the `SYS_NO_MESSAGE` (-107) `kosmos.h` says - this
         * compared with the second and, at its first deadline, returned
         * from serving altogether.
         */
        if (got == 0) {
            answer(&msg, sender);
        }

        deadlines();
    }
}
