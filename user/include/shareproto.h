/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SHAREPROTO_H
#define KOSMOS_SHAREPROTO_H

#include <stdint.h>

#include "diskproto.h"

/*
 * What a share is asked that is not a file (`docs/sharing.md`,
 * *`shareproto.h`*, step N2).
 *
 * smbfs answers `diskproto.h` for a share's files (step N3) and this for
 * the rest: whether a server answers, signing into one, what each is doing,
 * and letting one go. **The same request and reply as a disk's** - `struct
 * disk_request` in, `struct disk_reply` out - so smbfs checks one size, and
 * the operations are numbered above `diskproto.h`'s; what each carries is
 * a struct of its own in `u.data`.
 *
 * **Answered at once, every one.** `PROBE` and `CONNECT` start something
 * that takes a network's time and say only that it has started; whoever
 * asked then asks `STATUS` on its own clock - a window's, or `share`'s at
 * the prompt - to learn how it went. Nothing waits in smbfs for a server
 * (`CLAUDE.md`, *nothing on the desktop waits on a server*).
 */

#define SHARE_OP_PROBE       64u    /* does it answer: NEGOTIATE, and no more */
#define SHARE_OP_CONNECT     65u    /* sign in and connect to a share */
#define SHARE_OP_STATUS      66u    /* what each server is doing, a page from `offset` */
#define SHARE_OP_SHARES      67u    /* a server's shares, offered and connected (N6) */
#define SHARE_OP_DISCONNECT  68u    /* let a server go, and forget it */
#define SHARE_OP_RETRY       69u    /* Try now: a server away, signed into again now */

/*
 * **smbfs's own threads, and nobody else.** A waiter - one a server
 * connection, blocked in the stack's `NET_OP_POLL` - says what happened as
 * a call to smbfs's endpoint, which is the one place its main thread waits
 * (`smbfs.c`). It carries a word smbfs chose from the kernel's entropy when
 * it started, in `offset`; a request without it is refused like any
 * operation that does not exist, so the number is no door for a caller.
 */
#define SHARE_OP_WAITER      96u

/* What PROBE, CONNECT, DISCONNECT, RETRY and SHARES carry, in `u.data`. `password` crosses
 * once, in CONNECT, as a keystroke does; smbfs keeps its NT hash and never
 * the password (step N8 names a keyring entry instead). */
#define SHARE_ADDRESS_MAX    48u    /* "192.168.1.38:445", or a name and a port */
#define SHARE_NAME_MAX       40u    /* a share, or the name a server gives */
#define SHARE_ACCOUNT_MAX    32u
#define SHARE_SECRET_MAX    256u
#define SHARE_WHY_MAX       152u

struct share_ask {
    char address[SHARE_ADDRESS_MAX];
    char share[SHARE_NAME_MAX];
    char account[SHARE_ACCOUNT_MAX];
    char password[SHARE_SECRET_MAX];
};

/*
 * **Gone away, and back** (step N5). A server whose share was connected and
 * that stops answering - asked something and silent for ten seconds, or its
 * connection closed - is `AWAY`, and smbfs keeps it: its folders as last
 * listed, the NT hash, and a try planned, two seconds after, then four, and
 * so on up to a minute. A try that answers signs in again and connects the
 * share - a new session and a new tree - without anybody asked; one refused
 * for the account stops the tries. `RETRY` is Try now. A server never
 * connected, refused, or whose answer arrived changed (`DISK_ERR_ALTERED`)
 * is not tried again by itself.
 */

/* What a server is doing. */
#define SHARE_STATE_ASKING     1u   /* looked up, connected to, signed into: under way */
#define SHARE_STATE_ANSWERED   2u   /* a PROBE: it answered NEGOTIATE */
#define SHARE_STATE_CONNECTED  3u   /* signed in, the share connected */
#define SHARE_STATE_REFUSED    4u   /* it answered, and said no: `why` */
#define SHARE_STATE_AWAY       5u   /* not answering: `why` */

/* STATUS's answer: `count` of these in `u.data`, a page from `offset`, and
 * `more` when another page follows. `size` is how many STATUS requests
 * smbfs has answered since it started (N6): what a window's clock costs,
 * counted where it arrives rather than where it is asked. */
struct share_server {
    char     address[SHARE_ADDRESS_MAX];    /* as it was asked for */
    char     name[SHARE_NAME_MAX];          /* how it calls itself, once known */
    char     share[SHARE_NAME_MAX];
    char     account[SHARE_ACCOUNT_MAX];
    char     why[SHARE_WHY_MAX];            /* refused or away: in words */
    uint32_t state;                         /* SHARE_STATE_* */
    uint16_t dialect;                       /* 0x0202 ... 0x0311, once negotiated */
    uint8_t  signing;                       /* every message signed */
    uint8_t  sealing;                       /* every message encrypted */
    uint8_t  probe;                         /* asked only whether it answers */
    uint8_t  trying;                        /* away, and being signed into again now */
    uint16_t sign_ins;                      /* times signed in: 2 and more, again by itself */
    uint32_t next_try_ms;                   /* away: when it is tried again; 0, no try planned */
    uint64_t in_state_ms;                   /* how long it has been so: away since */
};

#define SHARE_PER_PAGE  (DISK_DATA_MAX / sizeof(struct share_server))

/*
 * Refusals of the request itself, in `error`, each with its sentence in
 * `u.data` (`length` bytes): above `diskproto.h`'s numbers, so a caller
 * reading a disk's errors cannot mistake one.
 */
#define SHARE_ERR_ADDRESS    64u    /* not an address, or not a share's */
#define SHARE_ERR_UNKNOWN    65u    /* DISCONNECT of a server nobody asked for */
#define SHARE_ERR_ALREADY    66u    /* CONNECT to a server already connected */
#define SHARE_ERR_ACCOUNT    67u    /* no account: guests are not let in */
#define SHARE_ERR_NO_MEMORY  68u
#define SHARE_ERR_NOT_KEPT   69u    /* RETRY of a server not kept to sign into again */
#define SHARE_ERR_NOT_IN     70u    /* SHARES of a server not signed into */
#define SHARE_ERR_NO_LIST    71u    /* SHARES: the server would not list them */

/*
 * **A server's shares** (step N6): what Connect to Server offers once a
 * server has been signed into - "choose one once it answers" - and what of
 * them is connected. SHARES carries `struct share_ask` with the address,
 * and is answered at once, as everything here is: `count` of these in
 * `u.data`, a page from `offset`, `more` when another follows.
 *
 * The server's own list comes through `srvsvc`'s NetShareEnum on `IPC$`
 * (libsmb2's `smb2-share-enum.c`), asked the first time SHARES is - or at
 * once, by a CONNECT that names no share, which signs in to `IPC$` alone.
 * **`bytes` says whether it has been heard**: 1 once it has, and the
 * entries are the server's folders - its printers, its pipes and the
 * shares it hides (a name ending in `$`) left out - with the ones asked for
 * marked; 0 while it is being asked, and the entries are only the shares
 * asked for; the caller asks again on its own clock. A server that will not
 * list them is refused with SHARE_ERR_NO_LIST and its words.
 *
 * **Several shares on one connection.** A CONNECT to a server already
 * signed into, as the same account, connects one share more on the same
 * session - a TREE_CONNECT and nothing else, no password asked or used.
 */
#define SHARE_TREE_OFFERED     0u   /* the server offers it; not asked for */
#define SHARE_TREE_ASKING      1u   /* TREE_CONNECT under way */
#define SHARE_TREE_CONNECTED   2u   /* a folder: /Network/<server>/<share> */
#define SHARE_TREE_REFUSED     3u   /* the server said no: STATUS's `why` */

struct share_offered {
    char     name[SHARE_NAME_MAX];
    uint32_t state;                         /* SHARE_TREE_* */
    uint32_t kind;                          /* srvsvc's STYPE_*, low two bits */
};

#define SHARE_OFFERED_PER_PAGE  (DISK_DATA_MAX / sizeof(struct share_offered))

_Static_assert(sizeof(struct share_ask) <= DISK_DATA_MAX,
               "what a share is asked fits where a page of bytes does");
_Static_assert(sizeof(struct share_server) == 336,
               "a server's status has no padding");
_Static_assert(SHARE_PER_PAGE >= 3, "three servers a page");
_Static_assert(sizeof(struct share_offered) == 48, "a share offered has no padding");

#endif /* KOSMOS_SHAREPROTO_H */
