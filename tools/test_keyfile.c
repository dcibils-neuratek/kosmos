/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The keyring's file, on the Mac (`docs/keyring.md`, step K2;
 * `user/servers/keyfile.c`).
 *
 *   - two hundred entries sealed and opened back, every field and secret;
 *   - **one byte changed anywhere** in a small file - every position, header,
 *     sealed part and tag - and at a stride through a large one: refused,
 *     with nothing of what was opened left in the caller's records or the
 *     file's buffer;
 *   - a changed version, a file cut short by one byte and by one entry, a
 *     file sealed by another key, and one whose key id was made to match
 *     while its seal was another key's: each refused, each zeroed;
 *   - an entry that is not one - id 0, an unknown kind, an unended string -
 *     sealed with the right key: refused as malformed;
 *   - **and the file holds no secret in any encoding**: not the password in
 *     UTF-8 or UTF-16, not its NT hash in bytes or in hex either case, not a
 *     title, an account or an address - and two writes of the same entries
 *     share nothing past the header's fixed part.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "keyfile.h"

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static int zeroed(const void *p, size_t bytes)
{
    const uint8_t *b = p;
    size_t i;

    for (i = 0; i < bytes; i++)
        if (b[i] != 0)
            return 0;
    return 1;
}

static int contains(const uint8_t *hay, size_t n, const void *needle, size_t m)
{
    size_t i;

    for (i = 0; m > 0 && i + m <= n; i++)
        if (memcmp(hay + i, needle, m) == 0)
            return 1;
    return 0;
}

static const char PASSWORD[] = "Correct Horse 9! battery";

static void fill(struct key_record *r, size_t count)
{
    size_t i;

    memset(r, 0, count * sizeof *r);
    for (i = 0; i < count; i++) {
        struct key_entry *e = &r[i].entry;

        e->id = (uint32_t)(i * 3 + 1);
        e->kind = KEY_KIND_SMB;
        e->flags = i % 2 ? KEY_AT_START : 0;
        e->created_unix = 1790000000ull + i;
        e->modified_unix = 1790000100ull + i;
        e->used_unix = 1790000200ull + i;
        e->uses = (uint32_t)i;
        e->used_by = 40 + (uint32_t)i;
        snprintf(e->used_by_name, sizeof e->used_by_name, "smbfs");
        snprintf(e->service, sizeof e->service, "smb://10.0.%zu.%zu:445", i / 250, i % 250);
        snprintf(e->account, sizeof e->account, "account-%zu", i);
        snprintf(e->title, sizeof e->title, "Studio Mac %zu (SMB)", i);
        snprintf(e->notes, sizeof e->notes, "the one under the stairs, %zu", i);
        memcpy(e->shares, "Projects\0Music\0", 16);
        r[i].secret_bytes = (uint32_t)snprintf((char *)r[i].secret, KEY_SECRET_MAX,
                                               "%s %zu", PASSWORD, i);
    }
}

static uint8_t KEY[32], OTHER[32], NONCE[12], NONCE2[12];

/* Open `file` and hold the answer to `want`: on a refusal, the records and
 * the sealed part zeroed. */
static void opens_as(uint8_t *file, size_t bytes, int want, const char *what)
{
    static struct key_record got[256];
    size_t count = 99;
    uint32_t next = 99;
    char line[160];
    int answer;

    memset(got, 0x5a, sizeof got);
    answer = keyfile_open(KEY, file, bytes, got, 256, &count, &next);
    snprintf(line, sizeof line, "%s: answered %d, not %d", what, answer, want);
    check(answer == want, line);

    if (want != KEYFILE_OK) {
        snprintf(line, sizeof line, "%s: refused, and left something", what);
        check(count == 0 && next == 0 && zeroed(got, sizeof got)
              && (bytes <= KEYFILE_HEADER_BYTES
                  || zeroed(file + KEYFILE_HEADER_BYTES, bytes - KEYFILE_HEADER_BYTES)),
              line);
    }
}

int main(void)
{
    static struct key_record put[200], got[256];
    static uint8_t file[200 * 832 + 128], copy[200 * 832 + 128], again[200 * 832 + 128];
    size_t count, i, small_bytes;
    uint32_t next;
    long bytes;
    int answer;

    for (i = 0; i < 32; i++) {
        KEY[i] = (uint8_t)(i * 7 + 1);
        OTHER[i] = (uint8_t)(i * 11 + 3);
    }
    for (i = 0; i < 12; i++) {
        NONCE[i] = (uint8_t)(0xa0 + i);
        NONCE2[i] = (uint8_t)(0x10 + i);
    }

    /* Two hundred, round trip. */
    fill(put, 200);
    bytes = keyfile_seal(KEY, NONCE, put, 200, 601, file, sizeof file);
    check(bytes == (long)keyfile_bytes(200) && keyfile_count((size_t)bytes) == 200,
          "two hundred entries sealed to their size");
    memcpy(copy, file, (size_t)bytes);
    answer = keyfile_open(KEY, copy, (size_t)bytes, got, 256, &count, &next);
    check(answer == KEYFILE_OK && count == 200 && next == 601,
          "two hundred entries opened, with the next id");
    check(memcmp(got, put, sizeof put) == 0, "every field and secret came back as put");

    /* No secret in any encoding, in the file as written. */
    {
        uint8_t utf16[2 * sizeof PASSWORD], nt[16];
        char hex[33], HEX[33];
        size_t n = strlen(PASSWORD);

        for (i = 0; i < n; i++) {
            utf16[2 * i] = (uint8_t)PASSWORD[i];
            utf16[2 * i + 1] = 0;
        }
        crypto_md4(utf16, 2 * n, nt);
        for (i = 0; i < 16; i++) {
            snprintf(hex + 2 * i, 3, "%02x", nt[i]);
            snprintf(HEX + 2 * i, 3, "%02X", nt[i]);
        }

        check(!contains(file, (size_t)bytes, PASSWORD, n), "the password in UTF-8 is in the file");
        check(!contains(file, (size_t)bytes, "Correct Horse", 13), "part of the password is in the file");
        check(!contains(file, (size_t)bytes, utf16, 2 * n), "the password in UTF-16 is in the file");
        check(!contains(file, (size_t)bytes, nt, 16), "the NT hash is in the file");
        check(!contains(file, (size_t)bytes, hex, 32), "the NT hash in hex is in the file");
        check(!contains(file, (size_t)bytes, HEX, 32), "the NT hash in HEX is in the file");
        check(!contains(file, (size_t)bytes, "Studio Mac", 10), "a title is in the file");
        check(!contains(file, (size_t)bytes, "account-1", 9), "an account is in the file");
        check(!contains(file, (size_t)bytes, "smb://", 6), "an address is in the file");
        check(!contains(file, (size_t)bytes, "Projects", 8), "a share is in the file");
        check(!contains(file, (size_t)bytes, KEY, 32), "the key is in the file");
    }

    /* A second write, a new nonce: nothing in common past the fixed header. */
    {
        long again_bytes = keyfile_seal(KEY, NONCE2, put, 200, 601, again, sizeof again);
        size_t same = 0;

        for (i = KEYFILE_HEADER_BYTES; i < (size_t)again_bytes; i++)
            same += file[i] == again[i];
        check(again_bytes == bytes && memcmp(file, again, 32) == 0
              && same < (size_t)bytes / 64,
              "two writes with two nonces share more than chance would");
    }

    /* A byte changed at a stride through the large file. */
    for (i = 0; i < (size_t)bytes; i += 997) {
        char what[96];

        memcpy(copy, file, (size_t)bytes);
        copy[i] ^= 0x01;
        snprintf(what, sizeof what, "200 entries, byte %zu changed", i);
        opens_as(copy, (size_t)bytes, i < 8 || (i >= 8 && i < 16)
                                      ? KEYFILE_NOT_OURS
                                      : i < 32 ? KEYFILE_OTHER_KEY : KEYFILE_ALTERED, what);
    }

    /* Every byte of a small file. */
    bytes = keyfile_seal(KEY, NONCE, put, 3, 10, file, sizeof file);
    small_bytes = (size_t)bytes;
    for (i = 0; i < small_bytes; i++) {
        char what[96];
        int want = i < 16 ? KEYFILE_NOT_OURS : i < 32 ? KEYFILE_OTHER_KEY : KEYFILE_ALTERED;

        memcpy(copy, file, small_bytes);
        copy[i] ^= 0x80;
        snprintf(what, sizeof what, "3 entries, byte %zu changed", i);
        opens_as(copy, small_bytes, want, what);
    }

    /* Cut short, by a byte and by an entry. */
    memcpy(copy, file, small_bytes);
    opens_as(copy, small_bytes - 1, KEYFILE_NOT_OURS, "a file a byte short");
    memcpy(copy, file, small_bytes);
    opens_as(copy, small_bytes - sizeof(struct key_record), KEYFILE_ALTERED,
             "a file an entry short");

    /* Another key: named as another key; and named as ours but sealed by it. */
    bytes = keyfile_seal(OTHER, NONCE, put, 3, 10, copy, sizeof copy);
    opens_as(copy, (size_t)bytes, KEYFILE_OTHER_KEY, "a file sealed by another key");
    bytes = keyfile_seal(OTHER, NONCE, put, 3, 10, copy, sizeof copy);
    keyfile_key_id(KEY, copy + 16);
    opens_as(copy, (size_t)bytes, KEYFILE_ALTERED,
             "another key's file, its key id made to match");

    /* The right key, the right size, and not entries. */
    {
        static struct key_record bad[3];
        const char *why[] = { "id 0", "an unknown kind", "an unended title",
                              "an id past the next", "a secret too long", "two of one id" };
        int k;

        for (k = 0; k < 6; k++) {
            fill(bad, 3);
            if (k == 0) bad[1].entry.id = 0;
            if (k == 1) bad[1].entry.kind = 9;
            if (k == 2) memset(bad[1].entry.title, 'x', KEY_TITLE_MAX);
            if (k == 3) bad[1].entry.id = 10;
            if (k == 4) bad[1].secret_bytes = KEY_SECRET_MAX + 1;
            if (k == 5) bad[2].entry.id = bad[0].entry.id;
            bytes = keyfile_seal(KEY, NONCE, bad, 3, 10, copy, sizeof copy);
            opens_as(copy, (size_t)bytes, KEYFILE_MALFORMED, why[k]);
        }
    }

    /* An empty keyring is a keyring; and room too small is said. */
    bytes = keyfile_seal(KEY, NONCE, put, 0, 1, copy, sizeof copy);
    answer = keyfile_open(KEY, copy, (size_t)bytes, got, 256, &count, &next);
    check(bytes == (long)keyfile_bytes(0) && answer == KEYFILE_OK && count == 0 && next == 1,
          "an empty keyring opens empty");
    check(keyfile_seal(KEY, NONCE, put, 3, 10, copy, keyfile_bytes(3) - 1) == KEYFILE_TOO_SMALL,
          "sealing into too little room is refused");
    bytes = keyfile_seal(KEY, NONCE, put, 3, 10, copy, sizeof copy);
    answer = keyfile_open(KEY, copy, (size_t)bytes, got, 2, &count, &next);
    check(answer == KEYFILE_TOO_SMALL && count == 0, "opening three into room for two is refused");

    if (failures) {
        printf("FAIL: %d of %d checks on the keyring's file\n", failures, checks);
        return 1;
    }

    printf("PASS: %d checks on the keyring's file (200 entries round trip; a "
           "byte changed at every position of a small file and through a large "
           "one, cut short, another key, a matched key id, six malformed - each "
           "refused and zeroed; no password, NT hash, title, account, address "
           "or key in it in any encoding)\n", checks);
    return 0;
}
