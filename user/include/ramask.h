/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_RAMASK_H
#define KOSMOS_RAMASK_H

/*
 * **The memory store, asked from C** - `ramproto.h` from the client's side,
 * once, as `diskask.h` is `diskproto.h`'s (`docs/w3.md` W3d4). The Storage
 * Kit asks with it, and so does the Lua namespace, through `sys.ram_call`:
 * the request's layout is written in one place, that header.
 *
 * A `struct ramask` is one door: the endpoint, the request and the reply,
 * a few kilobytes, so a caller keeps one rather than a stack frame of them.
 */

#include <stdint.h>

#include "kosmos.h"
#include "ramproto.h"

/* Not one of the server's errors: the call itself failed, and `status`
 * says how. */
#define RAMASK_NO_ANSWER 0xffffffffu

struct ramask {
    long           ep;
    long           status;              /* the last call's: 0, or IPC's refusal */
    struct message msg, rep;
};

/*
 * `op` on `path`, with the request's numbers - `count` is the request's
 * own, terms or known paths - the RAM_ATTRS_MAX slots of `attrs` (or none), `length` bytes of `data` copied into the union when
 * `data` is not NULL, and `region` (a capability, or -1) travelling with
 * it. RAM_OK, the server's error, or RAMASK_NO_ANSWER.
 */
uint32_t ramask(struct ramask *r, uint32_t op, const char *path, uint32_t offset,
                uint32_t length, uint32_t count, uint32_t packed, uint32_t flags,
                const struct ram_attr *attrs, const void *data, uint32_t data_length,
                long region);

const struct ram_reply *ramask_said(const struct ramask *r);

#endif
