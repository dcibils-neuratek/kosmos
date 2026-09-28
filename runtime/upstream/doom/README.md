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
data, and it goes beside `doom.lua` in `/Home/Apps/Doom`: a stick built by
`make x86-usb-image` copies one from the top of `~/Kosmos/home` there.

## An application of its own, not part of the system

**Doom is installed, not built in** (`docs/elf.md` step 5, 28 September).
These files and `user/installed/Doom/doom_kosmos.c` are linked by `make apps`
into `doom.elf`, an image of Doom's own, which lives in `/Home/Apps/Doom`
beside `doom.lua` and the WAD; the system's image carries none of it, in any
build. `doom.lua` reaches the engine as `use("doom.elf")`.

**So `doom.elf` is the GPLv2 work, and the system's image is not.** Doom is
GPLv2 and Kosmos is MIT; nothing here is linked dynamically, so an image
Doom is compiled into is a combined work under the GPL - and from 28
September the only such image is Doom's own. Until then `make` built Doom
into every ordinary image (`FULL=1`), which made each of them GPLv2 and made
every process on the machine carry a megabyte of code one of them called.
`LICENSE` has the list.

There is no `DOOM=1` any more: with nothing to compile Doom into, there is no
variant to name, and the objects are built once, into the lean userland's
directory, for `doom.elf`.

## The compile flags

These files are built with `-w -Wno-error`, and nothing else in this project
is. It is 1997 C - unused parameters, missing field initialisers - and it is
thirty years old and correct; `-Wall -Wextra -Werror` was not a habit then.
Kosmos's own half of the port is still held to the usual bar. The
alternative was patching eighty files to silence warnings, which is exactly
the modification the rule about vendored code forbids.
