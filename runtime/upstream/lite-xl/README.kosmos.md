# Lite XL, vendored

Upstream: <https://github.com/lite-xl/lite-xl>
Tag:      `v2.1.7`, commit `f1ab7de4ea1b4951b49ca5b817726a470ad43da7` (2024-12-05)
Licence:  MIT — rxi 2020, Francesco Abbate 2020-2022, Lite XL Team
          2022-present. See `LICENSE`.

**Unmodified, byte for byte**, which is the rule every vendored thing here
follows: what is in the tree is what the author released, and everything done
to it is a build step somebody can read. No Kosmos copyright line is added to
these files - adding one would be modifying them, which is the one thing that
rule forbids. `src/` and `data/` are both a `diff -r` away from upstream and
that is checked rather than asserted.

## Why v2.1.7 and not the branch tip

The tip has moved to **SDL3**, and with it from 91 SDL functions to about
250. Both numbers are the size of the shim somebody has to write, because
there is no SDL here; the release is the smaller of the two and is the last
one that was cut as a release. It can be moved forward once the shim exists
and there is something to move.

## Why this is a port and not a rewrite

**Lite XL is a Lua editor with a C rendering core**, which is very nearly the
shape of a Kosmos application already:

- **Lua 5.4**, which is exactly the interpreter this system carries.
- **Nineteen thousand lines of Lua** in `data/` - the editor itself: buffers,
  views, the command palette, syntax highlighting, the plugin system. That is
  the part somebody would have to write, and it does not need porting.
- **Ten thousand lines of C** in `src/`, of which about two thousand -
  `rencache.c`, `api/utf8.c`, `api/renderer.c`, `arena_allocator.c` - call no
  SDL function at all. `api/utf8.c` and `arena_allocator.c` compile clean
  against this toolchain today, unmodified, which is what said this was worth
  starting.

And the decisive one: **`renwindow.c` draws into a CPU pixel buffer and
pushes damage rectangles.** With `LITE_USE_SDL_RENDERER` undefined - the
default - the whole GPU path compiles out and what is left is
`SDL_GetWindowSurface` and `SDL_UpdateWindowSurfaceRects`. That is the
window manager protocol this system already has, under another name.

## What the build leaves out, and why

The line is drawn in the build rather than by deleting files, which is how
`LICENSE` draws the Doom line too - the tree stays what upstream released and
the Makefile says what is made from it.

- **`src/api/process.c`** (847 lines). Subprocess spawning, for plugins and
  language servers. Kosmos has `SYS_SPAWN` but nothing that looks like
  `fork`/`exec` with pipes, and `CLAUDE.md` forbids a POSIX personality
  outright. Plugins that shell out will not work; the editor does not need
  it to run.
- **`src/api/dirmonitor/`** except `dummy.c`. Watching a directory for
  changes needs inotify, kqueue or FSEvents. `dummy.c` is nine lines and is
  upstream's own answer for a platform with none of them.
- **`src/bundle_open.m`**. Objective-C, for opening files through macOS.
- **`src/api/regex.c`** wants PCRE2, which is a second large dependency. It
  is stubbed for now: `tokenizer.lua` takes either a Lua pattern *or* a
  regex, and four of the nine bundled languages use the regex form, so what
  this costs is syntax highlighting on those four and nothing else.

**Threads are not on the list, and that is worth saying**: `SDL_CreateThread`,
`SDL_CreateMutex` and `SDL_CreateCond` appear only in the two files above.
The editor core needs no threading shim at all, which is exactly as well,
because a Kosmos process has one thread and no way to make a second.

## The fonts

`data/fonts/` ships three faces and all three are kept.
`JetBrainsMono-Regular.ttf` is also in `assets/fonts/`, so it is in the tree
twice - which is the rule working as intended rather than an oversight, since
deduplicating it would mean editing what upstream released. `icons.ttf` is
Lite XL's own and has no equivalent here.
