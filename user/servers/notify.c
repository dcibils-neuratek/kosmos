/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Notifications: what applications have said, kept in order.
 *
 * A ledger (`notifyproto.h` is the agreement, `roadmap.md` *Notifications*
 * the reasoning). An application posts a title, a line and what a press
 * should open; this keeps it under the next number with the time it came
 * and **who sent it, as the kernel says** - `SYS_SENDER`, the process the
 * message came from and the file it runs - so nothing in the post decides
 * whose it is. Whatever shows them asks for what came after the last number
 * it saw, and applies the person's rules itself.
 *
 * **The history grows as it is used**, a chunk of pages at a time, up to a
 * ceiling the machine decides: a 4096th of its memory, which is about 3,400
 * notifications on the M700's 8 GB and 200 on a 512 MB QEMU guest. Past it
 * the oldest goes, and so it does past what the person asked to keep
 * (`NOTIFY_OP_KEEP`). Nothing is compiled in but the shape of one.
 *
 * **Numbers are never reused**, so a reader that remembers "the last I
 * showed was 41" is never shown 41 again after a clear, and one that asks
 * about a number that has gone is told nothing rather than something else.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "notifyproto.h"
#include "init/say.h"

#define CHUNK_PAGES   16u                   /* 64 KB: about a hundred */
#define CEILING_MIN   64u

static long console = -1;

static struct notify_entry **chunks;        /* mapped, `max_chunks` long */
static unsigned per_chunk;                  /* entries a chunk holds */
static unsigned max_chunks;
static unsigned capacity;                   /* the most ever kept */
static unsigned keep;                       /* the person's; 0 is capacity */

static uint32_t first_id = 1;               /* the oldest that may be kept */
static uint32_t next_id = 1;                /* the one the next post gets */
static uint32_t held;                       /* kept, not removed */

static unsigned limit(void)
{
    return (keep > 0 && keep < capacity) ? keep : capacity;
}

/* Where `id` lives, mapping its chunk first if `make`; NULL if not there. */
static struct notify_entry *slot(uint32_t id, bool make)
{
    unsigned at = (unsigned)((id - 1u) % capacity);
    unsigned c = at / per_chunk;

    if (chunks[c] == NULL) {
        long got;

        if (!make) {
            return NULL;
        }

        got = kosmos_map(CHUNK_PAGES);

        if (got < 0) {
            return NULL;
        }

        chunks[c] = (struct notify_entry *)(uintptr_t)got;
    }

    return &chunks[c][at % per_chunk];
}

/* `id` as it is kept, or NULL when it never was, has gone or was removed. */
static struct notify_entry *kept(uint32_t id)
{
    struct notify_entry *e;

    if (id < first_id || id >= next_id) {
        return NULL;
    }

    e = slot(id, false);
    return (e != NULL && e->id == id) ? e : NULL;
}

/* The oldest goes while there are more than the limit. */
static void trim(unsigned room_for)
{
    while (next_id - first_id + room_for > limit()) {
        if (kept(first_id) != NULL) {
            held--;
        }

        first_id++;
    }
}

/*
 * A text from the caller, ended and printable: cut at `max - 1` bytes or
 * its own end, and a control character made a space - a line feed in a
 * title is a banner two lines tall that nobody drew. Bytes from 0x80 up
 * are kept, which is UTF-8: a name with an accent stays one.
 */
static void take_text(char *dst, const char *src, size_t max)
{
    size_t i;

    for (i = 0; i + 1 < max && src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c == 0x7f) ? ' ' : (char)c;
    }

    dst[i] = '\0';
}

static void post(const struct notify_request *req, struct notify_reply *rep)
{
    struct sender_info who;
    struct notify_entry *e;
    struct say_line line;

    if (req->title[0] == '\0') {
        rep->error = NOTIFY_ERR_NO_TITLE;
        return;
    }

    /* Asked before anything else is done with the post: the message just
     * received is the one this describes. */
    if (kosmos_sender(&who) != 0) {
        rep->error = NOTIFY_ERR_NO_SENDER;
        return;
    }

    e = slot(next_id, true);

    if (e == NULL) {
        rep->error = NOTIFY_ERR_NO_MEMORY;
        return;
    }

    trim(1);

    memset(e, 0, sizeof(*e));
    e->id = next_id;
    e->flags = req->flags & NOTIFY_ALERT;
    e->at_counter = (uint64_t)kosmos_ticks();
    e->sender = who.id;
    take_text(e->name, who.name, sizeof(e->name));
    take_text(e->from, who.from, sizeof(e->from));
    take_text(e->title, req->title, sizeof(e->title));
    take_text(e->body, req->body, sizeof(e->body));
    take_text(e->open, req->open, sizeof(e->open));

    next_id++;
    held++;
    rep->entry.id = e->id;

    /* Said, so a test can wait for it and a person can read what was said
     * when the banner has gone. */
    say_begin(&line);
    say_text(&line, "notify: ");
    say_dec(&line, e->id);
    say_text(&line, " from ");
    say_text(&line, e->from[0] != '\0' ? e->from : e->name);
    say_text(&line, (e->flags & NOTIFY_ALERT) ? ", an alert: " : ": ");
    say_text(&line, e->title);
    say_send(console, &line);
}

static void answer(const struct message *in, uint64_t sender)
{
    struct message out;
    struct notify_reply *rep = (struct notify_reply *)(void *)out.data;
    struct notify_request req;
    uint32_t id;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(req)) {
        rep->error = NOTIFY_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    memcpy(&req, in->data, sizeof(req));

    switch (req.op) {
    case NOTIFY_OP_POST:
        post(&req, rep);
        break;

    case NOTIFY_OP_NEXT: {
        /* Counted in 64 bits, so "after the largest number there is" is
         * nothing rather than a wrap back to the first. */
        uint64_t from = (uint64_t)req.id + 1u;

        for (id = (from > first_id) ? (uint32_t)((from < next_id) ? from : next_id)
                                    : first_id;
             id < next_id; id++) {
            const struct notify_entry *e = kept(id);

            if (e != NULL) {
                rep->entry = *e;
                rep->count = 1;
                break;
            }
        }

        break;
    }

    case NOTIFY_OP_REMOVE: {
        struct notify_entry *e = kept(req.id);

        if (e != NULL) {
            e->id = 0;
            held--;
        }

        /* Not an error when it was not there: closing a banner the history
         * cleared a moment before is the same wish, already granted. */
        break;
    }

    case NOTIFY_OP_CLEAR:
        first_id = next_id;
        held = 0;
        break;

    case NOTIFY_OP_KEEP:
        keep = req.count;
        trim(0);
        break;

    default:
        rep->error = NOTIFY_ERR_BAD_OP;
        break;
    }

    rep->newest = next_id - 1u;
    rep->held = held;
    (void)kosmos_reply(sender, &out);
}

void notify_server(long endpoint, long console_cap)
{
    struct sysinfo info;
    unsigned long table_pages;
    long at;

    console = console_cap;
    memset(&info, 0, sizeof(info));
    (void)kosmos_sysinfo(&info);

    /* A 4096th of the machine: its pages, in bytes, over one entry. */
    capacity = (unsigned)(info.pages_total / sizeof(struct notify_entry));

    if (capacity < CEILING_MIN) {
        capacity = CEILING_MIN;
    }

    per_chunk = (unsigned)(CHUNK_PAGES * 4096u / sizeof(struct notify_entry));
    max_chunks = (capacity + per_chunk - 1u) / per_chunk;
    table_pages = (max_chunks * sizeof(*chunks) + 4095u) / 4096u;
    at = kosmos_map(table_pages);

    if (at < 0) {
        say(console, "notify: no memory for its history\n");
        return;
    }

    /* Zeroed by `kosmos_map`: every chunk unmapped until it is needed. */
    chunks = (struct notify_entry **)(uintptr_t)at;

    {
        struct say_line line;

        say_begin(&line);
        say_text(&line, "notify: serving /Notifications, keeping up to ");
        say_dec(&line, capacity);
        say_send(console, &line);
    }

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
