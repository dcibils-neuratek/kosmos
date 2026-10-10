/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The window server (`docs/astra-display.md` D3b-2): the process that will
 * hold the screen and compose the desktop, in C.
 *
 * **Started by init at boot**, as every C server is (Diego, 10 October:
 * "Let's go with option a"), with the screen and the console's endpoint, and
 * it outlives a window manager that ends or crashes. The window manager - and
 * only the system's, which init lends this door to - describes the desktop;
 * this composes it with the Graphics Kit's own code (`draw.c`,
 * `scene_core.c`) and decides nothing a person would choose.
 *
 * Built in steps, the desktop working after each. This is the first: the
 * door, and a greeting that says the screen's size.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "windowproto.h"
#include "init/say.h"

static long console = -1;
static bool have_screen;
static struct screen_info screen;

static void answer(const struct message *in, uint64_t sender)
{
    struct message out;
    struct ws_reply *rep = (struct ws_reply *)(void *)out.data;
    struct ws_request req;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length < sizeof(req)) {
        rep->error = WS_ERR_BAD_OP;
        (void)kosmos_reply(sender, &out);
        return;
    }

    memcpy(&req, in->data, sizeof(req));

    switch (req.op) {
    case WS_OP_HELLO:
        if (!have_screen) {
            rep->error = WS_ERR_NO_SCREEN;
            break;
        }

        rep->width = (uint32_t)screen.width;
        rep->height = (uint32_t)screen.height;
        break;

    default:
        rep->error = WS_ERR_BAD_OP;
        break;
    }

    (void)kosmos_reply(sender, &out);
}

void windowserver_server(long endpoint, long console_cap)
{
    console = console_cap;
    memset(&screen, 0, sizeof(screen));
    have_screen = kosmos_screen(&screen) >= 0;

    {
        struct say_line line;

        say_begin(&line);

        if (have_screen) {
            say_text(&line, "windowserver: the screen, ");
            say_dec(&line, (unsigned long)screen.width);
            say_text(&line, "x");
            say_dec(&line, (unsigned long)screen.height);
        } else {
            say_text(&line, "windowserver: no screen on this machine");
        }

        say_send(console, &line);
    }

    for (;;) {
        struct message msg;
        uint64_t sender = 0;

        if (kosmos_receive(endpoint, &msg, &sender, 0, 0) != 0) {
            return;
        }

        answer(&msg, sender);
    }
}
