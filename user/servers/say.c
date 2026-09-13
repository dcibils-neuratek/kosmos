/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Lines to the console server. `say.h` has why a driver talks this way.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "conproto.h"
#include "say.h"

/* Two bytes are always left: the newline `say_send` adds, and the end. */
#define ROOM  (SAY_LINE_MAX - 2)

void say_begin(struct say_line *line)
{
    line->at = 0;
    line->text[0] = '\0';
}

void say_text(struct say_line *line, const char *s)
{
    while (*s != '\0' && line->at < ROOM) {
        line->text[line->at++] = *s++;
    }

    line->text[line->at] = '\0';
}

void say_dec(struct say_line *line, unsigned long n)
{
    char digits[24];
    size_t d = 0;

    do {
        digits[d++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0 && d < sizeof(digits));

    while (d > 0 && line->at < ROOM) {
        line->text[line->at++] = digits[--d];
    }

    line->text[line->at] = '\0';
}

void say_hex(struct say_line *line, unsigned long n, unsigned digits)
{
    static const char hex[] = "0123456789abcdef";

    if (digits == 0 || digits > 16) {
        digits = 16;
    }

    while (digits > 0 && line->at < ROOM) {
        digits--;
        line->text[line->at++] = hex[(n >> (digits * 4u)) & 0xFu];
    }

    line->text[line->at] = '\0';
}

void say(long console, const char *s)
{
    struct message msg, rep;
    struct con_request *req = (struct con_request *)msg.data;
    size_t n = strlen(s);

    if (console < 0) {
        return;
    }

    if (n > CON_TEXT_MAX) {
        n = CON_TEXT_MAX;
    }

    memset(&msg, 0, sizeof(msg));
    msg.length = sizeof(*req);
    req->op = CON_OP_WRITE;
    req->length = (uint32_t)n;
    memcpy(req->text, s, n);

    (void)kosmos_call(console, &msg, &rep);
}

void say_send(long console, struct say_line *line)
{
    line->text[line->at++] = '\n';
    line->text[line->at] = '\0';

    say(console, line->text);
    say_begin(line);
}
