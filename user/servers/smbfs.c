/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * smbfs: the SMB client, one process for every share on every server
 * (`docs/sharing.md`, *`smbfs`, the client*; steps N2, *smbfs connects*,
 * and N3, *a share is a folder*).
 *
 * **A server in C**, a role of `init.elf` started at boot and idle until
 * somebody connects: what runs on behalf of another process does not get a
 * collector (`CLAUDE.md`, *Language split*). It speaks SMB 2 and 3 through
 * the SMB Kit - libsmb2 as vendored, on the network stack's ring
 * (`user/kits/smb/`) - and answers `shareproto.h` on its endpoint: PROBE,
 * CONNECT, STATUS and DISCONNECT. **And `diskproto.h`, for a share's files**
 * (N3): `/Network/<server>/<share>/...` is a folder as `/Home` is - LIST and
 * GETATTR from a listing kept in memory, READ into the caller's region or a
 * page, `.super` and `.device` - and every write is refused in words, since
 * the client reads only.
 *
 * **Signed and sealed** (N4): libsmb2 checks each answer's signature or
 * opens its seal, and one that does not hold ends the connection. smbfs
 * says so as that - `DISK_ERR_ALTERED`, never "not answering" - and a read
 * it ends hands nothing over: what libsmb2 had already put in the caller's
 * region is taken back (`read_answer`).
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
 *
 * **Gone away, and back** (N5). A connected server asked something that says
 * nothing for `AWAY_SECONDS`, or whose connection closes, is away: what was
 * held of it is answered - a read in words, a folder from memory marked as
 * last heard - and its record is kept, with the NT hash, which is all a
 * sign-in needs. It is tried again on smbfs's own clock, two seconds after,
 * then four, eight, up to a minute; a try that answers signs in and
 * connects the share again - a new session, a new tree, new handles - and
 * nobody is asked anything. A try the server refuses for the account ends
 * the tries, and so does DISCONNECT, which forgets the hash with the rest.
 * RETRY is Try now. A connected server nobody is asking anything is sent an
 * ECHO once a minute, so one that has gone to sleep is noticed by itself.
 *
 * **Several shares on one connection, and the list of them** (N6). A server
 * record is a connection and one session, and holds a tree per share asked
 * for: a CONNECT to a server already signed into, as the same account, is
 * one TREE_CONNECT more and asks for no password. SHARES answers what the
 * server offers, from `srvsvc`'s NetShareEnum on `IPC$` - a tree like any
 * other, never listed in `/Network`.
 *
 * **libsmb2 stamps a request with the tree that is current when the request
 * is made**, and some of its operations make their next request inside the
 * callback of the last - a listing's QUERY_DIRECTORY after QUERY_DIRECTORY,
 * then CLOSE; an open that follows a link; the share list's pipe. So those
 * are *chains*, and while one runs the current tree is its tree: smbfs
 * selects a tree before every request it makes and puts the chain's tree
 * back after, and a chain on another tree of the same server waits until
 * the running ones end (`chain_take`). One request on its own - a READ, a
 * CLOSE, `.super`'s compound - only selects and puts back.
 */

#include <errno.h>
#include <fcntl.h>
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
#include "libsmb2-share-enum.h"

/*
 * **How long a server has to answer**: connecting, negotiating, signing in
 * and connecting the share, all of it. A number in one place, as the design
 * asks; a quarter of a second is the bound for a question about names
 * (step N3), and this is the bound for a person waiting on a connect.
 */
#define ANSWER_SECONDS      10u

/* How long a waiter waits before looking again at what it should wait for. */
#define WAITER_SECONDS       1u

/*
 * **The bounds a question about names is held to** (`docs/sharing.md`,
 * *Nothing on the desktop waits on it*), each a number in one place:
 *
 *   - a listing heard less than `FRESH_MS` ago is answered from memory;
 *   - otherwise the server is asked, and the caller held - **for as long as
 *     the server keeps answering**, and at most `SILENT_MS` of its saying
 *     nothing at all. Then the caller is answered from memory, marked as
 *     last heard, or told the server is away.
 *
 * The bound is on the server's *silence*, not on the listing's length: a
 * folder of 2,000 names takes several replies, and under emulation longer
 * than a quarter of a second, from a server that is answering the whole
 * time. Holding a caller only while nothing at all arrives keeps the
 * design's promise - a server gone to sleep stops nobody for longer than
 * the bound - without calling a slow listing a missing server.
 */
#define FRESH_MS          2000u
#define SILENT_MS          250u

/*
 * **And for a folder never listed, five seconds**: there is nothing in
 * memory to answer with, so the quarter second would only turn a slow
 * server into a missing one - which is what the full gate found, a
 * 2,000-name listing called "not answering" on a machine busy with ninety
 * other suites (5 October 2026). Five seconds of a server saying nothing at
 * all is a server that is not there; the caller still never waits on one
 * that has gone, only for that long.
 */
#define SILENT_UNKNOWN_MS 5000u

/* The bound for a question about `f`: short when memory can answer it. */
#define SILENT_FOR(f)  ticks_of_ms((f)->known ? SILENT_MS : SILENT_UNKNOWN_MS)

/* A file's handle, unused this long, is closed (`docs/sharing.md`: a handle
 * per file being read, so a program reading a film a megabyte at a time
 * opens it once). */
#define HANDLE_IDLE_SECONDS  5u

/* SMB's own bound on a request, after which it ends in words. A server that
 * says nothing at all is away sooner, at `AWAY_SECONDS`; this is the bound on
 * one request a server that is otherwise answering never answers. */
#define SMB_TIMEOUT_SECONDS 30

/*
 * **Gone away, and back** (`docs/sharing.md` step N5), each a number in one
 * place. A connected server asked something - a listing, a read, an open,
 * an ECHO - that has said nothing at all for `AWAY_SECONDS` is away: the
 * bound a connect is given, since it is the same question, whether there is
 * anybody there. Silence, not a request's age, as `SILENT_MS` is: a
 * megabyte arriving slowly is a server answering. Then a try after
 * `RETRY_FIRST_SECONDS`, each wait twice the last, never more than
 * `RETRY_MOST_SECONDS`; and an ECHO once `ECHO_SECONDS` have passed with
 * nothing asked or heard.
 */
#define AWAY_SECONDS         ANSWER_SECONDS
#define RETRY_FIRST_SECONDS  2u
#define RETRY_MOST_SECONDS  60u
#define ECHO_SECONDS        60u

/* READs in flight for one caller, and the most each asks for. */
#define READS_IN_FLIGHT      2u
#define READ_PIECE   (1u << 20)

/*
 * **An answer changed on the way** (step N4). libsmb2 checks every signed
 * reply's signature and opens every sealed one, and when either does not
 * hold it stops reading the connection and says so in a sentence - there
 * is no number for it. These are its sentences (`socket.c`, `libsmb2.c`,
 * `smb3-seal.c` at the pinned commit), and `tools/run_share.py`'s first part
 * holds them to a relay that changes one byte: a library that rewords them
 * fails that suite rather than turning a forged answer into "not
 * answering".
 */
#define ALTERED_SIGNED       1u
#define ALTERED_SEALED       2u

/* What a waiter says, in `flags`. */
#define SAID_RESOLVED        1u
#define SAID_EVENT           2u
#define SAID_LEAVING         3u

struct server;

/*
 * **A share connected on a server's session** (N6): its tree, which the
 * server numbered when it was connected - a new number on every connection,
 * so a server signed into again connects each of them again. `IPC$` is one
 * too, for the list of shares, and is never a folder.
 */
struct tree {
    struct tree *next;
    struct server *server;
    char     name[SHARE_NAME_MAX];
    uint32_t id;                    /* the server's number, while connected */
    bool     connected;
    bool     asking;                /* TREE_CONNECT in flight */
    bool     refused;               /* the server said no: `pub.why` */
    bool     was_connected;         /* a folder in /Network/<server> from then on */
    bool     ipc;                   /* IPC$: for SHARES, never listed */
};

/* A name in a folder, as the server listed it. */
struct entry {
    char    *name;
    uint32_t kind;                  /* DISK_KIND_FILE or DISK_KIND_DIR */
    uint64_t size;
    uint64_t modified;              /* seconds since 1970, the server's stamp */
};

/*
 * A folder looked at: its names sorted once, each with its facts, and when
 * they were heard - what LIST pages from and GETATTR answers from without
 * asking the server again (`docs/sharing.md`, *A share is a disk*).
 */
struct folder {
    struct folder *next;
    struct server *server;
    struct tree *tree;              /* the share it is in */
    char    *path;                  /* within the share: "", "many", "inside/deeper" */
    struct entry *entries;
    unsigned count;
    bool     known;                 /* listed at least once */
    uint64_t heard;                 /* counter ticks: when that listing arrived */
    bool     asking;                /* a listing is in flight, or waiting to be */
    bool     deferred;              /* waiting: a chain on another tree runs */
    uint64_t asked;                 /* counter ticks: since when */
    uint64_t asked_id;              /* libsmb2's message id when it was asked */
    int      failed;                /* the last listing's error, or 0 */
    uint32_t nt;                    /* and the server's status for it */
};

/* A file being read: its handle, kept a few seconds after the last read. */
struct handle {
    struct handle *next;
    struct server *server;
    struct tree *tree;
    char    *path;
    struct smb2fh *fh;              /* libsmb2's, once open */
    bool     opening;
    bool     deferred;              /* its open waits for another tree's chain */
    int      failed;                /* the open's error, or 0 */
    uint32_t nt;
    uint64_t size;                  /* the file's, as the open said */
    uint64_t used;                  /* counter ticks: last asked for */
    unsigned busy;                  /* callers reading through it */
};

struct server {
    struct share_server pub;        /* what STATUS says of it */

    bool     probe;
    bool     forgotten;             /* DISCONNECT: freed once its waiter has left */
    bool     closing;               /* libsmb2's callbacks are no longer listened to */
    bool     settled;               /* a callback decided how it went */
    uint64_t since;                 /* counter ticks: when `pub.state` became so */
    uint64_t serviced;              /* counter ticks: libsmb2 last looked at its clock */
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

    /* The share, as a folder (N3). */
    bool     was_connected;         /* listed in /Network from then on */
    uint64_t heard;                 /* counter ticks: anything last arrived */
    uint64_t received;              /* bytes, as the ring counted them */
    struct folder *folders;
    struct handle *handles;
    struct disk_device cost;        /* `.device` */

    /* An answer that arrived changed (step N4): ALTERED_SIGNED or
     * ALTERED_SEALED, and the connection ended for it. */
    uint32_t altered;

    /* Gone away, and back (N5). */
    bool     waiting;               /* something asked of it is unanswered */
    uint64_t asked_at;              /* counter ticks: since when */
    bool     echoing;               /* an ECHO in flight */
    bool     retrying;              /* a try under way: connecting, signing in */
    uint64_t try_deadline;          /* counter ticks: when that try has failed */
    uint64_t next_try;              /* counter ticks; 0 is no try planned */
    unsigned backoff;               /* seconds the last wait was; 0 while connected */
    uint64_t sent_before;           /* SMB requests of connections now closed */

    /* Its shares (N6): a tree each, the first the one a connection begins
     * with; and the chain running, on which tree, how many. */
    struct tree *trees;
    struct tree *chain_tree;
    unsigned chains;

    /* What it offers, once asked (`SHARE_OP_SHARES`). */
    bool     list_wanted;           /* asked for, and not yet begun */
    bool     listing;               /* NetShareEnum in flight */
    bool     listed_once;           /* heard: `offered` is the server's */
    bool     list_refused;          /* it would not say: `list_why` */
    char     list_why[SHARE_WHY_MAX];
    struct share_offered *offered;
    unsigned offered_count;
};

/*
 * **A caller held** - its reply kept, as `net.c` keeps a caller parked in
 * CONNECT - until the server answers or a bound passes. Nothing here blocks:
 * the reply goes from the sweep after whatever completed it.
 */
#define HELD_NAMES   1u             /* LIST or GETATTR, waiting on a folder */
#define HELD_READ    2u             /* READ, into a region or a page */
#define HELD_SUPER   3u             /* `.super`: QUERY_INFO of the share */

struct held {
    struct held *next;
    struct server *server;
    uint64_t sender;
    uint32_t kind;
    struct disk_request req;

    struct folder *folder;          /* HELD_NAMES */
    uint64_t since;                 /* counter ticks: when it was held */

    struct tree *tree;              /* HELD_SUPER: the share asked about */

    struct handle *handle;          /* HELD_READ */
    long     cap;                   /* the caller's region, or -1 */
    uint8_t *at;
    uint64_t room;
    uint64_t want, issued, done;    /* bytes: to read, asked for, arrived */
    unsigned in_flight;
    bool     started;
    uint8_t  page[DISK_DATA_MAX];

    struct smb2_statvfs vfs;        /* HELD_SUPER */
    bool     super_done;

    int      failed;                /* an error from libsmb2, or 0 */
    uint32_t nt;
    bool     answered;
};

/* One READ in flight: which caller, where in the file, and how much. */
struct piece {
    struct held *held;
    uint64_t offset;
    uint32_t count;
};

static long endpoint = -1;
static long net = -1;
static long console = -1;
static uint64_t token;              /* a waiter's word (`SHARE_OP_WAITER`) */
static uint64_t counter_hz = 62500000u;
static uint64_t tick_hz = 250u;

static struct server **servers;     /* in the order they were asked for */
static unsigned server_count, server_room;

static struct held *helds;          /* callers held, newest first */

/* STATUS requests answered since smbfs started: said in each STATUS reply's
 * `size`, so a window's asking can be counted where it arrives (N6, the
 * display harness's measure of Tracker's clock). */
static uint64_t status_served;

static void fail_held(struct server *s);
static void close_handles(struct server *s);
static void forget_share(struct server *s);

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

/*
 * A server kept to be signed into again (N5): its share was connected, it is
 * away, and nothing ended it for good - not DISCONNECT, not a refusal of the
 * account, not an answer that arrived changed.
 */
static bool retryable(const struct server *s)
{
    return !s->forgotten && !s->probe && s->was_connected && s->altered == 0
           && s->pub.state == SHARE_STATE_AWAY;
}

/* Something asked of a connected server: its silence is counted from now,
 * unless something asked before is still unanswered. */
static void note_asked(struct server *s)
{
    if (!s->waiting) {
        s->waiting = true;
        s->asked_at = kosmos_ticks();
    }
}

/* Away, and why, as STATUS and every refusal say it: "MACPEER is not
 * answering - it closed the connection". A try that fails changes only the
 * why - the server has been away since it went, not since the last try. */
static void away_because(struct server *s, const char *reason, bool went)
{
    char why[SHARE_WHY_MAX];

    snprintf(why, sizeof(why), "%s is not answering - %s", s->pub.name[0] != '\0'
             ? s->pub.name : s->pub.address, reason);

    if (went) {
        become(s, SHARE_STATE_AWAY, why);
    } else {
        copy(s->pub.why, sizeof(s->pub.why), why);
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

/* Whether libsmb2 stopped because an answer did not hold, and which way. */
static uint32_t altered_how(struct smb2_context *smb2)
{
    const char *said = smb2 != NULL ? smb2_get_error(smb2) : NULL;

    if (said == NULL) {
        return 0;
    }

    if (strstr(said, "Wrong signature") != NULL
        || strstr(said, "not signed but signing is required") != NULL) {
        return ALTERED_SIGNED;
    }

    /* "Failed to decrypt PDU" from `smb3-seal.c`, overwritten by the
     * caller's own "Failed to decrypyt pdu" (sic) in `socket.c`. */
    if (strstr(said, "Failed to decryp") != NULL) {
        return ALTERED_SEALED;
    }

    return 0;
}

static const char *altered_words(uint32_t how)
{
    return how == ALTERED_SEALED ? "its seal did not open"
                                 : "its signature did not match";
}

/* The server's answer refused, and the connection ended for it. */
static void became_altered(struct server *s, uint32_t how)
{
    char why[SHARE_WHY_MAX];

    s->altered = how;
    snprintf(why, sizeof(why), "%s's answer was changed on the way - %s - "
             "so it was refused and the connection ended", named(s),
             altered_words(how));
    become(s, SHARE_STATE_AWAY, why);
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
            say_text(&line, s->pub.share[0] != '\0' ? s->pub.share : "signed in");
            say_text(&line, " as ");
            say_text(&line, s->pub.account);
            say_text(&line, s->pub.sign_ins > 1 ? ", signed in again" : "");
        }
        break;
    default:
        say_text(&line, s->pub.why);       /* which names the server */
        break;
    }

    say_send(console, &line);
}

/*------------------------------------------------------------------------
 * Trees, and the chains that run on them (N6).
 *----------------------------------------------------------------------*/

static struct tree *tree_find(struct server *s, const char *name);
static bool same_name(const char *a, const char *b);

/* The tree libsmb2 stamps the next request with. */
static void tree_select(struct server *s, const struct tree *t)
{
    if (s->smb2 != NULL && t != NULL && t->connected) {
        (void)smb2_select_tree_id(s->smb2, t->id);
    }
}

/* Back to the tree a running chain is on, so its next request - made in a
 * callback smbfs does not see - goes where it belongs. */
static void tree_back(struct server *s)
{
    if (s->chains > 0) {
        tree_select(s, s->chain_tree);
    }
}

/* A chain begun on `t`, or false: one on another tree is running, and this
 * one waits for it to end. */
static bool chain_take(struct server *s, struct tree *t)
{
    if (s->chains > 0 && s->chain_tree != t) {
        return false;
    }

    s->chains++;
    s->chain_tree = t;
    tree_select(s, t);
    return true;
}

/* A chain over: its last callback has run, and it makes no more requests. */
static void chain_give(struct server *s)
{
    if (s->chains > 0) {
        s->chains--;
    }

    if (s->chains == 0) {
        s->chain_tree = NULL;
    }
}

/* The tree a connection begins with: the first asked for and not refused. */
static struct tree *tree_first(struct server *s)
{
    struct tree *t;

    for (t = s->trees; t != NULL; t = t->next) {
        if (!t->refused) {
            return t;
        }
    }

    return NULL;
}

static struct tree *tree_add(struct server *s, const char *name, bool ipc)
{
    struct tree *t = tree_find(s, name), **link;

    if (t != NULL) {
        return t;
    }

    t = calloc(1, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }

    t->server = s;
    t->ipc = ipc;
    copy(t->name, sizeof(t->name), name);

    /* In the order asked for, which is the order `/Network/<server>` lists. */
    for (link = &s->trees; *link != NULL; link = &(*link)->next) {
    }

    *link = t;
    return t;
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

    forget_share(s);

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
        s->sent_before += s->smb2->message_id;
        close_handles(s);
        smb2_destroy_context(s->smb2);
        s->smb2 = NULL;

        /* A listing that was out is over with the connection, whether or
         * not libsmb2 called back for it - or waiting for a chain that
         * will not come: the next is asked of the next connection (N5). */
        {
            struct folder *f;
            struct tree *t;

            for (f = s->folders; f != NULL; f = f->next) {
                if (f->asking) {
                    f->asking = false;
                    f->deferred = false;
                    f->failed = -ENETRESET;
                    f->nt = SMB2_STATUS_SHUTDOWN;
                }
            }

            /* Every tree was this connection's numbering; the next one
             * connects them again (N6). */
            for (t = s->trees; t != NULL; t = t->next) {
                t->connected = false;
                t->asking = false;
                t->id = 0;
            }

            s->chains = 0;
            s->chain_tree = NULL;

            if (s->listing) {
                s->listing = false;
                s->list_wanted = !s->listed_once;
            }
        }
    }

    __atomic_store_n(&s->want, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s->stop, 1u, __ATOMIC_RELEASE);

    /* What was asked of it ends now: a read in words, a question about
     * names from memory as last heard (the sweep answers both). */
    fail_held(s);

    /* A try that ended is over, and the next is planned by `deadlines`, a
     * wait twice as long (N5). */
    s->waiting = false;
    s->echoing = false;
    s->retrying = false;
    s->next_try = 0;

    /* And the NT hash forgotten once nothing will sign in with it. */
    if (!retryable(s)) {
        memset(s->hash, 0, sizeof(s->hash));
    }
}

/*------------------------------------------------------------------------
 * How it went, in words.
 *----------------------------------------------------------------------*/

static void refused_or_away(struct server *s, int status)
{
    char why[SHARE_WHY_MAX], reason[SHARE_WHY_MAX];
    uint32_t nt = s->smb2 != NULL ? (uint32_t)smb2_get_nterror(s->smb2) : 0;
    struct smb_link link;
    bool linked = s->smb2 != NULL && smb_kit_link(smb2_get_fd(s->smb2), &link);
    uint32_t state = SHARE_STATE_REFUSED;
    bool for_good = true;           /* the server said no: a try changes nothing */

    (void)status;

    if (altered_how(s->smb2) != 0) {
        became_altered(s, altered_how(s->smb2));
        return;
    }

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
        snprintf(reason, sizeof(reason), "nothing at %s took the connection",
                 s->pub.address);
        state = SHARE_STATE_AWAY;
        for_good = false;
    } else if (!linked && smb_kit_last_refusal() != NET_OK) {
        snprintf(why, sizeof(why), "%s could not be reached (the network "
                 "said %u)", s->pub.address, (unsigned)smb_kit_last_refusal());
        snprintf(reason, sizeof(reason), "it could not be reached (the "
                 "network said %u)", (unsigned)smb_kit_last_refusal());
        state = SHARE_STATE_AWAY;
        for_good = false;
    } else if (linked && link.received == 0) {
        /* It took the connection, heard NEGOTIATE in SMB 2's form, and hung
         * up without a word: a server of SMB 1, which this machine does not
         * speak (`docs/sharing.md`, *Not here, and why*). Or, on a try, a
         * server still starting - which is a reason to try again. */
        snprintf(why, sizeof(why), "%s took the connection and hung up "
                 "without answering: it does not speak SMB 2 or 3",
                 s->pub.address);
        snprintf(reason, sizeof(reason), "it took the connection and hung up");
        for_good = false;
    } else {
        const char *said = s->smb2 != NULL ? smb2_get_error(s->smb2) : "";

        snprintf(why, sizeof(why), "%s: %s", named(s),
                 said != NULL && said[0] != '\0' ? said : "it stopped answering");
        snprintf(reason, sizeof(reason), "%s",
                 said != NULL && said[0] != '\0' ? said : "it stopped answering");
        for_good = false;
    }

    /* A try at a server that went away: still away, and why this try did
     * not answer - unless the server said no to the account, which no
     * number of tries changes. */
    if (s->retrying && !for_good) {
        away_because(s, reason, false);
        return;
    }

    become(s, state, why);
}

/*------------------------------------------------------------------------
 * libsmb2's callbacks: they decide, and the loop acts after.
 *----------------------------------------------------------------------*/

static void read_session(struct server *s)
{
    s->pub.dialect = smb2_get_dialect(s->smb2);
    s->pub.signing = s->smb2->sign ? 1 : 0;
    s->pub.sealing = s->smb2->seal ? 1 : 0;

    /*
     * **What the server calls itself** (`testing.md` 18.415): its NetBIOS
     * computer name, or its DNS name's first label, out of NTLM's challenge
     * as the SMB Kit heard it arrive (`ntlm_name.c`).
     *
     * This took the challenge's TargetName, which libsmb2 keeps as the
     * domain when it was given none - and smbfs gives none. Samba's peer
     * fills it with its NetBIOS name, so it read MACPEER; macOS's own server,
     * reached at 192.168.1.38, filled it with "192", and `share status`,
     * Tracker's Network group and the trail all called Diego's Mac that.
     * TargetName is the domain's name where there is one, which is not the
     * computer's. **With no name given the server is called by its whole
     * address** (`named`), never a piece of it: a name of digits and dots
     * is refused by the decoder, and libsmb2's domain is not read at all.
     */
    if (!smb_kit_server_name(smb2_get_fd(s->smb2), s->pub.name,
                             sizeof(s->pub.name))) {
        s->pub.name[0] = '\0';
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
        s->was_connected = true;
        s->heard = kosmos_ticks();
        s->pub.sign_ins++;

        /* Back, if it had gone (N5): the tries are over, and the next time
         * it goes they start again from the first. */
        s->retrying = false;
        s->next_try = 0;
        s->backoff = 0;

        /* From here a request the server never answers ends in words after
         * SMB's own bound; the loop services libsmb2 once a second while
         * anything is in flight, which is what the timeout asks. */
        smb2_set_timeout(s->smb2, SMB_TIMEOUT_SECONDS);

        /* The share it began with is the tree libsmb2 has just connected;
         * every other one asked for is connected on the same session, by
         * the loop (`trees_wanted`), once this callback is over (N6). */
        {
            struct tree *t = tree_first(s);

            if (t != NULL) {
                t->id = smb2_tree_id(s->smb2);
                t->connected = true;
                t->was_connected = !t->ipc;
            }
        }
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
        uint32_t how = altered_how(s->smb2);

        /* A connected server that hung up: away, and tried again on
         * smbfs's own clock (N5). Or one whose answer arrived changed,
         * which is said as that, and is not signed in again by itself. */
        if (how != 0) {
            became_altered(s, how);
        } else {
            away_because(s, "it closed the connection", true);
        }

        tell(s);
        server_close(s);
        return;
    }

    if (s->smb2 != NULL) {
        struct smb_link link;

        if (smb_kit_link(smb2_get_fd(s->smb2), &link)) {
            __atomic_store_n(&s->handle, link.handle, __ATOMIC_RELEASE);

            /* Anything at all arriving is the server answering: what a
             * question about names is held against (`SILENT_MS`). */
            if (link.received != s->received) {
                s->received = link.received;
                s->heard = kosmos_ticks();
            }
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
        struct tree *first = tree_first(s);

        rc = smb2_connect_share_async(s->smb2, s->target,
                                      first != NULL ? first->name : "IPC$",
                                      s->pub.account, connected, s);
    }

    /*
     * **The hash is kept** while the server may be signed into again (N5):
     * a server that comes back after sleep is signed into from it, without
     * asking anybody. `server_close` forgets it once nothing will try - a
     * server never connected, refused, changed on the way, or let go.
     */

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

    /* A server signed into again keeps the page its first waiter polled
     * through (N5); only its thread is new. */
    if (s->poll_set != NULL) {
        goto thread;
    }

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

thread:
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

            if (s->retrying) {
                away_because(s, why, false);
            } else {
                become(s, SHARE_STATE_AWAY, why);
            }

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

/* The conversation begun - a name looked up first, by the waiter, if the
 * address is one - for a CONNECT or PROBE, and for a try (N5). */
static void begin(struct server *s)
{
    if (!s->by_name) {
        start_smb(s);
    }

    /* The waiter, for the connection or for the name before it. */
    if ((s->smb2 != NULL || s->by_name) && !waiter_start(s)) {
        s->settled = true;

        if (s->retrying) {
            away_because(s, "no thread to wait for it with", false);
        } else {
            become(s, SHARE_STATE_REFUSED, "no thread to wait for it with");
        }

        server_close(s);
    }
}

/*
 * **A try** (N5): the same conversation as the first CONNECT - NEGOTIATE,
 * SESSION_SETUP from the NT hash kept, TREE_CONNECT - on a new connection,
 * so a new session and a new tree; the server's record, its folders and
 * its name stay as they were, and it stays away until the try answers.
 */
static void retry(struct server *s)
{
    s->retrying = true;
    s->settled = false;
    s->closing = false;
    s->next_try = 0;
    s->received = 0;
    s->try_deadline = kosmos_ticks() + ANSWER_SECONDS * counter_hz;
    __atomic_store_n(&s->want, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s->handle, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s->stop, 0u, __ATOMIC_RELEASE);
    begin(s);
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

    was = server_find(address);

    /*
     * **One share more on the same connection** (N6): a server already
     * signed into, as the same account, is asked for the share with a
     * TREE_CONNECT, and nothing else - no password is asked for or used.
     */
    if (was != NULL && was->pub.state == SHARE_STATE_CONNECTED && !probe) {
        char account[SHARE_ACCOUNT_MAX], share[SHARE_NAME_MAX];
        struct tree *t;

        field(account, sizeof(account), ask->account, sizeof(ask->account));
        field(share, sizeof(share), ask->share, sizeof(ask->share));

        if (!same_name(account, was->pub.account)) {
            char why[SHARE_WHY_MAX];

            snprintf(why, sizeof(why), "connected to %s as %s already: let it "
                     "go to sign in as %s", named(was), was->pub.account, account);
            refuse(rep, SHARE_ERR_ALREADY, why);
            return;
        }

        if (share[0] == '\0') {
            /* Signed in already: what it offers is SHARES's to say. */
            was->list_wanted = !was->listed_once && !was->listing;
            rep->error = DISK_OK;
            return;
        }

        t = tree_find(was, share);

        if (t != NULL && (t->connected || t->asking)) {
            refuse(rep, SHARE_ERR_ALREADY, t->connected ? "already connected to "
                   "that share" : "already asking for that share");
            return;
        }

        t = tree_add(was, share, false);

        if (t == NULL) {
            refuse(rep, SHARE_ERR_NO_MEMORY, "no memory for another share");
            return;
        }

        t->refused = false;
        copy(was->pub.share, sizeof(was->pub.share), share);
        was->pub.why[0] = '\0';
        rep->error = DISK_OK;
        return;                     /* connected by the loop: `trees_wanted` */
    }

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

        /*
         * The share it begins with - or, with none named, `IPC$` alone: a
         * sign-in and the list of what it offers, for "choose one once it
         * answers" (N6).
         */
        if (tree_add(s, s->pub.share[0] != '\0' ? s->pub.share : "IPC$",
                     s->pub.share[0] == '\0') == NULL) {
            server_free(s);
            refuse(rep, SHARE_ERR_NO_MEMORY, "no memory for the share");
            return;
        }

        s->list_wanted = s->pub.share[0] == '\0';
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
    }

    begin(s);
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

        /* Away (N5): trying now, or when the next try is. */
        if (retryable(s)) {
            bool due = s->next_try != 0 && s->next_try <= now;

            out[n].trying = (s->retrying || due) ? 1u : 0u;
            out[n].next_try_ms = (!s->retrying && s->next_try > now)
                ? (uint32_t)((s->next_try - now) * 1000u / counter_hz) : 0u;
        }

        n++;
    }

    rep->count = n;
    rep->length = (uint32_t)(n * sizeof(struct share_server));
    rep->offset = req->offset + n;

    if (req->offset == 0) {
        status_served++;
    }

    rep->size = status_served;

    for (; i < server_count; i++) {
        if (!servers[i]->forgotten) {
            rep->more = 1;
            break;
        }
    }
}

/*
 * **Try now** (N5): a server away is tried at once rather than when its wait
 * runs out - as soon as the last try's waiter has gone, which `deadlines`
 * sees. Answered at once, as CONNECT is; STATUS says how it went.
 */
static void retry_now(struct disk_request *req, struct disk_reply *rep)
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

    if (s->pub.state == SHARE_STATE_CONNECTED) {
        refuse(rep, SHARE_ERR_ALREADY, "already connected to it");
        return;
    }

    if (s->retrying) {
        refuse(rep, SHARE_ERR_ALREADY, "already trying it");
        return;
    }

    if (!retryable(s)) {
        refuse(rep, SHARE_ERR_NOT_KEPT, "it is not kept to sign into again: "
               "connect to it, with the password");
        return;
    }

    s->next_try = kosmos_ticks();
    rep->error = DISK_OK;
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

/*------------------------------------------------------------------------
 * A share's files: `diskproto.h` (step N3).
 *
 * `/Network`'s mount carries smbfs's capability beside the stack's, and the
 * namespace sends every file operation under it here, the path as the
 * mount sees it: `/` is `/Network` itself, `/MACPEER` a server, and
 * `/MACPEER/Projects/many/f0001.txt` a file in a share. Servers and shares
 * are answered from smbfs's own records and never reach the network.
 *----------------------------------------------------------------------*/

static uint64_t ms_of(uint64_t counter_ticks)
{
    return counter_ticks * 1000u / counter_hz;
}

static uint64_t ticks_of_ms(uint64_t ms)
{
    return ms * counter_hz / 1000u;
}

/* SMB's names do not care about case, so neither does anything matched
 * against them here. */
static bool same_name(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;

        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');

        if (x != y) return false;
        if (x == '\0') return true;
    }
}

static struct tree *tree_find(struct server *s, const char *name)
{
    struct tree *t;

    for (t = s->trees; t != NULL; t = t->next) {
        if (same_name(t->name, name)) {
            return t;
        }
    }

    return NULL;
}

/* A share in `/Network/<server>`: connected once, and not `IPC$`. */
static bool tree_listed(const struct tree *t)
{
    return !t->ipc && t->was_connected;
}

/* A server in `/Network`: one whose share was connected, and still known. */
static bool listed(const struct server *s)
{
    return !s->forgotten && !s->probe && s->was_connected;
}

/*
 * What a server is called in `/Network`: the name it gives itself, which is
 * what the mockup draws - or, when another listed server gives the same
 * name, that name and its address, so the two are told apart.
 */
static void server_label(const struct server *s, char *out, size_t room)
{
    unsigned i;

    for (i = 0; i < server_count; i++) {
        const struct server *o = servers[i];

        if (o != s && listed(o) && same_name(named(o), named(s))) {
            snprintf(out, room, "%s (%s)", named(s), s->pub.address);
            return;
        }
    }

    copy(out, room, named(s));
}

static struct server *server_called(const char *name)
{
    char label[SHARE_ADDRESS_MAX + SHARE_NAME_MAX + 4];
    unsigned i;

    for (i = 0; i < server_count; i++) {
        struct server *s = servers[i];

        if (!listed(s)) {
            continue;
        }

        server_label(s, label, sizeof(label));

        if (same_name(label, name) || same_name(s->pub.address, name)) {
            return s;
        }
    }

    return NULL;
}

/* Where a path is: `/Network` itself, a server, a share, or inside one. */
struct where {
    struct server *s;               /* NULL: `/Network` itself */
    struct tree *tree;              /* the share, when one is named */
    bool     share;                 /* the share named, or something in it */
    char     inside[DISK_PATH_MAX]; /* within the share, no slashes at the ends */
    const char *name;               /* the last part of `inside`, or "" */
};

static uint32_t where_is(const char *path, struct where *w)
{
    char part[DISK_PATH_MAX];
    const char *at = path;
    size_t n, len;

    memset(w, 0, sizeof(*w));
    w->name = "";

    while (*at == '/') at++;

    if (*at == '\0') {
        return DISK_OK;
    }

    n = strcspn(at, "/");
    memcpy(part, at, n);
    part[n] = '\0';
    w->s = server_called(part);

    if (w->s == NULL) {
        return DISK_ERR_KFS + 7;            /* no such file */
    }

    at += n;
    while (*at == '/') at++;

    if (*at == '\0') {
        return DISK_OK;
    }

    n = strcspn(at, "/");

    memcpy(part, at, n);
    part[n] = '\0';

    w->tree = tree_find(w->s, part);

    if (w->tree == NULL || !tree_listed(w->tree)) {
        return DISK_ERR_KFS + 7;
    }

    w->share = true;
    at += n;
    while (*at == '/') at++;

    copy(w->inside, sizeof(w->inside), at);
    len = strlen(w->inside);

    while (len > 0 && w->inside[len - 1] == '/') {
        w->inside[--len] = '\0';
    }

    /* `.` and `..` are the namespace's to walk, never a server's. */
    for (at = w->inside; *at != '\0'; at += n) {
        while (*at == '/') at++;
        n = strcspn(at, "/");

        if ((n == 1 && at[0] == '.') || (n == 2 && at[0] == '.' && at[1] == '.')) {
            return DISK_ERR_KFS + 18;       /* a path may not contain . or .. */
        }
    }

    w->name = strrchr(w->inside, '/') != NULL ? strrchr(w->inside, '/') + 1
                                             : w->inside;
    return DISK_OK;
}

/* A refusal with its words, which the namespace shows as they are. */
static void said(struct disk_reply *rp, uint32_t error, const char *format,
                 const char *a, const char *b, const char *c)
{
    rp->error = error;
    snprintf(rp->u.data, sizeof(rp->u.data), format, a, b, c);
    rp->length = (uint32_t)strlen(rp->u.data);
}

static void away(struct server *s, struct disk_reply *rp)
{
    char label[SHARE_ADDRESS_MAX + SHARE_NAME_MAX + 4];

    server_label(s, label, sizeof(label));
    said(rp, DISK_ERR_AWAY, "%s is not answering", label, "", "");
}

/* What libsmb2 said went wrong, as the namespace's words. */
static void refused(struct server *s, int failed, uint32_t nt, const char *path,
                    struct disk_reply *rp)
{
    if (s->altered != 0) {
        char label[SHARE_ADDRESS_MAX + SHARE_NAME_MAX + 4];

        server_label(s, label, sizeof(label));
        said(rp, DISK_ERR_ALTERED, "%s's answer was changed on the way - %s - "
             "and was refused; nothing of it was handed over", label,
             altered_words(s->altered), "");
        return;
    }

    if (s->closing || s->smb2 == NULL || nt == SMB2_STATUS_SHUTDOWN
        || nt == SMB2_STATUS_IO_TIMEOUT || failed == -ETIMEDOUT
        || failed == -ENETRESET || failed == -ECONNRESET) {
        away(s, rp);
    } else if (nt == SMB2_STATUS_OBJECT_NAME_NOT_FOUND
               || nt == SMB2_STATUS_OBJECT_PATH_NOT_FOUND
               || nt == SMB2_STATUS_NO_SUCH_FILE || failed == -ENOENT) {
        said(rp, DISK_ERR_KFS + 7, "no such file", "", "", "");
    } else if (nt == SMB2_STATUS_FILE_IS_A_DIRECTORY) {
        said(rp, DISK_ERR_KFS + 9, "that is a directory", "", "", "");
    } else if (nt == SMB2_STATUS_NOT_A_DIRECTORY || failed == -ENOTDIR) {
        said(rp, DISK_ERR_KFS + 8, "not a directory", "", "", "");
    } else if (nt == SMB2_STATUS_ACCESS_DENIED || failed == -EACCES) {
        said(rp, DISK_ERR_DENIED, "%s does not let %s read %s", named(s),
             s->pub.account, path);
    } else {
        const char *why = s->smb2 != NULL ? smb2_get_error(s->smb2) : "";

        said(rp, DISK_ERR_DENIED, "%s refused it: %s", named(s),
             why != NULL && why[0] != '\0' ? why : "it did not say why", "");
    }
}

/*------------------------------------------------------------------------
 * Folders, as listed.
 *----------------------------------------------------------------------*/

static struct folder *folder_get(struct server *s, struct tree *t,
                                 const char *path)
{
    struct folder *f;

    for (f = s->folders; f != NULL; f = f->next) {
        if (f->tree == t && same_name(f->path, path)) {
            return f;
        }
    }

    f = calloc(1, sizeof(*f));

    if (f == NULL || (f->path = strdup(path)) == NULL) {
        free(f);
        return NULL;
    }

    f->server = s;
    f->tree = t;
    f->next = s->folders;
    s->folders = f;
    return f;
}

static void entries_free(struct entry *e, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) {
        free(e[i].name);
    }

    free(e);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

/* libsmb2's answer to a listing: every name and its facts, sorted once. */
static void folder_listed(struct smb2_context *smb2, int status, void *data,
                          void *private_data)
{
    struct folder *f = private_data;
    struct server *s = f->server;
    struct smb2dir *dir = data;
    struct smb2dirent *ent;
    struct entry *e = NULL;
    unsigned count = 0, room = 0;
    uint64_t now = kosmos_ticks();

    f->asking = false;

    /* The chain's last callback: a listing makes no more requests. */
    if (!s->closing) {
        chain_give(s);
    }

    if (status != 0 || dir == NULL) {
        f->failed = status != 0 ? status : -EIO;
        f->nt = smb2_get_nterror(smb2);
        return;                     /* libsmb2 freed the listing itself */
    }

    while ((ent = smb2_readdir(smb2, dir)) != NULL) {
        if (strcmp(ent->name, ".") == 0 || strcmp(ent->name, "..") == 0) {
            continue;
        }

        if (count == room) {
            unsigned more = room == 0 ? 64u : room * 2u;
            struct entry *grown = realloc(e, more * sizeof(*e));

            if (grown == NULL) {
                break;
            }

            e = grown;
            room = more;
        }

        e[count].name = strdup(ent->name);

        if (e[count].name == NULL) {
            break;
        }

        e[count].kind = ent->st.smb2_type == SMB2_TYPE_DIRECTORY
                        ? DISK_KIND_DIR : DISK_KIND_FILE;
        e[count].size = e[count].kind == DISK_KIND_DIR ? 0 : ent->st.smb2_size;
        e[count].modified = ent->st.smb2_mtime;
        count++;
    }

    smb2_closedir(smb2, dir);

    if (ent != NULL) {              /* stopped short: out of memory */
        entries_free(e, count);
        f->failed = -ENOMEM;
        f->nt = 0;
        return;
    }

    qsort(e, count, sizeof(*e), by_name);
    entries_free(f->entries, f->count);
    f->entries = e;
    f->count = count;
    f->known = true;
    f->failed = 0;
    f->nt = 0;
    f->heard = now;

    s->cost.listing_requests += smb2->message_id - f->asked_id;
    s->cost.listing_counter_ticks += now - f->asked;
}

/* The server asked for a folder's names, unless it is being asked already. */
static bool folder_ask(struct folder *f)
{
    struct server *s = f->server;

    if (f->asking && !f->deferred) {
        return true;
    }

    if (s->smb2 == NULL || s->closing || s->pub.state != SHARE_STATE_CONNECTED
        || !f->tree->connected) {
        f->asking = f->deferred = false;
        return false;
    }

    if (!f->asking) {
        f->asked = kosmos_ticks();
    }

    /* A chain on another of the server's shares is running: this one waits
     * for it, and is begun by the loop when it ends (`chains_waiting`). */
    if (!chain_take(s, f->tree)) {
        f->asking = true;
        f->deferred = true;
        note_asked(s);
        return true;
    }

    f->asked_id = s->smb2->message_id;
    f->deferred = false;

    if (smb2_opendir_async(s->smb2, f->path, folder_listed, f) < 0) {
        chain_give(s);
        tree_back(s);
        f->asking = false;
        return false;
    }

    note_asked(s);
    f->asking = true;
    s->cost.listings++;
    s->cost.cache_misses++;
    return true;
}

/* How long the server has said nothing since `since`, in counter ticks. */
static uint64_t silent(const struct server *s, uint64_t since)
{
    uint64_t from = s->heard > since ? s->heard : since;
    uint64_t now = kosmos_ticks();

    return now > from ? now - from : 0;
}

/* LIST's page or GETATTR's facts, from a folder in memory. */
static void names_from(struct server *s, const struct disk_request *rq,
                       const struct where *w, struct folder *f, bool marked,
                       struct disk_reply *rp)
{
    unsigned i;

    if (marked) {
        rp->heard = 1;
        rp->heard_ms = s->heard != 0 ? ms_of(kosmos_ticks() - s->heard) : 0;
    }

    if (rq->op == DISK_OP_LIST) {
        uint32_t used = 0;

        for (i = (unsigned)rq->offset; i < f->count; i++) {
            size_t n = strlen(f->entries[i].name);

            if (used + n + 1u > DISK_DATA_MAX) {
                rp->more = 1;
                rp->offset = i;
                break;
            }

            memcpy(rp->u.data + used, f->entries[i].name, n + 1);
            used += (uint32_t)n + 1u;
            rp->count++;
        }

        rp->length = used;
        return;
    }

    for (i = 0; i < f->count; i++) {
        if (same_name(f->entries[i].name, w->name)) {
            rp->node.kind = f->entries[i].kind;
            rp->node.size = f->entries[i].size;
            rp->node.mtime = f->entries[i].modified;
            rp->node.modified = f->entries[i].modified;
            rp->node.dated = 1;
            return;
        }
    }

    said(rp, DISK_ERR_KFS + 7, "no such file", "", "", "");
}

/* The folder a question about names is answered from: the folder itself
 * for LIST, the one holding the name for GETATTR. */
static void names_folder(const struct disk_request *rq, const struct where *w,
                         char *out, size_t room)
{
    if (rq->op == DISK_OP_LIST) {
        copy(out, room, w->inside);
    } else {
        size_t n = (size_t)(w->name - w->inside);

        copy(out, room, w->inside);
        out[n > 0 ? n - 1 : 0] = '\0';
    }
}

/*
 * A question the server has not answered within its bound: from memory,
 * marked as last heard, or "away" if it has never been listed.
 */
static void names_unanswered(struct server *s, const struct disk_request *rq,
                             const struct where *w, struct folder *f,
                             struct disk_reply *rp)
{
    if (f != NULL && f->known) {
        names_from(s, rq, w, f, true, rp);
    } else {
        away(s, rp);
    }
}

/* A listing that came back, or failed: what the caller is told. */
static void names_settled(struct server *s, const struct disk_request *rq,
                          const struct where *w, struct folder *f,
                          struct disk_reply *rp)
{
    if (f->failed == 0) {
        names_from(s, rq, w, f, false, rp);
        return;
    }

    /* A folder the server says is not there is not there; a server that
     * went quiet or away is answered for from memory. */
    {
        struct disk_reply why;

        memset(&why, 0, sizeof(why));
        refused(s, f->failed, f->nt, w->inside, &why);

        if (why.error == DISK_ERR_AWAY) {
            names_unanswered(s, rq, w, f, rp);
        } else {
            *rp = why;
        }
    }
}

/*------------------------------------------------------------------------
 * Callers held, and the sweep that answers them.
 *----------------------------------------------------------------------*/

static struct held *held_add(struct server *s, uint32_t kind,
                             const struct disk_request *rq, uint64_t sender)
{
    struct held *h = calloc(1, sizeof(*h));

    if (h == NULL) {
        return NULL;
    }

    h->server = s;
    h->kind = kind;
    h->req = *rq;
    h->sender = sender;
    h->cap = -1;
    h->since = kosmos_ticks();
    h->next = helds;
    helds = h;
    return h;
}

static void region_let_go(struct held *h)
{
    if (h->at != NULL) {
        kosmos_share_unmap((unsigned long)(uintptr_t)h->at,
                           (unsigned long)(h->room / 4096u));
        h->at = NULL;
    }

    if (h->cap >= 0) {
        (void)kosmos_cap_drop(h->cap);
        h->cap = -1;
    }
}

static void reply_with(uint64_t sender, const struct disk_reply *rp)
{
    struct message out;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rp);
    memcpy(out.data, rp, sizeof(*rp));
    (void)kosmos_reply(sender, &out);
}

/* A READ answered: its bytes, or why not. */
static void read_answer(struct held *h, struct disk_reply *rp)
{
    struct server *s = h->server;
    uint64_t size = h->handle != NULL ? h->handle->size : 0;

    if (h->failed != 0) {
        if (h->failed == -ERANGE) {
            rp->error = DISK_ERR_REGION;
        } else {
            refused(s, h->failed, h->nt, h->req.path, rp);
        }

        /*
         * **A read that fails hands over nothing** (step N4). libsmb2 reads
         * a READ's data straight into the caller's region and checks the
         * signature after the last byte, so a reply changed on the way
         * has already landed when it is refused. The answer says it failed
         * and the bytes it wrote are taken back, so a caller that looks
         * anyway finds zeros, never a forgery.
         */
        if (h->at != NULL && h->issued > 0) {
            memset(h->at, 0, (size_t)(h->issued < h->room ? h->issued : h->room));
        }

        return;
    }

    rp->size = size;
    s->cost.read_counter_ticks += kosmos_ticks() - h->since;

    if (h->req.flags & DISK_REGION) {
        rp->bytes = h->done;
        return;
    }

    memcpy(rp->u.data, h->page, (size_t)h->done);
    rp->length = (uint32_t)h->done;
    rp->offset = h->req.offset + h->done;
    rp->more = rp->offset < size ? 1u : 0u;
}

static void super_answer(struct held *h, struct disk_reply *rp)
{
    struct server *s = h->server;
    struct disk_super *su = &rp->u.super;
    uint64_t block = h->vfs.f_bsize != 0 ? h->vfs.f_bsize : 512u;

    if (h->failed != 0) {
        refused(s, h->failed, h->nt, h->req.path, rp);
        return;
    }

    su->present = 1;
    su->formatted = 1;
    su->sector_size = (uint32_t)block;
    su->sectors = h->vfs.f_blocks;
    su->bytes = h->vfs.f_blocks * block;
    su->block_size = (uint32_t)block;
    su->blocks = h->vfs.f_blocks > 0xffffffffu ? 0xffffffffu : (uint32_t)h->vfs.f_blocks;
    su->free_known = 1;
    su->free_blocks = h->vfs.f_bavail;
    snprintf(su->where, sizeof(su->where), "%s's %s, over SMB %s%s%s", named(s),
             h->tree != NULL ? h->tree->name : s->pub.share,
             dialect_text(s->pub.dialect),
             s->pub.signing ? ", signed" : "", s->pub.sealing ? ", sealed" : "");
}

/* Every caller whose answer is now known, answered; let go of after. */
static void sweep(void)
{
    struct held **link = &helds;

    while (*link != NULL) {
        struct held *h = *link;
        struct server *s = h->server;
        struct disk_reply rp;
        bool done = false;

        memset(&rp, 0, sizeof(rp));

        if (h->kind == HELD_NAMES) {
            struct where w;

            if (where_is(h->req.path, &w) != DISK_OK || w.s != s) {
                away(s, &rp);
                done = true;
            } else if (!h->folder->asking) {
                names_settled(s, &h->req, &w, h->folder, &rp);
                done = true;
            } else if (silent(s, h->since) >= SILENT_FOR(h->folder)) {
                names_unanswered(s, &h->req, &w, h->folder, &rp);
                done = true;
            }
        } else if (h->kind == HELD_READ) {
            bool over = h->failed != 0 || (h->started && h->done >= h->want);

            if (over && h->in_flight == 0) {
                read_answer(h, &rp);
                done = true;
            }
        } else if (h->kind == HELD_SUPER) {
            if (h->super_done || h->failed != 0) {
                super_answer(h, &rp);
                done = true;
            }
        }

        if (!done) {
            link = &h->next;
            continue;
        }

        reply_with(h->sender, &rp);
        region_let_go(h);

        if (h->handle != NULL) {
            h->handle->busy--;
            h->handle->used = kosmos_ticks();
        }

        *link = h->next;
        memset(h, 0, sizeof(*h));
        free(h);
    }
}

/* A server let go of: what was asked of it and cannot now be answered by
 * it ends - a read in words, a question about names from memory. */
static void fail_held(struct server *s)
{
    struct held *h;

    for (h = helds; h != NULL; h = h->next) {
        bool finished = h->kind == HELD_READ && h->started && h->in_flight == 0
                        && h->done >= h->want;

        if (h->server == s && h->kind != HELD_NAMES && h->failed == 0
            && !finished && !h->super_done) {
            h->failed = -ENETRESET;
            h->nt = SMB2_STATUS_SHUTDOWN;
        }
    }
}

/*------------------------------------------------------------------------
 * Reading a file: a handle per file, READs straight into the caller's
 * region (`docs/sharing.md`, *What crosses in a region*).
 *----------------------------------------------------------------------*/

static void read_more(struct held *h);
static bool piece_send(struct held *h, uint64_t offset, uint32_t count);

static void piece_read(struct smb2_context *smb2, int status, void *data,
                       void *private_data)
{
    struct piece *p = private_data;
    struct held *h = p->held;
    struct server *s = h->server;

    (void)data;
    h->in_flight--;

    if (status < 0 || (uint32_t)status > p->count) {
        if (h->failed == 0) {
            h->failed = status < 0 ? status : -EIO;
            h->nt = smb2_get_nterror(smb2);
        }
    } else if (status == 0) {
        /* The file ended before the size its open gave: it shrank. */
        if (h->failed == 0) {
            h->failed = -EIO;
            h->nt = 0;
        }
    } else {
        h->done += (uint64_t)status;
        s->cost.read_bytes += (uint64_t)status;

        if ((uint32_t)status < p->count && !s->closing) {
            /* A short answer - fewer credits than asked for: the rest of
             * that piece asked for again, where it belongs. */
            (void)piece_send(h, p->offset + (uint64_t)status,
                             p->count - (uint32_t)status);
        }
    }

    free(p);

    if (!s->closing) {
        read_more(h);
    }
}

/* One READ of the file from `offset`, its bytes straight to where they
 * belong in the caller's region or page. */
static bool piece_send(struct held *h, uint64_t offset, uint32_t count)
{
    struct server *s = h->server;
    uint64_t at = offset - h->req.offset;
    uint8_t *to = (h->req.flags & DISK_REGION) ? h->at + at : h->page + at;
    struct piece *p = calloc(1, sizeof(*p));

    if (p == NULL) {
        h->failed = -ENOMEM;
        return false;
    }

    p->held = h;
    p->offset = offset;
    p->count = count;

    tree_select(s, h->handle->tree);

    if (smb2_pread_async(s->smb2, h->handle->fh, to, count, offset,
                         piece_read, p) < 0) {
        tree_back(s);
        free(p);
        h->failed = -EIO;
        h->nt = 0;
        return false;
    }

    tree_back(s);
    note_asked(s);
    h->in_flight++;
    s->cost.reads++;
    return true;
}

/* As many READs in flight as are allowed, from where the last one ended. */
static void read_more(struct held *h)
{
    struct server *s = h->server;
    uint32_t most;

    if (s->smb2 == NULL || s->closing) {
        return;
    }

    most = smb2_get_max_read_size(s->smb2);

    if (most == 0 || most > READ_PIECE) {
        most = READ_PIECE;
    }

    while (h->failed == 0 && h->in_flight < READS_IN_FLIGHT && h->issued < h->want) {
        uint64_t left = h->want - h->issued;
        uint32_t count = left < most ? (uint32_t)left : most;

        if (!piece_send(h, h->req.offset + h->issued, count)) {
            break;
        }

        h->issued += count;
    }
}

/* The file is open: how much there is to read, and the reading begun. */
static void read_begin(struct held *h)
{
    uint64_t size = h->handle->size;
    uint64_t want = h->req.offset < size ? size - h->req.offset : 0;

    if (h->req.flags & DISK_REGION) {
        if (h->req.bytes < want) {
            want = h->req.bytes;
        }

        if (want > h->room) {
            h->failed = -ERANGE;    /* a region too small: DISK_ERR_REGION */
            h->started = true;
            return;
        }
    } else if (want > DISK_DATA_MAX) {
        want = DISK_DATA_MAX;
    }

    h->want = want;
    h->started = true;
    read_more(h);
}

static void handle_opened(struct smb2_context *smb2, int status, void *data,
                          void *private_data)
{
    struct handle *f = private_data;
    struct held *h;

    f->opening = false;

    /* An open's chain ends here: a link followed was its only other step. */
    if (!f->server->closing) {
        chain_give(f->server);
    }

    if (status == 0 && data != NULL) {
        uint64_t end = 0;

        f->fh = data;
        (void)smb2_lseek(smb2, f->fh, 0, SEEK_END, &end);
        f->size = end;
    } else {
        f->failed = status != 0 ? status : -EIO;
        f->nt = smb2_get_nterror(smb2);
    }

    for (h = helds; h != NULL; h = h->next) {
        if (h->handle == f && !h->started && h->failed == 0) {
            if (f->failed != 0) {
                h->failed = f->failed;
                h->nt = f->nt;
            } else if (!f->server->closing) {
                read_begin(h);
            }
        }
    }
}

static void handle_closed(struct smb2_context *smb2, int status, void *data,
                          void *private_data)
{
    (void)smb2; (void)status; (void)data; (void)private_data;
}

/* The open sent - now, or, when another share's chain runs, by the loop
 * once it ends (`chains_waiting`). False when libsmb2 would not take it. */
static bool handle_open(struct handle *f)
{
    struct server *s = f->server;

    if (!chain_take(s, f->tree)) {
        f->deferred = true;
        return true;
    }

    f->deferred = false;

    if (smb2_open_async(s->smb2, f->path, O_RDONLY, handle_opened, f) < 0) {
        chain_give(s);
        tree_back(s);
        return false;
    }

    note_asked(s);
    return true;
}

/* The handle a file is read through: kept, or opened now. */
static struct handle *handle_get(struct server *s, struct tree *t,
                                 const char *path)
{
    struct handle *f;

    for (f = s->handles; f != NULL; f = f->next) {
        if (f->failed == 0 && f->tree == t && same_name(f->path, path)) {
            return f;
        }
    }

    f = calloc(1, sizeof(*f));

    if (f == NULL || (f->path = strdup(path)) == NULL) {
        free(f);
        return NULL;
    }

    f->server = s;
    f->tree = t;
    f->opening = true;
    f->used = kosmos_ticks();

    if (!handle_open(f)) {
        free(f->path);
        free(f);
        return NULL;
    }

    note_asked(s);

    f->next = s->handles;
    s->handles = f;
    return f;
}

static void handle_free(struct handle *f)
{
    free(f->path);
    memset(f, 0, sizeof(*f));
    free(f);
}

/*
 * Every open handle closed as the context goes: a CLOSE queued for each, so
 * `smb2_destroy_context` - which answers every queued request with
 * SHUTDOWN - hands each back and libsmb2 frees what it allocated for it.
 */
static void close_handles(struct server *s)
{
    struct handle *f;

    for (f = s->handles; f != NULL; f = f->next) {
        if (f->fh != NULL && s->smb2 != NULL) {
            tree_select(s, f->tree);
            (void)smb2_close_async(s->smb2, f->fh, handle_closed, NULL);
        }

        f->fh = NULL;
        f->deferred = false;

        if (f->failed == 0) {
            f->failed = -ENETRESET;
            f->nt = SMB2_STATUS_SHUTDOWN;
        }
    }
}

/* Handles nobody has read through for a while, closed; failed ones let go. */
static bool handles_idle(struct server *s, uint64_t now)
{
    struct handle **link = &s->handles;
    bool queued = false;

    while (*link != NULL) {
        struct handle *f = *link;
        bool idle = !f->opening && f->busy == 0
                    && (f->failed != 0
                        || now - f->used >= HANDLE_IDLE_SECONDS * counter_hz);

        if (!idle) {
            link = &f->next;
            continue;
        }

        if (f->fh != NULL && s->smb2 != NULL && !s->closing) {
            tree_select(s, f->tree);
            (void)smb2_close_async(s->smb2, f->fh, handle_closed, NULL);
            tree_back(s);
            queued = true;
        }

        *link = f->next;
        handle_free(f);
    }

    return queued;
}

/* All a server's memory of its share, when the server itself is let go. */
static void forget_share(struct server *s)
{
    struct held **link = &helds;

    sweep();

    /* Anything still held of it is answered, whatever it was waiting on. */
    while (*link != NULL) {
        struct held *h = *link;

        if (h->server != s) {
            link = &h->next;
            continue;
        }

        {
            struct disk_reply rp;

            memset(&rp, 0, sizeof(rp));
            away(s, &rp);
            reply_with(h->sender, &rp);
        }

        region_let_go(h);
        *link = h->next;
        free(h);
    }

    while (s->folders != NULL) {
        struct folder *f = s->folders;

        s->folders = f->next;
        entries_free(f->entries, f->count);
        free(f->path);
        free(f);
    }

    while (s->handles != NULL) {
        struct handle *f = s->handles;

        s->handles = f->next;
        handle_free(f);
    }

    while (s->trees != NULL) {
        struct tree *t = s->trees;

        s->trees = t->next;
        free(t);
    }

    free(s->offered);
    s->offered = NULL;
    s->offered_count = 0;
}

/*------------------------------------------------------------------------
 * Several shares on one session, and what a server offers (N6).
 *----------------------------------------------------------------------*/

/* A TREE_CONNECT answered: the share a folder, or why not, in words. */
static void tree_connected(struct smb2_context *smb2, int status, void *data,
                           void *private_data)
{
    struct tree *t = private_data;
    struct server *s = t->server;

    (void)data;
    t->asking = false;

    if (s->closing) {
        return;
    }

    if (status == SMB2_STATUS_SUCCESS) {
        /* libsmb2 made the new tree current as it read the answer; its
         * number is kept, and a running chain's tree put back. */
        t->id = smb2_tree_id(smb2);
        t->connected = true;
        t->was_connected = t->was_connected || !t->ipc;
        tree_back(s);

        if (!t->ipc) {
            struct say_line line;

            say_begin(&line);
            say_text(&line, "smbfs: ");
            say_text(&line, named(s));
            say_text(&line, "'s ");
            say_text(&line, t->name);
            say_text(&line, " connected on the same session");
            say_send(console, &line);
        }

        return;
    }

    tree_back(s);

    t->refused = true;

    if (t->ipc) {
        s->list_wanted = false;
        s->list_refused = true;
        snprintf(s->list_why, sizeof(s->list_why), "%s would not list its "
                 "shares", named(s));
        return;
    }

    if ((uint32_t)status == SMB2_STATUS_BAD_NETWORK_NAME) {
        snprintf(s->pub.why, sizeof(s->pub.why), "%s has no share called %s",
                 named(s), t->name);
    } else if ((uint32_t)status == SMB2_STATUS_ACCESS_DENIED) {
        snprintf(s->pub.why, sizeof(s->pub.why), "%s does not let %s open %s",
                 named(s), s->pub.account, t->name);
    } else {
        snprintf(s->pub.why, sizeof(s->pub.why), "%s refused %s (0x%08x)",
                 named(s), t->name, (unsigned)status);
    }

    {
        struct say_line line;

        say_begin(&line);
        say_text(&line, "smbfs: ");
        say_text(&line, s->pub.why);
        say_send(console, &line);
    }
}

/* One TREE_CONNECT, on the session there is: `\\server\share`, as
 * libsmb2's own connect writes it. */
static bool tree_ask(struct server *s, struct tree *t)
{
    struct smb2_tree_connect_request req;
    struct smb2_utf16 *unc;
    struct smb2_pdu *pdu;
    char text[SHARE_ADDRESS_MAX + SHARE_NAME_MAX + 4];

    snprintf(text, sizeof(text), "\\\\%s\\%s", s->target, t->name);
    unc = smb2_utf8_to_utf16(text);

    if (unc == NULL) {
        return false;
    }

    memset(&req, 0, sizeof(req));
    req.path_length = (uint16_t)(2u * unc->len);
    req.path = unc->val;

    /* Its answer makes the new tree current; `tree_connected` puts the
     * chain's back. The path is copied into the request as it is made. */
    pdu = smb2_cmd_tree_connect_async(s->smb2, &req, tree_connected, t);
    free(unc);

    if (pdu == NULL) {
        return false;
    }

    smb2_queue_pdu(s->smb2, pdu);
    t->asking = true;
    note_asked(s);
    return true;
}

/* What NetShareEnum answered: the server's folders, kept for SHARES. */
static void shares_listed(struct smb2_context *smb2, int status, void *data,
                          void *private_data)
{
    struct server *s = private_data;
    struct smb2_share_enum_reply *rep = data;
    struct share_offered *out = NULL;
    unsigned n = 0, i;

    s->listing = false;

    if (s->closing) {
        if (rep != NULL) {
            smb2_free_data(smb2, rep);
        }

        return;
    }

    chain_give(s);
    tree_back(s);

    if (status != 0 || rep == NULL || rep->level != SMB2_SHARE_INFO_1) {
        const char *said = smb2_get_error(smb2);

        s->list_refused = true;
        snprintf(s->list_why, sizeof(s->list_why), "%s would not list its "
                 "shares: %s", named(s), said != NULL && said[0] != '\0'
                 ? said : "it did not say why");

        if (rep != NULL) {
            smb2_free_data(smb2, rep);
        }

        return;
    }

    out = calloc(rep->entries_read != 0 ? rep->entries_read : 1, sizeof(*out));

    for (i = 0; out != NULL && i < rep->entries_read; i++) {
        const struct smb2_share_info_1 *one = &rep->share_info.info_1[i];
        const char *name = one->netname != NULL ? one->netname : "";
        size_t len = strlen(name);

        /*
         * **Folders only**: a printer, a pipe and a share hidden by its name
         * ending in `$` (`IPC$`, `ADMIN$`, `C$`) are not places to open.
         */
        if ((one->type & 3u) != SMB2_SHARE_TYPE_DISKTREE
            || (one->type & SMB2_SHARE_TYPE_HIDDEN) != 0
            || len == 0 || name[len - 1] == '$' || len >= SHARE_NAME_MAX) {
            continue;
        }

        copy(out[n].name, sizeof(out[n].name), name);
        out[n].kind = one->type & 3u;
        n++;
    }

    smb2_free_data(smb2, rep);

    free(s->offered);
    s->offered = out;
    s->offered_count = out != NULL ? n : 0;
    s->listed_once = true;
    s->list_refused = false;

    {
        struct say_line line;
        char count[16];

        snprintf(count, sizeof(count), "%u", s->offered_count);
        say_begin(&line);
        say_text(&line, "smbfs: ");
        say_text(&line, named(s));
        say_text(&line, " offers ");
        say_text(&line, count);
        say_text(&line, s->offered_count == 1 ? " share" : " shares");
        say_send(console, &line);
    }
}

/*
 * **What the loop begins, on a connected server**: each share asked for and
 * not yet connected, connected on the session; the list of shares, once
 * `IPC$` is; and whatever waited for a chain on another tree, once none
 * runs - every listing and open waiting on the same tree as the first one
 * found, together. Never from inside a callback: a callback only decides.
 */
static void trees_wanted(struct server *s)
{
    struct tree *t, *ipc = NULL;
    struct folder *f;
    struct handle *h;
    struct tree *next = NULL;
    bool asked = false;

    if (s->smb2 == NULL || s->closing || s->pub.state != SHARE_STATE_CONNECTED) {
        return;
    }

    for (t = s->trees; t != NULL; t = t->next) {
        if (!t->connected && !t->asking && !t->refused && tree_ask(s, t)) {
            asked = true;
        }

        if (t->ipc) {
            ipc = t;
        }
    }

    /* The list wanted: `IPC$` connected first, if it is not. */
    if (s->list_wanted && !s->listing) {
        if (ipc == NULL) {
            ipc = tree_add(s, "IPC$", true);

            if (ipc != NULL && tree_ask(s, ipc)) {
                asked = true;
            }
        } else if (ipc->connected && chain_take(s, ipc)) {
            if (smb2_share_enum_async(s->smb2, SMB2_SHARE_INFO_1, shares_listed,
                                      s) == 0) {
                s->listing = true;
                s->list_wanted = false;
                note_asked(s);
                asked = true;
            } else {
                chain_give(s);
                s->list_wanted = false;
                s->list_refused = true;
                snprintf(s->list_why, sizeof(s->list_why), "no memory to ask "
                         "%s for its shares", named(s));
            }

            tree_back(s);
        }
    }

    /* Listings and opens that waited for another tree's chain. */
    if (s->chains == 0) {
        for (f = s->folders; f != NULL && next == NULL; f = f->next) {
            if (f->deferred) next = f->tree;
        }

        for (h = s->handles; h != NULL && next == NULL; h = h->next) {
            if (h->deferred) next = h->tree;
        }
    }

    if (next != NULL) {
        for (f = s->folders; f != NULL; f = f->next) {
            if (f->deferred && f->tree == next && folder_ask(f)) {
                asked = true;
            }
        }

        for (h = s->handles; h != NULL; h = h->next) {
            if (h->deferred && h->tree == next) {
                if (!handle_open(h)) {
                    h->opening = false;
                    h->failed = -EIO;
                    h->nt = 0;
                }

                asked = true;
            }
        }
    }

    if (asked) {
        settle(s, 0);
    }
}

/* SHARES: what the server offers, each marked with what was asked of it. */
static void shares_page(struct disk_request *req, struct disk_reply *rep)
{
    struct share_ask *ask = (struct share_ask *)(void *)req->u.data;
    struct share_offered *out = (struct share_offered *)(void *)rep->u.data;
    char address[SHARE_ADDRESS_MAX];
    struct server *s;
    struct tree *t;
    unsigned i, at = 0, n = 0;

    field(address, sizeof(address), ask->address, sizeof(ask->address));
    s = server_find(address);

    if (s == NULL || s->probe || !s->was_connected) {
        refuse(rep, SHARE_ERR_NOT_IN, "not signed into it: connect to it first, "
               "with the account and the password");
        return;
    }

    if (s->list_refused && !s->listed_once) {
        bool any = false;

        for (t = s->trees; t != NULL; t = t->next) {
            any = any || !t->ipc;
        }

        if (!any) {
            refuse(rep, SHARE_ERR_NO_LIST, s->list_why);
            return;
        }
    }

    /* Asked for the first time: the loop begins it (`trees_wanted`). */
    if (!s->listed_once && !s->listing && !s->list_refused) {
        s->list_wanted = true;
    }

    rep->bytes = s->listed_once ? 1u : 0u;

#define OFFER(name_, state_, kind_) do {                                  \
        if (at++ >= req->offset) {                                      \
            if (n == SHARE_OFFERED_PER_PAGE) { rep->more = 1; goto full; } \
            memset(&out[n], 0, sizeof(out[n]));                         \
            copy(out[n].name, sizeof(out[n].name), (name_));            \
            out[n].state = (state_);                                    \
            out[n].kind = (kind_);                                      \
            n++;                                                        \
        }                                                               \
    } while (0)

    /* What it offers, in its order, each as asked for or not. */
    for (i = 0; i < s->offered_count; i++) {
        uint32_t state = SHARE_TREE_OFFERED;

        t = tree_find(s, s->offered[i].name);

        if (t != NULL) {
            state = t->connected ? SHARE_TREE_CONNECTED
                  : t->asking ? SHARE_TREE_ASKING
                  : t->refused ? SHARE_TREE_REFUSED
                  : t->was_connected ? SHARE_TREE_CONNECTED : SHARE_TREE_ASKING;
        }

        OFFER(s->offered[i].name, state, s->offered[i].kind);
    }

    /* And what was asked for that it does not list: a share it hides. */
    for (t = s->trees; t != NULL; t = t->next) {
        bool offered = false;

        if (t->ipc) {
            continue;
        }

        for (i = 0; i < s->offered_count && !offered; i++) {
            offered = same_name(s->offered[i].name, t->name);
        }

        if (!offered) {
            OFFER(t->name, t->connected ? SHARE_TREE_CONNECTED
                  : t->asking ? SHARE_TREE_ASKING
                  : t->refused ? SHARE_TREE_REFUSED : SHARE_TREE_ASKING,
                  SMB2_SHARE_TYPE_DISKTREE);
        }
    }

#undef OFFER
full:
    rep->count = n;
    rep->length = (uint32_t)(n * sizeof(struct share_offered));
    rep->offset = req->offset + n;
}

/*------------------------------------------------------------------------
 * The operations, as they arrive.
 *----------------------------------------------------------------------*/

/* `/Network` itself and a server: listed and described from smbfs's own
 * records, never from the network. */
static void above_shares(const struct disk_request *rq, const struct where *w,
                         struct disk_reply *rp)
{
    if (rq->op == DISK_OP_GETATTR) {
        rp->node.kind = DISK_KIND_DIR;
        return;
    }

    if (rq->op != DISK_OP_LIST) {
        if (rq->op == DISK_OP_READ) {
            rp->error = DISK_ERR_THE_DIRECTORY;
        } else {
            said(rp, DISK_ERR_BAD_OP, "%s holds servers and their shares, "
                 "and no files of its own", "/Network", "", "");
        }

        return;
    }

    if (w->s != NULL) {
        /* A server's shares: each connected on its session (N6), in the
         * order asked for - an away one's too, as last heard. */
        uint32_t used = 0;
        unsigned at = 0;
        struct tree *t;

        for (t = w->s->trees; t != NULL; t = t->next) {
            size_t n;

            if (!tree_listed(t) || at++ < rq->offset) {
                continue;
            }

            n = strlen(t->name);

            if (used + n + 1u > DISK_DATA_MAX) {
                rp->more = 1;
                rp->offset = at - 1u;
                break;
            }

            memcpy(rp->u.data + used, t->name, n + 1);
            used += (uint32_t)n + 1u;
            rp->count++;
        }

        rp->length = used;
        return;
    }

    {
        uint32_t used = 0;
        unsigned i, at = 0;

        for (i = 0; i < server_count; i++) {
            char label[SHARE_ADDRESS_MAX + SHARE_NAME_MAX + 4];
            size_t n;

            if (!listed(servers[i]) || at++ < rq->offset) {
                continue;
            }

            server_label(servers[i], label, sizeof(label));
            n = strlen(label);

            if (used + n + 1u > DISK_DATA_MAX) {
                rp->more = 1;
                rp->offset = at - 1u;
                break;
            }

            memcpy(rp->u.data + used, label, n + 1);
            used += (uint32_t)n + 1u;
            rp->count++;
        }

        rp->length = used;
    }
}

/* LIST and GETATTR inside a share: from memory, or held for the server. */
static bool names_op(const struct disk_request *rq, const struct where *w,
                     uint64_t sender, struct disk_reply *rp)
{
    struct server *s = w->s;
    char path[DISK_PATH_MAX];
    struct folder *f;
    uint64_t now = kosmos_ticks();
    struct held *h;
    bool here;

    /* The share itself is a folder, and that needs no asking. */
    if (rq->op == DISK_OP_GETATTR && w->inside[0] == '\0') {
        rp->node.kind = DISK_KIND_DIR;
        return false;
    }

    names_folder(rq, w, path, sizeof(path));
    f = folder_get(s, w->tree, path);

    if (f == NULL) {
        said(rp, DISK_ERR_DENIED, "no memory for a folder of %s's", named(s),
             "", "");
        return false;
    }

    /*
     * Heard a moment ago - or the next page of a listing being read. From a
     * server that is away (N5), memory is all there is, and every answer
     * from it says so, however recent; the server is not asked.
     */
    here = s->smb2 != NULL && !s->closing && s->pub.state == SHARE_STATE_CONNECTED
           && w->tree->connected;

    if (f->known && ((here && now - f->heard < ticks_of_ms(FRESH_MS))
                     || (rq->op == DISK_OP_LIST && rq->offset > 0))) {
        s->cost.cache_hits++;
        names_from(s, rq, w, f, !here, rp);
        return false;
    }

    if (!folder_ask(f)) {
        names_unanswered(s, rq, w, f, rp);
        return false;
    }

    settle(s, 0);

    /* Asked a while ago and nothing heard since: the bound has passed for
     * whoever asks now as well. */
    if (!f->asking) {
        names_settled(s, rq, w, f, rp);
        return false;
    }

    if (silent(s, f->asked) >= SILENT_FOR(f)) {
        names_unanswered(s, rq, w, f, rp);
        return false;
    }

    h = held_add(s, HELD_NAMES, rq, sender);

    if (h == NULL) {
        names_unanswered(s, rq, w, f, rp);
        return false;
    }

    h->folder = f;
    h->since = f->asked;
    return true;
}

/* A region the caller handed over, mapped for as long as the read takes. */
static bool region_take(struct held *h, long cap)
{
    long pages, at;

    if (cap < 0 || (pages = kosmos_mem_size(cap)) <= 0
        || (at = kosmos_mem_map(cap)) < 0) {
        return false;
    }

    h->cap = cap;
    h->at = (uint8_t *)(uintptr_t)at;
    h->room = (uint64_t)pages * 4096u;
    return true;
}

static bool read_op(const struct disk_request *rq, const struct where *w,
                    uint64_t sender, long *cap, struct disk_reply *rp)
{
    struct server *s = w->s;
    struct held *h;

    if (w->inside[0] == '\0') {
        rp->error = DISK_ERR_THE_DIRECTORY;
        return false;
    }

    if (s->smb2 == NULL || s->closing || s->pub.state != SHARE_STATE_CONNECTED
        || !w->tree->connected) {
        away(s, rp);
        return false;
    }

    h = held_add(s, HELD_READ, rq, sender);

    if (h == NULL) {
        said(rp, DISK_ERR_DENIED, "no memory to read %s's file", named(s), "", "");
        return false;
    }

    if ((rq->flags & DISK_REGION) && !region_take(h, *cap)) {
        helds = h->next;
        free(h);
        rp->error = DISK_ERR_REGION;
        return false;
    }

    if (rq->flags & DISK_REGION) {
        *cap = -1;                  /* the held caller's now, given back with it */
    }

    h->handle = handle_get(s, w->tree, w->inside);

    if (h->handle == NULL) {
        h->failed = -ENOMEM;
        return true;                /* answered by the sweep, in words */
    }

    h->handle->busy++;
    h->handle->used = kosmos_ticks();

    if (!h->handle->opening) {
        read_begin(h);
    }

    settle(s, 0);
    return true;
}

static void supered(struct smb2_context *smb2, int status, void *data,
                    void *private_data)
{
    struct held *h = private_data;

    (void)data;

    if (status == 0) {
        h->super_done = true;
    } else {
        h->failed = status;
        h->nt = smb2_get_nterror(smb2);
    }
}

/*
 * A share's files, `diskproto.h`. True when the caller is held, its reply
 * to go from the sweep; false when `rp` is the answer now.
 */
static bool files_op(const struct disk_request *rq, uint64_t sender, long *cap,
                     struct disk_reply *rp)
{
    struct where w;
    uint32_t e;

    if (memchr(rq->path, '\0', DISK_PATH_MAX) == NULL) {
        rp->error = DISK_ERR_BAD_OP;
        return false;
    }

    e = where_is(rq->path, &w);

    if (e != DISK_OK) {
        said(rp, e, e == DISK_ERR_KFS + 7 ? "no such file"
                    : "a path may not contain . or ..", "", "", "");
        return false;
    }

    switch (rq->op) {
    case DISK_OP_WRITE:
    case DISK_OP_DELETE:
    case DISK_OP_RENAME:
    case DISK_OP_MKDIR:
    case DISK_OP_SETATTR:
    case DISK_OP_FORMAT:
        /*
         * **Read only, said as such** (`docs/sharing.md`): refused with a
         * sentence rather than attempted. Read-write is step N10.
         */
        if (w.s != NULL && w.share) {
            char label[SHARE_ADDRESS_MAX + SHARE_NAME_MAX + 4];

            server_label(w.s, label, sizeof(label));
            said(rp, DISK_ERR_READ_ONLY, "%s's %s is open read only", label,
                 w.tree->name, "");
        } else {
            said(rp, DISK_ERR_READ_ONLY, "%s holds servers and their shares, "
                 "and is read only", "/Network", "", "");
        }
        return false;

    case DISK_OP_QUERY:
        said(rp, DISK_ERR_BAD_OP, "a share has no attributes to search by",
             "", "", "");
        return false;

    default:
        break;
    }

    if (w.s == NULL || !w.share) {
        if (rq->op == DISK_OP_SUPER || rq->op == DISK_OP_DEVICE) {
            rp->error = DISK_ERR_RESERVED;
            return false;
        }

        above_shares(rq, &w, rp);
        return false;
    }

    switch (rq->op) {
    case DISK_OP_LIST:
    case DISK_OP_GETATTR:
        return names_op(rq, &w, sender, rp);

    case DISK_OP_READ:
        return read_op(rq, &w, sender, cap, rp);

    case DISK_OP_DEVICE:
        rp->u.device = w.s->cost;
        rp->u.device.requests = w.s->sent_before
                                + (w.s->smb2 != NULL ? w.s->smb2->message_id : 0);
        return false;

    case DISK_OP_SUPER: {
        struct held *h;

        if (w.s->smb2 == NULL || w.s->closing
            || w.s->pub.state != SHARE_STATE_CONNECTED || !w.tree->connected) {
            away(w.s, rp);
            return false;
        }

        h = held_add(w.s, HELD_SUPER, rq, sender);

        if (h == NULL) {
            said(rp, DISK_ERR_DENIED, "no memory to ask %s", named(w.s), "", "");
            return false;
        }

        h->tree = w.tree;

        /* One compound - CREATE, QUERY_INFO, CLOSE - made at once: the tree
         * selected for it and put back, no chain. */
        tree_select(w.s, w.tree);

        if (smb2_statvfs_async(w.s->smb2, "", &h->vfs, supered, h) < 0) {
            h->failed = -EIO;
        } else {
            note_asked(w.s);
        }

        tree_back(w.s);

        settle(w.s, 0);
        return true;
    }

    default:
        rp->error = DISK_ERR_BAD_OP;
        return false;
    }
}

static void answer(struct message *msg, uint64_t sender)
{
    struct message out;
    struct disk_request req;
    struct disk_reply *rep = (struct disk_reply *)(void *)out.data;
    struct server *leaving = NULL;
    long cap = msg->cap_plus_one != 0 ? (long)msg->cap_plus_one - 1 : -1;
    bool held = false;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rep);

    /* Exactly the shape, or nothing is read of it. */
    if (msg->length != sizeof(req)) {
        if (cap >= 0) {
            (void)kosmos_cap_drop(cap);
        }

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

    case SHARE_OP_RETRY:
        retry_now(&req, rep);
        break;

    case SHARE_OP_SHARES:
        shares_page(&req, rep);
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

    case DISK_OP_LIST:
    case DISK_OP_READ:
    case DISK_OP_WRITE:
    case DISK_OP_DELETE:
    case DISK_OP_RENAME:
    case DISK_OP_MKDIR:
    case DISK_OP_GETATTR:
    case DISK_OP_SETATTR:
    case DISK_OP_QUERY:
    case DISK_OP_SUPER:
    case DISK_OP_DEVICE:
    case DISK_OP_FORMAT:
        held = files_op(&req, sender, &cap, rep);
        break;

    default:
        rep->error = DISK_ERR_BAD_OP;
        break;
    }

    /* A region handed over and not kept by a held read is given back on
     * every path, as `diskfs` gives it back. */
    if (cap >= 0) {
        (void)kosmos_cap_drop(cap);
    }

    memset(&req, 0, sizeof(req));

    if (!held) {
        (void)kosmos_reply(sender, &out);
    }

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

/* Whether anything of a server's is waiting on the network. */
static bool busy(const struct server *s)
{
    const struct folder *f;
    const struct held *h;

    const struct tree *t;

    if (s->echoing || s->listing) {
        return true;
    }

    for (t = s->trees; t != NULL; t = t->next) {
        if (t->asking) {
            return true;
        }
    }

    for (f = s->folders; f != NULL; f = f->next) {
        if (f->asking) {
            return true;
        }
    }

    for (h = helds; h != NULL; h = h->next) {
        if (h->server == s) {
            return true;
        }
    }

    return false;
}

static void echoed(struct smb2_context *smb2, int status, void *data,
                   void *private_data)
{
    struct server *s = private_data;

    (void)smb2; (void)status; (void)data;
    s->echoing = false;             /* what it answered, `settle` heard */
}

/*
 * **Gone away, and back** (N5), on smbfs's clock: a connected server asked
 * something and silent for `AWAY_SECONDS` is away; one nobody has asked
 * anything for a minute is sent an ECHO, so that is noticed too; a server
 * away has its next try planned, each wait twice the last up to a minute,
 * and tried when it comes; and a try that has not answered within the
 * bound a connect has is a try that failed.
 */
static void gone_and_back(struct server *s, uint64_t now)
{
    if (s->pub.state == SHARE_STATE_CONNECTED && s->smb2 != NULL && !s->closing) {
        uint64_t from;

        if (!busy(s)) {
            s->waiting = false;
        }

        from = s->heard > s->asked_at ? s->heard : s->asked_at;

        if (s->waiting && now - from >= AWAY_SECONDS * counter_hz) {
            char reason[64];

            snprintf(reason, sizeof(reason), "nothing heard from it for %u seconds",
                     AWAY_SECONDS);
            away_because(s, reason, true);
            tell(s);
            server_close(s);
            return;
        }

        if (!busy(s) && now - s->heard >= ECHO_SECONDS * counter_hz
            && smb2_echo_async(s->smb2, echoed, s) == 0) {
            s->echoing = true;
            note_asked(s);
            settle(s, 0);
        }

        return;
    }

    if (s->retrying) {
        if (now >= s->try_deadline) {
            struct smb_link link;
            bool took = s->smb2 != NULL
                        && smb_kit_link(smb2_get_fd(s->smb2), &link) && link.taken;
            char reason[96];

            snprintf(reason, sizeof(reason), took
                     ? "it took the connection and did not answer within %u seconds"
                     : "it did not answer within %u seconds", ANSWER_SECONDS);
            s->settled = true;
            away_because(s, reason, false);
            tell(s);
            server_close(s);
        }

        return;
    }

    if (!retryable(s)) {
        return;
    }

    if (s->next_try == 0) {
        s->backoff = s->backoff == 0 ? RETRY_FIRST_SECONDS
                     : (s->backoff * 2u > RETRY_MOST_SECONDS ? RETRY_MOST_SECONDS
                                                             : s->backoff * 2u);
        s->next_try = now + (uint64_t)s->backoff * counter_hz;
    } else if (now >= s->next_try && s->thread < 0 && s->smb2 == NULL) {
        retry(s);
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

    for (i = 0; i < server_count; i++) {
        if (!servers[i]->forgotten && !servers[i]->probe) {
            gone_and_back(servers[i], now);
        }
    }

    /*
     * A connected server with something in flight is serviced once a
     * second, which is how libsmb2's own timeout runs; a handle nobody has
     * read through for a while is closed.
     */
    for (i = 0; i < server_count; i++) {
        struct server *s = servers[i];
        bool queued;

        if (s->smb2 == NULL || s->closing || s->pub.state != SHARE_STATE_CONNECTED) {
            continue;
        }

        queued = handles_idle(s, now);
        trees_wanted(s);

        if (busy(s) && now - s->serviced >= counter_hz) {
            s->serviced = now;
            settle(s, smb2_service(s->smb2, 0));
        } else if (queued) {
            settle(s, 0);
        }
    }
}

/* Scheduler ticks to the nearest deadline, or 0 for none. */
static unsigned long next_wait(void)
{
    uint64_t now = kosmos_ticks(), soonest = 0;
    unsigned i;

    const struct held *h;

    for (i = 0; i < server_count; i++) {
        const struct server *s = servers[i];
        const struct handle *f;
        uint64_t d = s->deadline;

        if (s->pub.state == SHARE_STATE_ASKING && d != 0
            && (soonest == 0 || d < soonest)) {
            soonest = d;
        }

        /* N5: a try that may have failed, and the next one planned - once
         * the last try's waiter has gone, which wakes this loop itself. */
        if (s->retrying) {
            d = s->try_deadline;

            if (soonest == 0 || d < soonest) soonest = d;
        } else if (retryable(s) && s->next_try == 0) {
            soonest = now;                  /* its next try is still to plan */
        } else if (retryable(s) && s->thread < 0) {
            d = s->next_try;

            if (soonest == 0 || d < soonest) soonest = d;
        }

        if (s->smb2 == NULL || s->closing) {
            continue;
        }

        if (s->pub.state == SHARE_STATE_CONNECTED) {
            uint64_t from = s->heard > s->asked_at ? s->heard : s->asked_at;

            d = s->waiting ? from + AWAY_SECONDS * counter_hz
                           : s->heard + ECHO_SECONDS * counter_hz;

            if (soonest == 0 || d < soonest) soonest = d;
        }

        if (busy(s)) {
            d = s->serviced + counter_hz;

            if (soonest == 0 || d < soonest) soonest = d;
        }

        for (f = s->handles; f != NULL; f = f->next) {
            if (!f->opening && f->busy == 0) {
                d = f->used + HANDLE_IDLE_SECONDS * counter_hz;

                if (soonest == 0 || d < soonest) soonest = d;
            }
        }
    }

    /* A question about names held: the moment its server's silence passes
     * the bound. */
    for (h = helds; h != NULL; h = h->next) {
        if (h->kind == HELD_NAMES) {
            uint64_t from = h->server->heard > h->since ? h->server->heard : h->since;
            uint64_t d = from + SILENT_FOR(h->folder);

            if (soonest == 0 || d < soonest) soonest = d;
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
        sweep();
    }
}
