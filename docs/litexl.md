# Porting Lite XL

**A real editor, and the reason it is worth the trouble.** Kosmos has `edit`,
which is enough to write a program into a file and run it, and it is not an
editor anybody would choose. Lite XL is one people use daily, and it is
already most of the shape this system is: nineteen thousand lines of Lua
sitting on a small C core.

`runtime/upstream/lite-xl/README.kosmos.md` records the vendoring and what
the build leaves out. This is the plan and where it has got to.

---

## Why it fits

Three facts, and the third is the one that decided it.

**Lua 5.4.** Exactly the interpreter this system carries. The editor itself -
buffers, views, the command palette, the plugin system, syntax highlighting -
is Lua and does not need porting at all.

**The C is small and mostly not SDL.** Ten thousand lines, of which about two
thousand call no SDL function. `api/utf8.c` and `arena_allocator.c` compile
against this toolchain *unmodified*, today, and are in the image behind
`LITEXL=1` so that it stays true.

**It draws into a pixel buffer and pushes damage rectangles.** With
`LITE_USE_SDL_RENDERER` undefined - upstream's default - the GPU path
compiles out of `renwindow.c` and what remains is `SDL_GetWindowSurface` and
`SDL_UpdateWindowSurfaceRects`. That is the window manager protocol Kosmos
already has, under another name. A port that had to bring a GPU pipeline
would not be starting.

## The size of it, measured rather than guessed

| file | lines | SDL functions | what happens to it |
|---|---:|---:|---|
| `api/system.c` | 1180 | 41 | the shim's bulk: events, clipboard, time, cursor |
| `renderer.c` | 739 | 14 | FreeType becomes `stb_truetype` |
| `main.c` | 266 | 12 | entry and the event loop |
| `renwindow.c` | 142 | 17 | a shared surface and damage |
| `rencache.c`, `api/renderer.c` | 743 | 0 | compile once the SDL *types* exist |
| `api/utf8.c`, `arena_allocator.c` | 1364 | 0 | **compile today, unmodified** |
| `api/process.c` | 847 | 12 | dropped |
| `api/dirmonitor/` | ~560 | 13 | dropped, `dummy.c` is upstream's own answer |
| `api/regex.c` | 385 | 0 | stubbed; wants PCRE2 |

Ninety-one SDL functions in total, and the fifteen or so behind
`LITE_USE_SDL_RENDERER` are not among the ones that matter.

**No threading shim is needed.** `SDL_CreateThread`, `SDL_CreateMutex` and
`SDL_CreateCond` appear only in `process.c` and `dirmonitor.c`, both dropped -
which is as well, because a Kosmos process has one thread and no syscall to
make a second.

## Where it has got to

- [x] **Step 1. Vendored, and the free files build.** `v2.1.7` in
      `runtime/upstream/lite-xl/`, byte for byte, checked with `diff -r`.
      `make LITEXL=1` puts `api/utf8.c` and `arena_allocator.c` into the
      image, which links and boots.
- [x] **Step 2. `SDL.h`.** `user/lib/litexl/SDL.h` and
      `user/lib/litexl_sdl.c`: the types, and the software surface -
      `SDL_Surface` over ordinary memory, `FillRect`, `BlitScaled`,
      `MapRGB`/`MapRGBA`/`GetRGBA`, clip rects, `IntersectRect`.
      `rencache.c` and `api/renderer.c` fell out of it as predicted, both
      unmodified. **Five translation units compile; `make litexl` says so
      every time.**
- [ ] **Step 3. The window.** `renwin_get_surface` returns a Kosmos shared
      surface; `renwin_update_rects` becomes the damage the compositor
      already takes.
- [ ] **Step 4. The renderer.** `renderer.c`'s 48 FreeType calls become
      `stb_truetype`, which `user/lib/docfont.c` already drives with a
      glyph cache on the PDF path. This is the step with real work in it
      and the least new risk.
- [ ] **Step 5. `system.c`.** Events from the window manager, the clipboard,
      the clock, the cursor. The window operations Kosmos has no concept of
      - opacity, hit-test, an icon - are stubbed and say so, rather than
      pretending.
- [ ] **Step 6. `main.c`, and then the Lua.** Which is the point: if the
      first five are right, the nineteen thousand lines run.

## What step two turned out to be

**`-Iuser/lib/litexl` is the whole mechanism.** Upstream says
`#include <SDL.h>` in three headers and every source file reaches SDL
through them, so putting a directory with that name on the include path
turns "port the editor" into "write these functions" - and the vendored
tree stays byte for byte what upstream released.

Three things worth recording, because none was in the plan:

**The shim has to include the C headers.** `rencache.c` calls `realloc` and
`rand` without including `<stdlib.h>`, which is not sloppiness: SDL's own
`SDL_stdinc.h` promises them. A shim that left them out would make upstream
look broken and the fix would have to be a patch to a vendored file - which
is the thing this arrangement exists to avoid.

**`rand` and `srand` were missing from the libc**, and that is where they
went rather than into the shim. They are C, not SDL; the first caller
reached them through `<SDL.h>` the way every SDL program does, and the next
will reach them through `<stdlib.h>` like everybody else. The generator is
the one printed in the C standard, and `runtime/libc/misc.c` says plainly
that it is not for anything that must not be guessed.

**`SDL_BlitScaled` has exactly one caller and one shape.** `ren_draw_rect`
makes a one-pixel surface, writes a colour into it and stretches it over a
rectangle - which is how Lite XL fills with alpha. Nearest-neighbour is not
an approximation for that, it is exact, and the alpha has to be honoured or
every translucent overlay turns opaque.

The build now carries two lists. `LITEXL_SRCS` goes into the image and
links; `LITEXL_STAGED` compiles and does not, because `rencache.c` and
`api/renderer.c` call the `ren_*` and `renwin_*` functions steps three and
four will write. `make litexl` compiles both and reports where the edge is,
so the compiler says which files are done rather than a checklist saying so
once.

## Two things deliberately given up

**Subprocesses.** `api/process.c` wants `fork`/`exec` with pipes, and
`CLAUDE.md` forbids a POSIX personality outright. Plugins that shell out -
language servers, formatters - will not work. The editor does not need them
to be an editor.

**Regex.** `api/regex.c` wants PCRE2, a second large dependency, and is
stubbed for now. `tokenizer.lua` accepts either a Lua pattern *or* a regex,
and four of the nine bundled languages use the regex form - so the cost is
syntax highlighting on those four and nothing else. Worth revisiting once
there is an editor to revisit it in.
