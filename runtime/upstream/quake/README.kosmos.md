# Chocolate Quake, vendored

Upstream: <https://github.com/Henrique194/chocolate-quake>
Commit:   `edb820937c31e62acf6a1ef1433fbf3e5fab19ba` (2026-09-07)
Licence:  GPL. Every one of the 106 `src/*.c` files, and 64 of the 67
          headers, carries id Software's notice - "either version 2 of the
          License, or (at your option) any later version". The project's
          `LICENSE` is the text of version 3. The three headers with no
          notice are the end screen's bitmap fonts, which this build does
          not compile.

**Unmodified, byte for byte.** `src/` was `diff -r` identical to upstream
when it was copied, and what is done to it is a build step somebody can
read, the rule every vendored tree here follows. No Kosmos copyright line is
added to these files.

Copied: `src/`, `LICENSE`, `README.md`, `CHANGELOG.md`, `config.h.in`,
`CMakeLists.txt`. Left out, as packaging rather than the game: `.github/`,
`cmake/`, `dist/`, `external/` (a vcpkg submodule), `.clang-format`,
`.gitignore`, `.gitmodules`, `CMakePresets.json`, `vcpkg.json`.

## Why this Quake

The plan started from **quakegeneric**, which is to Quake what doomgeneric
is to Doom - and its README says it "can only compile for 32-bit
architechtures". Kosmos is 64-bit and only 64-bit. Quake stores pointers in
32-bit integers in places, and fixing that here would have meant patching a
vendored tree.

So the port starts from a Quake that is already 64-bit clean. Two were
looked at:

- **Chocolate Quake** reproduces WinQuake 1.09 - the same code quakegeneric
  is made from - and ships arm64 builds, which is the instruction set Kosmos
  runs on. It is written against SDL2, but SDL reaches the engine itself
  only for integer types, libc under `SDL_` names, a timer and two network
  types, and the rest of SDL is confined to the platform modules Kosmos
  replaces anyway.
- **libretro's TyrQuake** is 64-bit clean too, but thirty-two of its engine
  files include `libretro-common` headers directly and it carries a
  rendering layer with a Vulkan backend, so porting it would have meant
  porting libretro as well.

## What the build takes, and what it replaces

`make QUAKE=1` compiles 78 of upstream's files: the engine - rendering,
the client and server, the progs interpreter, the menus, sound mixing,
loopback networking and the connection book `net_socket.c` keeps.
`make quake` compiles those alone, to say whether they still build.

**Replaced rather than patched**, by `user/lib/quake_kosmos.c`:

- `main.c` - the Lua side owns the loop, as it does Doom's;
- `sys/src/sys.c` - files, time, errors and quitting;
- all of `video/src/` - an SDL window and renderer;
- `input/src/in_main.c`, `in_keyboard.c`, `in_mouse.c`, `in_gamepad.c`;
- `sound/src/snd_sdl.c`, and music with its codecs: `bgmusic.c`,
  `snd_codec.c`, `snd_wave.c`, `snd_mp3.c`, `snd_mp3tag.c`, `snd_vorbis.c`,
  `snd_flac.c`;
- `net/src/net_udp.c` and `net_dgrm.c`, the only network code that calls
  SDL_net, and `net_drivers.c`, the table naming them;
- `end_screen/`, the text screen shown on quitting.

**`user/lib/quake/` is what the engine's own SDL includes resolve to**:
`SDL_stdinc.h` with eight integer types, two macros and fifteen libc names,
plus the standard headers the real one includes - `common.h` names `FILE`
and includes nothing that declares it; `SDL.h` for the timer calls in
`host.c`; `SDL_net.h` for the address and socket types two network headers
keep in structures; `SDL_events.h` naming an event type the replaced input
files took.

**`kosmos_quake.h` is included ahead of every file** for the two POSIX
habits of `console.c`: `unlink` becomes ISO C's `remove`, and the `open`,
`write` and `close` of the `-condebug` log do nothing. No POSIX name was
added to Kosmos for either.

**Quake's own headers are on the path with `-iquote`**, not `-I`, because
`console.h` and `screen.h` are also the names of two headers in `kernel/`.

## What it asked of Kosmos's libc

- `fscanf`, which reads the CD track at the start of every demo. The
  scanner moved to `runtime/libc/scan.c` and takes a length, because a demo
  is a file inside the pak and what follows it is the next file, not a NUL.
  Moving it found that `%f` stored a `double` where the standard says
  `float` - harmless for Doom's `%lf`, and four bytes past every value for
  Quake's. `tools/test_scan.c` checks both.
- `vsprintf` and `fgetc`, and `EBADF`, `EFAULT` and `EINVAL`.

## What it asked of the rest of Kosmos

- **A bigger region.** The pak is read into one region, and a region was
  capped at 16 MB - "a double-buffered full screen" - where the pak is 18.3 MB.
  `MEMOBJ_PAGES_MAX` is 32 MB now; `kernel/memobj.h` has the reason, and
  "mem: a region the size of Quake's pak" in the C suite holds it.
- **A bigger stack, for the engine alone.** `R_EdgeDrawing` keeps its edge
  and surface lists on the stack, a 205 KB frame with `R_RenderWorld`'s
  80 KB inside it, and a process's stack here is 256 KB - one contiguous run
  that every process pays for. Rather than raise that for all of them,
  `quake_kosmos.c` maps a megabyte with a guard page under it and runs
  `Host_Init` and `Host_Frame` there, through `kosmos_call_on_stack` in
  `runtime/libc/callstack-*.S`.

## What works, and what is left

The shareware attract loop plays - `demo1.dem` in e1m3, drawn at 320 by 240
and shown twice the size - the menus answer, and a command typed at Quake's
console runs. `make quake-check PAK=...` checks exactly that.

- **No sound.** `SNDDMA_Init` finds no device, and music is not built.
- **Looking around is a drag**: the window manager reports pointer movement
  only while the button is held.
- **Nothing is written**: `config.cfg`, saves and the `-condebug` log are
  refused, because this libc opens no file for writing.
- **A question asked inside a frame is answered no.** `SCR_ModalMessage`
  waits for a key within one frame, and keys arrive between frames here.
- **The video menu is empty**: the window is the size the Lua side made.

## The pak is not here and will not be

`pak0.pak` from the shareware release is 18,689,235 bytes, MD5
`5906e5998fc3d896ddaf5e6a62e03abb`. It goes on a disk:

    build/host/lua tools/kfs.lua create quake.img 64 pak0.pak:/home/id1/pak0.pak

and `wm quake` reads it into a region, which the libc is told is
`pak0.pak`.
