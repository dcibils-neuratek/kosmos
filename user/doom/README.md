# user/doom

Doom, from **doomgeneric** (github.com/ozkl/doomgeneric), which is Chocolate
Doom, which is id Software's 1997 source release. The 79 `.c` files named by
doomgeneric's own Makefile, plus its headers, byte for byte as released.
The platform variants it ships - SDL, Xlib, Win32, Allegro - are not here;
Kosmos's is `doomgeneric_kosmos.c`, which is ours and carries the Kosmos
header.

## The licence, which is the reason this is a build option

**Doom is GPLv2.** `LICENSE` is id's, as shipped. Kosmos is MIT.

There is no dynamic linking here - `layout.md` says so and means it - so
anything compiled in is *linked* in, and a Kosmos image containing Doom is a
combined work under the GPL. That is not a problem to be solved, it is a
fact to be respected, and it is why:

    make DOOM=1

is how you get it and why the default build does not. Kosmos's own sources
stay MIT and are unaffected; an image built with `DOOM=1` is a GPL work and
would have to be distributed under those terms, source and all. This is a
learning project on one machine, so nothing is being distributed - but the
line is drawn in the build rather than in a comment, because a licence that
depends on somebody remembering is not a licence boundary.

## The other reason it is a build option

**The image is copied into every process.** `roadmap.md` records the cost and
`procs` shows it: every process here holds its own copy, which is why they
all report the same few megabytes. Doom is about a megabyte of code, and a
desktop with eighteen processes on it would pay that eighteen times over for
something seventeen of them will never call.

## What Kosmos provides

doomgeneric asks a platform for six functions - `DG_Init`, `DG_DrawFrame`,
`DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle` - and hands
back `DG_ScreenBuffer` as 32-bit pixels, which is already the format every
surface in this system uses. There is no palette conversion and no scaler.

`DG_GetKey` wants presses *and* releases, which is what `hal_key_held`
exists for: a character stream cannot say that a key is still down, and
walking forward in a game is exactly that question.

The WAD does not come through `fopen`. Doom's file layer is replaced by one
that reads from memory, and the bytes are put there by the Lua side through
the ordinary filesystem - the same division `pdf.lua` already uses, where
Lua does the I/O and C does the loops.
