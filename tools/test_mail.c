/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The Mail Kit's reading, on the Mac (`user/kits/mail/mime.c`, `docs/mail.md`
 * M1): messages written here, every byte of them known, and each answer
 * held to the byte - fields folded over lines and encoded in two ways, a
 * quoted name with a comma in it, a date and its zone, a multipart inside a
 * multipart, base64 broken into lines, quoted-printable with a soft break in
 * Windows-1252, an attachment named by RFC 2231, a message of HTML alone, a
 * boundary that appears in the text, a message cut short, and bytes that are
 * not a message at all. Every name and address in them is made up.
 */

#include "mime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static struct mime_msg msg;

static void parse(const char *text)
{
    check(mime_parse(&msg, (const uint8_t *)text, strlen(text)) == 0, "a message did not parse");
}

static const char *field(size_t part, const char *name)
{
    static char out[1024];

    if (mime_header(&msg, part, name, out, sizeof out) < 0) return NULL;
    return out;
}

/* A part's content as a string, and its length. */
static const char *content(size_t p, size_t *len)
{
    static uint8_t out[64 * 1024];
    size_t n = mime_part_bytes(&msg, p, out, sizeof out);

    out[n < sizeof out ? n : sizeof out - 1] = '\0';
    *len = n;
    return (const char *)out;
}

static long find(const char *type)
{
    for (size_t i = 0; i < msg.nparts; i++) {
        if (strcmp(msg.part[i].type, type) == 0) return (long)i;
    }

    return -1;
}

int main(void)
{
    char said[400];
    size_t n;
    int ok;

    /* 1. A plain message: folded fields, encoded words B and Q, addresses. */
    parse("From: \"Ferreira, Tom\xc3\xa1s\" <tomas@example.org>\r\n"
          "To: Lena Moreau <lena@example.com>, bob@example.net (Bob),\r\n"
          "  =?UTF-8?B?w4FuZ2VsYQ==?= <angela@example.com>\r\n"
          "Subject: =?UTF-8?Q?Caf=C3=A9_on?= =?ISO-8859-1?Q?_S=E1bado?=\r\n"
          "  and the photos\r\n"
          "Date: Tue, 6 Oct 2026 09:41:07 +0200\r\n"
          "Message-ID: <a1@example.org>\r\n"
          "\r\n"
          "Hello Lena,\r\n"
          "\r\n"
          "> what you wrote, quoted\r\n"
          "Here are   the photos.\r\n");

    snprintf(said, sizeof said, "the subject came out as %s", field(0, "subject") ? field(0, "subject") : "nothing");
    check(field(0, "Subject") && strcmp(field(0, "Subject"), "Caf\xc3\xa9 on S\xc3\xa1" "bado and the photos") == 0, said);
    check(field(0, "x-not-there") == NULL, "a field the message does not have was found");

    {
        struct mime_address a[4];
        size_t k = mime_addresses(field(0, "to"), a, 4);

        snprintf(said, sizeof said, "To read as %zu: %s <%s>, %s <%s>, %s <%s>", k,
                 a[0].name, a[0].address, a[1].name, a[1].address, a[2].name, a[2].address);
        check(k == 3 && strcmp(a[0].name, "Lena Moreau") == 0
              && strcmp(a[0].address, "lena@example.com") == 0
              && strcmp(a[1].name, "Bob") == 0 && strcmp(a[1].address, "bob@example.net") == 0
              && strcmp(a[2].name, "\xc3\x81ngela") == 0, said);

        k = mime_addresses(field(0, "from"), a, 4);
        check(k == 1 && strcmp(a[0].name, "Ferreira, Tom\xc3\xa1s") == 0
              && strcmp(a[0].address, "tomas@example.org") == 0,
              "a quoted name with a comma in it was split");
    }

    {
        int64_t t = mime_date(field(0, "date"), &ok);

        /* 2026-10-06 07:41:07 UTC. */
        snprintf(said, sizeof said, "the date read as %lld", (long long)t);
        check(ok && t == 1791272467LL, said);
        check(mime_date("6 Oct 26 09:41 GMT", &ok) == 1791279660LL && ok,
              "a date with a two-figure year, no seconds and GMT read wrong");
        mime_date("whenever", &ok);
        check(!ok, "words that are not a date were read as one");
    }

    {
        char pv[80];

        mime_preview(&msg, pv, sizeof pv);
        snprintf(said, sizeof said, "the preview was \"%s\"", pv);
        check(strcmp(pv, "Hello Lena, Here are the photos.") == 0, said);
    }

    /* 2. Mixed, with an alternative inside it and an attachment. */
    parse("From: rides@greenway.example.org\n"
          "Subject: Saturday\n"
          "MIME-Version: 1.0\n"
          "Content-Type: multipart/mixed; boundary=\"outer\"\n"
          "\n"
          "This is the preamble, which nobody reads.\n"
          "--outer\n"
          "Content-Type: multipart/alternative; boundary=inner\n"
          "\n"
          "--inner\n"
          "Content-Type: text/plain; charset=windows-1252\n"
          "Content-Transfer-Encoding: quoted-printable\n"
          "\n"
          "Coffee at the mill =96 forty kilometres, a long=\n"
          " ride. The line --outer in the text is not a boundary.\n"
          "--inner\n"
          "Content-Type: text/html; charset=utf-8\n"
          "\n"
          "<p>Coffee at the <b>mill</b> &amp; back.</p>\n"
          "--inner--\n"
          "--outer\n"
          "Content-Type: application/gpx+xml\n"
          "Content-Disposition: attachment;\n"
          " filename*=UTF-8''route%20to%20the%20m%C3%BChle.gpx\n"
          "Content-Transfer-Encoding: base64\n"
          "Content-ID: <route@greenway>\n"
          "\n"
          "PGdweD5yaXZlcjwv\n"
          "Z3B4Pg==\n"
          "--outer--\n"
          "And an epilogue.\n");

    snprintf(said, sizeof said, "%zu parts, not 5 (mixed, alternative, plain, html, attachment)", msg.nparts);
    check(msg.nparts == 5, said);
    check(msg.part[0].multipart && msg.part[1].multipart && msg.part[1].parent == 0
          && msg.part[2].parent == 1 && msg.part[3].parent == 1 && msg.part[4].parent == 0,
          "the parts are not a mixed holding an alternative and an attachment");

    {
        long t = find("text/plain"), h = find("text/html"), a = find("application/gpx+xml");
        const char *s;

        check(t >= 0 && strcmp(msg.part[t].charset, "windows-1252") == 0
              && strcmp(msg.part[t].encoding, "quoted-printable") == 0,
              "the plain part's charset and encoding were not read");

        s = content((size_t)t, &n);
        snprintf(said, sizeof said, "the plain part came out as \"%s\"", s);
        check(strcmp(s, "Coffee at the mill \xe2\x80\x93 forty kilometres, a long ride. "
                        "The line --outer in the text is not a boundary.") == 0, said);

        s = content((size_t)h, &n);
        check(strcmp(s, "<p>Coffee at the <b>mill</b> &amp; back.</p>") == 0,
              "the HTML part was not as written");

        snprintf(said, sizeof said, "the attachment's name was \"%s\", its disposition \"%s\", its cid \"%s\"",
                 msg.part[a].name, msg.part[a].disposition, msg.part[a].cid);
        check(a >= 0 && strcmp(msg.part[a].name, "route to the m\xc3\xbchle.gpx") == 0
              && strcmp(msg.part[a].disposition, "attachment") == 0
              && strcmp(msg.part[a].cid, "route@greenway") == 0, said);

        s = content((size_t)a, &n);
        check(n == 16 && memcmp(s, "<gpx>river</gpx>", 16) == 0,
              "base64 broken over two lines did not come back whole");
    }

    {
        char pv[120];

        mime_preview(&msg, pv, sizeof pv);
        check(strncmp(pv, "Coffee at the mill \xe2\x80\x93 forty", 23) == 0,
              "the preview of a mixed message was not its plain text");
    }

    /* 3. HTML alone: its preview without the tags. */
    parse("Subject: Order 40213\r\n"
          "Content-Type: text/html; charset=\"iso-8859-1\"\r\n"
          "\r\n"
          "<html><body><h1>Shipped</h1><p>Two brackets &amp; screws, caf\xe9 sold separately.</p></body></html>\r\n");

    {
        char pv[120];

        mime_preview(&msg, pv, sizeof pv);
        snprintf(said, sizeof said, "the HTML preview was \"%s\"", pv);
        check(strcmp(pv, "Shipped Two brackets & screws, caf\xc3\xa9 sold separately.") == 0, said);
    }

    /* 3b. A newsletter's head and style are not its words. */
    parse("Subject: News\r\n"
          "Content-Type: text/html; charset=utf-8\r\n"
          "\r\n"
          "<html><head><title>Ignore</title><STYLE type=\"text/css\">td, a { font-family: Arial; }"
          "</STYLE></head><body><script>var x = 1;</script><p>Real words.</p></body></html>\r\n");

    {
        char pv[120];

        mime_preview(&msg, pv, sizeof pv);
        snprintf(said, sizeof said, "a newsletter's preview was \"%s\"", pv);
        check(strcmp(pv, "Real words.") == 0, said);
    }

    /* 4. Cut short: no closing boundary. The parts there were are kept. */
    parse("Content-Type: multipart/mixed; boundary=b\n\n--b\n\nfirst\n--b\n\nsecond, and then noth");
    check(msg.nparts == 3, "a multipart cut short did not keep the parts it had");
    check(strcmp(content(2, &n), "second, and then noth") == 0, "the last, cut part was not kept");

    /* 5. Not a message at all; and nesting past the limit. */
    check(mime_parse(&msg, (const uint8_t *)"", 0) != 0, "nothing was parsed as a message");

    {
        static char deep[64 * 1024];
        size_t o = 0;

        for (int i = 0; i < 40; i++) {
            o += (size_t)snprintf(deep + o, sizeof deep - o,
                                  "Content-Type: multipart/mixed; boundary=d%d\n\n--d%d\n", i, i);
        }

        o += (size_t)snprintf(deep + o, sizeof deep - o, "\nbottom\n");
        parse(deep);
        snprintf(said, sizeof said, "forty nested multiparts made %zu parts, deeper than %u",
                 msg.nparts, MIME_DEPTH_MOST);
        check(msg.nparts <= MIME_DEPTH_MOST, said);
    }

    /* 6. Characters: Windows-1252's euro, a bad UTF-8 byte replaced. */
    {
        uint8_t out[32];
        size_t k = mime_to_utf8("Windows-1252", (const uint8_t *)"\x80" "5", 2, out, sizeof out);

        check(k == 4 && memcmp(out, "\xe2\x82\xac" "5", 4) == 0, "Windows-1252's euro was not made UTF-8");

        k = mime_to_utf8("utf-8", (const uint8_t *)"a\xffz", 3, out, sizeof out);
        check(k == 5 && memcmp(out, "a\xef\xbf\xbdz", 5) == 0, "a byte that is not UTF-8 was not replaced");
    }

    /* 7. Writing (M6): a plain message to the byte, then the hard ones read
     *    back through the reading half. */
    {
        static uint8_t out[16 * 1024];
        static char text[8 * 1024];
        struct mime_address to[2], cc[1];
        struct mail_draft d;
        size_t n;
        char said[512];
        const char *want =
            "Date: Tue, 6 Oct 2026 09:41:07 +0200\r\n"
            "From: Lena Moreau <lena@example.com>\r\n"
            "To: bob@example.org\r\n"
            "Subject: Coffee on Saturday\r\n"
            "Message-ID: <k1.a2@example.com>\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Transfer-Encoding: 7bit\r\n"
            "\r\n"
            "Hello Bob,\r\n"
            "\r\n"
            ". a line that begins with a dot stays as it is\r\n";

        memset(to, 0, sizeof to);
        memset(cc, 0, sizeof cc);
        strcpy(to[0].address, "bob@example.org");
        memset(&d, 0, sizeof d);
        d.from_name = "Lena Moreau";
        d.from_address = "lena@example.com";
        d.to = to;
        d.nto = 1;
        d.subject = "Coffee on Saturday";
        d.text = (const uint8_t *)"Hello Bob,\n\n. a line that begins with a dot stays as it is\n";
        d.text_len = strlen((const char *)d.text);
        d.date = 1791272467LL;
        d.zone = 120;
        d.message_id = "k1.a2@example.com";

        n = mail_build(&d, out, sizeof out);
        out[n < sizeof out ? n : sizeof out - 1] = 0;
        snprintf(said, sizeof said, "a plain message was written as:\n%s", (char *)out);
        check(n == strlen(want) && memcmp(out, want, n) == 0, said);

        /* Too little room: the answer is the room it needs. */
        check(mail_build(&d, out, 10) == n, "a message given too little room did not say how much it needs");

        /* Now everything that must be encoded. */
        strcpy(to[0].name, "Tom\xc3\xa1s Ferreira");
        strcpy(to[0].address, "tomas@example.org");
        strcpy(to[1].name, "Moreau, Lena");
        strcpy(to[1].address, "lena@example.com");
        strcpy(cc[0].address, "bob@example.org");
        d.nto = 2;
        d.cc = cc;
        d.ncc = 1;
        d.subject = "Re: Caf\xc3\xa9 on S\xc3\xa1" "bado, and a subject long enough that its encoded words "
                    "must go over more than one line \xe2\x80\x93 done";
        d.zone = -180;
        d.in_reply_to = "orig.1@example.org";
        d.references = "<first.0@example.org> <orig.1@example.org>";

        /* A body of accents, an = sign, trailing space, a line of 200, a dot. */
        strcpy(text, "Caf\xc3\xa9 = coffee   \r\n");
        for (int i = 0; i < 200; i++) strcat(text, i % 10 == 9 ? " " : "x");
        strcat(text, "\n.\nlast line with no end");
        d.text = (const uint8_t *)text;
        d.text_len = strlen(text);

        n = mail_build(&d, out, sizeof out);
        check(n < sizeof out, "the encoded message did not fit its test's room");

        {
            int longest = 0, col = 0, ascii_only = 1;

            for (size_t i = 0; i < n; i++) {
                if (out[i] >= 0x80) ascii_only = 0;
                if (out[i] == '\n') { if (col > longest) longest = col; col = 0; } else col++;
            }

            snprintf(said, sizeof said, "a written line was %d long (CR counted), or not ASCII", longest);
            check(longest <= 77 && ascii_only, said);
        }

        check(mime_parse(&msg, out, n) == 0, "a written message did not parse back");
        check(field(0, "Subject") && strcmp(field(0, "Subject"), d.subject) == 0,
              "the encoded subject did not read back as written");
        check(field(0, "Bcc") == NULL, "a written message had a Bcc");
        check(field(0, "In-Reply-To") && strcmp(field(0, "In-Reply-To"), "<orig.1@example.org>") == 0,
              "In-Reply-To did not read back");
        check(field(0, "Content-Transfer-Encoding")
              && strcmp(field(0, "Content-Transfer-Encoding"), "quoted-printable") == 0,
              "a body of accents was not sent quoted-printable");

        {
            struct mime_address a[8];
            size_t k = mime_addresses(field(0, "To"), a, 8);

            snprintf(said, sizeof said, "To read back as %zu: '%s' <%s>, '%s' <%s>", k,
                     a[0].name, a[0].address, a[1].name, a[1].address);
            check(k == 2 && strcmp(a[0].name, "Tom\xc3\xa1s Ferreira") == 0
                  && strcmp(a[0].address, "tomas@example.org") == 0
                  && strcmp(a[1].name, "Moreau, Lena") == 0, said);
        }

        {
            int ok = 0;
            int64_t t = mime_date(field(0, "Date"), &ok);

            snprintf(said, sizeof said, "the written date %s read back as %lld", field(0, "Date"), (long long)t);
            check(ok && t == 1791272467LL, said);
        }

        {
            size_t k;
            const char *body = content(0, &k);
            char expect[8 * 1024];

            strcpy(expect, "Caf\xc3\xa9 = coffee   \r\n");
            for (int i = 0; i < 200; i++) strcat(expect, i % 10 == 9 ? " " : "x");
            strcat(expect, "\r\n.\r\nlast line with no end\r\n");
            snprintf(said, sizeof said, "the body read back as [%.300s]", body);
            check(k == strlen(expect) && memcmp(body, expect, k) == 0, said);
        }

        /* With files (M7): multipart/mixed, each file whole again, its
         * name read back, the text kept - and a line of the text that
         * looks like the boundary does not end a part. */
        {
            static uint8_t pic[5000];
            struct mail_attachment att[2];
            const char *t2 = "Here they are.\n--=_kosmos_ is not a boundary here\n";
            size_t k2;
            const char *body2;
            long jp = -1, tp = -1;

            for (size_t i = 0; i < sizeof pic; i++) pic[i] = (uint8_t)((i * 37 + 11) & 0xff);

            att[0].name = "fotos de S\xc3\xa1" "bado.jpg";
            att[0].type = "image/jpeg";
            att[0].bytes = pic;
            att[0].len = sizeof pic;
            att[1].name = "notes.txt";
            att[1].type = "text/plain";
            att[1].bytes = (const uint8_t *)"a short note\r\n";
            att[1].len = 14;
            d.att = att;
            d.natt = 2;
            d.text = (const uint8_t *)t2;
            d.text_len = strlen(t2);

            n = mail_build(&d, out, sizeof out);
            check(n < sizeof out && mime_parse(&msg, out, n) == 0, "a message with files did not parse back");
            snprintf(said, sizeof said, "a message with two files parsed as %zu parts", msg.nparts);
            check(msg.nparts == 4 && msg.part[0].multipart, said);
            jp = find("image/jpeg");
            tp = find("text/plain");
            check(jp > 0 && strcmp(msg.part[jp].name, "fotos de S\xc3\xa1" "bado.jpg") == 0
                  && strcmp(msg.part[jp].disposition, "attachment") == 0,
                  "the picture's name or disposition did not read back");
            body2 = jp > 0 ? content((size_t)jp, &k2) : "";
            check(jp > 0 && k2 == sizeof pic && memcmp(body2, pic, sizeof pic) == 0,
                  "the picture's bytes did not come back whole");
            body2 = tp > 0 ? content((size_t)tp, &k2) : "";
            check(tp > 0 && strstr(body2, "--=_kosmos_ is not a boundary here") != NULL,
                  "the text with a boundary-like line did not come back");
            d.att = NULL;
            d.natt = 0;
        }

        {
            char w[256];
            size_t k = mail_encode_words("plain words", w, sizeof w);

            check(k == 11 && memcmp(w, "plain words", 11) == 0, "ASCII words were encoded");
            k = mail_encode_words("\xe2\x82\xac", w, sizeof w);
            check(k == 16 && memcmp(w, "=?UTF-8?B?4oKs?=", 16) == 0, "the euro sign was not one encoded word");
        }
    }

    if (fails == 0) {
        printf("PASS: %d checks on the Mail Kit's reading and writing (folded fields, encoded words B and Q "
               "in two charsets, addresses with a quoted comma and a group of three, a date and "
               "its zone, a mixed holding an alternative and an attachment, quoted-printable in "
               "Windows-1252 with a soft break, base64 over lines, an RFC 2231 name, previews of "
               "text and of HTML, a message cut short, nesting held to its limit, characters; a plain message "
               "written to the byte, and one of encoded names, subject and body read back whole; "
               "one with two files, a name not ASCII, read back byte for byte)\n",
               checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on the Mail Kit's reading and writing\n", fails, checks + fails);
    return 1;
}
