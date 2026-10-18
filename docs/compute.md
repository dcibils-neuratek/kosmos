# The Compute Kit, and GPU Fractal

Programs of their own on the GPU's execution units, from Lua. Diego, 18
October 2026: "build the compute kit so lua can run its own kernels and
also i want a lua example program that visually shows the use of gpu
compute", and the same afternoon: "I WANT CAFESA3e to use gpu compute for
its rendering engine if possible beyond the cpu raytracing option".

This document is the feature set and the architecture of both: the **Compute
Kit**, which any program uses, and **GPU Fractal**, the application that
shows it. `compute.html` is the window and `compute-architecture.png` what
they stand on. Cafesa3D's renderer on the GPU is the kit's second user, and
the reason it is a kit rather than a part of one application (*kits supply,
applications orchestrate*, `CLAUDE.md`).

What exists already (`m700-2d.md` 7b, `testing.md` 18.681): the driver runs
one fixed kernel - intel-gpu-tools' fill - over a grid, on the render
engine's GPGPU pipeline, and `gpu compute` at the prompt asks for it. What is
missing is everything that makes it a program's: its own kernel, its own
buffers, its own constants, and a way to write the kernel on Kosmos.

## What a person can do

**With the Compute Kit**, from any Lua program:

- **Write a kernel in the GPU's own assembly**, as text in the program - the
  syntax Mesa's tools print (`mov (16|M0) r5.0<1>:ud r1.0<0;1,0>:ud`) - and
  have it assembled on Kosmos, in the program's own process, with the line a
  mistake is on said.
- **Give it buffers**: a buffer is a region of memory shared with the GPU -
  numbers to read, pixels to write - made by the kit and read and written by
  the program as a string of bytes or, for pixels, as a surface a window can
  show without a copy.
- **Run it over a grid**: so many thread groups across and down, each told
  where it is, with up to 256 bytes of constants every thread sees - a
  zoom, a time, a colour.
- **Know when it is done**, without waiting if it would rather not: a run
  answers at once with a ticket, and the program asks whether it is done or
  waits for it.
- **See what it cost**: the GPU's own time for the run, from its clock.

**With GPU Fractal**, the example:

- **The Mandelbrot set, computed on the GPU**, filling the window - 1024 by
  768 pixels, every one a kernel's thread working out how fast it escapes
  and choosing its colour.
- **Drag to pan, scroll to zoom**, each frame computed again; the number of
  iterations rises as it zooms, so the detail holds.
- **GPU or processor, side by side**: a switch runs the same picture on the
  processor instead, in C, and the two timings sit together - "GPU 3.8 ms ·
  processor 410 ms" - which is the point of the example.
- **The kernel, shown**: a panel with the kernel's text as the program gives
  it to the kit, so the example is also the lesson (`roadmap.md`: the IDE's
  GPU compute lesson is written on this).

**And Cafesa3D**, after: its Rendered view drawn by a ray tracer on the GPU,
beside the processor's, chosen in its menu; the same scene, held to the
processor's picture, and **the time each took shown side by side** - Diego:
"cafesa 3d should show the increased rendering speed from cpu to gpu right?".
A ray tracer is a far larger kernel than the Mandelbrot's - rays, triangles,
a tree of boxes, materials - and is not written in assembly by hand: it
wants D1's language (C), or Mesa's own compiler run on the Mac from OpenCL or
GLSL. Its step comes after the kit and the example have proved the
machinery on something small.

## What it is made of

| Piece | Supplied by | New or as it is |
|---|---|---|
| The assembler - text to the GPU's 128-bit instructions | **`eu.lua`, a new library**: the subset a compute kernel needs - moves, arithmetic, comparisons, `sel`, loops by `jmpi`/`while`, the data port's reads and writes, and the end of a thread - each instruction's bits as Mesa's `gen_encoding_pre_xe.cpp` lays them for generation 9 | **new**; held bit for bit to Mesa's `gentool` on the Mac by a host test |
| Buffers, a run, a ticket, the time | **the Compute Kit**, `user/kits/compute/`, `use("/Kosmos/Kits/compute")`: a buffer is a region the kit makes and hands the driver; a run is a kernel, its buffers, its constants and a grid | **new**, C |
| The run on the hardware | **the GPU's driver**, two new verbs: `GPU_OP_KERNEL` - a kernel's bytes, **checked** (below) and kept - and `GPU_OP_RUN` - the kernel, up to eight buffers by id, the constants, the grid - set up as the fill already is: the state page, `PIPELINE_SELECT`, `STATE_BASE_ADDRESS`, `MEDIA_VFE_STATE`, the descriptor, `GPGPU_WALKER`; answers with the render engine's fence | **new** verbs, on 7b's machinery |
| Buffers in the GPU's table | `GPU_OP_BUFFER_IMPORT` and `_FORGET`, as the window server's pictures | as it is |
| Pixels on the screen | a buffer that is a surface: `gfx`'s, shown by the window as any picture | as it is |
| The window, the switch, the panel | the UI kit | as it is |
| The processor's Mandelbrot, for the comparison | GPU Fractal's own C (`bin/apps/fractal/`), a loop over pixels - which `CLAUDE.md` says is never Lua | **new**, small |

**Control by message, data by shared memory**: a run is one message - which
kernel, which buffers, the constants, the grid - and the numbers and pixels
never travel in it; they are in the buffers, which the program and the GPU
both see.

**C or Lua**: the assembler is Lua - it turns text into numbers once, when
a program starts, and its bug is a stack trace; the kit's door is C; the
driver is C. Nothing per pixel is Lua.

## Two decisions, Diego's - taken on 18 October 2026: A and A

Diego chose **D1 A** - assembly text, assembled on Kosmos by `eu.lua` - and
**D2 A** - every kernel checked before it runs. Recorded in `README.md`'s
decision log.

**D1 - how a kernel is written on Kosmos.**

- **A - assembly text, assembled by `eu.lua` on Kosmos** *(recommended)*.
  The GPU's own instructions, which is what a kernel is; a lesson can show
  every instruction and what it does. A subset first - what a compute
  kernel needs - checked against Mesa's assembler instruction by instruction
  on the Mac, so a kernel assembled on Kosmos is the one `gentool` would
  make.
- **B - assembled on the Mac only**, shipped with the program as numbers,
  as the fill kernel is now. Nothing to build on Kosmos, and no kernel
  written in the IDE.
- **C - a small language of its own** - expressions and a loop, compiled to
  the instructions above. Friendlier, and a compiler to write and keep
  correct; it is a later step on top of A, not instead of it.

**D2 - what keeps a kernel in its own memory.** The GPU has one table of
memory - the global GTT - and everything the driver ever mapped is in it:
the screens, every window's picture, the rings. A kernel that addresses
memory by number (a *stateless* message) could read or write any of it, and
the GPU checks nothing.

- **A - every kernel checked before it runs** *(recommended)*. The driver
  decodes each of a kernel's instructions when it is handed over; a message
  to memory may name only a binding table entry - one of the buffers the
  program gave this run - never an address. A kernel that could reach
  anything else is refused, and its line said. What a kernel can touch is
  what it was handed, which is the capability rule one layer down.
- **B - a GPU address space for each program** (per-process GTTs, PPGTT):
  the hardware's own answer, and the right one in the end; a larger piece
  of the driver, and a step of its own after A.
- **C - trust it**, for now. Not recommended: a Lua program would be able to
  draw over another's window, or worse.

## The busiest path

**A frame of GPU Fractal at 1024 by 768**: the program writes the view - a
centre, a scale, the iterations - into the constants, and asks for a run of
64 by 768 thread groups, sixteen pixels each: one message. The driver writes
about seventy dwords into a state page and the ring, and answers with a
ticket. The EUs do the work - about 786,000 threads, each up to a few
hundred iterations. The program asks for the ticket when the window paints;
the pixels are already in the buffer the window shows. **The processor's
part is a message and a paint**; the GPU's, a few milliseconds by estimate
on the M700's 24 EUs, which the first run will measure.

**Coherence**, learned in 7d (`testing.md` 18.686): the GPU's sampler did
not see all of what the processor had just written to ordinary memory. A
compute kernel reads and writes through the data port, not the sampler, and
the kit's buffers are made write-combining - as the fill's are, which are
exact - so what the processor wrote is in memory before a run, and what the
GPU wrote is read from memory after. The first run holds both to it.

## Steps

1. **C1 - `eu.lua`**, the assembler's subset, and the host test against
   `gentool`: every instruction the fill kernel and a Mandelbrot kernel use,
   bit for bit. **Done in 0.11.288** (`testing.md` 18.687).
2. **C2 - the driver's verbs**: `GPU_OP_KERNEL` with D2's check,
   `GPU_OP_RUN`; the fill kernel run through them, from Lua, its result held
   as 7b's is. **Built** (`testing.md` 18.688): the verbs, the check in
   `intelgpu_kernel.c` held to thirty kernels on the Mac, and `gpu kernel`;
   its run on the M700 is what is left. Two corrections came out of it: a
   `jmpi` counts from the instruction after it, and a send's destination is
   `:uw` at a stride of one, as Mesa's compiler writes it.
3. **C3 - the Compute Kit**: buffers, runs, tickets and the GPU's time, from
   Lua; a host-side check of what it sends. **Built** (`testing.md` 18.689):
   `/Kosmos/Kits/compute`, the GPU's own time by `GPU_OP_RUN_TIME`, and
   `gpu kernel` on it; held on the Mac by `test_compute.c`.
4. **C4 - GPU Fractal**, as drawn: the Mandelbrot kernel, the processor's in
   C, the switch and the timings; its suite under QEMU (no GPU there - it
   says so and draws the processor's), the picture in the dated screenshot,
   and the two pictures held to each other on the M700. **Built** (`testing.md`
   18.690): `wm fractal`, the kernel in `eukernels.lua`, `fractal.c`,
   `x86-fractal`; the GPU's picture and the two held to each other wait
   for the M700.
5. **C5 - Cafesa3D on the GPU**, its own design first (`cafesa3d` docs): a
   ray tracer as a kernel on the same kit.
6. **C6 - the IDE's lesson**, on GPU Fractal - Diego, 18 October: "the
   compute kit needs a learning lesson on the ide tutorials to explain what
   a compute kernel is, how it differs from regular cpu programs", and
   "gpu compute is very different from cpu and they are completely
   complementary so its good to teach that". So the lesson teaches the two
   as partners, not rivals: **the processor** - a few fast cores, each
   running its own program with its own branches, good at decisions and at
   anything that must happen one step after another; **the GPU** - hundreds
   of lanes running one small program, the kernel, in lockstep on
   different data, good at the same arithmetic done millions of times. What
   a kernel is (one pixel's or one item's work, written once, run for each);
   threads, lanes and a grid; why a branch costs a GPU what it does not cost
   a processor (lanes that disagree wait for each other); memory shared
   through buffers rather than variables; and how the two divide a job -
   the processor deciding what to compute and the GPU computing it, as GPU
   Fractal does every frame. **With diagrams of both** (Diego: "we can
   design visual diagrams to understand both paradigms"): the processor's
   few cores each following its own path through a program, beside the
   GPU's grid of lanes stepping through one kernel together - and what a
   branch does to each; a frame of GPU Fractal divided between the two.
   Drawn with the other lessons and agreed before it is written. **Drawn**
   (18 October): `compute-lesson.html` - lesson 15, "A program for the
   GPU", at the end of Part four; its project a moving pattern, a kernel of
   about twenty instructions with no loop; four diagrams and a table of the
   two side by side. **Agreed** - Diego, the same evening: "go ahead with
   the recommended choices for the lesson". **Written** (`testing.md`
   18.691): `ide-tutorial/15-gpu.html`, its four diagrams and its project,
   `15-GPU`; the pattern's own pictures wait for the M700.

Only x86-64 runs the tests for now (`CLAUDE.md`), and the GPU itself is the
M700's.
