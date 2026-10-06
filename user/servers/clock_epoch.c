/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The date, for a server in C: `/Devices/clock`'s `epoch` field, asked of
 * the devices server (`devproto.h`). Moved out of `diskfs.c` on 6 October
 * 2026, when the keyring became its second user (`docs/keyring.md`, K4).
 */

#include <string.h>

#include "kosmos.h"
#include "devproto.h"

#include "clock_epoch.h"

uint64_t clock_epoch(long devices)
{
    struct message msg, rep;
    struct dev_request *req = (struct dev_request *)msg.data;
    const struct dev_reply *r = (const struct dev_reply *)rep.data;

    if (devices < 0) {
        return 0;
    }

    memset(&msg, 0, sizeof msg);
    msg.length = sizeof *req;
    req->op = DEV_OP_READ;
    memcpy(req->name, "clock", sizeof "clock");

    if (kosmos_call(devices, &msg, &rep) != 0 || rep.length < sizeof *r
        || r->error != DEV_OK) {
        return 0;
    }

    for (uint32_t i = 0; i < r->count && i < DEV_FIELDS; i++) {
        if (strncmp(r->field[i].name, "epoch", DEV_NAME_MAX) == 0
            && r->field[i].kind == DEV_KIND_NUMBER) {
            return r->field[i].number;
        }
    }

    return 0;
}
