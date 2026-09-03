# doomgeneric

Vendored unmodified from https://github.com/ozkl/doomgeneric, which is
Chocolate Doom with its platform layer reduced to six functions. Licence in
`LICENSE.doomgeneric` (GPL-2.0, id Software's original release under the
terms Chocolate Doom carries) and upstream's own notes in
`README.doomgeneric.txt`.

**Nothing here is edited**, the same rule `lua/upstream/` and
`runtime/upstream/stb/` follow. What Kosmos has to supply lives outside this
directory: `DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`,
`DG_GetKey` and `DG_SetWindowTitle`, which are the whole of the port and the
whole of the interesting part.

`pixel_t` is `uint32_t` here and the framebuffer is XRGB8888, so a frame is
a blit rather than a conversion.

**The WAD is not here and will not be.** `doom1.wad` is 4 MB of shareware
data; it goes on the disk with `tools/kfs.lua put`, which is what a
filesystem is for.

## Why it is `make DOOM=1` and not part of the image

**The licence.** Doom is GPLv2 and Kosmos is MIT. There is no dynamic
linking here - `layout.md` says so and means it - so anything compiled in is
*linked* in, and a Kosmos image containing Doom is a combined work under the
GPL. That is not a problem to be solved, it is a fact to be respected, and
the line is drawn in the build rather than in a comment because a licence
boundary that depends on somebody remembering is not a boundary. Kosmos's
own sources stay MIT and are unaffected; an image built with `DOOM=1` is a
GPL work.

**And the size.** The image is copied into every process - `roadmap.md`
records the cost and `procs` shows it, which is why every process reports
the same few megabytes. Doom is about a megabyte of code, and a desktop with
eighteen processes would pay that eighteen times for something seventeen of
them will never call.

`DOOM=1` gets its own `VARIANT`, so its objects never mix with an ordinary
build's: they are compiled with different flags, and `make` compares
timestamps rather than command lines.

## The compile flags

These files are built with `-w -Wno-error`, and nothing else in this project
is. It is 1997 C - unused parameters, missing field initialisers - and it is
thirty years old and correct; `-Wall -Wextra -Werror` was not a habit then.
Kosmos's own half of the port is still held to the usual bar. The
alternative was patching eighty files to silence warnings, which is exactly
the modification the rule about vendored code forbids.
