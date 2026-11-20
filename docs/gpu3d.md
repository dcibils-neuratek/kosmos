<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# A 3D API for Kosmos - which one, and how it reaches the GPU

Written on 17 November 2026, before any of it is built. Diego, the same
day: "we need to implement a 3d api like vulkan, opengl es or other one to
be able to run 3d programs in gpu. analyze and recommend the best move
forward". **This is the document to decide from, not a start on the work.**
`gpu3d-architecture.png` (from `gpu3d-architecture.html`) is the picture.
Where a number has a source it says so; where a figure is an estimate it is
labelled as one.

---

## In short

- **Vulkan is the contract, and Mesa supplies it.** Every GPU Kosmos has to
  meet - the M700's Gen9, the T14's Gen12, the Mac Pro's GCN 1.0, the Pi 5's
  VideoCore VII, and the processor itself - has a maintained, conformant
  Vulkan driver in Mesa: `anv`, `radv`, `v3dv` and `lavapipe`. **OpenGL and
  OpenGL ES come on top of it, from Mesa's Zink**, so there is one GL
  implementation for every machine rather than one per GPU, and WebGL,
  Skia, SDL, ImGui, Godot and old GL ports all find what they ask for.
- **Mesa is ported, not rewritten, and it meets Kosmos at Mesa's own seam.**
  `anv` already talks to its kernel driver through a table of ten
  functions (`anv_kmd_backend.h`), which is how Intel added its new Xe
  kernel driver beside i915. i915's half is 2,255 lines and Xe's 1,568
  (Mesa 26.2.5, already on this Mac in `build/downloads/mesa` for
  `gentool`). **A third half, Kosmos's, speaks a declared protocol to
  `intelgpu`** - no DRM shim, no ioctl, no i915 interface brought into the
  system.
- **Software first, in QEMU, then the M700.** `lavapipe` (Vulkan) and Zink
  over it give the whole API on every machine before any hardware driver -
  so the gate tests the API, the window path, GL, and WebGL in JetSki, all
  in QEMU, which is the only active target. Then on the M700 only the
  bottom changes: `anv` over `intelgpu`.
- **`intelgpu` has to grow the one thing it was never built for: many
  programs on the GPU at once, each in memory of its own.** Today it runs
  one legacy ring with every buffer in the global GTT, and checks each
  compute kernel instruction by instruction to keep it in its buffers
  (`compute.md` D2 A). Mesa's shaders address memory by pointer, and `anv`
  refuses a GPU address space under 4 GB. So the driver grows **execlists,
  a hardware context per program, and a GPU page table per context
  (PPGTT)** - D2's option B, which `compute.md` called "the right one in
  the end".
- **Nebula needs nothing new.** Regions are buffer objects, a region's
  capability is how a buffer is shared, pulses carry fences, and
  `MEM_MAP_RX` - built for JavaScriptCore's JIT - is what `lavapipe`'s
  LLVM needs to run the shaders it compiles. The open question is an IOMMU,
  as it is for storage.

---

## 1. What exists

**What draws in 3D today: four rasterisers on the processor, a GPU reached
only by hand-written kernels, and an EGL that says there is no GPU.**

| What | Where | What it is |
|---|---|---|
| The GL Kit | `user/kits/gl/` | **TinyGL**, OpenGL 1.1 with a fixed pipeline, in software; `kosmos_gl_run` puts a TinyGL program in a window; eight of TinyGL's demos (`glgears`, `glteapot` and the rest) and `cube3d` |
| The EGL Kit | `user/kits/egl/egl_kosmos.c` | **an EGL with no display** (`w6.md` W6b, Diego 13 November): every call answers "there is none", so WebKit paints with Skia on the processor; its GL ES functions stop the process if reached. Written to be the door a GPU API later fills |
| The 3D Kit | `user/kits/3d/` | Cafesa3D's engine: formats, meshes, a rasteriser for Solid and Wireframe (`k3d_raster.c`), the ray tracer (`k3d_trace.c`) and its GPU twin's scene packing (`k3d_gpu.c`) |
| The Game Kit | `user/kits/game/` | lines, fills, spheres and rings into a surface (`gamesoft.c`), for the planet simulation |
| The Compute Kit | `user/kits/compute/` | programs of one's own on the M700's execution units: `eu.lua` assembly or the `kc.lua` language, checked by the driver, run by `GPU_OP_RUN` |
| Quake, Doom | `user/installed/` | their own software renderers |

**Nothing is Mesa-derived** except tools on the Mac: `gentool`, Mesa's
assembler, holds `eu.lua` to it bit for bit, and `tools/render_batch.c`
builds the render engine's state from Mesa's `genxml`.

**The M700's driver, `intelgpu`** (`user/drivers/display/intelgpu.c`,
3,463 lines; `user/include/gpuproto.h`), a process at `/Devices/gpu`:

- the display engine: the cursor plane, GPU buffers in the global GTT, page
  flips (`m700-2d.md` steps 3-4);
- forcewake, and the **legacy ring** on the blitter and the render engine,
  fenced by `MI_FLUSH_DW` and `PIPE_CONTROL` (steps 5, 7a; `testing.md`
  18.666, 18.680);
- the blitter's copy to the screen (6a), the render engine's 3D pipeline
  drawing and blending rectangles from textures (7c, 18.682-18.685) - **GPU
  composing tried, not yet choosable** (`state.md`; `rendering.md` R3);
- the GPGPU pipeline: kernels checked instruction by instruction, runs and
  lists of runs, timed by the GPU's clock (`compute.md`, `cafesa3d-gpu.md`).

What it does not have, and what every 3D API needs: **more than one
hardware context** (one ring, one state for everyone), **a GPU address
space per program** (everything shares the global GTT, which is why
kernels are checked), **execlists**, preemption, and recovery from a hung
batch.

**The machines** (`m700-gpu.md`, `thinkpad.md`, `xavier.md`, `hal.md`):

| Machine | GPU | Mesa's drivers | Its kernel half in Linux |
|---|---|---|---|
| QEMU `virt`, q35 | virtio-gpu, **2D only** on this Mac's QEMU 11.1.1 (no virglrenderer; tried 2 October and paused, `roadmap.md` 4h d) | `lavapipe`, `llvmpipe`; `venus` and `virgl` if the host had them | none |
| M700 | Intel HD 530, Gen9 GT2, `8086:1912`, 24 EUs | `anv` (Vulkan 1.3), `iris` (GL 4.6, GL ES 3.2) | i915 |
| ThinkPad T14 | Intel Iris Xe, Gen12 Tiger Lake, `8086:9A49` | `anv`, `iris` | i915 or Xe |
| Mac Pro 2013 | two AMD FirePro, GCN 1.0 (Tahiti / Pitcairn) | `radv`, `radeonsi` | amdgpu, with firmware for each engine |
| Raspberry Pi 5 | VideoCore VII | `v3dv` (Vulkan 1.3), `v3d` (GL ES 3.1) | v3d, small |

**And the consumers waiting**: WebGL and Skia's GPU backend in JetSki
(WebKit's ANGLE is in the tree, with its Vulkan backend's 146 files,
`build/webkit/src/Source/ThirdParty/ANGLE/src/libANGLE/renderer/vulkan`);
Astra's GPU compositor (`m700-2d.md` 7d); Cafesa3D's Solid view; Quake and
GL ports; the IDE's GL lessons.

## 2. The question, split in two

"Which 3D API" is two questions with different answers:

1. **Which API do programs see?** That is about what ports and programs
   ask for, and about what Kosmos's own programs, Lua among them, can use.
2. **Which API is the contract every GPU driver must supply?** That is
   about how many drivers there are to write or port, and how much of the
   rest is shared.

Linux, Fuchsia, Android and Haiku's recent work have all converged on the
same answer to the second: **Vulkan**, with GL layered above it where a
native GL driver is not worth having. The first question then has an
answer of "both": Vulkan for what wants it, GL and GL ES for everything
else.

## 3. The options compared

### The API programs see, and the contract beneath

| Option | What programs get | Drivers to supply per GPU | Who wants it | Effort | Verdict |
|---|---|---|---|---|---|
| **A. Vulkan underneath, GL and GL ES by Zink** | Vulkan 1.3; GL 4.6 and GL ES 3.2 (Zink's levels on a capable driver) | **one**: the Vulkan driver | everyone: games, SDL, Godot, ImGui, Skia (Ganesh GL and Vulkan, Graphite Vulkan), WebKit's ANGLE, Blender 4's Vulkan | the KMD seam once per GPU family | **recommended** |
| B. GL ES first by `iris`, Vulkan later | GL 4.6 and GL ES 3.2 on Intel only | two per GPU in the end (`iris` and `anv`, `radeonsi` and `radv`) | GL ports, WebKit | a little less at first, more in total | second best |
| C. Vulkan only, GL by ANGLE | Vulkan; GL ES 3.x only, no desktop GL | one | Fuchsia's choice; WebKit and Chromium | as A, minus Zink | narrower: no desktop GL for old ports |
| D. A Kosmos-native API (a "3D Kit", Metal-like) as the contract | whatever we design | **a driver per GPU written by us**, with a shader compiler | Kosmos's own programs only; every port needs a translation layer | years | not as the contract; as a door above Vulkan, yes |
| E. WebGPU (`webgpu.h`, Dawn or wgpu) as the contract | WebGPU | it sits *on* Vulkan, so still a Vulkan driver | JetSki's WebGPU, a few new programs | Dawn is Chromium-sized C++; wgpu is Rust, which Kosmos has no toolchain for | a library above Vulkan later, for JetSki |

**Why A over B.** `iris` would be faster on Intel than Zink over `anv` -
how much is unknown on Gen9 and has to be measured, not assumed - and it
reaches GL without Vulkan. But Kosmos meets four GPU families, and B means
two drivers on each: two kernel halves, two sets of bugs, two things to
keep in step. With A there is one driver per family, and GL is the same
code on every machine, the processor included, so a GL bug found under
QEMU is the GL bug on the M700. And `iris` remains available later on the
same seam (it has its own `iris_kmd_backend.h`) if the measurement says
Zink costs too much - an addition, not a change of direction.

**Why A over C.** ANGLE gives GL ES only. Quake's GL ports, the Mesa demos
the IDE would teach from, SDL games written to desktop GL and anything old
want desktop GL with the compatibility profile. Zink gives both - and
ANGLE stays where WebKit already uses it, for WebGL (§5.6).

**Why not D as the contract.** A GPU API is the easy half; a driver is a
shader compiler and the hardware's whole state model. Mesa's Intel
compiler alone is 211,382 lines, NIR under it 152,544 (counted in Mesa
26.2.5). Kosmos already wrote a compiler of its own for compute kernels
(`kc.lua`) and found on the M700 that the GPU did not beat the processor at
ray tracing (`cafesa3d-gpu.md` R8); a graphics compiler that matches Mesa's
is not a project for one person. **What D is right for is a door**: the
part BeOS's OpenGL Kit was - `BGLView`, a GL context in a window - which is
Kosmos glue over a standard API, not a new API (§5.1).

**Why not E as the contract.** WebGPU's implementations are themselves
layers over Vulkan, Metal and D3D12; choosing it still needs a Vulkan
driver underneath, and adds a large library on top. It belongs above
Vulkan, when JetSki wants WebGPU.

### How the driver code arrives

| Path | Mesa modified? | What crosses into the system | Precedent | Verdict |
|---|---|---|---|---|
| **1. Mesa, with a Kosmos KMD backend** | a backend added at Mesa's seam, in files of our own, plus a build patch | a **declared protocol** (`gpuproto.h`): contexts, binds, submissions, fences | Fuchsia (`anv` and Magma), Intel's own Xe backend | **recommended** |
| 2. Mesa unmodified over a DRM shim | no | **i915's uAPI**: GEM handles, `execbuffer2`, syncobj, about twenty ioctls' structures, mapped to messages | Haiku's RadeonGfx: its own `libdrm.so` over IPC to a GPU server in userland, `libdrm_amdgpu` left as is | works, but Linux's shape becomes the server's protocol |
| 3. Our own driver, Mesa's compiler borrowed | the compiler only | ours | none at this scale | the compiler is the easy part to borrow and the state code is the hard part to write |
| 4. Our own driver and compiler | - | ours | - | years; see D |

**The seam decides path 1.** A DRM shim keeps Mesa untouched, which is
attractive, and Haiku shows it can work. Its price is that the GPU server
must then speak i915's interface - GEM handles, relocations, `execbuffer2`'s
flags, syncobj semantics - because that is what Mesa's half sends. It is a
fixed shape, so it does not break *a server receives exactly what it
expects*; but it is Linux's shape, with a decade of legacy in it, and it is
a different shape for each GPU family (amdgpu's is another twenty). The
KMD backend is narrower: ten functions - create, map, close a buffer;
bind and unbind it in the GPU's address space; execute command buffers
with waits and signals (§5.3). It is maintained upstream, because Intel
uses it for Xe, and it is exactly where Fuchsia put Magma.

`radv` has the same kind of seam (`radv_radeon_winsys`, today only an
amdgpu implementation), and `lavapipe` needs no kernel at all, only a
window to present into. `v3dv` calls its ioctls directly (18 sites in
`broadcom/vulkan`), so the Pi would need a thin shim or a small backend
patch; that is the Pi's question, not today's.

### What Fuchsia's Magma teaches

Magma is the closest relative Kosmos has: a microkernel, drivers in
userland, Mesa's `anv` in each application. Its lessons, from its own
documentation:

- **Two halves**: a client driver in each program's address space, and a
  *system driver* (MSD) in a privileged process that sets up memory spaces
  and hardware contexts, maps buffers, schedules command buffers, handles
  faults and power. That is `intelgpu` and the Kosmos backend.
- **IPC instead of ioctls** - a protocol of its own design.
- **Every client its own GPU address space**, so isolation holds on the GPU
  as it does on the processor. Without it, nothing stops one program's
  shader reading another's window.
- **Vulkan only underneath**, GL by translation.
- Its Intel system driver, `msd-intel-gen`, is about 9,000 lines and
  already lists `1912` (`m700-gpu.md` §5): the best map Kosmos has of
  execlists, contexts and PPGTT on this exact GPU.

## 4. The recommendation

**Vulkan as the contract every GPU driver supplies, from Mesa, reaching
`intelgpu` through a Kosmos KMD backend; GL and GL ES from Mesa's Zink on
top; `lavapipe` first so the whole API exists in QEMU before any hardware;
then `anv` on the M700.**

The main reason is **one driver per GPU, and that driver is someone
else's maintained, conformant code**. Kosmos's work is the parts that are
genuinely Kosmos's - the GPU server's contexts, memory and scheduling, the
protocol, the window path - and every one of those is written once and
used by every API above it. It is also the only order that keeps the
gate honest: with `lavapipe`, everything above the hardware - Vulkan, the
swapchain into a window, Zink's GL, WebGL in JetSki - is tested in QEMU in
every gate, and the M700 changes only the bottom layer.

It fits the principles without bending them:

- **Control by message, data by shared memory**: command buffers, vertex
  data and textures live in regions the program and the GPU share; a
  submission is one message saying where and how much.
- **Capabilities by index**: a buffer is a region, and reaching it is
  holding its capability; the GPU's own page table per context enforces the
  same thing on the GPU side.
- **No hardware addresses outside `hal/`**: as now, `intelgpu` is handed
  its BAR by the board.
- **Language split**: Mesa and the server are C and C++; Lua reaches the
  GPU through kits.
- **POSIX names inside a process only**: Mesa links against
  `runtime/posix/` (pthreads, `mmap`), and nothing POSIX crosses into the
  system. No ioctl exists anywhere in the path.

## 5. The architecture

### 5.1 The doors

- **The Vulkan Kit**, `#include <vulkan/vulkan.h>` from C, linked in.
  `vkCreateInstance` enumerates what `/Devices/gpu` offers and the
  processor's `lavapipe`, the way the loader would - but there is no
  loader and no `dlopen`: every Mesa driver is linked in and chosen at run
  time, since *nothing here is linked dynamically*. Its one Kosmos
  extension is the surface: **`VK_KOSMOS_surface`**, a swapchain into a
  Kosmos window. It was written `VK_KHR_kosmos_surface` here until it was
  built (G3, 0.12.7): an extension is named `KHR` only when Khronos ratifies
  it, so a platform's own takes its vendor's tag, as `VK_GGP_` and `VK_QNX_`
  do.
- **The GL Kit** keeps its name and its door and gains Mesa: GL 4.6
  compatibility and GL ES 3.2 by Zink, EGL with a Kosmos platform.
  `kosmos_gl_run` keeps its shape, so TinyGL's demos - which are GL 1.1
  programs - run on Mesa unchanged. From Lua, `use("/Kosmos/Kits/gl")`
  keeps its calls, now over a real GL.
- **The EGL Kit gets its display**, which is what it was written to wait
  for (`egl_kosmos.c`): `eglGetDisplay` answers, and WebKit and Skia
  take their GPU paths without being patched.
- **Lua does not get raw Vulkan.** A Vulkan program is a thousand lines of
  structures before a triangle, and none of that is the kind of code a Lua
  program is for. Lua's doors are GL, the 3D Kit's scene (Cafesa3D's Solid
  view drawn by Vulkan, later) and the Game Kit - each implemented in C on
  Vulkan. That is BeOS's OpenGL Kit idea: the Kit is the window and the
  context, the API is the standard one.

### 5.2 Where the driver code lives

In the program's own process, as on Linux and Fuchsia: Mesa's `anv` with
its compiler, ISL and the Vulkan runtime. It needs **no privilege**: it
writes commands into its own buffers and asks the server to run them.
Compiling a shader produces GPU instructions, which are data to the
processor, so `anv` needs no executable mapping. `lavapipe` does - LLVM
compiles each shader to processor code. **It needs no double mapping**,
measured in G1 (`testing.md` 18.921): LLVM's JITLink writes a function's
pages while they are readable and writable, then turns them readable and
executable with `mprotect` before anything runs - one mapping, never
writable and executable at once, which Nebula already allows. The two
mappings of one region are JavaScriptCore's (`jit.md`), because its JIT
patches code that is running; LLVM's never touches code it has finished.

Mesa's source is fetched, not committed, as WebKit's is - Mesa 26.2.5 is
already in `build/downloads/mesa` - and built by its own Meson with a
Kosmos cross file. What is Kosmos's own lives in the tree and is compiled
into that build: the KMD backend, the WSI backend, the EGL platform, a
`vk_sync` type, and the device query - perhaps 4,000-6,000 lines in all,
an estimate from the sizes of i915's and Xe's backends, Mesa's headless
WSI and Haiku's EGL platform.

### 5.3 The GPU server and its protocol

`intelgpu` remains the one process that owns the GPU. It grows a 3D half,
in `gpuproto.h`, declared as every Kosmos protocol is - fixed fields, fixed
counts:

| Verb | What it does | Mesa's hook it answers |
|---|---|---|
| `GPU_OP_QUERY` | the device: its ID, its EUs and slices, the timestamp's rate, the GTT size | what i915's query ioctl tells `intel_device_info` |
| `GPU_OP_CONTEXT_NEW` / `_FORGET` | a hardware context with a page table of its own, a priority band | `vkCreateDevice` |
| `GPU_OP_BIND` / `GPU_OP_UNBIND` | a region (its capability handed with the message) entered in the context's page table at a GPU address the client chose, with caching | `vm_bind_bo`, `vm_unbind_bo`, `vm_bind` |
| `GPU_OP_EXEC` | run a batch at a GPU address and length in a context, after fixed-count waits, signalling a timeline value | `queue_exec_locked`, `queue_exec_async` |
| `GPU_OP_FENCE_WAIT` | send a pulse when a timeline reaches a value | `vk_sync` waits |

**Buffers are regions.** `gem_create` makes a region, `gem_mmap` maps it,
`gem_close` lets it go. The server holds a region's capability while it is
bound, so its pages cannot go while the GPU might still use them - the
same rule that keeps every shared region alive. Sharing a buffer with
another process, Linux's dma-buf, is handing its capability on.

**The client chooses its GPU addresses** (`anv` does, "softpin"), and the
server only checks that they are in range and enters the pages. A context
with its own page table reaches only what was bound in it - the
capability rule, one layer down - so the instruction check that
`compute.md` D2 A needed becomes a second guard rather than the only one.

**Command buffers are never messages.** The batch is written by Mesa into
a region bound in the context; `GPU_OP_EXEC` says where. Run as a
non-privileged batch in a per-context address space, the hardware itself
refuses global-GTT access and privileged registers, which is the model
Linux trusts on Gen9's render engine.

### 5.4 Fences and synchronisation

Each context has a **fence page**: the GPU writes the timeline's value
into it at the end of each batch (`PIPE_CONTROL` with a post-sync write,
as `intelgpu`'s fences already do) and raises `MI_USER_INTERRUPT`. The
client maps the page read-only and checks a value without asking anybody;
to sleep, it asks for a **pulse** when the value is reached, and the
server's interrupt thread sends it (`pulses.md`, `ipc_pulse` in Nebula).
That is a Vulkan timeline semaphore made of Kosmos's own pieces, and it is
`vk_sync`'s interface: Mesa's runtime makes binary semaphores and fences
from timelines.

Across processes - a swapchain image handed to the window manager - the
fence travels as (context, value), and the window manager either checks
the page or composes the image's previous frame until it is reached.
**Nothing waits on hardware**: a present is answered at once.

### 5.5 To the screen

- **With `lavapipe`**, a swapchain image *is* a shared surface: linear,
  in a region, its pitch the surface's, and presenting is the message the
  window manager already takes - "buffer 2 is live" - which is why
  `cube3d` holds 60 frames a second today. No copy. **As built in G3 it
  copies once**, and that is a departure: a Kosmos window's buffer begins
  under its title band, at an offset no page boundary falls on, so it
  cannot be handed to lavapipe as an image's memory. Swapchain images are
  Mesa's own CPU images, and a present copies their rows into the window's
  back buffer and commits it - 64 by 64 pixels, 16 KB a frame, for the
  test's window. A window whose pixels start on a page is the zero-copy
  path, and it is in the roadmap.
- **With `anv`**, swapchain images are GPU buffers (`GPU_OP_BUFFER_NEW`).
  The processor's compositor reading them is slow - write-combining or
  uncached reads, and tiled layouts it cannot read at all - so the
  present path is **the GPU compositor** (`m700-2d.md` 7d) sampling them as
  textures, or for a full-screen program **a flip** of its own image
  (step 4c), no copy at all. Until 7d is choosable, a linear image copied
  by the blitter into the window's surface is the fallback, and its cost
  is measured.
- **Astra's compositor itself** can be a Vulkan program later, on the same
  driver as everything else; whether it moves, or keeps the driver's own
  `draw`, is Diego's (Yours to decide, 6).

### 5.6 JetSki

With the EGL Kit answering, WebKit's ANGLE - already the WebGL
implementation on every WebKit port - runs over Mesa's GL ES, as on Linux,
and Skia's Ganesh GL backend becomes available. WebGL in JetSki is then a
build flag and a test, not a project. ANGLE's own Vulkan backend is the
alternative, one layer fewer, and is the question for when it is measured.
WebGPU later sits on Vulkan through Dawn.

### 5.7 Sharing the GPU, and staying responsive

The processor's scheduler is bands with immediate preemption; the GPU's
should be the same idea. The server keeps a queue per context and a
**priority band per context**: Astra's compositor above every application,
interactive programs above background work (a Final render, a compute
run). Execlists submit two contexts at a time and let the driver preempt;
Gen9 preempts at command, draw and thread-group boundaries, by the PRM's
account (to be checked against Vol 7 before relying on it), so a long draw
call is the bound and a frame's composing waits at most that.

**A batch that never ends** - a shader in an infinite loop - is the GPU's
version of a process spinning. The server times each batch; past a bound
it resets the engine, marks the context lost (`VK_ERROR_DEVICE_LOST` for
that program) and goes on with the others. One program's hang must never
be the desktop's.

### 5.8 What Nebula adds

**Nothing new for any step below**, which is the point of putting GPU
memory in regions:

| Need | What already supplies it |
|---|---|
| buffer objects, shared and kept alive | regions, held by capability |
| the GPU's view of a buffer's pages | `SYS_MEM_PHYS`, a page at a time - the GPU's page tables do not need contiguous memory |
| write-combining and cached mappings | `MEM_WRITE_COMBINE` and the default; Gen9 shares the last-level cache, so most buffers can stay cached |
| the device's registers and GTT | `SYS_DEV_MAP`, its bound raised to 16 MB in October |
| the GPU's interrupt | an interrupt capability, MSI on the PC |
| a fence's wake-up | pulses |
| `lavapipe`'s compiled shaders | `mprotect`, read-write then read-execute, one mapping (G1) |

Two things to watch rather than add: a region given on touch
(`MEM_ON_TOUCH`) has no pages until touched, so binding must populate it
first; and GPU memory should count against the process that made it, so
GPU Monitor can say who holds what. **The open question is the IOMMU.**
Without VT-d, a mistake in the server's page tables is a device writing
anywhere in memory - the clients cannot cause it, their batches run in
their own address spaces, but the server can. That is storage's question
too (`roadmap.md`, *storage at full speed* 4), and the recommendation is
the same: live without it for now, and treat the page-table code as the
most carefully tested in the driver.

### 5.9 The busiest path

**A frame of a Vulkan program on the M700**: the program records its
command buffer - Mesa writes the batch into a region, in its own process;
`vkQueueSubmit` is **one message**, `GPU_OP_EXEC`; the server writes the
context to the submission port and returns; the GPU draws into the
swapchain image and writes the fence; its interrupt becomes a pulse;
`vkQueuePresentKHR` is **one message** to the window manager, the image
and its fence; the GPU compositor samples it at the next refresh. The
processor's part of a frame: recording, two messages and a pulse. Where
the time goes is the GPU's, and the first figures to take are
submit-to-fence latency (the legacy ring's was 5 us, 18.680) and frames a
second on `vkcube` and `glgears` at 1920 by 1080.

Under `lavapipe` the same frame is LLVM-compiled shaders rasterising on
every core into the shared surface, and one message to present.

## 6. The steps

**Scheduled on 18 November 2026**: G1 to G5 straight after the cleanup
that ends at 0.12.0, before the daily-driver items; G6 to G9 after those
(`roadmap.md`, *The order agreed on 18 November 2026*). Diego: "Yes do the
3d api work after the cleanup is done".

Each is a revision with a test that stays and fits the 5-10 minute gate.
Only x86-64 suites run for now; the AArch64 build must still compile, and
so `lavapipe` is built for both.

1. **G1 - Mesa and LLVM build for Kosmos.** A Meson cross file, Mesa's
   `util`, NIR and the Vulkan runtime against `runtime/posix/`; LLVM's
   libraries for x86-64 and AArch64 code generation, with an ORC JIT.
   *Test* `x86-llvm`: a function compiled at run time in QEMU returns its
   answer. *Measure*: the build's time, once, cached like WebKit's; the
   image's size. **LLVM's half built in 0.12.1** (`testing.md` 18.921):
   LLVM 23.1.2 with seven patches in `runtime/patches/llvm/`, its JIT on
   JITLink's own memory manager, `setUpInactivePlatform` and no process
   symbols, since Kosmos has no `dlopen`. **Mesa's half built in 0.12.2**
   (`testing.md` 18.922): Mesa 26.2.5 by its own Meson, configured for
   `lavapipe` with two patches in `runtime/patches/mesa/`, its trial links
   answered by `tools/mesa_trial_ld.py`; `x86-mesa` has NIR build, fold,
   print and serialize a shader. G1 is done.
2. **G2 - `lavapipe`, headless.** Vulkan in QEMU: a triangle and a textured
   cube rendered to an image, read back, held to a reference picture.
   *Test* `x86-vulkan`: the pictures, and a short run of the Khronos
   conformance suite (dEQP-VK) chosen to fit a minute. **The triangle
   drawn in 0.12.3** (`testing.md` 18.923): `lavapipe` linked in with no
   loader, patch 0003 for what it reaches for, and `rtasm`'s writable and
   executable code left out. **The textured cube in 0.12.4** (18.924): a
   vertex buffer, a matrix, a texture and a depth buffer, held to a ray cast
   through every pixel. dEQP-VK next: the suite writes every message
   through a C++ string stream, so libc++'s locales were turned on for it
   (Diego, 19 November 2026; `testing.md` 18.925). **And the suite in
   0.12.6** (18.926): VK-GL-CTS 1.4.7.1 built by its own CMake for a
   target of Kosmos's (`tools/cts_build.py`, `runtime/ports/cts/`), its
   `info` and `api` groups in `vkcts.elf` with lavapipe, and a minute of
   them in `x86-vulkan-cts`. It found four things the Vulkan loader
   answers on Linux and nothing answered here - a layer that is not
   there, a device that is none, layers counted into an empty array - and
   lavapipe listing extensions without what they depend on and counting
   the machine's memory as the process's; Mesa's patch 0004 answers each.
   G2 is done.
3. **G3 - a window.** `VK_KOSMOS_surface`, swapchain images as shared
   surfaces, presenting as `cube3d` does. *Demo*: `vkcube`. *Test*: the
   picture QEMU scans out, as `make screenshot` checks; frames a second
   under `-icount` as a regression number only.
4. **G4 - GL by Zink, EGL with a display.** The GL Kit on Mesa, TinyGL
   retired (Yours to decide, 5). *Demos*: `glgears` and the other seven,
   unchanged; `cube3d`; Mesa's GL ES 2 gears. *Test*: their pictures, and
   a dEQP-GLES subset.
5. **G5 - WebGL in JetSki.** ANGLE over the EGL Kit, `ENABLE_WEBGL` on.
   *Test*: a page drawing a WebGL triangle, held to its picture, in the
   browser's suite. Skia's GPU path stays a separate decision, measured.
6. **G6 - `intelgpu` for many programs.** Execlists in place of the legacy
   ring, a hardware context per client, a 48-bit page table per context,
   the 3D verbs, fence pages and pulses, the hang timer and engine reset.
   *Test on the Mac*: page-table entries and context images held to the
   PRM and `genxml`, as `fat_decode.c` is held to a disk; *under QEMU*: the
   protocol against a stand-in engine that runs only `MI_` commands, so
   the gate holds the protocol where there is no GPU; *on the M700*: two
   processes each storing to its own buffer at the same addresses, neither
   seeing the other's, and a deliberate hang recovered. The Compute Kit
   moves onto contexts here.

   **G6 in steps** (19 November 2026, from Linux 6.12's i915 -
   `intel_lrc.c`, `intel_execlists_submission.c`, `gen8_ppgtt.c`,
   `intel_gtt.c` - fetched into `build/downloads/i915/`). Each step proves
   one thing, and the first engine moved is the one nobody uses:
   - **G6a - contexts and page tables, on the Mac** - built in 0.12.8,
     with G6b and G6c (`testing.md` 18.928). `intelgpu_lrc.c`: a
     Gen9 context image laid out from i915's own tables of the registers
     each engine saves (`gen9_rcs_offsets`, `gen9_xcs_offsets`), filled as
     i915 fills a new one - context control with restore inhibited, the
     ring, the page table's root, `STOP_RING` off - its descriptor, the
     context status buffer's events read, and a four-level 48-bit page
     table (PML4, PDP, PD, PT) built over pages it is handed, each entry
     present, writable and of a private-PAT index. *Test*:
     `test_intelgpu_lrc`, the image held to `intel_lrc_reg.h`'s fixed
     indices - which are constants, not computed - the descriptor to its
     bits, and every mapped address walked back through the four levels
     to its page.
   - **G6b - one context on the video engine, on the M700.** VCS0 at
     0x12000 is idle on the desktop, so execlists are turned on there and
     nowhere else: run-list mode, a status page, one context with a page
     table of its own, a ring of `MI_STORE_DWORD_IMM` to a GPU address only
     its page table maps, and its completion seen in the status buffer.
     The render engine and the blitter stay on their rings, so the desktop
     draws as before whatever happens. `gpu el` over Telnet.
   - **G6c - two contexts, one address.** Two contexts each with its own
     page table, the same GPU address in both mapped to different pages;
     each writes its own value, and neither page holds the other's.
   - **G6d - the render engine and the blitter on execlists** - built in
     0.12.9 (`testing.md` 18.929). The desktop's compositing and copies
     run in two contexts of the driver's own, one an engine, through a
     page table that mirrors the global GTT entry for entry (i915's
     aliasing PPGTT): every entry `gtt_write` makes is made there too,
     and the firmware's framebuffer is copied in once, so every address
     the driver already gives means the same page either way. A context
     is submitted only while its engine is idle - its image laid out
     afresh, restore inhibited, from where the last run's ring ended -
     and work given while it runs goes when the status buffer says it
     went idle: from any wait, or from the driver's loop, which comes
     back within a tick while an engine is busy. A request begins with
     the engine's TLB invalidated when the mirror changed. **The hang
     watch**: an engine whose fence has not moved for `GPU_HANG_MS`,
     3 s, is reset as i915 resets one on Gen9 - the ring stopped,
     `RING_RESET_CTL`, `GDRST` twice, execlists on again - its work
     dropped and its fence written as reached. The legacy ring is gone.
     *Test*: every engine, copy, 3D, compute and run check on the M700
     as before, and `gpu hang` / `gpu hang render` - a batch that jumps
     to itself, reset by the watch, a store after; *control*, the reset
     without `GDRST`, after which the engine stays stuck.
   - **G6e - the protocol** - built in 0.12.10 (`testing.md` 18.930).
     `GPU_OP_CONTEXT_NEW`, `_FORGET`, `_BIND`, `_UNBIND`, `_EXEC`,
     `_FENCE_WAIT` and `_CONTEXT_STATE` (§5.3), in one declared shape,
     `struct gpu_ctx_request`, for many programs at once.
     - **The core** is `intelgpu_ctx.c` and touches no hardware: a
       context's bindings, its timeline, its queue of work whose waits are
       not met, and the pulses it owes.
     - **An engine is a backend.** On the M700 each context gets its own
       ring, image, fence page and page table, taking turns after the
       driver's own work. Elsewhere, the **stand-in** runs a batch's `MI_`
       commands on the processor through the same page table.
     - **Kept across runs**: an image is restored when another context ran
       in between, and its restore is inhibited when the same one ran last,
       since the engine still holds its state. Restoring the render
       engine's 22 pages cost 88 us a submission.
     - **The default state**: a new program's context begins from the
       engine's default state, recorded once as the engine starts (i915's
       `default_state`). Without it, the M700 showed one program's
       registers in the next one's first run.
     - **The fence page** is write-back, so it can be handed read-only.
     - **Pulses** come from the driver's loop, a tick at most after the
       value is reached; the engines' interrupt is owed.
7. **G7 - `anv` on the M700.** `anv_kosmos.c`, the backend; `vkcube`, then
   a dEQP-VK subset run on the machine by a program that says what passed.
   *Measure*: submit-to-fence latency, `vkcube` frames a second.

   **G7 in steps** (19 November 2026, after a trial build of Mesa 26.2.5
   with `-Dvulkan-drivers=swrast,intel`). What the trial found:
   - **Most of Intel's half builds for Kosmos as it is.** ISL, BLORP,
     the generated `genxml`, NIR's Intel passes and Intel's internal
     shaders compiled unchanged. The rest failed in a few bounded kinds:
     the i915 and Xe code, which is DRM `ioctl`s; one DRM-only field in
     `vk_image`; `os_mman.h` and `util/futex.h`, which name POSIX and
     Linux; two `errno` values (`ECANCELED`, `ENOBUFS`) and `imaxabs`,
     which the libc lacks; and `anv`'s build-id, which a static image
     has none of.
   - **Two tools run on the Mac during the build.** `anv` carries
     kernels written in OpenCL C - its query copies, indirect draws and
     BLORP's - which Mesa compiles to SPIR-V with `mesa_clc` and turns
     into NIR-building C with `vtn_bindgen2`. A cross build uses copies
     of those built natively (`-Dmesa-clc=system`), against Homebrew's
     LLVM 23, SPIRV-LLVM-Translator 23.1.2 and SPIRV-Tools, all three
     already on the Mac.
   - **The kernel driver is a type of its own, `INTEL_KMD_TYPE_KOSMOS`**,
     beside i915 and Xe, in directories of its own (`dev/kosmos/`,
     `common/kosmos/`, `vulkan/kosmos/`). **i915's and Xe's code stays in
     the build**, compiled against the refused `ioctl` - which answers
     `ENOTTY` and is never reached, since every choice by the type sends a
     Kosmos device to Kosmos's code - as SQLite's and OpenSSL's are
     (`runtime/posix/refused/sys/ioctl.h`). Leaving it out would have meant
     editing every file that names it, for no gain. Twelve shared files
     choose by the type, and Kosmos takes Xe's side of most choices: no
     relocations, addresses chosen by `anv` and bound, one address space
     per process.
   - **The device is found by asking, not by opening.** `anv` enumerates
     DRM nodes; on Kosmos it asks for `/Devices/gpu` (Mesa's `enumerate`
     hook), and a program granted no GPU, or on a machine whose GPU the
     driver does not know, is told there is no device. The capability is
     held for the life of the process and never closed, by the physical
     device or the logical one.
   - **A `vk_sync` is a point, and timelines are Mesa's emulation over
     it.** A point is `{context, value}` once a submission will signal
     it; the processor reads its context's fence page to know whether it
     is reached, and a thread that must wait makes an endpoint, asks for
     `GPU_OP_FENCE_WAIT`'s pulse for each point, and receives with its
     deadline. A point signalled or submitted on the processor pulses
     every endpoint waiting at that moment, so no wait is a loop on a
     clock.

   The steps:
   - **G7a - `anv` built, and the device described.** The Kosmos type's
     device info: `8086:1912` from `/Devices/gpu`, Mesa's own table for
     it, and the topology and memory the driver reports. *Test under
     QEMU*: an image with `anv` linked that enumerates no Intel device
     where there is none, without an error, and Intel's compiler
     compiling a SPIR-V compute shader to Gen9 instructions in a Kosmos
     process. *On the M700*: the device listed as Mesa names it.
     **Built in 0.12.11** (`testing.md` 18.931). `lavapipe` is not beside it: one image cannot link both, since each
     carries Mesa's runtime and `vk_icdGetInstanceProcAddr`, so
     `anvtest.elf` is an image of its own beside `vulkantest.elf` - and a
     loader choosing between them is what G7c's program will need.
   - **G7b - memory and submission.** A buffer object is a region,
     mapped for the processor and bound in the program's context at the
     address `anv` chose (`GPU_OP_BIND`). A queue is a context, and
     `queue_exec` hands it the first batch of the chain (`GPU_OP_EXEC`).
     A `vk_sync` is a point on a context's timeline: met when the fence
     page says so, waited for by `GPU_OP_FENCE_WAIT`'s pulse. *Test on
     the M700*: `vkCmdFillBuffer`, then a compute dispatch writing a
     buffer, read back right.
     **Built in 0.12.12** (`testing.md` 18.932): one context a device,
     since `anv` binds before any queue exists; a slab's buffers share
     one mapping of their region; unbinding waits for the context to go
     idle. On the M700 both halves of a 64 KB buffer read back right,
     the fill in 3-4 ms and the dispatch in 6.5 ms, recording to answer.
   - **G7c - `vkcube` in a window**, its swapchain images rendered by
     the GPU and presented through `VK_KOSMOS_surface` (G3). *Measure*:
     frames a second.
     **Built in 0.12.13** (`testing.md` 18.933): `vulkantest`'s cube on
     `anv`, its picture as the reference pixel for pixel, and 300 frames
     presented in a window found on the screen by VNC. The swapchain is
     Mesa's buffer blit, the GPU's tiled image copied into a linear buffer
     the processor reads, and the backend waits on its fence before
     copying. 66-127 frames a second at 64 by 64 - the round trip, not the
     GPU; a large window's number is owed.
   - **G7d - dEQP-VK on the M700**, the subset `x86-vulkan-cts` runs on
     `lavapipe`, by a program that says what passed. *Measure*:
     submit-to-fence latency.
     **Built in 0.12.14** (`testing.md` 18.934): `vkcts-anv.elf` on the
     HD 530, 3,565 cases - 1,441 passed, 2,124 not supported, none failed
     - once a queue asking for more than MEDIUM was refused. Submit to
     fence: 8 ms in the middle, 177 µs at best, because `intelgpu` hears
     a fence on its next tick and not from the GPU's interrupt; that
     interrupt is G7e.
   - **G7e - the GPU's interrupt.** The render engine's user interrupt at
     the end of each batch (`MI_USER_INTERRUPT`), the GT's interrupt
     registers armed (`GEN8_GT_IER`/`IMR`/`IIR`, the master control), and
     `intelgpu` waiting on its line and its endpoint at once
     (`kosmos_irq_wait_any`, as `e1000` does), so a fence is heard when the
     GPU writes it. *Measure*: submit-to-fence latency, against 18.934's 8
     ms.
8. **G8 - GL and WebGL on the M700.** Zink over `anv`; `glgears` at 1920 by
   1080 against TinyGL's figure; a Quake GL port (QuakeSpasm) installed in
   `/Home/Apps`; WebGL in JetSki. *Measure*: frames a second, and Zink
   against `iris` if the number asks for it.
9. **G9 - sharing and presenting.** Priority bands per context,
   preemption, GPU time per program in GPU Monitor; full-screen flips for
   a game; the GPU compositor sampling swapchain images. *Test on the
   M700*: the desktop's frame time held while a program saturates the GPU.
   **After G7, a flight simulator** (Diego, 19 November 2026, and the
   same day the order: "GPU first: G3, G6, G7, then simulator" - G4, G5,
   G8 and G9 after it): a simple
   application of Kosmos's own that shows what the API does, designed first
   as every app is (`roadmap.md`).
10. **Later, by machine**: the T14's Gen12 on the same `anv` (new context
    layouts, no GuC needed on Tiger Lake as i915 runs it); the Pi 5's
    `v3dv` when the board is here, its kernel half the smallest of all;
    the Mac Pro's `radv` last, since GCN needs firmware loaded into
    several engines and an amdgpu-shaped server.

**Effort, an estimate labelled as one.** G1 to G5 are porting and glue,
all tried in QEMU over and over before anything else: weeks, set mostly by
how Mesa and LLVM take to a new system. G6 is the risky one, for the
reason every GPU step has been - a GPU that ignores a command says
nothing, and none of it runs under QEMU - with Fuchsia's `msd-intel-gen`
and i915 as the references: one to three weeks of sessions, each ending at
the M700. G7 is the backend and its bugs; G8 and G9 are measuring.

## 7. Risks and unknowns

- **Size.** The parts of Mesa this needs - NIR and the compiler, the
  Intel compiler, `anv`, ISL, the Vulkan runtime, `util`, gallium's
  auxiliary, `llvmpipe`, `lavapipe`, Zink, the GL frontend, EGL - are
  about 1.3 million lines (counted per directory in Mesa 26.2.5), and LLVM
  more again. Comparable to WebKit, which Kosmos already builds; but every
  program that links GL carries it, and an installed application carries
  its own copy. `FULL=0` should leave it out, as it leaves the browser.
- **LLVM on Kosmos** is the biggest single build and the newest kind of
  JIT here. If it will not go, Vulkan has no software path (`lavapipe` has
  no interpreter), and GL alone falls back to `softpipe`, which is slow.
- **TCG's speed.** `llvmpipe` under emulation is slow; the suites must draw
  small pictures and few of them to stay in budget.
- **Gen9's future in Mesa.** `anv` builds for Gen9 in 26.2.5
  (`intel/vulkan/meson.build`), but Intel has already moved Gen7 and Gen8
  into separate legacy drivers. A pinned Mesa release is the answer, and
  a move of Gen9 would be a fork in the road to watch, not a failure.
- **Execlists and PPGTT on Gen9 with no emulator**: a wrong bit in a
  context image hangs the engine. The step is sized for that.
- **Coherency.** 7d found the sampler not seeing what the processor had
  just written (18.686). Mesa sets caching per buffer by MOCS; the server's
  defaults must agree with what `anv` expects.
- **The present path on the M700** depends on the GPU compositor (7d), not
  yet choosable; until then a copy, whose cost is unknown.
- **No IOMMU** (5.8).
- **Mesa's assumptions about its OS**: `/proc`, `sysconf`, futexes (`util`
  falls back to mutexes without them), `dlopen` (avoided by linking
  statically). Each is a small patch or a `runtime/posix/` function, and
  each is found by building.

## 8. Yours to decide

**DECIDED on 17 November 2026: every recommendation, as written.** Diego: "Yes to all recommendations" - Vulkan underneath with GL and GL ES by Zink, Mesa with a Kosmos kernel-mode backend at its seam, in the order of section 4.

1. **Vulkan as the contract, GL and GL ES by Zink** *(recommended)* - one
   driver per GPU, GL the same everywhere. Or GL ES first by `iris`, faster
   on Intel and Intel only; or Vulkan with GL ES by ANGLE only, no desktop
   GL.
2. **Mesa with a Kosmos KMD backend** *(recommended)*, the server speaking
   a protocol of our own. Or Mesa unmodified over a DRM shim, the server
   speaking i915's interface; or a driver of our own, which this document
   argues is years.
3. **Software first, in QEMU** - `lavapipe`, LLVM and Zink, G1-G5
   *(recommended)*, so the gate tests the whole API. Or straight to `anv` on
   the M700, tested only there; or a QEMU with virglrenderer and Venus on
   this Mac, which was tried and paused on 2 October and would still not
   run on the Mac's GPU without ANGLE or MoltenVK.
4. **`intelgpu` grows execlists, contexts and a page table per context**
   (G6) *(recommended)* - `compute.md` D2 B, taken now because `anv` needs
   it. The kernel check stays as a second guard for the Compute Kit's
   kernels.
5. **TinyGL retired when G4 lands** *(recommended)*: one GL, its demos
   running on Mesa unchanged - a second copy of GL is the defect
   `CLAUDE.md` names. Or kept beside it as a fallback.
6. **Astra's GPU compositor**: finish 7d on the driver's own `draw` now,
   and move it onto Vulkan once G7 is stable *(recommended)* - or wait for
   Vulkan and write it once.
7. **WebGL in JetSki by ANGLE over Mesa's GL ES** *(recommended)*, as
   WebKit's Linux ports do; or ANGLE's own Vulkan backend.
8. **Mesa pinned at 26.2.5** *(recommended)*, the release already used for
   `gentool`, moved deliberately.
9. **The order of the other GPUs**: the T14's Gen12, then the Pi 5 when it
   is here, then the Mac Pro's GCN *(recommended)*.
10. **No IOMMU for now** *(recommended)*, as storage lives without one.

---

Sources outside the tree: Mesa 26.2.5's source (`build/downloads/mesa`);
Fuchsia's Magma overview, <https://fuchsia.dev/fuchsia-src/development/graphics/magma>;
Mesa's KMD backend for Xe, <https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/20418>;
Haiku's RadeonGfx and lavapipe, <https://discuss.haiku-os.org/t/vulkan-lavapipe-software-rendering-is-working-on-haiku/11363>
and <https://dev.haiku-os.org/ticket/9919>;
Intel's Gen8 split, <https://www.phoronix.com/news/Intel-Mesa-Splitting-Gen8>.
