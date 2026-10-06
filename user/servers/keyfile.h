/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_KEYFILE_H
#define KOSMOS_KEYFILE_H

#include <stddef.h>
#include <stdint.h>

#include "keyproto.h"

/*
 * The keyring's file, sealed whole (`docs/keyring.md`, *Where the secrets
 * live*; step K2). Pure functions over bytes: no disk, no process, no
 * clock - the keyring server reads and writes the file, and this says what
 * is in it. Compiled on the Mac as `kfs.c` is, and held there by
 * `tools/test_keyfile.c`.
 */

#define KEYFILE_KEY_BYTES    32u    /* AES-256 */
#define KEYFILE_NONCE_BYTES  12u
#define KEYFILE_TAG_BYTES    16u
#define KEYFILE_VERSION       1u
#define KEYFILE_PLACE_FILE    0u    /* the key in /Keyring/machine-key */

/* One entry as it lies in the file: what LIST shows, and the secret. */
struct key_record {
    struct key_entry entry;
    uint32_t secret_bytes;
    uint32_t reserved;          /* 0 */
    uint8_t  secret[KEY_SECRET_MAX];
};

/* What the file is before the records: the header - all of it the sealing's
 * associated data - then, sealed, how many and the next id. */
#define KEYFILE_HEADER_BYTES   44u  /* magic 8, version 4, place 4, key id 16, nonce 12 */
#define KEYFILE_COUNTS_BYTES    8u  /* count 4, next id 4 */

/* What opening answers. Every one but OK has zeroed what it was handed to
 * fill, and the plaintext it decrypted. */
#define KEYFILE_OK          0
#define KEYFILE_NOT_OURS   -1   /* not a keyring file, or not this version */
#define KEYFILE_OTHER_KEY  -2   /* sealed by another key than this one */
#define KEYFILE_ALTERED    -3   /* the tag does not hold: changed, or damaged */
#define KEYFILE_MALFORMED  -4   /* it opened, and what is inside is not entries */
#define KEYFILE_TOO_SMALL  -5   /* the caller's room is not enough */

/* The bytes a file of `count` entries takes. */
size_t keyfile_bytes(size_t count);

/* How many entries a file of `bytes` holds, or -1 if no file is that size.
 * Read before opening, to size what `keyfile_open` fills. */
long keyfile_count(size_t bytes);

/* Which key this is, as the header says it: 16 bytes of a SHA-256 over a
 * label and the key - names the key, gives nothing of it. */
void keyfile_key_id(const uint8_t key[KEYFILE_KEY_BYTES], uint8_t id[16]);

/* Seal `count` entries into `out`, which has `room` bytes; the nonce must be
 * new for every write - the keyring draws it from the generator. Answers the
 * file's size, or KEYFILE_TOO_SMALL. */
long keyfile_seal(const uint8_t key[KEYFILE_KEY_BYTES],
                  const uint8_t nonce[KEYFILE_NONCE_BYTES],
                  const struct key_record *records, size_t count,
                  uint32_t next_id, uint8_t *out, size_t room);

/* Open `file`, `bytes` long, into `records` - room for `room` of them - and
 * say how many and the next id. Decrypts in place, so `file` is the
 * caller's to throw away after; on any failure, `records` and `file`'s
 * sealed part are zeroed. */
int keyfile_open(const uint8_t key[KEYFILE_KEY_BYTES], uint8_t *file, size_t bytes,
                 struct key_record *records, size_t room,
                 size_t *count, uint32_t *next_id);

_Static_assert(sizeof(struct key_record) == 832, "key_record has no padding");

#endif /* KOSMOS_KEYFILE_H */
