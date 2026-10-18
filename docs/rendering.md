# Rendering modes

How the desktop is drawn, chosen in Preferences and tried live. Diego, 18
October 2026: "We need options", "Let's setup a preferences panel for the
gpu where the user can select the desktop rendering settings and try it",
"Because we are trying many things and some work better than others", "I
feel that there were several steps that worked good without using a lot of
memory in the gpu", "perhaps we can create like 4 o 5 rendering settings
form the basic frame buffer we were using before any gpu work that worked
great all the way up to gpu composing", and "And indicate what mode does
what".

This document is the feature set and the architecture; `rendering.html` is
the page and `rendering-architecture.png` the picture of what it stands
on. `composing.png` is the background: the six stages a frame goes
through and who does each.

## The modes

Each is one of the steps measured on the M700 (`m700-2d.md`, `testing.md`
18.657 to 18.669), and each adds to the one before.

| Mode | What it does | GPU memory, at 1720 x 1440 | Measured, a dragged window |
|---|---|---|---|
| **Framebuffer** | The processor composes every frame and copies it into the screen the firmware lit, while the display reads it. The pointer is drawn into each frame. No GPU at all - what Kosmos did before 17 October, and what every machine without an Intel GPU does | none | a core busy: 514 frames composed a second, the pointer in each |
| **Cursor plane** | As above, and the pointer is the display's: a 64 x 64 picture it lays over the screen, so moving the mouse composes nothing | 16 KB | |
| **Page flips** | As above, and two screens: the processor copies each frame into the one not shown, and the display switches to it at its vertical blank - no tearing - with the desktop composing once a refresh | one more screen, 9.5 MB | 60 frames a second, 5.2 s busy of 30, 0.82 ms composing and 0.38 ms copying a frame |
| **Blitter copy** | As above, and the copy into the hidden screen is the GPU's blit engine's | the window server's back buffer in the GPU's table - shared, not copied | 57 frames a second, 4.8 s busy of 30, 0.80 ms composing, no copying; the blitter 1.4% busy at rest |
| **Blitter composing** *(experimental)* | As above, and windows' opaque interiors copied by the blitter straight from their buffers; the processor composes only edges, shadows and translucency | every window's buffers in the GPU's table | 42 frames a second - the blitter cannot keep up - 3.9 s busy of 30, 0.43 ms composing |
| **GPU composing** *(not yet)* | The render engine composes the whole frame - blending, glass, animation - into the screen's buffer; the processor only describes it | textures and the screens | step 7 |

**The default is Blitter copy**, the best measured; a machine where a mode
cannot be had shows it with the reason - "needs an Intel GPU" under QEMU,
where Framebuffer is all there is, and Cursor plane is virtio-gpu's.

## What a person can do

- **See every mode**, in order, each with its name, one sentence of what
  it does, **a strip of the six stages coloured by who does each** - the
  processor, a GPU engine, the display - as `composing.png` draws them, and
  what it costs in GPU memory.
- **Choose one and see it at once**: the desktop switches without a
  restart, and a bar says "Keeping Blitter copy in 15 s - Keep it - Go
  back". Nothing answered in 15 seconds goes back, so a mode that breaks
  the screen undoes itself.
- **See what the chosen mode is doing now**, under the list, a second at a
  time: frames shown a second, the processor's time a frame, the GPU's
  memory and the blitter's busy share - the numbers that tell one mode from
  another, on the machine in front of you.
- **Find it again after a restart**: the mode kept is the one the desktop
  starts in.
- And the desktop's label (`GPU: HD 530 - cursor plane, page flips`) and
  GPU Monitor's "Desktop drawn with" say the mode in use.

## What it is made of

| Piece | Supplied by | New or as it is |
|---|---|---|
| The page, its rows | Preferences and `settings.lua`: a group **Rendering** on the Displays page | the group new; Preferences as it is |
| **The mode list** - names, sentences, the stage strips, the memory | a control of its own, `kind = "rendering"`, drawn by Preferences as `brightness` is; the modes' words and stages in **`rendering.lua`**, a small library - GPU Monitor and the label read the same table | **new** |
| **The keep-or-go-back bar** | the UI kit's notice bar, as Network's "Forget this network?" | as it is |
| Where the choice is kept | `settings.APPEARANCE`, key `rendering`, as the look is | a key new |
| **The switch, live** | the window manager, told as the look is told; it does the pointer itself (`gfx.cursor` or `gpu.cursor`, or composited) and asks the window server for the rest with **`WS_OP_RENDERING`** - flips on or off, the copy here or on the blitter, the blitter composing or not | **new**: one verb in `windowproto.h` |
| Flips, the blitter, composing on it | the window server's `flips_start` and `flips_stop`, `blitting`, `blit_composing` - all there, made switchable rather than decided once | as it is, rearranged |
| What a mode can be on this machine | the GPU's driver (`gpu.stats`: a GPU or none) and the window manager (virtio's cursor or not) | as it is |
| The live numbers | `gpu.stats` and `gpu.since` (flips, the blitter, memory); the window server's reply (`compose_us`, `copy_us`), added up by the window manager and asked for by Preferences | **new**: a small `frames`-like question to the window manager |

**Control by message**: the mode is one small message to the window
manager, which tells the window server; no pixels move for it.

**C or Lua**: the switch is C in the window server and Lua in the window
manager; the page is Lua.

## The busiest path

**A switch**: the window manager hides or shows its pointer plane (two
registers through the GPU's driver), and the window server turns flips or
the blitter on or off - which at most makes or keeps the second screen
(made once, kept, as now), and composes the whole screen once so the new
mode starts from a whole frame. Under a frame's time.

**While the page is open**: one `gpu.stats` and one question to the window
manager a second. Nothing when it is closed.

## Steps

1. **R1 - modes made switchable**: `WS_OP_RENDERING` in the window server
   and its use in the window manager, a mode from the settings file at
   start; under QEMU, Framebuffer and Cursor plane switched and the screen
   held to the same picture by the display suites; on the M700 each mode
   tried and measured. **Done in 0.11.272** (`testing.md` 18.670), with
   `rendering` at the prompt; the measuring is R2's page.
2. **R2 - the page**: the Rendering group as drawn, the strips, the
   keep-or-go-back bar, the live numbers; its suite choosing a mode and
   letting it go back; the picture in the dated screenshot.
3. **R3 - GPU composing** joins the list when step 7 is built.

Only x86-64 runs the tests for now (`CLAUDE.md`).
