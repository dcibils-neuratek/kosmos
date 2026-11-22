/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Quake III's keys, characters, buttons and mouse (`docs/quake3e.md` §5),
 * in place of `sdl_input.c`.
 *
 * The window's events come from the Window Kit's `kw_poll`, and go into
 * the engine's queue (`Sys_QueEvent`) in Quake III's own words:
 *
 * - **`SE_KEY`, a transition**, from `KW_RAWKEY` - the board's keycodes,
 *   Linux's numbering, down and up - through `KEYS` below. A held key is
 *   what moving is, so it has to be the transition and not the character.
 * - **`SE_CHAR`, a character**, from `KW_KEY`, for the console and the
 *   menus' fields. Only printable ones: Backspace and Escape go as SDL's
 *   port sends them, from their transitions, so that none arrives twice.
 * - **`SE_MOUSE`, movement**, from `KW_MOTION`, which comes only while the
 *   window has the pointer (`kw_capture`). The menus' cursor and the view
 *   both turn on it, so the pointer is asked for at the game's first click
 *   and whenever it is given back and clicked again - and given back by the
 *   game itself while the console is down, where a person wants the arrow.
 *   The window manager gives it back on Super and on a focus change
 *   whatever the game wants; a `KW_CAPTURE` release says so.
 * - **The close box** is `quit`, as SDL's window's is.
 */

#include "client.h"

#include "kits/window/kosmos_window.h"

#include "q3_kosmos.h"

/* Linux's keycodes (`input-event-codes.h`) to Quake III's (`keycodes.h`):
 * a letter or a digit is its lower-case character. Zero is no key. */
static const unsigned short KEYS[128] = {
    [1]  = K_ESCAPE,
    [2]  = '1', [3] = '2', [4] = '3', [5] = '4', [6] = '5',
    [7]  = '6', [8] = '7', [9] = '8', [10] = '9', [11] = '0',
    [12] = '-', [13] = '=', [14] = K_BACKSPACE, [15] = K_TAB,
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r', [20] = 't',
    [21] = 'y', [22] = 'u', [23] = 'i', [24] = 'o', [25] = 'p',
    [26] = '[', [27] = ']', [28] = K_ENTER, [29] = K_CTRL,
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f', [34] = 'g',
    [35] = 'h', [36] = 'j', [37] = 'k', [38] = 'l', [39] = ';',
    [40] = '\'', [41] = K_CONSOLE, [42] = K_SHIFT, [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v', [48] = 'b',
    [49] = 'n', [50] = 'm', [51] = ',', [52] = '.', [53] = '/',
    [54] = K_SHIFT, [55] = K_KP_STAR, [56] = K_ALT, [57] = K_SPACE,
    [58] = K_CAPSLOCK,
    [59] = K_F1, [60] = K_F2, [61] = K_F3, [62] = K_F4, [63] = K_F5,
    [64] = K_F6, [65] = K_F7, [66] = K_F8, [67] = K_F9, [68] = K_F10,
    [69] = K_KP_NUMLOCK,
    [71] = K_KP_HOME, [72] = K_KP_UPARROW, [73] = K_KP_PGUP,
    [74] = K_KP_MINUS, [75] = K_KP_LEFTARROW, [76] = K_KP_5,
    [77] = K_KP_RIGHTARROW, [78] = K_KP_PLUS, [79] = K_KP_END,
    [80] = K_KP_DOWNARROW, [81] = K_KP_PGDN, [82] = K_KP_INS,
    [83] = K_KP_DEL,
    [87] = K_F11, [88] = K_F12,
    [96] = K_KP_ENTER, [97] = K_CTRL, [98] = K_KP_SLASH, [100] = K_ALT,
    [102] = K_HOME, [103] = K_UPARROW, [104] = K_PGUP,
    [105] = K_LEFTARROW, [106] = K_RIGHTARROW, [107] = K_END,
    [108] = K_DOWNARROW, [109] = K_PGDN, [110] = K_INS, [111] = K_DEL,
    [119] = K_PAUSE,
};

static qboolean captured;       /* the window manager says we have it */
static qboolean asked;          /* and we have asked, and not heard back */
static qboolean wanted = qtrue; /* the game wants it: not in the console */
static int      last_down;      /* the key whose character may follow */

static void want_pointer(qboolean on)
{
    if (q3_window == NULL) {
        return;
    }

    if (on && !captured && !asked) {
        asked = kw_capture(q3_window, 1) ? qtrue : qfalse;
    } else if (!on && (captured || asked)) {
        kw_capture(q3_window, 0);
        captured = asked = qfalse;
    }
}

static void key(int t, int k, qboolean down)
{
    Sys_QueEvent(t, SE_KEY, k, down, 0, NULL);

    if (!down) {
        return;
    }

    /* SDL's port's characters for the keys that are not characters, so
     * the console edits as it does everywhere else. */
    if (k == K_BACKSPACE) {
        Sys_QueEvent(t, SE_CHAR, 'h' - 'a' + 1, 0, 0, NULL);
    } else if (k == K_ESCAPE) {
        Sys_QueEvent(t, SE_CHAR, K_ESCAPE, 0, 0, NULL);
    } else if (keys[K_CTRL].down && k >= 'a' && k <= 'z') {
        Sys_QueEvent(t, SE_CHAR, k - 'a' + 1, 0, 0, NULL);
    }
}

static void take(const struct kw_event *e, int t)
{
    switch (e->type) {
    case KW_RAWKEY: {
        int k = (e->key >= 0 && e->key < 128) ? KEYS[e->key] : 0;
        qboolean down = e->action == KW_PRESS ? qtrue : qfalse;

        if (k != 0) {
            key(t, k, down);
        }

        last_down = down ? k : 0;
        break;
    }

    case KW_KEY:
        /* The console's key types no character into the console it opens,
         * and Control's letters were sent with their transitions. */
        if (last_down == K_CONSOLE || keys[K_CTRL].down) {
            break;
        }

        if (e->key >= 32 && e->key < 127) {
            Sys_QueEvent(t, SE_CHAR, e->key, 0, 0, NULL);
        }
        break;

    case KW_MOTION:
        if (captured && (e->dx != 0 || e->dy != 0)) {
            Sys_QueEvent(t, SE_MOUSE, e->dx, e->dy, 0, NULL);
        }
        break;

    case KW_CAPTURE:
        captured = e->action == KW_PRESS ? qtrue : qfalse;
        asked = qfalse;
        Com_DPrintf("the pointer %s\n", captured ? "is the game's" : "was given back");
        break;

    case KW_POINTER:
        if (e->action == KW_MOVE) {
            break;
        }

        /* A click while the pointer is not ours asks for it, and fires
         * nothing: it was a person putting the game in front. */
        if (!captured) {
            if (e->action == KW_PRESS && wanted) {
                want_pointer(qtrue);
            }
            break;
        }

        Sys_QueEvent(t, SE_KEY, e->button == KW_RIGHT ? K_MOUSE2 : K_MOUSE1,
                     e->action == KW_PRESS ? qtrue : qfalse, 0, NULL);
        break;

    case KW_WHEEL: {
        int k = e->amount > 0 ? K_MWHEELUP : K_MWHEELDOWN;

        if (e->amount != 0) {
            Sys_QueEvent(t, SE_KEY, k, qtrue, 0, NULL);
            Sys_QueEvent(t, SE_KEY, k, qfalse, 0, NULL);
        }
        break;
    }

    case KW_CLOSE:
        Cbuf_ExecuteText(EXEC_NOW, "quit Closed window\n");
        break;

    default:
        break;
    }
}

/* Every event the window has had since the last look. `Com_EventLoop`
 * calls it before it reads the queue, as `unix_main.c`'s does. */
void Sys_SendKeyEvents(void)
{
    struct kw_event e;

    if (q3_window == NULL) {
        return;
    }

    int t = Sys_Milliseconds();

    while (kw_poll(q3_window, &e, 0)) {
        take(&e, t);
    }
}

/* Once a frame, before `Com_Frame`: the pointer given back while the
 * console is down, and asked for again when it goes up - which is when a
 * person goes back to playing, so no click is needed for it. */
void IN_Frame(void)
{
    qboolean console = (Key_GetCatcher() & KEYCATCH_CONSOLE) ? qtrue : qfalse;

    if (console && wanted) {
        wanted = qfalse;
        want_pointer(qfalse);
    } else if (!console && !wanted) {
        wanted = qtrue;
        want_pointer(qtrue);
    }
}

void IN_Init(void)
{
    captured = asked = qfalse;
    wanted = qtrue;
    last_down = 0;

    /* At the start, the game is in front - it has just been opened - so
     * the pointer is asked for at once. A refusal is only that: the next
     * click asks again. */
    want_pointer(qtrue);
}

void IN_Shutdown(void)
{
    want_pointer(qfalse);
}

qboolean Key_CapsLockOn(void)
{
    return keys[K_CAPSLOCK].down;
}

char *Sys_GetClipboardData(void)
{
    return NULL;
}

void Sys_SetClipboardBitmap(const byte *bitmap, int length)
{
    (void)bitmap;
    (void)length;
}
