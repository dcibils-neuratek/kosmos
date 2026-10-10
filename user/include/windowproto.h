/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WINDOWPROTO_H
#define KOSMOS_WINDOWPROTO_H

#include <stdint.h>

/*
 * **The manager door**: what the window manager may ask the window server
 * (`docs/astra-display.md` D3b-2, `user/servers/windowserver.c`).
 *
 * The window server is started by init at boot and holds the screen
 * (decided by Diego, 10 October); it composes what the window manager
 * describes and decides nothing a person would choose. This door is lent
 * only to the system's window manager - `/Kosmos/Programs/wm.lua`, which
 * declares `needs windowserver` - so no other program can take the screen.
 *
 * It grows step by step, each step a version of this header: the greeting
 * first, then the scene and its pictures, then composing.
 */

#define WS_OP_HELLO        1u   /* -> the screen's size; the door works */

#define WS_OK              0u
#define WS_ERR_BAD_OP      1u
#define WS_ERR_NO_SCREEN   2u   /* the server was not given the screen */

struct ws_request {
    uint32_t op;
    uint32_t reserved;
};

struct ws_reply {
    uint32_t error;
    uint32_t width;             /* hello: the screen, in pixels */
    uint32_t height;
    uint32_t reserved;
};

_Static_assert(sizeof(struct ws_request) == 8,
               "the window server's request is two words: wm.lua packs \"<I4I4\"");
_Static_assert(sizeof(struct ws_reply) == 16,
               "the window server's reply is four words: wm.lua reads \"<I4I4I4I4\"");

#endif /* KOSMOS_WINDOWPROTO_H */
