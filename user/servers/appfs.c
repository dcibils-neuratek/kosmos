/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Running: which running application answers to which name.
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
 *
 * **A name lasts as long as the endpoint registered under it.** Nobody has
 * to unregister for that to be true, which matters: the window manager
 * stopped with Control-C does not, and a process that is killed cannot. See
 * `forget_the_dead`.
 *
 * **And only the process that registered a name may take it away.** The
 * kernel says who sent each message (`SYS_SENDER`), the registry keeps it
 * beside the name, and an `unregister` from anybody else is refused. It
 * used to destroy the endpoint under any name for any caller, so a program
 * handed `/Running` could take `wm`'s away. What is taken away is this
 * server's hold on the endpoint, never the endpoint: the application owns
 * it and destroys it itself (`ui.lua`, the window's `close`).
 *
 * **A capability that arrives and is not kept is dropped**, on every path:
 * a refused register, and one sent with a lookup, a list or an unregister.
 * Each stayed in this server's table for good before, a slot a careless or
 * hostile client could spend until the table was full.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "kosmos.h"
#include "appproto.h"

struct entry {
    bool     in_use;
    long     cap;
    uint32_t owner;     /* the process that registered it (`SYS_SENDER`) */
    char  name[APP_NAME_MAX];
};

/*
 * **The table grows**, doubling from a page's worth when a register finds
 * it full, and is never given back - the kernel's pools' shape, and the
 * same reason: a slot is claimed in bounded time and nothing fragments. Its
 * ceiling is the memory this process can have, which is the machine's; a
 * register past that is refused as full, as every register was past 24
 * (`cleanup.md` finding 206).
 */
static struct entry *apps;
static unsigned apps_made;

static bool grow(void)
{
    unsigned want = apps_made ? apps_made * 2u : APP_PAGE;
    struct entry *more = realloc(apps, (size_t)want * sizeof(*more));

    if (more == NULL) {
        return false;
    }

    memset(more + apps_made, 0, (size_t)(want - apps_made) * sizeof(*more));
    apps = more;
    apps_made = want;
    return true;
}

static struct entry *find(const char *name)
{
    unsigned i;

    for (i = 0; i < apps_made; i++) {
        /* Whatever its case, as every name (`roadmap.md` 6s). */
        if (apps[i].in_use && strcasecmp(apps[i].name, name) == 0) {
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
 * Only while the first is still there. A name whose holder has gone was
 * taken out before this runs, so a window manager started again is `wm`
 * rather than `wm2`.
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

/*
 * Every entry whose endpoint has gone, taken out before a request is
 * answered.
 *
 * This server used to keep them, and a name outlived what it named. The
 * window manager registers as `wm` and, stopped with Control-C, destroys its
 * endpoint without unregistering: the next one was filed as `wm2`, and a
 * lookup of `wm` handed out a capability that named nothing. The Tracker
 * `desktop` starts died of it - "no such path: /Running/wm", under a desktop
 * that was running.
 *
 * So the registry does not wait to be told. The kernel ends an endpoint
 * with the process that made it, and `kosmos_cap_check` asks whether each
 * capability held here still names one, without using it. Calling the
 * application to find out would make this a process any hung application
 * could stop - the same reason `lookup` hands an endpoint out rather than
 * forwarding to it.
 *
 * At the top of every request rather than on a clock, because nothing sees
 * a stale entry without asking: `register` finds the name free, `lookup`
 * and `list` find only the living, and a full table has had its dead slots
 * back before it says it is full. A holder that ends between the sweep and
 * the reply is handed out once more, and fails the way any stale capability
 * does.
 */
static void forget_the_dead(void)
{
    unsigned i;

    for (i = 0; i < apps_made; i++) {
        if (apps[i].in_use && kosmos_cap_check(apps[i].cap) != 0) {
            /* Dropped, not destroyed: the endpoint has already gone, and
             * what comes back is the slot in this server's own table. */
            (void)kosmos_cap_drop(apps[i].cap);
            memset(&apps[i], 0, sizeof(apps[i]));
        }
    }
}

static void answer(const struct message *in, uint64_t sender, long cap)
{
    struct message out;
    struct app_reply *rep = (struct app_reply *)(void *)out.data;
    const struct app_request *req =
        (const struct app_request *)(const void *)in->data;
    char name[APP_NAME_MAX];
    struct sender_info who;
    struct entry *e;
    unsigned i;
    uint32_t from;
    bool kept = false;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(*req)) {
        rep->error = APP_ERR_BAD_OP;

        if (cap >= 0) {
            (void)kosmos_cap_drop(cap);
        }

        (void)kosmos_reply(sender, &out);
        return;
    }

    memcpy(name, req->name, APP_NAME_MAX);
    name[APP_NAME_MAX - 1] = '\0';
    from = req->from;

    /* Who asked, as the kernel says: asked before anything else is done
     * with the request, since the message just received is the one this
     * describes. A sender that has already gone is nobody. */
    memset(&who, 0, sizeof(who));

    if (kosmos_sender(&who) != 0) {
        who.id = 0;
    }

    forget_the_dead();

    switch (req->op) {
    case APP_OP_REGISTER:
        if (cap < 0) {
            rep->error = APP_ERR_NO_CAP;
            break;
        }

        for (i = 0; i < apps_made && apps[i].in_use; i++) {
            /* looking for a free slot */
        }

        if (i == apps_made && !grow()) {
            rep->error = APP_ERR_FULL;
            break;
        }

        if (name[0] == '\0') {
            copy_name(name, "?");
        }

        unique(name);

        apps[i].in_use = true;
        apps[i].cap = cap;
        apps[i].owner = who.id;
        copy_name(apps[i].name, name);
        kept = true;

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

    case APP_OP_LIST: {
        uint32_t skip = from;

        for (i = 0; i < apps_made; i++) {
            if (!apps[i].in_use) {
                continue;
            }

            if (skip > 0) {
                skip--;
            } else if (rep->count < APP_PAGE) {
                copy_name(rep->names[rep->count++], apps[i].name);
            } else {
                rep->more = 1;
                break;
            }
        }

        break;
    }

    case APP_OP_UNREGISTER:
        e = find(name);

        if (e != NULL && (who.id == 0 || e->owner != who.id)) {
            rep->error = APP_ERR_NOT_YOURS;
            break;
        }

        if (e != NULL) {
            (void)kosmos_cap_drop(e->cap);
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

    if (cap >= 0 && !kept) {
        (void)kosmos_cap_drop(cap);
    }

    (void)kosmos_reply(sender, &out);
}

void appfs_server(long endpoint)
{
    (void)grow();               /* a page's worth to start; more on demand */

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
