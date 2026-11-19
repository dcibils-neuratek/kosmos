/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_APPPROTO_H
#define KOSMOS_APPPROTO_H

#include <stdint.h>

/*
 * What you may ask the /Running registry, written down.
 *
 * The fourth protocol here to be a definition rather than a convention, and
 * the first where a *capability* travels in both directions: `register`
 * arrives with an endpoint and `lookup` answers with one. That needs no room
 * in these structs at all - a message carries a capability beside its bytes,
 * in `cap_plus_one`, precisely because a capability is not data and must not
 * be marshalled as any. It is the kernel that translates the index, and a
 * number copied into a struct would be a number meaning nothing on the other
 * side.
 *
 * Which is worth stating plainly: **this protocol is small because the
 * interesting thing it moves is not in it.**
 */

#define APP_OP_REGISTER    1u
#define APP_OP_LOOKUP      2u
#define APP_OP_LIST        3u
#define APP_OP_UNREGISTER  4u

#define APP_OK             0u
#define APP_ERR_NO_CAP     1u   /* register arrived without an endpoint */
#define APP_ERR_NO_APP     2u
#define APP_ERR_FULL       3u
#define APP_ERR_BAD_OP     4u
#define APP_ERR_NOT_YOURS  5u   /* unregister by a process that did not register it */

#define APP_NAME_MAX      24u

/*
 * **How many names one `list` reply carries - a page, not a ceiling.**
 * This was `APP_MAX`, and it was both: the registry's table was 24 entries
 * and a twenty-fifth application was refused "too many applications
 * registered", against *no compiled-in limits* (`cleanup.md` finding 206).
 * The table grows now, and a list longer than a page is asked for again
 * `from` where the last reply stopped, until a reply says no `more` - as
 * the window manager's list of windows is.
 */
#define APP_PAGE          24u

struct app_request {
    uint32_t op;
    char     name[APP_NAME_MAX];
    uint32_t from;                          /* list: the names to skip */
};

struct app_reply {
    uint32_t error;
    uint32_t count;                         /* names filled, for list */
    uint32_t more;                          /* list: 1 when names are left */
    char     name[APP_NAME_MAX];            /* what register settled on */
    char     names[APP_PAGE][APP_NAME_MAX];
};

_Static_assert(sizeof(struct app_request) == 32,
               "init.lua packs the /Running request as <I4c24I4");
_Static_assert(sizeof(struct app_reply) <= 2048,
               "a /Running reply must fit in one message - lower APP_PAGE");

#endif /* KOSMOS_APPPROTO_H */
