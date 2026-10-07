# The Window Kit in C - a design, for Diego to decide

Written 7 October 2026, after TinyCC C1-C6 (`docs/tinycc.md`, decision 2:
"C apps print first; a small Window Kit in C is its own step after C6").
Nothing here is built.

## What it is for

A C app that opens a window of its own, draws into it and answers keys and
the pointer - with no Lua written by whoever wrote the app. Today a C app's
window is a few lines of Lua around its C (the Mandelbrot template), because
every door to the window manager is Lua: `ui.lua` and `wmproto.lua` build
tables and `sys.call` sends them.

**Who else wants it** (the premise): a C game or emulator (the SNES, a
future PlayStation, TinyEMU's screen - `roadmap.md`), a C server that one day
shows a window, and Lua apps' own frame loop, if the door it uses stops
allocating (below).

## What a C app writes

```c
#include "kosmos_window.h"

int kosmos_app(void)
{
    struct kw_window *w = kw_open("Plasma", 640, 400, KW_RESIZABLE);
    struct kw_event e;

    for (;;) {
        struct kw_surface s = kw_surface(w);        /* pixels, w, h, pitch */
        draw(s.pixels, s.width, s.height, s.pitch);
        kw_commit(w, 0, 0, s.width, s.height);      /* this buffer is live */

        while (kw_poll(w, &e, 16)) {                /* wait up to 16 ms */
            if (e.type == KW_CLOSE) return 0;
            if (e.type == KW_RESIZE) break;         /* new surface next pass */
            if (e.type == KW_KEY) ...;
            if (e.type == KW_POINTER) ...;          /* x, y, buttons */
        }
    }
}
```

Five calls: `kw_open`, `kw_surface`, `kw_commit`, `kw_poll`, `kw_close`. The
pixels are the shared region the window manager already composes from - two
buffers, "buffer 2 is live" - so nothing new happens on the frame path: the
same flip, no copy. Resizing is the kit's: on a resize event it makes the
new region and sends it, as `window:take_size` does. The pitch is never
`width * 4`, and the kit says so in the struct rather than leaving it to be
guessed.

## The one real choice: what crosses to the window manager

The window manager is Lua and speaks **tables** - `wmproto.lua` says it is
the stand-in for a declared shape "where both sides are Lua". A C caller
makes that untrue.

**(a) The kit speaks tables.** It builds the same tables `ui.lua` builds, on
the process's Lua state, through the serializer that `sys.call` already
uses. Nothing in the window manager changes; the kit is small; it can be
built and tested in a step. What it keeps: tables on the frame path - a
commit and a poll each a table each way, sixty times a second, garbage in
the collector of a C app that otherwise makes none.

**(b) A declared shape, `wmproto.h`, for the frame path.** `commit` and
`poll` - and `open`, so a C app needs nothing else - as fixed structs, which
the window manager reads with `string.unpack` beside its table handlers, as
the console's did before `con.wait`. This is what *a server receives exactly
what it expects* says a boundary should be, and it is the console's lesson
again: moving `con.wait` to a struct took the desktop from 4.2 KB of garbage
a pass to 0.6, and its worst collecting pass from 5.75 ms to 1.40. Lua apps
could move their commit and poll onto the same door later and gain the
same. Costs more: a header, the window manager taught a second encoding of
three requests, and a suite for both.

**Recommended: (b)**, with (a)'s kit as its first step - the C API above
does not change between them, so a C app written against (a) is unchanged
when (b) lands, and (b) is measured (`frames`' allocation column) rather
than argued.

## Steps, each with its test

1. **W1** - `kosmos_window.h` and the kit over tables (a); a C template,
   *Plasma*, a window with an animation, keys and close, in New Project's
   C kind beside Primes. `run_tcc.py`: built, opened, a frame on the screen,
   closed by its close event.
2. **W2** - `wmproto.h`: `open`, `commit` and `poll` as structs; the window
   manager answering both encodings; the kit moved onto it. Measured: a C
   app's allocation per frame, before and after.
3. **W3** - (if W2's numbers say so) `ui.lua`'s direct windows onto the
   same door, and the desktop's `frames` measured again.

## Diego's decisions

7 October 2026: "Yes to all".

1. **(b)**, built as W1 (the kit over tables) and then W2 (`wmproto.h`, the
   frame path as structs), measured.
2. **The template is Plasma.**
3. **`kosmos_window.h`**, and the kit at **`/Kosmos/Kits/window`**.

## What was asked

1. **(a) or (b)** - tables, or a declared shape for the frame path
   (recommended: (b), built as W1 then W2).
2. **The template**: *Plasma* (an animation, keys, close) - or another.
3. **The header's name**: `kosmos_window.h`, and the kit `/Kosmos/Kits/window`.
