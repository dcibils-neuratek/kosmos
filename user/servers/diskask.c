/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * A disk server, asked from C (`diskask.h`).
 */

#include <string.h>

#include "diskask.h"

uint32_t diskask(struct diskask *d, uint32_t op, const char *path, uint32_t flags,
                 uint64_t offset, uint64_t bytes, long region, const void *u,
                 uint32_t length)
{
    struct disk_request *rq = (struct disk_request *)(void *)d->msg.data;
    const struct disk_reply *rp = (const struct disk_reply *)(const void *)d->rep.data;
    size_t n = path != NULL ? strlen(path) : 0;

    if (d->ep < 0) {
        return DISK_ERR_NO_DISK;
    }

    if (n >= sizeof rq->path || length > sizeof rq->u.data) {
        return DISK_ERR_BAD_OP;
    }

    memset(&d->msg, 0, sizeof d->msg);
    memset(&d->rep, 0, sizeof d->rep);
    d->msg.length = sizeof *rq;
    d->msg.cap_plus_one = region >= 0 ? (uint32_t)region + 1u : 0u;
    rq->op = op;
    rq->flags = flags;
    rq->offset = offset;
    rq->bytes = bytes;
    rq->length = length;
    memcpy(rq->path, path != NULL ? path : "", n);

    if (u != NULL && length > 0) {
        memcpy(rq->u.data, u, length);
    }

    /* What was sent is not kept: the keyring's sealed bytes travel here. */
    if (kosmos_call(d->ep, &d->msg, &d->rep) != 0 || d->rep.length < sizeof *rp) {
        memset(&d->msg, 0, sizeof d->msg);
        return DISK_ERR_NO_DISK;
    }

    memset(rq->u.data, 0, sizeof rq->u.data);
    return rp->error;
}

const struct disk_reply *diskask_said(const struct diskask *d)
{
    return (const struct disk_reply *)(const void *)d->rep.data;
}

bool diskask_region(long *cap, uint8_t **at, uint64_t bytes)
{
    long a;

    if (*cap >= 0) {
        return true;
    }

    if ((*cap = kosmos_mem_create((bytes + 4095u) / 4096u)) < 0) {
        return false;
    }

    if ((a = kosmos_mem_map(*cap)) < 0) {
        kosmos_cap_drop(*cap);
        *cap = -1;
        return false;
    }

    *at = (uint8_t *)(uintptr_t)a;
    return true;
}

void diskask_drop(long *cap)
{
    if (*cap >= 0) {
        kosmos_cap_drop(*cap);
        *cap = -1;
    }
}
