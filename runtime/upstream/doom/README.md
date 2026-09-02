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
