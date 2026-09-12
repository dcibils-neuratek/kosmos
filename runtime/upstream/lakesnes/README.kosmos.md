# LakeSnes, vendored

Upstream: <https://github.com/dinkc64/LakeSnes>, dink's fork of
          <https://github.com/angelo-wf/LakeSnes>
Commit:   `048a0d72568668f74e0cdf371dcd9e8efd614965` (2025-04-11)
Licence:  MIT, in `LICENSE.txt` - "angelo_wf and contributors". `cx4.c`, the
          one file the fork added whole, says "(c) 2023 dink, License: MIT"
          at its top.

**Unmodified, byte for byte.** `snes/` was `diff -r` identical to upstream
when it was copied, and what is done to it is a build step somebody can
read, the rule every vendored tree here follows. No Kosmos copyright line is
added to these files.

Copied: `snes/`, `LICENSE.txt`, `README.md`, `bugs.md`. Left out, as the
frontend and packaging rather than the console: `main.c` (SDL),
`tracing.c` and `tracing.h` (a debugger's disassembler), `zip/` (a zip reader
and Miniz, for zipped ROMs), `resources/`, `Makefile`, `.github/`.

## Why this LakeSnes

LakeSnes is a Super Nintendo emulator in plain C whose core was written to
be a library, with the SDL program as a thin frontend over it. That is what
makes it the port to start from: the platform layer is the frontend, and
the frontend is the one file Kosmos does not take.

**dink's fork rather than angelo_wf's**, which is where it began. dink
maintains the SNES core FinalBurn Neo carries, and the fork is that work:
the Capcom CX4 (Mega Man X2 and X3), cached address timing and a cheaper
`snes_runCycle`, and fixes to dozens of individual games. It also replaces
the SPC700's 64-byte boot ROM - Nintendo's code - with bytes an algorithm
produces, so no piece of a console's firmware is in this tree.

## What the build takes, and what it replaces

All twelve `snes/*.c` files, which is the list upstream's `Makefile` names
for the core. `user/lib/snes_kosmos.c` stands where `main.c` was:
`snes.start` loads a ROM from an address, `snes.frame` runs a frame and
copies its picture into a window's surface, `snes.button` sets a button on
the first pad, `snes.log` drains what the core printed. `user/bin/snes.lua`
is the loop.

**Nothing was asked of Kosmos.** The core compiles against this system's
headers with no shim and no forced include: it wants `malloc`, `realloc`,
`free`, `memcpy`, `memset`, `strcmp`, `strlen`, `printf`, and `sin`, `cos`,
`tan`, `asin`, `atan` and `sqrt`, which the libc and musl's maths already
have. It opens no file, reads no clock, starts no thread and never calls
`exit`. Its largest stack frame is 544 bytes, in `snes_loadRom`.

It is built with `-w -Wno-error`, as every vendored tree here is, and
`snes_kosmos.c` is held to the usual flags.

## Why it is a build option

**Not for its licence.** LakeSnes is MIT, so an image carrying it is exactly
as MIT as one without - unlike Doom and Quake, which make the image a GPL
work.

**For its size.** The image is copied into every process, and the core is
about 94 KB of code and 9 KB of `.bss`, paid by the shell and the Deskbar
for a console neither of them runs. `FULL=1`, the default, turns it on, as
it does Doom and the browser; `FULL=0` leaves it out.

## What it costs a process that runs it

- **A 16 MB table.** dink's cached address timing is one byte for every
  address on the 24-bit bus, allocated the first time the console resets.
- **The ROM, up to three times while it loads**: the region `snes.lua`
  reads it into, the copy `snes_loadRom` pads to a power of two, and the
  cartridge's own. The padded copy is freed once the cartridge has it.
- **A 512 by 480 picture**, a megabyte, allocated when a ROM starts rather
  than declared, because a declared one would be in every process's
  `.bss`.

A six-megabyte ROM therefore peaks near 36 MB of a process's 48.

## How fast, and where the time goes

**Natively the core is fast.** Timed off the machine on an Apple M4, 3600
frames each - a minute of game - with the frame, the picture and the samples
counted:

| ROM | a frame | the worst |
|---|---|---|
| Mortal Kombat II | 2.04 ms | 5.9 ms |
| Super Mario All-Stars + Super Mario World | 2.46 ms | 6.7 ms |
| Top Gear 2 | 2.34 ms | 7.4 ms |

against a frame of 16.6 ms. `-O3`, upstream's own flag, is 3 to 5% quicker
than this build's `-O2`; building the picture is 1% of a frame and copying
it into the surface another 1%.

**Under QEMU's TCG it is not.** `make snes-check` on the All-Stars ROM, on
its title and game-select screens: **about 22 frames a second of 60, 25 to
34 ms a frame** - roughly ten times the native cost, which is what TCG does
to an interpreter. That is a QEMU number and says nothing about a Pi 5; it
says the game runs in slow motion under `make qemu` and that sound, which
has to be fed at the console's rate, cannot be judged there.

**And the time is the picture processor, not the processors.** Sampled
natively, `ppu_getPixel` is 58% and `ppu_handlePixel` 14%: nearly three
quarters of a frame is choosing, pixel by pixel, which of up to twelve
layer-and-priority pairs shows. The 65816 and the SPC700 together are under
a fifth. `gfx.md` 19.14 is why that is measured natively: the vector unit is
where that loop would go faster, and TCG translates vector instructions one
at a time.

## What works, and what is left

A ROM is read from the drive into a region, the core loads it, frames run at
the console's own rate - 60.0988 Hz, or 50.007 for a PAL cartridge - into a
512 by 480 window, and the keyboard is the first pad.

- **No sound yet.** `snes_setSamples` is not called. It is the next step, and
  the one that makes the console's rate matter: the audio server's ring, fed
  from C, would pace the frames rather than the counter.
- **No saves.** Battery RAM and save states are bytes the core hands back,
  and this libc opens no file for writing, so they have to go out through
  the namespace from Lua.
- **One pad**, and no way to choose a ROM but its name.
- A button held while the window loses the focus stays down until it is
  pressed and released again.

## ROMs are not here and will not be

They go on the drive, in `/home/roms/snes`:

    make image FILES="game.sfc:/home/roms/snes/game.sfc"

and `wm snes` opens the first one there, or `wm snes:<name>` a named one.
