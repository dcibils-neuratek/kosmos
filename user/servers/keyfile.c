/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The keyring's file (`docs/keyring.md`; step K2): every entry, sealed
 * whole with AES-256-CCM through the Crypto Kit's one door.
 *
 *   "KOSKEYR1"   8   magic
 *   version      4   1
 *   key place    4   0: the key is /Keyring/machine-key
 *   key id      16   which key sealed this, in the clear
 *   nonce       12   random, new at every write
 *   -- sealed --
 *   count        4
 *   next id      4   ids are never reused
 *   records      n   struct key_record each: the entry and its secret
 *   -- --
 *   tag         16
 *
 * **The header is the associated data**, so a changed version, place, key id
 * or nonce is refused as surely as a changed byte of an entry. **Sealed
 * whole, not each secret**: titles and addresses say which servers this
 * machine signs into, and a file sealed whole says nothing but its size.
 *
 * **The key id** lets the keyring tell "another machine's key" from
 * "damaged" - two different sentences for a person, and only the second is
 * worth worrying about. It is a hash of the key under a label of its own,
 * so it says nothing that would help find the key.
 *
 * Integers are little-endian, written byte by byte rather than as a cast
 * struct, so the file is the same from every machine Kosmos runs on. The
 * records are `struct key_record` as they lie, which `keyproto.h`'s and
 * `keyfile.h`'s asserts hold to one layout without padding; every machine
 * Kosmos runs on is little-endian and 64-bit, which `CLAUDE.md` decides.
 */

#include <string.h>

#include "crypto.h"
#include "keyfile.h"

static const char MAGIC[8] = { 'K', 'O', 'S', 'K', 'E', 'Y', 'R', '1' };

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

size_t keyfile_bytes(size_t count)
{
    return KEYFILE_HEADER_BYTES + KEYFILE_COUNTS_BYTES
         + count * sizeof(struct key_record) + KEYFILE_TAG_BYTES;
}

long keyfile_count(size_t bytes)
{
    size_t fixed = keyfile_bytes(0);

    if (bytes < fixed || (bytes - fixed) % sizeof(struct key_record) != 0)
        return -1;

    return (long)((bytes - fixed) / sizeof(struct key_record));
}

void keyfile_key_id(const uint8_t key[KEYFILE_KEY_BYTES], uint8_t id[16])
{
    static const char label[] = "Kosmos keyring: which key";
    struct sha256 s;
    uint8_t out[32];

    sha256_init(&s);
    sha256_update(&s, label, sizeof label);
    sha256_update(&s, key, KEYFILE_KEY_BYTES);
    sha256_final(&s, out);
    memcpy(id, out, 16);
}

long keyfile_seal(const uint8_t key[KEYFILE_KEY_BYTES],
                  const uint8_t nonce[KEYFILE_NONCE_BYTES],
                  const struct key_record *records, size_t count,
                  uint32_t next_id, uint8_t *out, size_t room)
{
    size_t bytes = keyfile_bytes(count);
    size_t sealed = KEYFILE_COUNTS_BYTES + count * sizeof(struct key_record);
    uint8_t *body = out + KEYFILE_HEADER_BYTES;

    if (room < bytes || count > UINT32_MAX)
        return KEYFILE_TOO_SMALL;

    memcpy(out, MAGIC, 8);
    put32(out + 8, KEYFILE_VERSION);
    put32(out + 12, KEYFILE_PLACE_FILE);
    keyfile_key_id(key, out + 16);
    memcpy(out + 32, nonce, KEYFILE_NONCE_BYTES);

    put32(body, (uint32_t)count);
    put32(body + 4, next_id);
    if (count > 0)
        memcpy(body + KEYFILE_COUNTS_BYTES, records, count * sizeof(struct key_record));

    if (crypto_aes_ccm_seal(key, KEYFILE_KEY_BYTES, nonce, KEYFILE_NONCE_BYTES,
                            out, KEYFILE_HEADER_BYTES, body, sealed,
                            body + sealed, KEYFILE_TAG_BYTES) != 0) {
        memset(out, 0, bytes);
        return KEYFILE_TOO_SMALL;   /* a size CCM's 12-byte nonce cannot count */
    }

    return (long)bytes;
}

/* Whether a string field ends inside itself. */
static int ended(const char *s, size_t room)
{
    return memchr(s, 0, room) != NULL;
}

/* What is inside, held to what an entry can be: a key that sealed garbage
 * of the right size is not a keyring either. */
static int well_formed(const struct key_record *r, size_t count, uint32_t next_id)
{
    size_t i, j;

    for (i = 0; i < count; i++) {
        const struct key_entry *e = &r[i].entry;

        if (e->id == 0 || e->id >= next_id
            || e->kind < KEY_KIND_SMB || e->kind > KEY_KIND_MAIL
            || r[i].secret_bytes > KEY_SECRET_MAX || r[i].reserved != 0
            || !ended(e->used_by_name, sizeof e->used_by_name)
            || !ended(e->service, sizeof e->service)
            || !ended(e->account, sizeof e->account)
            || !ended(e->title, sizeof e->title)
            || !ended(e->notes, sizeof e->notes)
            || !ended(e->shares, sizeof e->shares))
            return 0;

        for (j = 0; j < i; j++)
            if (r[j].entry.id == e->id)
                return 0;
    }

    return 1;
}

int keyfile_open(const uint8_t key[KEYFILE_KEY_BYTES], uint8_t *file, size_t bytes,
                 struct key_record *records, size_t room,
                 size_t *count, uint32_t *next_id)
{
    long holds = keyfile_count(bytes);
    uint8_t id[16];
    uint8_t *body = file + KEYFILE_HEADER_BYTES;
    size_t sealed;
    int answer = KEYFILE_OK;

    *count = 0;
    *next_id = 0;

    if (holds < 0 || memcmp(file, MAGIC, 8) != 0
        || get32(file + 8) != KEYFILE_VERSION
        || get32(file + 12) != KEYFILE_PLACE_FILE) {
        answer = KEYFILE_NOT_OURS;
        goto refused;
    }

    keyfile_key_id(key, id);
    if (memcmp(file + 16, id, 16) != 0) {
        answer = KEYFILE_OTHER_KEY;
        goto refused;
    }

    if ((size_t)holds > room) {
        answer = KEYFILE_TOO_SMALL;
        goto refused;
    }

    sealed = KEYFILE_COUNTS_BYTES + (size_t)holds * sizeof(struct key_record);

    /* The door zeroes `body` itself when the tag does not hold. */
    if (crypto_aes_ccm_open(key, KEYFILE_KEY_BYTES, file + 32, KEYFILE_NONCE_BYTES,
                            file, KEYFILE_HEADER_BYTES, body, sealed,
                            body + sealed, KEYFILE_TAG_BYTES) != 0) {
        answer = KEYFILE_ALTERED;
        goto refused;
    }

    if (get32(body) != (uint32_t)holds) {
        answer = KEYFILE_MALFORMED;
        goto refused;
    }

    if (holds > 0)
        memcpy(records, body + KEYFILE_COUNTS_BYTES, (size_t)holds * sizeof(struct key_record));
    *next_id = get32(body + 4);
    memset(body, 0, sealed);

    if (!well_formed(records, (size_t)holds, *next_id)) {
        answer = KEYFILE_MALFORMED;
        goto refused;
    }

    *count = (size_t)holds;
    return KEYFILE_OK;

refused:
    if (bytes > KEYFILE_HEADER_BYTES)
        memset(body, 0, bytes - KEYFILE_HEADER_BYTES);
    memset(records, 0, room * sizeof(struct key_record));
    *count = 0;
    *next_id = 0;
    return answer;
}
