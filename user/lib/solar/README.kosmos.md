# Solar System: the portable core, carried unmodified

This directory is the `solar/` half of Diego's **solar-system-portable**
project - a 3D solar system simulator in pure Lua with a software
rasterizer, no LÖVE, no FFI, no C and no GPU. It runs on Lua 5.1 through
5.5 and LuaJIT; Kosmos embeds PUC Lua 5.4.8.

**It is carried here byte for byte and is not to be edited.** That is the
port brief's instruction and it is also the right discipline: the same files
run on a Mac under stock `lua`, under LÖVE, and here, so a difference in
behaviour is a difference in the *host* rather than in a copy that drifted.
If something here truly has to change, the change belongs upstream in the
portable project and comes back as a fresh copy.

For that reason these files do **not** carry the one-line Kosmos copyright
header every file this project writes carries. They are the same author's
work arriving from another repository, and adding a line to them would be
modifying them - the rule `lua/upstream/` and `runtime/upstream/` follow.
The MIT terms in `LICENSE` cover them, the author being the same.

## What is here

| File | What it does |
|---|---|
| `sim.lua` | Orbital mechanics, body data, stars, belts, the calendar. No I/O, no graphics. |
| `soft.lua` | The rasterizer: points, anti-aliased lines, text, lit spheres, rings, the sun. |
| `app.lua` | Camera, scene, HUD, input handling. Host-agnostic. |
| `noise.lua` | Value noise and fbm, for the ring profile. |
| `font.lua` | Bitmap font loader. |
| `fonts/f*.lua` | Five baked sizes - 12, 24, 36, 48 and 72 px - loaded on demand. |

## The host is ours; the core is not

A host owes the core four things and nothing else: a clock, somewhere to
put a finished frame, an input queue, and optionally a way to read a file.
Kosmos's host is `user/bin/solar.lua`. The reference hosts from the portable
project - `headless.lua`, LÖVE's `main.lua`, `gpu-reference/` - are
deliberately **not** copied here: they are desktop material and would be
three more things to keep in step.

## Two notes on what is inside these files

**`require`, which Kosmos does not have.** The core loads its own modules
with `require "solar.sim"`, and this system's loader is `use("/lib/x.lua")`
- a file in the process's namespace, with no package path and no global
module table, on purpose (`user/init/init.lua`). The host installs a small
`require` that maps `solar.x` onto `use("/lib/solar/x.lua")`, so the core is
satisfied without being touched.

**The font is DejaVu Sans Mono**, baked into `fonts/f*.lua` as bitmaps by
the portable project's `tools/mkfont.py`. DejaVu is under the Bitstream Vera
licence, which permits redistribution; it is named here because anything
vendored in this repository names its licence beside it, and a font baked
into a Lua table is still a font somebody else drew.
