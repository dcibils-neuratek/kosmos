# Cafesa3D on the GPU

Cafesa3D's renderer on the GPU's execution units, beside the processor's,
with the time each took shown side by side. Diego, 18 October 2026: "I WANT
CAFESA3e to use gpu compute for its rendering engine if possible beyond the
cpu raytracing option", and "cafesa 3d should show the increased rendering
speed from cpu to gpu right?". This is `compute.md`'s step C5. It is
designed here before it is built: the feature set, the architecture, the
decisions that are Diego's, and the steps. `cafesa3d-gpu.html` is how it
looks, and `cafesa3d-gpu-architecture.png` what it stands on.

## What a person can do

- **Choose the renderer** in the Render tab: Processor or GPU, beside
  Final and Preview. The choice applies to both the Rendered view and F12's
  Render window, and the scene file keeps it. Where there is no Intel GPU,
  GPU is shown and cannot be chosen, and says why.
- **See the speed**: the Render window says this render's time and the
  other renderer's last time for the same scene, size and samples, side by
  side, with how many times as fast. The Rendered view's corner says which
  renderer drew it and its samples a second.
- **Trust the picture**: the first time both renderers have drawn the same
  render, the Render window says how close the GPU's picture is to the
  processor's. In Preview that is pixel for pixel, within a level or two
  of rounding. In Final it is the noise both are left with, since the two
  draw their random numbers differently. A faster picture that is wrong is
  never shown as a win.
- **Everything the processor draws, the GPU draws**: boxes, planes,
  spheres and cylinders as exact shapes; meshes by their triangles through
  a hierarchy of boxes; the materials - colour, roughness, metal, glass and
  how it bends; the lamps and the sky; and the bounces. Image textures come
  in the last step (R7), and until then a textured object is drawn in its
  base colour, which the window says.

## What it is made of

The ray tracer today is `user/kits/3d/k3d_trace.c`: 1,820 lines of C,
four lanes wide on the processor's vector unit, spread over every core a
tile at a time. Its GPU twin has to do the same work in a kernel of its own,
and **how that kernel is written is the decision this design turns on**
(E1 below). The GPU Fractal's kernel is 47 instructions written by hand. A
ray tracer that walks a tree of boxes, meets triangles, shades five kinds of
material and bounces is thousands, and nobody should keep that many by hand.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The tracer's kernel | written in **Kosmos's own kernel language** (E1 A): C-shaped - numbers, vectors of three, `if`, `while`, functions - compiled on Kosmos to `eu.lua`'s assembly | **new**: the language and its compiler, `kc.lua`, a library any program uses |
| Branches that lanes disagree on | the GPU's own structured control flow - `if`, `else`, `endif`, `while`, `break` - which keeps each lane's mask in hardware; `eu.lua` and the driver's check learn them, every jump held inside the kernel | **new** in `eu.lua` and `intelgpu_kernel.c` |
| Square roots, division, sines | the GPU's `math` instruction, already allowed by the check | `eu.lua` learns its forms |
| The scene, in the GPU's memory | **`k3d_gpu.c`, new in the 3D Kit**: objects, the hierarchy flattened to arrays, triangles, materials and lamps, packed into Compute Kit buffers from what `k3d_render_new` already builds | **new**, C, in the kit beside `k3d_trace.c` |
| Reading the scene, keeping a ray's stack | the data port's untyped reads and writes through the run's buffers: a traversal stack per lane in a buffer of its own, since the check refuses registers addressed through `a0` and memory the run was not handed | `eu.lua` learns the messages; the check already allows them |
| Samples gathered | a float buffer the kernel adds each pass to; a second small kernel turns it into the picture's colours, as `k3d_render_paint` does | **new** kernels, in the language |
| Each pass | the **Compute Kit**: one `compute.run`, a ticket, the GPU's own time | as it is |
| Kernels tested where there is no GPU | **an emulator of the execution units**, in C on the Mac: the instructions `eu.lua` writes, run as the hardware would, lane by lane, so every kernel - the fill, the Mandelbrot, the lesson's, the tracer's - is checked in `make test` against the processor's answer | **new**, `tools/` |
| The choice, the times, the comparison | Cafesa3D's Render tab and Render window | as they are, with a row and a line more |

**Control by message, data by shared memory**: the scene goes into
buffers once a scene change, the accumulated samples stay in a buffer, and
each pass is one message - the camera, the pass, the bounces, as constants.

**C or Lua**: the compiler is Lua, like `eu.lua`. It turns text into
numbers once, when a scene is first rendered, and its bug is a stack trace.
Packing the scene is C (`k3d_gpu.c`), since it is a loop over every triangle.
The tracer is the GPU's. Nothing per pixel is Lua.

### The language, in brief

What the tracer needs, and nothing more for a first version:

- `float`, `int`, `uint`, `bool`, and `vec3` of floats; arithmetic, the
  comparisons, `&&` and `||`; `sqrt`, `rsqrt`, `min`, `max`, `abs`,
  `floor`, `sin`, `cos`, `pow`, `dot`, `cross`, `normalize`.
- `if`, `else`, `while`, `for`, `break`, `continue`, `return`.
- Functions, inlined - the GPU has no stack to call on.
- Every variable is a lane's own: the compiler runs sixteen or eight
  pixels at once and never says so. `uniform` marks what is the same for
  every lane - the constants - and is kept once.
- Buffers by name: `load(nodes, i)` and `store(stack, i, v)` become the
  data port's untyped messages through the run's binding table, so the
  driver's check passes what the compiler writes by construction.
- SIMD8 or SIMD16 per kernel. A tracer holds many values at once and is
  compiled SIMD8, as Intel's own drivers compile heavy shaders, so each
  value takes one register rather than two.

It is compiled to `eu.lua`'s text, which is then assembled and checked as
any kernel is, so the compiler is never trusted with the GPU's safety: a
mistake in it is a refused kernel or a wrong picture, never a write
somewhere else.

## Decisions, Diego's - taken on 18 October 2026: the four recommended

Diego: "go ahead with the recommended choices for C5" - **E1 A**, a kernel
language of Kosmos's own compiled on Kosmos; **E2**, Preview first; **E3**,
an emulator of the execution units on the Mac; **E4**, a textured object in
its base colour until R7. Recorded in `README.md`'s decision log.

**E1 - how the ray tracer's kernel is written.**

- **A - Kosmos's own kernel language, compiled on Kosmos** *(recommended)*.
  The step `compute.md` D1 C named "a later step on top of A". The tracer is
  written once, C-shaped, and every later kernel - a video filter, a sound
  effect, a simulation - is written the same way, in the IDE, by anyone.
  It costs a compiler, a few thousand lines of Lua with its tests, and an
  emulator to test it with. The driver's check stays the safety: whatever
  the compiler writes is checked.
- **B - Mesa's own compiler, run on the Mac**, from OpenCL C (`intel_clc`).
  This is the industrial compiler and the best code, but it needs LLVM and
  the SPIR-V tools built on the Mac. Kernels could not be written on Kosmos,
  as D1 B was not chosen for. Its kernels address memory with 64-bit
  pointers and spill to stateless scratch, which the check refuses (D2 A),
  so it would need a GPU address space for each program (D2 B) first, a
  large driver step of its own.
- **C - by hand, in assembly, a smaller tracer**: exact shapes only, no
  meshes, Preview only, about a thousand instructions kept by hand. It is
  the quickest to a first picture and covers a fraction of the scenes. The
  rest would fall back to the processor, and every change would be made in
  assembly.

**E2 - what comes first.** Preview first, then Final *(recommended)*.
Preview is deterministic, so its picture can be held to the processor's
pixel for pixel, which finds the tracer's mistakes. Final's random paths can
only be held statistically. Or both at once.

**E3 - testing without a GPU.** An emulator of the execution units on the
Mac *(recommended)*. Without one, every kernel is tested only on the M700 by
hand, and the gate - which runs in QEMU, with no GPU - would check nothing
the GPU does. With it, the tracer's kernel is run on small scenes in `make
test` and held to `k3d_trace.c`'s picture.

**E4 - scenes with image textures before R7.** Drawn in the object's base
colour on the GPU, with a line saying so, *(recommended)*; or the GPU
renderer refused for such a scene until R7.

## The busiest path

**A pass of the Rendered view at 1280 by 720, Preview, on the GPU**: the
processor writes the camera and the pass number into the constants and
asks for one run of 160 by 720 thread groups, eight pixels each. That is
one message. The GPU traces 921,600 rays and their shadow rays, walking the
hierarchy's nodes from a buffer, and adds each pixel's colour into the float
buffer. A second run turns the sums into colours in the picture the view
shows. **The processor's part is two messages and a paint.**

The GPU's time is unknown until it is measured. The estimate from the M700's
24 execution units, against the processor's four cores, is several times as
fast for Preview, and less for Final, whose paths diverge. The first run will
replace the estimate, which is the point of showing the two times.

## Steps

Each is tested where it lands, on the Mac by the emulator and in QEMU by
the suites, and then on the M700.

1. **R1 - `eu.lua` and the check learn what a tracer needs.** The structured
   branches `if`, `else`, `endif`, `while`, `break` and `cont`, each target
   held inside the kernel; `math`'s forms; the untyped reads and writes.
   Every new form is held to Mesa's gentool in the corpus.
2. **R2 - the emulator**, in C on the Mac: the subset run lane by lane,
   with masks, flags and the sends to buffers. It is held to the GPU's
   answers already known - the fill, the Mandelbrot against `fractal.c`,
   the lesson's pattern - and then to the M700's.
3. **R3 - the language**, `kc.lua`: lexer, parser, checks, the lanes'
   control flow, registers, and `eu.lua` text out. Its tests compile
   programs and run them in the emulator.
4. **R4 - the scene in buffers**, `k3d_gpu.c`: what `k3d_render_new`
   builds, flattened to arrays, held on the Mac to the processor's
   structures.
5. **R5 - Preview on the GPU**: the tracer in the language, held in the
   emulator to `k3d_trace.c`'s Preview on small scenes, then on the M700 to
   the full scene.
6. **R6 - Cafesa3D**: the Renderer choice, the times side by side, and
   the comparison line; its suite under QEMU, where GPU says why it cannot
   be chosen.
7. **R7 - Final on the GPU**, path tracing, and image textures.

Only x86-64 runs the tests for now (`CLAUDE.md`), and the GPU itself is
the M700's.
