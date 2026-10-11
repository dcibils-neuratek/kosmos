/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SMBDPROTO_H
#define KOSMOS_SMBDPROTO_H

#include <stdint.h>

/*
 * What `smbd` is told, and what it says (`docs/sharing.md` N12c).
 *
 * **Its own door, lent to the image's Servers window and `sharing` program
 * alone**, as the keyring's `manage` door goes to Passwords: whoever holds it
 * decides what this machine offers the network. init starts `smbd` at every
 * boot, idle, holding no port; it serves when this says START, with the
 * shares this has set and the account this has named.
 *
 * **The password crosses once**, in ACCOUNT, and `smbd` keeps only its NT
 * hash - in the keyring, under KEY_KIND_SHARING through a door of its own -
 * which is all an NTLMv2 sign-in needs. Nothing gives the password back.
 *
 * The shares are set one a message, a share being a name and a folder of
 * `/Home`; the window keeps them, and sends them again at each START.
 */

#define SMBD_OP_STATE        1u  /* how it is */
#define SMBD_OP_START        2u  /* u.start: serve the shares set, as the account named */
#define SMBD_OP_STOP         3u  /* stop serving; every connection ends */
#define SMBD_OP_SHARES_CLEAR 4u  /* no shares */
#define SMBD_OP_SHARE        5u  /* u.share: one more */
#define SMBD_OP_ACCOUNT      6u  /* u.account: a name, and a password whose NT hash is kept */
#define SMBD_OP_CONNECTION   7u  /* the connection after u.after (N12c-3) */
#define SMBD_OP_DISCONNECT   8u  /* the connection u.after names (N12c-3) */

#define SMBD_STOPPED  0u
#define SMBD_SERVING  1u
#define SMBD_WAITING  2u        /* asked to serve; the port would not open yet - again each second */

#define SMBD_ENCRYPT     1u     /* SMB 3's AES on the wire, asked of every client */
#define SMBD_LOCAL_ONLY  2u     /* only from this machine's own network (N12c-3) */

#define SMBD_SHARE_WRITE 1u     /* read and write (N14); refused until then */

#define SMBD_NAME_MAX     64u
#define SMBD_PATH_MAX    512u
#define SMBD_HOST_MAX     16u   /* NetBIOS's fifteen, and a zero */
#define SMBD_PASSWORD_MAX 128u

struct smbd_start {
    uint16_t port;
    uint16_t flags;             /* SMBD_ENCRYPT, SMBD_LOCAL_ONLY */
    uint32_t reserved;
    char     account[SMBD_NAME_MAX];
    char     hostname[SMBD_HOST_MAX];
};

struct smbd_share {
    char     name[SMBD_NAME_MAX];   /* "Music" */
    char     path[SMBD_PATH_MAX];   /* "/Home/Music" */
    uint32_t flags;                 /* SMBD_SHARE_WRITE */
    uint32_t reserved;
};

struct smbd_account {
    char     name[SMBD_NAME_MAX];
    uint32_t password_bytes;
    uint32_t reserved;
    char     password[SMBD_PASSWORD_MAX];
};

struct smbd_request {
    uint32_t op;
    uint32_t reserved;
    union {
        struct smbd_start   start;
        struct smbd_share   share;
        struct smbd_account account;
        uint32_t            after;  /* CONNECTION, DISCONNECT: a connection's id */
    } u;
};

/* A connection, as CONNECTION answers it. How long it has been connected
 * crosses as milliseconds (`CLAUDE.md`, *Two clocks*). */
struct smbd_connection {
    uint32_t id;                /* from 1 */
    uint8_t  address[4];        /* the client's, IPv4 */
    uint64_t for_ms;
    char     account[SMBD_NAME_MAX];
    char     share[SMBD_NAME_MAX];  /* the last share it opened; "" for none */
};

struct smbd_reply {
    uint32_t error;             /* SMBD_ERR_*, 0 when it was done */
    uint32_t state;             /* SMBD_STOPPED, SERVING, WAITING */
    uint16_t port;
    uint16_t flags;
    uint32_t shares;
    uint32_t connections;
    int32_t  listen_error;      /* WAITING: what the stack last said, a number */
    char     account[SMBD_NAME_MAX];
    char     hostname[SMBD_HOST_MAX];
    struct smbd_connection connection;  /* CONNECTION */
};

/* Errors are numbers; the sentence is the window's to compose. */
#define SMBD_ERR_BAD_OP      1u
#define SMBD_ERR_SHARE       2u  /* a name empty, too long, or taken; a path not in /Home */
#define SMBD_ERR_NO_FOLDER   3u  /* the path is not a folder on the disk */
#define SMBD_ERR_NO_ACCOUNT  4u  /* no account named, or none kept by that name */
#define SMBD_ERR_KEYRING     5u  /* the keyring would not keep it, or is not there */
#define SMBD_ERR_NO_SHARES   6u  /* START with nothing to share */
#define SMBD_ERR_PASSWORD    7u  /* an empty password, or a longer one than fits */
#define SMBD_ERR_SERVING     8u  /* START while serving: STOP first */
#define SMBD_ERR_NONE        9u  /* no connection after that one */
#define SMBD_ERR_LATER      10u  /* read and write, before N14 */

_Static_assert(sizeof(struct smbd_start) == 88, "smbd_start has no padding");
_Static_assert(sizeof(struct smbd_share) == 584, "smbd_share has no padding");
_Static_assert(sizeof(struct smbd_account) == 200, "smbd_account has no padding");
_Static_assert(sizeof(struct smbd_request) == 592, "smbd_request has no padding");
_Static_assert(sizeof(struct smbd_connection) == 144, "smbd_connection has no padding");
_Static_assert(sizeof(struct smbd_reply) == 248, "smbd_reply has no padding");
_Static_assert(sizeof(struct smbd_reply) <= 2048, "a reply fits in one message");

#endif /* KOSMOS_SMBDPROTO_H */
