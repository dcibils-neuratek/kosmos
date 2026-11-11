/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The memory store, asked from C (`ramask.h`).
 */

#include <string.h>

#include "ramask.h"

uint32_t ramask(struct ramask *r, uint32_t op, const char *path, uint32_t offset,
                uint32_t length, uint32_t count, uint32_t packed, uint32_t flags,
                const struct ram_attr *attrs, const void *data, uint32_t data_length,
                long region)
{
    struct ram_request *rq = (struct ram_request *)(void *)r->msg.data;
    const struct ram_reply *rp = (const struct ram_reply *)(const void *)r->rep.data;
    size_t n = path != NULL ? strlen(path) : 0;

    r->status = 0;

    if (n >= sizeof rq->path || data_length > sizeof rq->u.data) {
        return RAM_ERR_BAD_OP;
    }

    memset(&r->msg, 0, sizeof r->msg);
    memset(&r->rep, 0, sizeof r->rep);
    r->msg.length = sizeof *rq;
    r->msg.cap_plus_one = region >= 0 ? (uint32_t)region + 1u : 0u;
    rq->op = op;
    rq->offset = offset;
    rq->length = length;
    rq->count = count;
    rq->packed = packed;
    rq->flags = flags;
    memcpy(rq->path, path != NULL ? path : "", n);

    if (attrs != NULL) {
        memcpy(rq->attrs, attrs, sizeof rq->attrs);
    }

    if (data != NULL && data_length > 0) {
        memcpy(rq->u.data, data, data_length);
    }

    r->status = kosmos_call(r->ep, &r->msg, &r->rep);

    if (r->status != 0 || r->rep.length < sizeof *rp) {
        return RAMASK_NO_ANSWER;
    }

    return rp->error;
}

const struct ram_reply *ramask_said(const struct ramask *r)
{
    return (const struct ram_reply *)(const void *)r->rep.data;
}
