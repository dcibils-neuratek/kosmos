/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_NOTIFYPROTO_H
#define KOSMOS_NOTIFYPROTO_H

#include <stdint.h>

/*
 * What you may ask the notification server, at `/Notifications`
 * (`roadmap.md`, *Notifications*; the drawing is `docs/notifications.html`).
 *
 * A ledger. An application posts - a render finished, a stick arrived - and
 * the server keeps it, numbered, with **who sent it said by the kernel**
 * rather than by the post: `SYS_SENDER` names the process the message came
 * from, and the file it runs is said once, before its first line, so a
 * program cannot post as another. Whatever shows notifications - the
 * Deskbar's banners and its history - asks what came after the last number
 * it saw, one at a time, and applies the person's rules (Do Not Disturb, an
 * application turned off) itself: the server has no namespace to read a
 * setting from, and keeps everything it is told.
 *
 * **One notification a reply**, because one is about 600 bytes and a
 * message is 2048: `next` answers the oldest kept after a number, and a
 * reader walks the history by asking again with the number it got. They
 * are rare - somebody's render, a stick - so a walk of two hundred is two
 * hundred round trips of a few microseconds, and nothing recurs on a clock.
 *
 * **`at_counter` is the counter**, `kosmos_ticks`, when the post arrived -
 * not a date, since the server has no clock to read one from. A reader
 * turns it into a time with the counter's frequency from `/Devices/cpu`, in
 * one function (`notify.lua`'s `when`), as `CLAUDE.md`'s two clocks ask of
 * every number that crosses a boundary.
 */

#define NOTIFY_OP_POST     1u   /* title, body, open, flags -> id */
#define NOTIFY_OP_NEXT     2u   /* the oldest kept with an id after `id` */
#define NOTIFY_OP_REMOVE   3u   /* one, by id */
#define NOTIFY_OP_CLEAR    4u   /* every one */
#define NOTIFY_OP_KEEP     5u   /* keep at most `count`; 0: as many as it can */

#define NOTIFY_OK              0u
#define NOTIFY_ERR_BAD_OP      1u
#define NOTIFY_ERR_NO_TITLE    2u   /* a post says something, or is refused */
#define NOTIFY_ERR_NO_SENDER   3u   /* the kernel could not say who posted */
#define NOTIFY_ERR_NO_MEMORY   4u   /* no room for the history to grow */

/* A post that stays until it is closed - a program that stopped, a timer -
 * rather than a banner that goes by itself. */
#define NOTIFY_ALERT       1u

#define NOTIFY_TITLE_MAX   64u
#define NOTIFY_BODY_MAX   256u
#define NOTIFY_OPEN_MAX   128u
#define NOTIFY_NAME_MAX    16u
#define NOTIFY_FROM_MAX   128u

struct notify_request {
    uint32_t op;
    uint32_t flags;                     /* post: NOTIFY_ALERT */
    uint32_t id;                        /* next: after it; remove: it */
    uint32_t count;                     /* keep */
    char     title[NOTIFY_TITLE_MAX];
    char     body[NOTIFY_BODY_MAX];
    char     open[NOTIFY_OPEN_MAX];     /* what a press opens: a path */
};

struct notify_entry {
    uint32_t id;                        /* from 1, never reused */
    uint32_t flags;
    uint64_t at_counter;                /* kosmos_ticks when it arrived */
    uint32_t sender;                    /* the process that posted */
    uint32_t reserved;
    char     name[NOTIFY_NAME_MAX];     /* what that process called itself */
    char     from[NOTIFY_FROM_MAX];     /* the file it runs; empty: built in */
    char     title[NOTIFY_TITLE_MAX];
    char     body[NOTIFY_BODY_MAX];
    char     open[NOTIFY_OPEN_MAX];
};

struct notify_reply {
    uint32_t error;
    uint32_t count;                     /* next: 1 with `entry`, 0 none */
    uint32_t newest;                    /* the last id given, kept or not */
    uint32_t held;                      /* how many are kept */
    struct notify_entry entry;          /* post: the id given, in entry.id */
};

_Static_assert(sizeof(struct notify_request) == 464,
               "the notify request is four words and three texts, and "
               "notify.lua packs it as \"<I4I4I4I4c64c256c128\"");
_Static_assert(sizeof(struct notify_entry) == 616,
               "a notification is notify.lua's ENTRY");
_Static_assert(sizeof(struct notify_reply) == 632,
               "the notify reply is four words and an entry");
_Static_assert(sizeof(struct notify_reply) <= 2048,
               "a notify reply must fit in one message");

#endif /* KOSMOS_NOTIFYPROTO_H */
