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

## Sound, and the device as the clock

**Samples go into the ring from C.** `snes.sound(ring, rate)` takes the ring
`audio.open` made and the rate `audio.format` reports. Each `snes.frame` then
asks the core for one frame of sound at that rate - 735 device frames at
44100 over 60, with the fraction carried, so a rate that does not divide
evenly neither drifts nor rounds - and writes it straight into the slot at
the ring's `write` index, publishing each slot once it is full. That is
`sys.pcm_into`'s technique, and it means no Lua string exists at either end:
a minute of sound allocates nothing.

**The device paces the console, not the counter.** `snes.lua` runs a frame
whenever less than one frame's worth of sound is waiting in the ring, which
holds the console at exactly the rate its sound needs, with no clock
arithmetic and no second clock for the picture to drift against. The ring is
sized from the numbers - two frames of sound and two slots of slack - rather
than left at the default. Without a sound device, the counter paces it as it
did before there was sound.

**60 and 50, not 60.0988 and 50.007.** A real console draws 60.0988 frames a
second, and this file said so at first. But `apu.c` clocks the SPC700 at
32040 Hz *per 60.0 Hz frame*, and per 50.0 for PAL, so 60 and 50 are the rates
at which this core's sound has its own pitch - upstream's frontend asks for
48000 / 60 samples a frame for the same reason. The rate now comes from the
core, as `snes.start`'s second result, and is not written anywhere else.

**Heard off the machine, because TCG cannot judge it by ear.** `make
snes-check` gives the guest virtio-sound with QEMU's WAV writer behind it. On
Super Mario All-Stars it recorded 24 seconds over about 65 of running - the
device takes periods as they arrive, so the file is what the core made - 75%
of it sound, and no frames dropped.

**And the recording is the right sound, not merely a loud one.** The same
ROM run natively with no input makes 3583 sounding periods in its first 1500
frames. The first 479 of them, console frames 101 to 355, are in the guest's
recording byte for byte and in order; the first miss comes after the harness
has begun pressing Enter, and the two runs stop being the same game. The
sample format, the channel order, the rate, the slot writing and the server's
unity mix all have to be exact for that to hold.

## What works, and what is left

A ROM is read from the drive into a region, the core loads it, and frames
run into a 512 by 480 window at the core's own rate - 60, or 50 for a PAL
cartridge - with their sound going through the audio server and the keyboard
as the first pad.

- **Under TCG the sound has gaps.** The core makes about a third of a second
  of sound each second there, so the device plays what arrives and waits for
  the rest. Nothing on the machine can fix that; it is judged natively, as
  above.
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
