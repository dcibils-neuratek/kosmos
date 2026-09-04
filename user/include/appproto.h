/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_APPPROTO_H
#define KOSMOS_APPPROTO_H

#include <stdint.h>

/*
 * What you may ask the /app registry, written down.
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

#define APP_NAME_MAX      24u
#define APP_MAX           24u   /* applications registered at once */

struct app_request {
    uint32_t op;
    char     name[APP_NAME_MAX];
};

struct app_reply {
    uint32_t error;
    uint32_t count;                         /* names filled, for list */
    char     name[APP_NAME_MAX];            /* what register settled on */
    char     names[APP_MAX][APP_NAME_MAX];
};

_Static_assert(sizeof(struct app_reply) <= 2048,
               "an /app reply must fit in one message - lower APP_MAX");

#endif /* KOSMOS_APPPROTO_H */
