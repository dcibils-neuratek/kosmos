/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a server calls itself, out of NTLM's challenge (`testing.md` 18.415,
 * `user/kits/smb/ntlm_name.c`), on this Mac: challenges built here as
 * MS-NLMP 2.2.1.2 lays them out - as Samba's peer sends one, as macOS's
 * server sent the one that named Diego's Mac "192", and with no name at all -
 * and cut short, and not challenges.
 *
 *   make build/host/test_ntlmname && build/host/test_ntlmname
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ntlm_name.h"

static unsigned checks, failed;

static void check(bool ok, const char *what, const char *got)
{
    checks++;

    if (!ok) {
        failed++;
        printf("FAIL: %s (got \"%s\")\n", what, got);
    }
}

/* A challenge being built: the fixed part, then the payload. */
static uint8_t msg[1024];
static size_t at;

static void put16(size_t where, uint32_t v)
{
    msg[where] = (uint8_t)v;
    msg[where + 1] = (uint8_t)(v >> 8);
}

static void put32(size_t where, uint32_t v)
{
    put16(where, v);
    put16(where + 2, v >> 16);
}

/* UTF-8 (ASCII and the BMP, which is all these use) as UTF-16LE, at `at`. */
static size_t utf16(const char *s)
{
    size_t from = at;
    const unsigned char *p = (const unsigned char *)s;

    while (*p) {
        uint32_t c;

        if (*p < 0x80) {
            c = *p++;
        } else if ((*p & 0xe0) == 0xc0) {
            c = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else {
            c = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6)
                | (p[2] & 0x3f);
            p += 3;
        }

        put16(at, c);
        at += 2;
    }

    return at - from;
}

/* One AV pair; `units` raw code units when `text` is NULL. */
static void pair(uint16_t id, const char *text, const uint16_t *units, size_t n)
{
    size_t head = at;
    size_t bytes = 0;

    at += 4;

    if (text != NULL) {
        bytes = utf16(text);
    } else {
        for (size_t i = 0; i < n; i++) {
            put16(at, units[i]);
            at += 2;
        }

        bytes = 2 * n;
    }

    put16(head, id);
    put16(head + 2, (uint32_t)bytes);
}

/*
 * A challenge: TargetName, then TargetInfo holding a NetBIOS computer name
 * and a DNS computer name when given, and the timestamp every real one has,
 * to MsvAvEOL. Returns its length.
 */
static size_t challenge(const char *target, const char *nb, const char *dns)
{
    size_t info_from, n;

    memset(msg, 0, sizeof(msg));
    memcpy(msg, "NTLMSSP", 8);
    put32(8, 2);                              /* CHALLENGE_MESSAGE */
    put32(20, 0x00a28235u);                   /* flags, as Samba's */
    memcpy(msg + 24, "\x01\x23\x45\x67\x89\xab\xcd\xef", 8);
    at = 56;                                  /* past the version, too */

    if (target != NULL) {
        put32(16, (uint32_t)at);
        n = utf16(target);
        put16(12, (uint32_t)n);
        put16(14, (uint32_t)n);
    }

    info_from = at;

    if (nb != NULL) {
        pair(1, nb, NULL, 0);
    }

    pair(2, "WORKGROUP", NULL, 0);            /* the domain's NetBIOS name */

    if (dns != NULL) {
        pair(3, dns, NULL, 0);
    }

    {
        static const uint16_t when[4] = { 0x1234, 0x5678, 0x9abc, 0x01dc };
        pair(7, NULL, when, 4);
    }

    put16(at, 0);
    put16(at + 2, 0);
    at += 4;

    put32(44, (uint32_t)info_from);
    put16(40, (uint32_t)(at - info_from));
    put16(42, (uint32_t)(at - info_from));

    return at;
}

static int name_of(size_t len, char *out, size_t room)
{
    return ntlm_challenge_name(msg, len, out, room);
}

int main(void)
{
    char name[64];
    size_t len;
    int r;

    /* Samba's peer, as `tools/smbpeer.py` runs it: its NetBIOS name. */
    len = challenge("MACPEER", "MACPEER", "diegos-mac-mini.local");
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_FOUND && strcmp(name, "MACPEER") == 0,
          "Samba's peer is MACPEER, its NetBIOS computer name", name);

    /* macOS's server reached by address, as it named itself on 6 October:
     * "192" as TargetName - and the computer's own names beside it. */
    len = challenge("192", "DIEGOS-MAC-MINI", "Diegos-Mac-mini.local");
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_FOUND && strcmp(name, "DIEGOS-MAC-MINI") == 0,
          "\"192\" as TargetName loses to the NetBIOS computer name", name);

    len = challenge("192", NULL, "Diegos-Mac-mini.local");
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_FOUND && strcmp(name, "Diegos-Mac-mini") == 0,
          "with no NetBIOS name, the DNS name's first label, as Finder shows it",
          name);

    len = challenge("192", "192", "192.168.1.38");
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_NONE && name[0] == '\0',
          "names that are an address, or a piece of one, are no name - the "
          "caller says the whole address", name);

    /* No name given at all: nothing, and the whole address is the caller's. */
    len = challenge(NULL, NULL, NULL);
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_NONE && name[0] == '\0',
          "a challenge with no name in it gives none", name);

    /* TargetName alone, when it is a name, is still a name. */
    len = challenge("MACPEER", NULL, NULL);
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_FOUND && strcmp(name, "MACPEER") == 0,
          "TargetName alone, when it is a name", name);

    /* UTF-8 out, a curly apostrophe whole. */
    len = challenge("192", "Diego\xe2\x80\x99s Mac", NULL);
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_FOUND && strcmp(name, "Diego\xe2\x80\x99s Mac") == 0,
          "a name beyond ASCII arrives as UTF-8", name);

    /* Cut at a character when it does not fit, never inside one. */
    len = challenge(NULL, "ab\xe2\x80\x99", NULL);
    r = name_of(len, name, 5);
    check(r == NTLM_NAME_FOUND && strcmp(name, "ab") == 0,
          "a name too long is cut at a character", name);

    /* A lone surrogate is no name; the next one is taken. */
    {
        static const uint16_t lone[3] = { 'M', 0xd800, 'X' };
        size_t info_from;

        len = challenge(NULL, NULL, NULL);
        info_from = 56;
        at = info_from;
        pair(1, NULL, lone, 3);
        pair(3, "macpeer.local", NULL, 0);
        put16(at, 0);
        put16(at + 2, 0);
        at += 4;
        put32(44, (uint32_t)info_from);
        put16(40, (uint32_t)(at - info_from));
        r = name_of(at, name, sizeof(name));
        check(r == NTLM_NAME_FOUND && strcmp(name, "macpeer") == 0,
              "a NetBIOS name with a lone surrogate gives way to the DNS one", name);
    }

    /* Cut short anywhere: more is needed, and nothing is read past it. */
    len = challenge("MACPEER", "MACPEER", "diegos-mac-mini.local");

    for (size_t cut = 0; cut < len; cut++) {
        r = name_of(cut, name, sizeof(name));

        if (cut >= 8 && r != NTLM_NAME_SHORT) {
            check(false, "a challenge cut short asks for more", name);
            break;
        }
    }

    checks++;

    /* Not a challenge: a NEGOTIATE, and something else entirely. */
    len = challenge("MACPEER", NULL, NULL);
    put32(8, 1);
    check(name_of(len, name, sizeof(name)) == NTLM_NAME_NOT,
          "a NEGOTIATE_MESSAGE is not a challenge", name);
    memcpy(msg, "NTLMSSQ", 8);
    check(name_of(len, name, sizeof(name)) == NTLM_NAME_NOT,
          "nor is anything without the signature", name);

    /* A pair's length past TargetInfo's end stops the walk, and no more. */
    len = challenge("192", "MACPEER", NULL);
    put16(56 + 6 + 2, 0xfff0);                /* NetBIOS name's length */
    r = name_of(len, name, sizeof(name));
    check(r == NTLM_NAME_NONE || (r == NTLM_NAME_FOUND && strcmp(name, "MACPEER") != 0),
          "a pair that claims more than there is is not read past", name);

    if (failed > 0) {
        printf("FAIL: %u of %u checks on what a server calls itself\n", failed, checks);
        return 1;
    }

    printf("PASS: %u checks on what a server calls itself, out of NTLM's "
           "challenge (Samba's MACPEER, macOS's \"192\" refused, the DNS "
           "name's first label, and the whole address when none)\n", checks);
    return 0;
}
