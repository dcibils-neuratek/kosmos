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

## Why it is a build option, and which builds turn it on

**`make` turns it on.** `FULL=1` is the default, and it builds the whole
system, Doom and the browser included, because the machine you sit in front
of should be the whole machine - the decision log has that row. `make
FULL=0` leaves Doom out, and so do `make test` and `make bench`, which build
images of their own.

**So an ordinary image is a GPLv2 work.** Doom is GPLv2 and Kosmos is MIT.
There is no dynamic linking here - `layout.md` says so and means it - so
anything compiled in is *linked* in, and a Kosmos image containing Doom is a
combined work under the GPL. That is not a problem to be solved, it is a
fact to be respected, and the line is drawn in the build rather than in a
comment because a licence boundary that depends on somebody remembering is
not a boundary. Kosmos's own sources stay MIT and are unaffected, and
`FULL=0` is the image to hand somebody who needs an MIT one.

**And the size.** The image is copied into every process - `roadmap.md`
records the cost and `procs` shows it, which is why every process reports
the same few megabytes. Doom is about a megabyte of code, paid by every
process on the machine for something one of them calls. `FULL=1` pays it
on purpose, and `FULL=0` does not.

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
