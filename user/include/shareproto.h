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
/* 67 is `SHARE_OP_SHARES`, a server's shares through `srvsvc` - step N3. */
#define SHARE_OP_DISCONNECT  68u    /* let a server go, and forget it */

/*
 * **smbfs's own threads, and nobody else.** A waiter - one a server
 * connection, blocked in the stack's `NET_OP_POLL` - says what happened as
 * a call to smbfs's endpoint, which is the one place its main thread waits
 * (`smbfs.c`). It carries a word smbfs chose from the kernel's entropy when
 * it started, in `offset`; a request without it is refused like any
 * operation that does not exist, so the number is no door for a caller.
 */
#define SHARE_OP_WAITER      96u

/* What PROBE, CONNECT and DISCONNECT carry, in `u.data`. `password` crosses
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

/* What a server is doing. */
#define SHARE_STATE_ASKING     1u   /* looked up, connected to, signed into: under way */
#define SHARE_STATE_ANSWERED   2u   /* a PROBE: it answered NEGOTIATE */
#define SHARE_STATE_CONNECTED  3u   /* signed in, the share connected */
#define SHARE_STATE_REFUSED    4u   /* it answered, and said no: `why` */
#define SHARE_STATE_AWAY       5u   /* not answering: `why` */

/* STATUS's answer: `count` of these in `u.data`, a page from `offset`, and
 * `more` when another page follows. */
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
    uint32_t probe;                         /* asked only whether it answers */
    uint32_t reserved;
    uint64_t in_state_ms;                   /* how long it has been so */
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

_Static_assert(sizeof(struct share_ask) <= DISK_DATA_MAX,
               "what a share is asked fits where a page of bytes does");
_Static_assert(sizeof(struct share_server) == 336,
               "a server's status has no padding");
_Static_assert(SHARE_PER_PAGE >= 3, "three servers a page");

#endif /* KOSMOS_SHAREPROTO_H */
