# GPU Monitor

The graphics processor, in a window: how busy each of its engines is, how
fast it runs and how long it sleeps, what it draws from the power supply,
the memory it holds and who holds it, and what the display is doing - and
whether the desktop is drawn with its help. Diego, 17 October 2026: "we
need to vreate a gpu monitor app now to see memory pressure, gpu usage,
etc", with Windows' GPU page as the picture; and "i need to know if kosmos
is using gpu acceleration on the desktop in some way, like a label
somewhere in the desktop, perhaps beside the kernel version".

This document is the feature set and the architecture; `gpumonitor.html`
is the window and `gpumonitor-architecture.png` the picture of what it
stands on. The system monitors' fourth, beside Monitor, Network Monitor
and Disk Monitor, and built as they are: the kit's `ui.history` and
`ui.meter_card`, and a server - here the GPU's driver - that already keeps
what it is asked for.

## What a person can do

- **See the GPU**: its maker and model, how busy it is and how fast it runs
  now.
- **See each engine's busyness over the last minute** - render, blitter,
  video, video enhance. **An engine Kosmos sends no work to says so**:
  until the blitter (`m700-2d.md` step 5) every engine is "no work sent to
  it yet", which is the truth today, rather than a 0% that looks like
  a measurement.
- **See its clock and its sleep**: the GPU's frequency over the minute, and
  how much of each second it spent asleep in RC6 - an idle GPU is asleep,
  and this is what says the desktop's flips cost it nothing.
- **See its power**, from RAPL's graphics energy counter.
- **See its memory**: the bytes in GPU buffers over the minute, against
  what the GTT addresses and the stolen memory the firmware set aside -
  and **who holds it**, a buffer a row: its holder, its size, what it is
  for (scanned out, the cursor, later a texture or a render target).
- **See the display**: its size and refresh, flips a second, frames the
  window server held back, and the pipe's underruns.
- **See what the desktop is drawn with** - cursor plane, page flips, later
  the 3D compositor - the same words as the label on the desktop.
- **Choose the pace**, from the dots, as the other monitors.

**And the desktop's label** (agreed the same evening): the version line at
the bottom right of the desktop ends with what draws the desktop -
`GPU: HD 530 - cursor plane, page flips` on the M700, `GPU: virtio -
cursor` under QEMU with virtio-gpu, `GPU: none - software` with ramfb.
The window manager knows each already.

## What it is made of

**The application is the thinnest layer there is**: it asks and draws.
Everything it shows is read by the driver that holds the GPU.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the graphs, the card | the UI kit: `ui.history`, `ui.meter_card` | as Network and Disk Monitor made them |
| **Clock, sleep, flips, underruns, the buffers and their holders** | **the GPU's driver**, `/Devices/gpu`: one new verb, `GPU_OP_STATS`, a snapshot of counters - `GEN6_RPSTAT1`'s current frequency (Gen9: bits 31:23, in units of 50/3 MHz), `GEN6_GT_GFX_RC6` (0x138108, the time asleep), `PIPE_FLIPCOUNT` and `PIPE_FRMCOUNT` (flips and frames, so the refresh is measured rather than computed from timings), the pipe's underrun bit, and its own table of buffers | **new**, in the driver |
| A buffer's holder | the driver, counting by caller as the disk servers do (`progcount.c`, `SYS_SENDER`) | **new**, small |
| Frames held back | the window server's own count, said to the driver with each flip | **new**, a word |
| **Power** | **Nebula**: RAPL's graphics-plane energy counter (`MSR_PP1_ENERGY_STATUS`) is a model-specific register, which only ring 0 reads - a field in `sysinfo`, beside the processor's | **new**, small |
| Each engine's busy time | the driver, from each engine's own timestamps, once it sends that engine work | **later**: the blitter first (step 5) |
| A holder's name | `sys.processes()` | as it is |
| **The desktop's label** | the window manager: `OUT.hw_cursor` and whether the window server said it flips | **new**, a few lines |

**Asked for, not sent** (control by message): the window asks the driver
once a pace, and the driver answers from registers it reads then and a
table it keeps anyway. Nothing is sampled on a timer of the driver's own,
so a machine with no GPU Monitor open pays nothing.

**C or Lua.** Register reads and the buffer table are C, in the driver.
The window is Lua: one request a pace, differences taken, a column added,
a paint.

**Two things the M700 has to answer first**, before the window is built
on them: whether `GEN6_RPSTAT1` and the RC6 counter read true without
forcewake (i915 reads some GT registers under it), and RAPL's energy unit
on this processor (`MSR_RAPL_POWER_UNIT`). Step G1 prints them.

## The busiest path

**The window, once a pace**: one `GPU_OP_STATS` to the driver - a handful
of register reads and a copy of its buffer table into the reply - and one
`sysinfo` for the energy counter. Two messages a second at the default
pace.

**The driver, on every flip**: nothing new. The flip and frame counts are
the display's own registers; the driver adds nothing to the path a frame
takes.

## Steps

1. **G0 - the label**: the version line says what draws the desktop; the
   display suites read it under QEMU (`virtio - cursor`, `none - software`).
2. **G1 - the driver says**: `GPU_OP_STATS` and a program `gpustat` that
   prints it, as `netstat` and `diskstat` do; RAPL's graphics energy in
   `sysinfo`. On the M700: the clock, RC6, flips and frames read and seen
   to move while a window is dragged; under QEMU, "no Intel GPU".
3. **G2 - the window**: as drawn, its suite reading "no Intel GPU" under
   QEMU and the window drawn; its picture in the dated screenshot.
4. **G3 - an engine's busyness**, when the blitter has work (step 5).

Only x86-64 runs the tests for now (`CLAUDE.md`).
