/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * /Temporary: files, attributes and live queries, in memory - and `/Home`
 * on a machine with no disk.
 *
 * The seventh server to move and the last one that will. It is also the one
 * whose conversion cost something rather than only buying: ramfs was what
 * `ROLE_RELOAD` reloaded and what `help("demos")` let you *watch* being
 * reloaded, and with it in C there is no server left in the system whose
 * code can be replaced while it runs. That was decided rather than
 * discovered - see `docs/design.md` - and the honest description is that hot
 * reload is a feature this system had and removed, not one that lost a
 * tiebreaker.
 *
 * **This file is the server; the store is `ramstore.c`**, since 5 October
 * 2026: what a request does is there, where the host can hold it to its
 * behaviour without a machine (`tools/test_ramstore.c`), and what is here is
 * receiving, replying, and how much the store may hold.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "ramproto.h"
#include "ramstore.h"

void ram_reply(uint64_t to, const struct ram_reply *rep)
{
    struct message out;

    memset(&out, 0, sizeof(out));
    out.length = sizeof(*rep);
    memcpy(out.data, rep, sizeof(*rep));

    (void)kosmos_reply(to, &out);
}

/*
 * **Half of the machine's memory**, which is what Linux's tmpfs takes by
 * default and for the same reason: room for whatever a person keeps here -
 * a song, a document's pictures, `/Home` on a machine with no disk - and
 * the other half left for the programs that made it. The pages the kernel
 * manages, rather than the first range of RAM, since a PC's memory comes in
 * several (`hal_ram_ranges`).
 *
 * Asked once, at the start. A machine does not change its memory while it
 * runs, and a store that recomputed its ceiling as the free pages moved
 * would refuse a write at one moment that it took the moment before.
 */
static size_t half_the_machine(void)
{
    struct sysinfo info;

    memset(&info, 0, sizeof(info));

    if (kosmos_sysinfo(&info) != 0 || info.pages_total == 0) {
        return 64u * 1024u * 1024u;   /* told nothing: a modest store, not none */
    }

    return (size_t)info.pages_total * 4096u / 2u;
}

void ramfs_server(long endpoint)
{
    ram_store_init(half_the_machine());

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        /* Nothing here happens on its own: a watcher is woken by a write,
         * and a write is a message. */
        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        ram_answer(msg.data, msg.length, sender);
    }
}
