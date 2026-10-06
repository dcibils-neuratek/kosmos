/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_KEYPROTO_H
#define KOSMOS_KEYPROTO_H

#include <stdint.h>

/*
 * What the keyring is asked, and what it keeps (`docs/keyring.md`,
 * *`keyproto.h`, a declared shape*; step K2).
 *
 * **The keyring decides by door, never by name.** Each door is an endpoint
 * of its own, handed to one process: `smb` to smbfs, which may list, get,
 * put and forget entries of kind SMB and no other; `manage` to Passwords,
 * which sees every entry without its secret, forgets and edits any, and
 * may `REVEAL` one - the Show button, Diego's decision 5. An operation a
 * door does not allow is `KEY_ERR_NOT_THIS_DOOR`, and a kind it does not
 * see answers as though nothing were kept.
 *
 * **The secret is the password itself**, sealed (decision 4: "i need to know
 * the password at some point"). smbfs is handed it at a sign-in, works out
 * the NT hash, and keeps that only while connected, as it does today.
 *
 * **One entry a reply**, walked by asking for the one after the last id -
 * `notifyproto.h`'s way, for the same reason: they are rare.
 */

#define KEY_OP_LIST     1u  /* the entry after `id`, of the kinds this door sees */
#define KEY_OP_GET      2u  /* smb: the secret kept for (service, account) */
#define KEY_OP_PUT      3u  /* smb: a secret for (service, account), new or replacing */
#define KEY_OP_FORGET   4u  /* by id: manage, any; smb, its own kind */
#define KEY_OP_EDIT     5u  /* manage: a title, notes, the at-start switch */
#define KEY_OP_STATE    6u  /* manage: how many, and how the file opened */
#define KEY_OP_REVEAL   7u  /* manage: the secret of `id`, for Show */

#define KEY_KIND_SMB    1u  /* a share's password */
#define KEY_KIND_WIFI   2u  /* later */
#define KEY_KIND_WEB    3u  /* later */
#define KEY_KIND_MAIL   4u  /* later */

#define KEY_AT_START    1u  /* smb: connect its shares when Kosmos starts */

#define KEY_SERVICE_MAX 128u    /* "smb://192.168.1.38:445", as smbfs spells it */
#define KEY_ACCOUNT_MAX  64u
#define KEY_TITLE_MAX    64u
#define KEY_NOTES_MAX   256u
#define KEY_SHARES_MAX  128u    /* smb: "Projects\0Music\0" */
#define KEY_SECRET_MAX  128u    /* a Wi-Fi key is 63; a password, whatever it is */
#define KEY_NAME_MAX     16u

/* What LIST answers: never the secret. Every time is seconds from `time()`,
 * named `_unix` (`CLAUDE.md`, *Two clocks*). */
struct key_entry {
    uint32_t id;                /* from 1, never reused */
    uint16_t kind, flags;       /* KEY_KIND_*, KEY_AT_START */
    uint64_t created_unix, modified_unix, used_unix;
    uint32_t uses;
    uint32_t used_by;           /* the process, as SYS_SENDER said */
    char     used_by_name[KEY_NAME_MAX];
    char     service[KEY_SERVICE_MAX];
    char     account[KEY_ACCOUNT_MAX];
    char     title[KEY_TITLE_MAX];
    char     notes[KEY_NOTES_MAX];
    char     shares[KEY_SHARES_MAX];
};

/* A request: `entry` carries what the operation names - `id`, or `kind`,
 * `service` and `account`, or the fields to set - and `secret` is read for
 * PUT alone. */
struct key_request {
    uint32_t op;
    uint32_t secret_bytes;
    struct key_entry entry;
    uint8_t  secret[KEY_SECRET_MAX];
};

/* How the keyring's file opened, for STATE. */
#define KEY_FILE_NEW        0u  /* there was none: an empty keyring */
#define KEY_FILE_OPENED     1u
#define KEY_FILE_SET_ASIDE  2u  /* it did not open; kept aside, an empty one begun */

/* A reply: `entry` for LIST, GET and REVEAL; `secret` for GET and REVEAL
 * alone; for STATE, `count` and `file`. */
struct key_reply {
    uint32_t error;             /* KEY_ERR_*, 0 when it was done */
    uint32_t secret_bytes;
    uint32_t count;
    uint32_t file;              /* KEY_FILE_* */
    struct key_entry entry;
    uint8_t  secret[KEY_SECRET_MAX];
};

/* Errors are numbers; the sentence is Passwords' to compose. */
#define KEY_ERR_BAD_OP        1u
#define KEY_ERR_NOT_THIS_DOOR 2u    /* an operation, or a kind, this door is not for */
#define KEY_ERR_NONE          3u    /* nothing kept for that */
#define KEY_ERR_FULL          4u
#define KEY_ERR_DISK          5u    /* the file could not be written; nothing changed */
#define KEY_ERR_SEALED        6u    /* the file did not open */
#define KEY_ERR_TOO_LONG      7u    /* a secret over KEY_SECRET_MAX */

_Static_assert(sizeof(struct key_entry) == 696, "key_entry has no padding");
_Static_assert(sizeof(struct key_request) == 832, "key_request has no padding");
_Static_assert(sizeof(struct key_reply) == 840, "key_reply has no padding");
_Static_assert(sizeof(struct key_reply) <= 2048, "a reply fits in one message");

#endif /* KOSMOS_KEYPROTO_H */
