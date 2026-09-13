/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_SAY_H
#define KOSMOS_SERVERS_SAY_H

/*
 * A line to the console server, for a driver that holds the console's
 * endpoint and not the console.
 *
 * A driver may not own the console, because `owns_console` also hands out
 * every key and pointer event on the machine; so it reports the way any
 * other process does, as a client sending a write. This was two functions in
 * `powerbutton.c` and moved here when the xHCI driver became the second
 * process to need them: two copies of a request builder are two places a
 * change to `conproto.h` has to reach.
 *
 * A line is built in a fixed buffer - a server has no formatted-print
 * library - and sent in one request, so it cannot arrive in pieces with
 * another process's output between them. Anything past the buffer is cut.
 */

#include <stddef.h>

#define SAY_LINE_MAX  160

struct say_line {
    char   text[SAY_LINE_MAX];
    size_t at;
};

void say_begin(struct say_line *line);
void say_text(struct say_line *line, const char *s);
void say_dec(struct say_line *line, unsigned long n);
void say_hex(struct say_line *line, unsigned long n, unsigned digits);

/* Ends the line with a newline, sends it, and leaves it empty. */
void say_send(long console, struct say_line *line);

/* One string exactly as it stands, newline and all. */
void say(long console, const char *s);

#endif /* KOSMOS_SERVERS_SAY_H */
