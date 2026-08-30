#ifndef KOSMOS_SERIALIZE_H
#define KOSMOS_SERIALIZE_H

/*
 * Lua values over a message.
 *
 * The same code on both sides of the boundary: the kernel's copy and every
 * process build it. That is not a convenience, it is the point. If the two
 * sides had separate implementations they would drift, and a protocol whose
 * two ends disagree is worse than one with an IDL, because at least an IDL
 * generates both from one description.
 */

struct lua_State;

#ifdef KOSMOS_USER
#include "kosmos.h"         /* struct message, as a process sees it */
#else
#include "ipc.h"            /* struct message, as the kernel sees it */
#endif

#define SERIALIZE_OK             0
#define SERIALIZE_ERR_TOO_BIG   (-1)    /* does not fit in a message */
#define SERIALIZE_ERR_DEPTH     (-2)    /* nested too deep, or a cycle */
#define SERIALIZE_ERR_TYPE      (-3)    /* a value that cannot cross */
#define SERIALIZE_ERR_MALFORMED (-4)    /* the bytes are not a value */

/* Serialises the value at `index` into `m`. The stack is unchanged. */
int serialize_pack(struct lua_State *L, int index, struct message *m);

/* Pushes the value `m` holds. On failure the stack is left as it was
 * found, rather than holding half a table. */
int serialize_unpack(struct lua_State *L, const struct message *m);

/* A sentence for an error code, for putting in a Lua error. */
const char *serialize_error(int rc);

#endif /* KOSMOS_SERIALIZE_H */
