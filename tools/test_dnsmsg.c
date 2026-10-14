/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A DNS message's names and records (`user/servers/dnsmsg.c`), on this Mac.
 *
 *   build/host/test_dnsmsg
 *
 * A name written and read back; a multicast DNS answer as a printer or a
 * Mac sends one - a PTR, an SRV, a TXT and an A, their names compressed
 * against each other - read record by record; and what somebody on the
 * network might send instead: a pointer onto itself, two pointing at each
 * other, a pointer forward, a label past the end, a name longer than 255,
 * a record longer than the message. Each refused, none read past the end.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dnsmsg.h"

static int failed, passed;

static void check(bool ok, const char *what)
{
    if (ok) {
        passed++;
    } else {
        failed++;
        printf("  FAIL: %s\n", what);
    }
}

static unsigned put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; return 2; }

int main(void)
{
    uint8_t m[512];
    char name[DNS_NAME_MAX + 1];
    unsigned n, at, next;
    struct dns_header h;
    struct dns_record r;

    /* Written and read back. */
    n = dns_put_name(m, sizeof(m), "_smb._tcp.local", 15);
    check(n == 17 && m[0] == 4 && m[5] == 4 && m[10] == 5 && m[16] == 0,
          "a name written as labels, ended by the empty one");
    check(dns_read_name(m, n, 0, name, sizeof(name), &next) && strcmp(name, "_smb._tcp.local") == 0
          && next == n, "and read back, ending where it ends");
    check(dns_put_name(m, sizeof(m), "a..b", 4) == 0, "an empty label refused");
    check(dns_put_name(m, sizeof(m), "x.", 2) == 0, "a dot at the end refused");

    /*
     * An answer, as multicast DNS sends one: a PTR from the service type to
     * an instance, the instance's SRV naming its host and port, its TXT,
     * and the host's A - each later name a pointer into an earlier one.
     */
    memset(m, 0, sizeof(m));
    at = 0;
    at += put16(m + at, 0);                 /* id: 0 in multicast DNS */
    at += put16(m + at, 0x8400);            /* an authoritative answer */
    at += put16(m + at, 0);
    at += put16(m + at, 4);                 /* four answers */
    at += put16(m + at, 0);
    at += put16(m + at, 0);
    {
        unsigned type_at = at, inst_at, host_at;

        at += dns_put_name(m + at, sizeof(m) - at, "_smb._tcp.local", 15);
        at += put16(m + at, DNS_TYPE_PTR);
        at += put16(m + at, DNS_CLASS_IN);
        at += put16(m + at, 0);
        at += put16(m + at, 4500);
        at += put16(m + at, 9);             /* the data: "Studio" and a pointer */
        inst_at = at;
        m[at++] = 6;
        memcpy(m + at, "Studio", 6);
        at += 6;
        m[at++] = (uint8_t)(0xc0 | (type_at >> 8));
        m[at++] = (uint8_t)type_at;

        /* SRV: owner the instance, by pointer; priority, weight, port, target. */
        m[at++] = (uint8_t)(0xc0 | (inst_at >> 8));
        m[at++] = (uint8_t)inst_at;
        at += put16(m + at, DNS_TYPE_SRV);
        at += put16(m + at, 0x8001);        /* IN, and flush */
        at += put16(m + at, 0);
        at += put16(m + at, 120);
        at += put16(m + at, 6 + 14);
        at += put16(m + at, 0);
        at += put16(m + at, 0);
        at += put16(m + at, 445);
        host_at = at;
        m[at++] = 6;
        memcpy(m + at, "studio", 6);
        at += 6;
        m[at++] = 5;
        memcpy(m + at, "local", 5);
        at += 5;
        m[at++] = 0;

        /* TXT, empty but for one zero-length string. */
        m[at++] = (uint8_t)(0xc0 | (inst_at >> 8));
        m[at++] = (uint8_t)inst_at;
        at += put16(m + at, DNS_TYPE_TXT);
        at += put16(m + at, 0x8001);
        at += put16(m + at, 0);
        at += put16(m + at, 4500);
        at += put16(m + at, 1);
        m[at++] = 0;

        /* A: the host, by pointer, and its address. */
        m[at++] = (uint8_t)(0xc0 | (host_at >> 8));
        m[at++] = (uint8_t)host_at;
        at += put16(m + at, DNS_TYPE_A);
        at += put16(m + at, 0x8001);
        at += put16(m + at, 0);
        at += put16(m + at, 120);
        at += put16(m + at, 4);
        m[at++] = 192; m[at++] = 168; m[at++] = 1; m[at++] = 38;
    }
    n = at;

    check(dns_read_header(m, n, &h) && h.answers == 4 && (h.flags & 0x8000) != 0,
          "the header: four answers, a response");
    at = DNS_HEADER;
    check(dns_read_record(m, n, &at, &r) && strcmp(r.name, "_smb._tcp.local") == 0
          && r.type == DNS_TYPE_PTR && r.ttl == 4500 && r.length == 9, "the PTR record");
    check(dns_read_name(m, n, r.data, name, sizeof(name), NULL)
          && strcmp(name, "Studio._smb._tcp.local") == 0,
          "its data: the instance, through a pointer back to the type");
    check(dns_read_record(m, n, &at, &r) && strcmp(r.name, "Studio._smb._tcp.local") == 0
          && r.type == DNS_TYPE_SRV && (r.class & 0x7fff) == DNS_CLASS_IN && (r.class & 0x8000),
          "the SRV, its owner a pointer to a name that is itself half pointer");
    check(r.length == 20 && m[r.data + 4] == (445 >> 8) && m[r.data + 5] == (445 & 0xff)
          && dns_read_name(m, n, r.data + 6, name, sizeof(name), NULL)
          && strcmp(name, "studio.local") == 0, "its port, 445, and its host");
    check(dns_read_record(m, n, &at, &r) && r.type == DNS_TYPE_TXT && r.length == 1,
          "the TXT");
    check(dns_read_record(m, n, &at, &r) && strcmp(r.name, "studio.local") == 0
          && r.type == DNS_TYPE_A && r.length == 4 && m[r.data] == 192 && m[r.data + 3] == 38,
          "the A: studio.local is 192.168.1.38");
    check(at == n, "and the message read to its end, exactly");

    /* What somebody else might send. */
    {
        uint8_t bad[64];

        memset(bad, 0, sizeof(bad));
        bad[12] = 0xc0; bad[13] = 12;           /* a pointer to itself */
        check(!dns_read_name(bad, 14, 12, name, sizeof(name), NULL), "a pointer onto itself refused");

        bad[20] = 0xc0; bad[21] = 22;           /* forward */
        check(!dns_read_name(bad, 30, 20, name, sizeof(name), NULL), "a pointer forward refused");

        bad[30] = 3; bad[31] = 'a'; bad[32] = 0xc0; bad[33] = 30;  /* label, then back to itself */
        check(!dns_read_name(bad, 34, 30, name, sizeof(name), NULL),
              "a pointer back to its own name's start, a loop, refused");

        bad[40] = 20; bad[41] = 'x';           /* a label longer than what is left */
        check(!dns_read_name(bad, 45, 40, name, sizeof(name), NULL), "a label past the end refused");

        bad[50] = 0x80;                          /* a reserved label type */
        check(!dns_read_name(bad, 52, 50, name, sizeof(name), NULL), "a reserved label type refused");
    }
    {
        uint8_t big[600];
        unsigned i, k = 0;

        for (i = 0; i < 5; i++) {               /* 5 x 63-byte labels: 320 bytes */
            big[k++] = 63;
            memset(big + k, 'a', 63);
            k += 63;
        }

        big[k++] = 0;

        {
            /* Room for all of it, so the rule refuses it and not the room. */
            char roomy[600];

            check(!dns_read_name(big, k, 0, roomy, sizeof(roomy), NULL),
                  "a name longer than 255 refused, with room for it");
        }
    }
    {
        struct dns_record rec;
        uint8_t shortm[40];
        unsigned a2 = 0, len;

        a2 += dns_put_name(shortm, sizeof(shortm), "x.local", 7);
        a2 += put16(shortm + a2, DNS_TYPE_A);
        a2 += put16(shortm + a2, 1);
        a2 += put16(shortm + a2, 0);
        a2 += put16(shortm + a2, 10);
        a2 += put16(shortm + a2, 200);          /* says 200 bytes follow */
        len = a2 + 4;                            /* and 4 do */
        a2 = 0;
        check(!dns_read_record(shortm, len, &a2, &rec), "a record longer than the message refused");
    }

    /* A record written (N13) is a record read back the same: its owner, type,
     * class with the flush bit, time to live and data. */
    {
        uint8_t w[128], srv[64];
        unsigned at = 0, sl, wl;
        struct dns_record rec;

        srv[0] = 0; srv[1] = 0; srv[2] = 0; srv[3] = 0;        /* priority, weight */
        srv[4] = 0x01; srv[5] = 0xbd;                           /* port 445 */
        sl = 6 + dns_put_name(srv + 6, sizeof(srv) - 6, "kosmos.local", 12);
        wl = dns_put_record(w, sizeof(w), "KOSMOS._smb._tcp.local", DNS_TYPE_SRV,
                            0x8001u, 120, srv, sl);
        check(wl > 0 && dns_read_record(w, wl, &at, &rec) && at == wl
              && strcmp(rec.name, "KOSMOS._smb._tcp.local") == 0 && rec.type == DNS_TYPE_SRV
              && rec.class == 0x8001u && rec.ttl == 120 && rec.length == sl
              && memcmp(w + rec.data, srv, sl) == 0,
              "a record written reads back the same");
        check(dns_put_record(w, 20, "KOSMOS._smb._tcp.local", DNS_TYPE_SRV, 1, 120, srv, sl) == 0,
              "a record that does not fit is refused");
        check(dns_put_record(w, sizeof(w), "a..b", DNS_TYPE_A, 1, 120, srv, 4) == 0,
              "a record whose name is not one is refused");
    }

    if (failed) {
        printf("FAIL: %d of %d checks on DNS messages\n", failed, failed + passed);
        return 1;
    }

    printf("PASS: %d checks on DNS messages (a name written and read; a multicast DNS answer - "
           "PTR, SRV, TXT and A, compressed - read to its end; a pointer onto itself, forward "
           "and in a loop, a label past the end, a reserved label, a name over 255 and a "
           "record past the message, refused; a record written read back the same)\n", passed);
    return 0;
}
