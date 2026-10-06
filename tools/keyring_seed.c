/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A keyring made on the Mac, for a disk a suite boots (`docs/keyring.md`,
 * K6): `machine-key` and `keyring` in a folder, the second sealed by
 * `user/servers/keyfile.c` itself - the machine's own format, so what the
 * keyring opens on the machine is exactly what it would have written.
 *
 *   keyring_seed DIR < entries
 *
 * One entry a line, its fields between bars:
 *
 *   service|account|title|share,share|password|at_start|notes|uses
 *
 * **Made-up entries only**: a suite's disk is a test's, and the gallery's
 * picture is published - never a real account or password here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyfile.h"

static void put(char *to, size_t room, const char *from)
{
    snprintf(to, room, "%s", from != NULL ? from : "");
}

int main(int argc, char **argv)
{
    static struct key_record records[64];
    static uint8_t out[sizeof records + 128];
    uint8_t key[KEYFILE_KEY_BYTES], nonce[KEYFILE_NONCE_BYTES];
    char line[2048], path[1024];
    size_t count = 0;
    long bytes;
    FILE *f;

    if (argc != 2) {
        fprintf(stderr, "usage: keyring_seed DIR < entries\n");
        return 2;
    }

    arc4random_buf(key, sizeof key);
    arc4random_buf(nonce, sizeof nonce);

    while (count < 64 && fgets(line, sizeof line, stdin) != NULL) {
        struct key_record *r = &records[count];
        char *field[8] = { 0 };
        char *at = line;
        int n = 0;

        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        while (n < 8) {
            field[n++] = at;
            at = strchr(at, '|');
            if (at == NULL) break;
            *at++ = '\0';
        }

        memset(r, 0, sizeof *r);
        r->entry.id = (uint32_t)count + 1;
        r->entry.kind = KEY_KIND_SMB;
        r->entry.flags = field[5] != NULL && strcmp(field[5], "1") == 0 ? KEY_AT_START : 0;
        r->entry.created_unix = 1790000000ull + count * 86400ull;
        r->entry.modified_unix = r->entry.created_unix + 3600;
        r->entry.uses = field[7] != NULL ? (uint32_t)atoi(field[7]) : 0;
        r->entry.used_unix = r->entry.uses ? r->entry.modified_unix + 7200 : 0;
        put(r->entry.used_by_name, sizeof r->entry.used_by_name, r->entry.uses ? "smbfs" : "");
        put(r->entry.service, sizeof r->entry.service, field[0]);
        put(r->entry.account, sizeof r->entry.account, field[1]);
        put(r->entry.title, sizeof r->entry.title, field[2]);
        put(r->entry.notes, sizeof r->entry.notes, field[6]);

        /* "A,B" -> "A\0B\0" */
        if (field[3] != NULL) {
            size_t o = 0;

            for (char *s = strtok(field[3], ","); s != NULL && o + strlen(s) + 2 < KEY_SHARES_MAX;
                 s = strtok(NULL, ",")) {
                memcpy(r->entry.shares + o, s, strlen(s));
                o += strlen(s) + 1;
            }
        }

        put((char *)r->secret, sizeof r->secret, field[4]);
        r->secret_bytes = (uint32_t)strlen((char *)r->secret);
        count++;
    }

    bytes = keyfile_seal(key, nonce, records, count, (uint32_t)count + 1, out, sizeof out);
    if (bytes < 0) {
        fprintf(stderr, "keyring_seed: it would not seal\n");
        return 1;
    }

    snprintf(path, sizeof path, "%s/machine-key", argv[1]);
    if ((f = fopen(path, "wb")) == NULL || fwrite(key, 1, sizeof key, f) != sizeof key) {
        perror(path);
        return 1;
    }
    fclose(f);

    snprintf(path, sizeof path, "%s/keyring", argv[1]);
    if ((f = fopen(path, "wb")) == NULL || fwrite(out, 1, (size_t)bytes, f) != (size_t)bytes) {
        perror(path);
        return 1;
    }
    fclose(f);

    memset(key, 0, sizeof key);
    printf("keyring_seed: %zu %s sealed into %s\n", count, count == 1 ? "entry" : "entries", argv[1]);
    return 0;
}
