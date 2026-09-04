/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /app: which running application answers to which name.
 *
 * A registry rather than a filesystem. An application publishes its own
 * endpoint here under a name, and anything that wants to script it asks for
 * that name and is handed the endpoint itself - so `beos.md` 17.2's
 * scripting works through the same namespace everything else does, and
 * without a global table of running programs that anybody could read.
 *
 * **The capability is the point and it is not in the protocol.** `register`
 * arrives with an endpoint beside its bytes and `lookup` replies with one;
 * neither is marshalled, because the kernel translates an index between two
 * processes and a number copied into a struct would mean nothing on the
 * other side. `appproto.h` is small for that reason.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "appproto.h"

struct entry {
    bool  in_use;
    long  cap;
    char  name[APP_NAME_MAX];
};

static struct entry apps[APP_MAX];

/* Registration order, so a listing is the order things started rather than
 * whatever the table happened to hold. Sorted by the caller if it wants
 * alphabetical; this keeps the fact it has. */
static unsigned registered;

static struct entry *find(const char *name)
{
    unsigned i;

    for (i = 0; i < APP_MAX; i++) {
        if (apps[i].in_use && strcmp(apps[i].name, name) == 0) {
            return &apps[i];
        }
    }

    return NULL;
}

static void copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);

    memcpy(dst, src, (n < APP_NAME_MAX) ? n : APP_NAME_MAX - 1);
    dst[(n < APP_NAME_MAX) ? n : APP_NAME_MAX - 1] = '\0';
}

/*
 * A second application of the same name gets a number rather than replacing
 * the first: two clocks are two clocks.
 *
 * The suffix is appended in place with the terminator recomputed, because
 * there is no `snprintf` worth pulling in for one digit and a name that
 * filled the array must still end.
 */
static void unique(char *name)
{
    unsigned n = 2;
    size_t at = strlen(name);

    if (find(name) == NULL) {
        return;
    }

    if (at > APP_NAME_MAX - 3) {
        at = APP_NAME_MAX - 3;
    }

    while (n < 100) {
        if (n < 10) {
            name[at] = (char)('0' + n);
            name[at + 1] = '\0';
        } else {
            name[at] = (char)('0' + n / 10);
            name[at + 1] = (char)('0' + n % 10);
            name[at + 2] = '\0';
        }

        if (find(name) == NULL) {
            return;
        }

        n++;
    }
}

static void answer(const struct message *in, uint64_t sender, long cap)
{
    struct message out;
    struct app_reply *rep = (struct app_reply *)(void *)out.data;
    const struct app_request *req =
        (const struct app_request *)(const void *)in->data;
    char name[APP_NAME_MAX];
    struct entry *e;
    unsigned i;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(*req)) {
        rep->error = APP_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    memcpy(name, req->name, APP_NAME_MAX);
    name[APP_NAME_MAX - 1] = '\0';

    switch (req->op) {
    case APP_OP_REGISTER:
        if (cap < 0) {
            rep->error = APP_ERR_NO_CAP;
            break;
        }

        for (i = 0; i < APP_MAX && apps[i].in_use; i++) {
            /* looking for a free slot */
        }

        if (i == APP_MAX) {
            rep->error = APP_ERR_FULL;
            break;
        }

        if (name[0] == '\0') {
            copy_name(name, "?");
        }

        unique(name);

        apps[i].in_use = true;
        apps[i].cap = cap;
        copy_name(apps[i].name, name);
        registered++;

        copy_name(rep->name, name);
        break;

    case APP_OP_LOOKUP:
        e = find(name);

        if (e == NULL) {
            rep->error = APP_ERR_NO_APP;
            break;
        }

        /* The endpoint itself, for the caller to hold. Beside the bytes, not
         * in them - see the note at the top. */
        out.cap_plus_one = (uint32_t)(e->cap + 1);
        copy_name(rep->name, e->name);
        break;

    case APP_OP_LIST:
        for (i = 0; i < APP_MAX && rep->count < APP_MAX; i++) {
            if (apps[i].in_use) {
                copy_name(rep->names[rep->count++], apps[i].name);
            }
        }

        break;

    case APP_OP_UNREGISTER:
        e = find(name);

        if (e != NULL) {
            (void)kosmos_endpoint_destroy(e->cap);
            memset(e, 0, sizeof(*e));
        }

        /* Not an error when it was not there. Unregistering something that
         * has already gone is what a tidy shutdown looks like from the other
         * side of a race, and refusing it would make every caller check
         * first for no benefit. */
        break;

    default:
        rep->error = APP_ERR_BAD_OP;
        break;
    }

    (void)kosmos_reply(sender, &out);
}

void appfs_server(long endpoint)
{
    memset(apps, 0, sizeof(apps));

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        /* Blocking with no deadline: nothing here happens on its own. */
        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender,
               (msg.cap_plus_one > 0) ? (long)msg.cap_plus_one - 1 : -1);
    }
}
