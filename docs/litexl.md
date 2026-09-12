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
- [x] **Step 3. The window.** And it turned out to need no new file at
      all: `SDL_GetWindowSurface` returns a view onto the Lua side's pixels
      and `SDL_UpdateWindowSurfaceRects` records damage, so **upstream's
      `renwindow.c` compiles and works unmodified**. Six translation units
      now, and 35 checks on the shim in `make test`.
- [x] **Step 4. The renderer.** `user/lib/litexl_render.c`, on
      `stb_truetype`. **The whole rendering half now links into a Kosmos
      image** - seven translation units, nothing waiting - and 49 checks in
      `make test` rasterise a real font and look at the pixels.
- [x] **Step 5. `system.c`**, replaced rather than shimmed - and for a
      different reason than step four. Ten translation units, all in the
      image, 58 checks in `make test`.
- [x] **Step 6. `main.c`, and then the Lua.** `core.init()` returns on
      the machine: the 78 files are in the image, `require` runs over the
      namespace, and the editor's core initialises with real fonts through
      `stb_truetype`.
- [x] **Step 7. The editor.** A window, Lite XL's own `core.run()`, keys and
      the pointer in its shape, files through `io.open`, and all 26 bundled
      plugins. `wm litexl:/home/notes.txt` opens the file, takes typing and
      saves it. `tools/test_litexl_host.lua` checks the launcher's decisions
      on this machine in `make test`, and `make litexl-check` checks the whole
      editor through the files it saved; `make prepush` runs both.

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

## Who owns the window, which decided step three

**`user/lib/doom_kosmos.c` had already answered this** and its reasoning is
the one that matters: *a port that owns its own loop is an application that
cannot be closed, which on this desktop means a window the compositor keeps
drawing for ever.* So the Lua side owns the window and the loop, and the C
side is handed a surface and asked to fill it. Doom works that way and Lite
XL does now too.

Which made step three smaller than the plan said. Rather than writing a
Kosmos `renwindow.c`, the shim implements the two functions upstream's
already calls - `SDL_GetWindowSurface` and `SDL_UpdateWindowSurfaceRects` -
and upstream's file compiles and works untouched. The surface is a *view*
onto memory the Lua side owns, made with `SDL_CreateRGBSurfaceFrom`, so
what the editor's renderer writes goes straight into the window's own
buffer with no copy anywhere.

**The editor never touches the framebuffer**, and it is worth being plain
about that because "writes into a surface" sounds like it might. Lite XL is
a `direct` window in the sense `user/bin/procs.lua` defines - it owns a
region the compositor blits from - exactly as Doom and the cubes are. The
window manager remains the only thing that touches the screen.

Damage is a fixed array of 64 rectangles with an overflow flag, not a
growing list. Past the bound the honest answer is "all of it": a compositor
handed two hundred rectangles is slower than one handed the whole window,
which is the same trade `wm.lua` makes about damage, arrived at from the
other side.

## Is any of this still SDL?

Barely, and it is worth saying so plainly rather than leaving the filename
to imply otherwise.

Nothing from SDL is linked, vendored or downloaded. `user/lib/litexl_sdl.c`
is Kosmos C: a pixel buffer with fill, blit and a clip rectangle. And Lite
XL's *hot* path never enters it - `renderer.c`'s glyph loop writes straight
into `surface->pixels` through `->pitch`, with a comment saying it avoids
`SDL_GetRGBA` because that was a measured regression. That is already what
`CLAUDE.md` asks for: a loop over bytes, in C.

**What the SDL shape buys is that the vendored tree stays `diff -r`
identical to upstream.** Removing the name would mean editing about 2,300
lines of somebody else's C to avoid 150 lines of adapter, and turning a
vendored library into a fork. The shim is also the seam: `gfx.c` keeps its
fill and blit `static` today, and if the duplication ever shows up in a
profile, exporting them and calling them from here is a local change rather
than a rewrite.

## Why step four replaced a file instead of shimming under it

**`renderer.c` does not merely call FreeType - it edits glyph outlines.**
`FT_Outline_Translate` for subpixel positioning, `FT_Outline_Embolden` for
synthetic bold, `FT_Outline_Transform` with a shear matrix for synthetic
italic. `stb_truetype` rasterises straight from the font's glyph data and
has no editable outline to hand back, so an `ft2build.h` shim of the kind
`SDL.h` is would mean *implementing a font engine* rather than adapting
one.

So the build leaves `renderer.c` out and `user/lib/litexl_render.c`
provides `renderer.h`'s interface instead. **That is still not a fork**:
the vendored tree is byte for byte what upstream released and one more file
simply is not compiled, exactly as `api/process.c` and `api/dirmonitor/`
are not.

What it costs, and it should be read rather than discovered:

- **No subpixel (LCD) antialiasing.** `FONT_ANTIALIASING_SUBPIXEL` is
  accepted and rendered grayscale. Slightly softer on an LCD, identical
  everywhere else.
- **No synthetic bold or italic**, those being the outline transforms. A
  bold face has to be a bold *file*, which is how `assets/fonts/` is
  organised anyway - IBM Plex ships four.
- **No hinting.** At the sizes an editor uses, on this framebuffer, that is
  a difference somebody would have to be told about to notice.

And a font's bytes come from the Lua side through `litexl_font_provide`,
because `ren_font_load` takes a *filename* and there is no `fopen` here: a
path means nothing without a namespace. `doom_kosmos.c` met the same wall
with the WAD and answered it the same way. Lite XL asks for a path built
out of a `DATADIR` that does not exist, so the match is on the file's name.

**The tests rasterise rather than assert.** They load a real
`IBMPlexSans-Regular.ttf`, draw into a surface and count lit pixels, check
that ten glyphs are wider than one, and check that text stops at its clip
rectangle. One of them failed on the first run and the renderer was right:
`RenColor` is `{ b, g, r, a }`, blue first, so a positional initialiser
asks for a different colour than it reads back. The check uses designated
initialisers now and says why.

## Step five, where the port met the central rule

**`api/system.c` is two things, and only one of them is SDL.** The other is
a POSIX filesystem: it includes `<unistd.h>`, `<dirent.h>` and
`<sys/stat.h>`, and calls `opendir`, `stat`, `chdir`, `realpath`, `mkdir`
and `remove`. `CLAUDE.md` names most of that list explicitly as the
personality this system will not have.

And shimming it would not merely be disallowed, it would be **impossible to
do honestly**: `stat("/foo")` has no meaning here, because there is no
global tree for the path to be in. A shim would have to invent one.

So it is replaced. What fell out is worth recording, because it was not the
shape expected:

**Almost none of `system` is computation.** Reading a directory, taking the
clipboard, moving a window, waiting for a key - every one is a conversation
with a server, and on this system a conversation is had from Lua, where the
namespace and the window manager's endpoint are. So `litexl_system.c` is a
*shape*: it registers the thirty-two names Lite XL expects and forwards
most of them to a host table the Kosmos side installs. C keeps the
interface; Lua does the talking, which is the same division
`doom_kosmos.c` arrived at one layer down.

Two functions are genuinely computation and live in `litexl_match.c`, away
from Lua so they can be tested without a machine: `fuzzy_match`, which runs
over every file in the project on every keystroke of the command palette,
and `path_before`, which orders every listing.

Three modules are registered as tables whose functions refuse with a
reason - `process`, `dirmonitor` and `regex`. Registered rather than
absent, because Lite XL's Lua does `require "process"` at the top of files
that may never use one: an empty table lets the require succeed and the
*use* fail, at the point somebody asks for something impossible, with a
sentence saying why.

## Step six, and what the Lua actually needed

**The editor's Lua loads.** `start.lua` runs, `require "core"` resolves the
whole 78-file graph, and `core.init()` returns - on stock Lua 5.4 with the
six C modules stubbed. `tools/test_litexl_lua.lua` does it in `make test`,
on the build machine, because the editor is Lua 5.4 and so is
`build/host/lua`: no window, no font and no emulator needed to find out
whether the module graph still resolves.

That test decided the shape of the C half rather than confirming it. Two
things came out of the first run and neither was in the plan:

- **`luaL_requiref(L, name, fn, 1)` sets each module as a *global***, and
  `start.lua` relies on it: it says `system.get_file_info` with no
  `require` in sight.
- **`dirmonitor` cannot refuse.** It had been written to raise, on the
  grounds that Kosmos cannot watch a directory. `core.init()` then died at
  `core/dirwatch.lua:41`, because the editor makes a monitor at startup and
  indexes it whether or not anything is ever watched. It answers the way
  upstream's own nine-line `dummy.c` does now - "single", `-1`, nothing -
  and the editor rescans instead of being told. **A module that refuses is
  only honest when nobody needs it to exist.**

And the list the host has to provide for startup, which is short and is
measured rather than guessed:

    absolute_path  chdir  get_file_info  get_time  list_dir  mkdir
    set_window_bordered  set_window_hit_test

Every one of those is a question for a server, which is why `system` is
shaped the way step five left it.

**What was left was integration, not discovery.** `data/`'s 78 Lua files had
to reach the image and be findable - Kosmos has `use()` and a namespace
where Lite XL has `require` and `package.path`, so that was a loader to
write - and the host table had to be implemented against `fs`, the window
manager and the clipboard rather than stubbed. Neither was unknown work by
then, and the next two sections are both of them done.

## Step six, on the machine

    kosmos> run /home/lxstart.lua
    kit: ok, system=true renderer=true
    font JetBrainsMono-Regular.ttf: 112172 bytes
    start.lua:    true
    require core: true
    core.init(): true

**That was the editor's constructor completing, not the editor running.**
`core.run()` had not been called, nothing was drawn, and no keystroke was
delivered - step seven, below, is where those arrived. Worth stating
plainly, because "it initialises" reads like more than it is.

Five things had to give, and each was a real limit rather than a guess:

**`require`, over `use`.** A module name becomes candidate paths and the
namespace answers - about fifteen lines. They are not the same function:
`use` takes a *path* and reads what this program was handed, `require`
takes a *name* and searches a global `package.path`. Building the second
out of the first is what keeps Lite XL's modules inside the capability
rule, rather than giving 19,000 lines of vendored Lua an ambient loader.

**`BIN_NAME_MAX` was 24**, and the longest key here is 36 -
`litexl/core/commands/findreplace.lua`. The symptom pointed elsewhere:
`string.pack` refused the field, so reading a perfectly ordinary file
failed with *"bad argument #4 to 'pack'"* from a line about packing, while
the short-named files kept working. `init.lua` now derives the format
string, its size assertion and the listing stride from one constant, which
is what stopped three copies of `24` from disagreeing.

**The stack.** 64 KB, and `core.init()` ran out of it. The symptom was the
good one - a write fault exactly at `sp`, on the guard page that exists to
make this findable - so the number could be raised in confidence rather
than guessed. `kernel/process.h` carries the arithmetic: 2.4 MB across
thirteen processes, measured, because the first version of that comment
claimed the cost was address space and it is not.

**No `os`, `debug` or `dofile`** in a Kosmos sandbox. Provided by
`litexl.lua` deliberately, so the vendored tree needs no patch, and
declared in `tools/luaglobals.py` so the check that guards against stray
globals still guards everything else.

**A kit is the door.** `main.c` is not compiled and `api_load_libs` was its
job, so nothing would have registered `system` or `renderer` in a Kosmos
process. `use("/kits/litexl")` does it, through the namespace like every
other kit.

The 78 files went into the **library** store rather than one of their own,
which is the honest place for them and cost nothing: `binfs.c` finds an
entry with `strcmp`, so a key with slashes reads straight out and no
server, role or capability had to be invented.

## Step seven, and what the editor needed

    kosmos> wm litexl:/home/notes.txt

A window with the file in a tab and the tree view beside it, and typing that
reaches the file when it is saved. The four steps this section used to list
were the right four, and two of them turned out different in the doing.

**The editor keeps its own loop.** The plan was to drive `core.run()` a frame
at a time, the way Doom is driven. It cannot be: the scheduler for Lite XL's
background work - the cursor blink, highlighting, a project scan - is a local
inside `core/init.lua`, so a caller stepping frames from outside would never
run it. So `core.run()` runs unchanged, and Kosmos does its part inside the
two calls that loop makes when it waits. `system.wait_event` and
`system.sleep` show the frame just drawn and then ask the window manager what
happened - for no longer than asked, and never for longer than a quarter of a
second. A close from the desktop arrives as Lite XL's own `quit`.

**A direct window has two buffers, and Lite XL draws only what changed.**
`commit` shows the buffer that was drawn and hands back the other, which
still holds the frame before. So the rectangles just shown are copied across,
and `swap_window` points the renderer at the other buffer's pixels without
treating it as fresh - which would have made every commit the whole window.

**The window is named for what is open.** Lite XL composes its title -
`~/notes.txt - Lite XL`, with a `*` after the name while there are unsaved
changes - and calls `system.set_window_title` only when it changes. That is
`win:retitle`, the request Photo and the browser already make, so the tab and
the Deskbar follow the document. The window manager says each rename, as
`wm: window Lite XL is now ~/notes.txt - Lite XL`, which is how
`make litexl-check` sees the title follow a file through an edit and a save.
This document used to list the title as unfinished, on the grounds that the
window manager had no way to rename a window. It had one all along; the
editor never asked.

**Keys arrive as two streams, and go to Lite XL in the order SDL uses.**
`rawkey` gives the transitions, which become `keypressed` and `keyreleased`
by SDL's names; the character stream becomes `textinput`, except with
Control or Alt held. Transitions go first, so a stroke that runs a binding
tells Lite XL to drop the text behind it. The pointer is the first button
only, with movement while it is held, and a double click is counted from how
soon and how near the next press lands.

**`io.open`, over the namespace.** Lite XL loads and saves every document
through it, and this sandbox has no `io`. A file opened for reading is its
whole text; one opened for writing is stored whole when it is closed, through
`fs.write`, which already takes a value larger than a message on every
mount. `mkdir` is a `fs.send`, and `absolute_path` and `chdir` keep the
working directory the editor believes in.

**The installed tree is worked out from keys.** The build stores `data/`
flat in `/lib`, under `litexl/...`, and Lite XL finds its plugins, colours and
languages by listing directories. A name is a file when a key is exactly that
and a directory when keys continue past it - which is enough for all 26
bundled plugins to load, the tree view among them.

**The fonts come from the image.** `provide_image_font` hands the renderer a
face compiled into the image, with no disk and no second copy. JetBrains Mono
comes out of `assets/fonts/`, the table `gfx` draws from. Lite XL's UI face
and its icon font come out of a table of their own that only a `LITEXL=1`
image carries - in `fonts_table` they would be offered by `gfx.fonts()`, and
Appearance would list `icons` as a face for the desktop.

They were stood in for at first, because `FiraSans-Regular.ttf` and
`icons.ttf` are in the vendored tree with no licence beside them. Their terms
are recorded beside them now. Fira Sans's are in Lite XL's own
`licenses/licenses.md`: SIL OFL 1.1. `icons.ttf`'s are in nothing upstream
wrote, so `LICENSE.icons` gives the evidence with the terms - the font's own
name table says Fontello made it, a Lite XL maintainer said its icons are
Font Awesome 4, whose fonts are SIL OFL 1.1, and five of its glyphs are named
for Lite XL itself.

**Control-C is not copy.** The window manager stops the desktop on it before
any window sees the key, so copy, cut, paste and select-all come the way
every application here gets them - Control plus a letter - and each becomes
the Lite XL command it names.

**The launcher's decisions are a library.** Paths, the installed tree, files,
the event queue, key names and the damage rectangle are plain Lua in
`user/lib/litexl_host.lua`, with no system call in it, so
`tools/test_litexl_host.lua` checks them on this machine in `make test`.

### The fault, and how it was found

After Control-N and `hello`, the editor opened an "Open File" prompt and made
no document. The keys were not the problem: `wm litexl:--trace` prints every
raw key and every event on its way in, and every press and release was
there, in order. What it also prints - every command Lite XL runs and
everything it logs - said `doc:select-lines` and `core:open-file`, and no
`core:new-doc`. Lite XL had seen `ctrl+l` and `ctrl+o`, with Control still
held after its release.

**The queue consumed an event by clearing its slot.** Once everything had
been taken, `#` answered nought while the read position had moved on, so the
count of waiting events went below zero; `poll_event` refilled only when that
count was exactly nought, and the next events were written into slots the
read position had already passed, where nothing read them. A Control release
was among them. The queue now moves past an entry and never clears one, and
with the old consumption put back `tools/test_litexl_host.lua` fails on the
two checks that describe it: a drained queue is empty, and an event pushed
after a drain is the next one out.

### What is left

- **The wheel and the other buttons.** The window manager delivers the first
  button and no wheel, so scrolling is the scrollbar and the keys.
- **Resizing.** A direct window's buffers are allocated once, so the window
  is sized from the screen when it opens and stays that size.

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
