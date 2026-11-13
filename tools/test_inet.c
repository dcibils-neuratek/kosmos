/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The POSIX layer's addresses as text, held to this Mac's**
 * (`runtime/posix/inet.c`): every string in a table read by both
 * `inet_pton`s, for both families and one neither knows, the answers and
 * the bytes compared; then every address whose groups are zero in each of
 * the 256 ways eight groups can be, and the IPv4 ones in IPv6's clothes,
 * written by both `inet_ntop`s and the text compared.
 *
 * Kosmos's are compiled in under other names, against this Mac's headers,
 * so the two run side by side in one program. `KOSMOS_INET_BREAK=1`
 * answers -1 for IPv6 as Kosmos's did until 0.11.402, which WebCore read as
 * yes (`testing.md` 18.797) - and which this must catch.
 */

/* This Mac's own, declared under their own names first; Kosmos's then
 * include the same headers and find them already read. */
#include <arpa/inet.h>

#define inet_pton kosmos_inet_pton
#define inet_ntop kosmos_inet_ntop
#define inet_aton kosmos_inet_aton
#define inet_addr kosmos_inet_addr
#define inet_ntoa kosmos_inet_ntoa
#include "../runtime/posix/inet.c"
#undef inet_pton
#undef inet_ntop
#undef inet_aton
#undef inet_addr
#undef inet_ntoa

#include <stdlib.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *texts[] = {
    /* IPv4 */
    "0.0.0.0", "127.0.0.1", "255.255.255.255", "1.2.3.4", "256.1.1.1", "1.2.3",
    "1.2.3.4.5", "01.2.3.4", "1.2.3.04", "1..2.3", ".1.2.3", "1.2.3.", "1.2.3.4 ",
    " 1.2.3.4", "0x1.2.3.4", "1.2.3.1000", "",
    /* IPv6 */
    "::", "::1", "1::", "1::2", "::ffff:1.2.3.4", "::1.2.3.4", "1:2:3:4:5:6:7:8",
    "1:2:3:4:5:6:7::", "::2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7",
    "1:2:3:4:5:6::8", "1:2:3:4:5:6:1.2.3.4", "1:2:3:4:5:6:7:1.2.3.4",
    "1::2::3", ":::", ":1::2", "1::2:", "1:", ":1", ":", "12345::", "fffff::",
    "FFFF::abcd", "fe80::1%en0", "[::1]", "::1.2.3", "::1.2.3.4.5", "::256.1.1.1",
    "2001:db8::ff00:42:8329", "2001:0db8:0000:0000:0000:ff00:0042:8329",
    "0:0:0:0:0:0:0:0", "0:0:0:0:0:0:0:1", "::ffff:0.0.0.0", "1:2:3:4:5::1.2.3.4",
    "::0:1.2.3.4", "1.2.3.4::", "g::1", "1:::2", "::1:", "1::1.2.3.4", "localhost",
    "a", "abcd", "abcd:", ":abcd", "::abcd:1.2.3.4", "1:2:3:4:5:6:7:8::",
    "::1:2:3:4:5:6:7", "::1:2:3:4:5:6:7:8",
};

static int failures;

static void read_them(int broken)
{
    static const int families[] = { AF_INET, AF_INET6, AF_UNIX };
    size_t t, f;

    for (t = 0; t < COUNT(texts); t++) {
        for (f = 0; f < COUNT(families); f++) {
            unsigned char mine[16] = { 0 }, theirs[16] = { 0 };
            int fam = families[f];
            int a = broken && fam == AF_INET6 ? -1 : kosmos_inet_pton(fam, texts[t], mine);
            int b = inet_pton(fam, texts[t], theirs);

            /* The one difference, and it is chosen: Apple's reads a zone
             * after a `%` - an interface's name - and glibc's does not.
             * Kosmos has no interfaces to name, so it reads as glibc's. */
            if (fam == AF_INET6 && strchr(texts[t], '%') != NULL)
                b = 0;
            size_t size = fam == AF_INET ? 4 : 16;

            if (a != b || (a == 1 && memcmp(mine, theirs, size) != 0)) {
                printf("FAIL: inet_pton(%d, \"%s\") is %d here and %d on the Mac\n",
                       fam, texts[t], a, b);
                failures++;
            }
        }
    }
}

static void write_one(const unsigned char b[16])
{
    char mine[INET6_ADDRSTRLEN], theirs[INET6_ADDRSTRLEN];
    const char *a = kosmos_inet_ntop(AF_INET6, b, mine, sizeof mine);
    const char *c = inet_ntop(AF_INET6, b, theirs, sizeof theirs);
    unsigned char back[16];

    if (a == NULL || c == NULL || strcmp(a, c) != 0) {
        printf("FAIL: inet_ntop wrote \"%s\" where the Mac wrote \"%s\"\n",
               a ? a : "(nothing)", c ? c : "(nothing)");
        failures++;
        return;
    }

    if (kosmos_inet_pton(AF_INET6, a, back) != 1 || memcmp(back, b, 16) != 0) {
        printf("FAIL: \"%s\" does not read back as what it was written from\n", a);
        failures++;
    }
}

static void write_them(void)
{
    static const unsigned values[] = { 0x1, 0xab0, 0xffff };
    unsigned pattern, v, i;
    unsigned char b[16];

    for (pattern = 0; pattern < 256; pattern++) {
        for (v = 0; v < COUNT(values); v++) {
            for (i = 0; i < 8; i++) {
                unsigned w = pattern & (1u << i) ? values[(v + i) % COUNT(values)] : 0;
                b[2 * i] = (unsigned char)(w >> 8);
                b[2 * i + 1] = (unsigned char)w;
            }
            write_one(b);
        }
    }

    /* IPv4 in IPv6's clothes, and its near misses. */
    static const unsigned char four[][4] = { { 1, 2, 3, 4 }, { 0, 0, 0, 1 }, { 0, 0, 0, 0 },
                                             { 0, 1, 0, 0 }, { 255, 255, 255, 255 } };
    for (i = 0; i < COUNT(four); i++) {
        for (v = 0; v < 4; v++) {
            memset(b, 0, 16);
            if (v == 1) { b[10] = 0xff; b[11] = 0xff; }
            if (v == 2) { b[10] = 0xff; b[11] = 0xfe; }
            if (v == 3) { b[8] = 0x00; b[9] = 0x01; b[10] = 0xff; b[11] = 0xff; }
            memcpy(b + 12, four[i], 4);
            write_one(b);
        }
    }

    /* And IPv4 itself, and a room too small. */
    char text[INET_ADDRSTRLEN];
    unsigned char a4[4] = { 192, 168, 1, 40 };
    if (kosmos_inet_ntop(AF_INET, a4, text, sizeof text) == NULL || strcmp(text, "192.168.1.40")) {
        printf("FAIL: inet_ntop(AF_INET) wrote \"%s\"\n", text);
        failures++;
    }
    if (kosmos_inet_ntop(AF_INET, a4, text, 12) != NULL || errno != ENOSPC) {
        printf("FAIL: inet_ntop(AF_INET) fitted 12 characters and a nul in 12\n");
        failures++;
    }
}

int main(void)
{
    int broken = getenv("KOSMOS_INET_BREAK") != NULL;

    read_them(broken);
    write_them();

    if (failures) {
        printf("FAIL: %d answers differ from the Mac's\n", failures);
        return 1;
    }

    printf("PASS: inet_pton read %zu strings in three families and inet_ntop wrote "
           "%u IPv6 addresses, every answer as this Mac's\n",
           COUNT(texts), 256u * 3u + 20u);
    return 0;
}
