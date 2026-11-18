<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Blender on Kosmos - how hard, and what it would take

Written on 17 November 2026, before any of it is built. Diego, the same
day: "How hard is to port blender3d app to kosmos?" **This is a study to
decide from, not a start on the work.** Where a figure has a source it says
so; where it is an estimate it is labelled as one.

What was read: Blender's `main` branch on 17 November 2026, which calls
itself **5.4 alpha** (`BKE_blender_version.h`: `BLENDER_VERSION 504`), its
`CMakeLists.txt`, `build_files/cmake/platform/platform_unix.cmake`,
`build_files/build_environment/cmake/versions.cmake`, the GHOST and GPU
module directories, and `intern/cycles/blender/addon/`. Copies of the three
CMake files are in this session's scratch directory, outside the tree;
nothing was put in the repository.

---

## In short

- **Hard, and the hardness is breadth rather than depth.** Nothing Blender
  needs is unknown to Kosmos: it is C++20 on pthreads, `mmap` and files,
  which is exactly what `runtime/posix/` was built for WebKit to stand on.
  But it is about **three million lines of C++ and half a million of
  Python** (estimate, from GitHub's byte count of the repository: 113.7 MB
  of C++, 21.1 MB of Python), standing on **roughly fifteen libraries it
  cannot start without and another thirty it can be built without**, and on
  **a second language runtime, CPython, which Kosmos does not have.**
- **Three things decide the size, and only one is Blender's own code:**
  1. **CPython 3.13, embedded.** Blender's menus, panels, headers, its
     keymap and the registration of Cycles itself are Python
     (`scripts/startup/bl_ui/`, 81 files; `keymap_data/blender_default.py`,
     453 KB; `intern/cycles/blender/addon/`). Built without Python, Blender
     opens with empty headers, no keys and **no Cycles**. So Python is not
     optional for anything a person would call Blender.
  2. **The viewport needs OpenGL 4.3 or Vulkan 1.1 with a dozen extensions**
     (`gl_backend.cc`: `epoxy_gl_version() >= 43`, else "unsupported";
     `vk_backend.cc`: dynamic rendering, timeline semaphores, buffer device
     address and the rest). Kosmos has TinyGL, OpenGL 1.1. **This is all of
     `gpu3d.md`** - Mesa, LLVM, `lavapipe`, Zink - and Blender would be its
     biggest user, not a reason to do it differently.
  3. **The required image stack**: OpenImageIO, OpenColorIO, OpenEXR and
     Imath, oneTBB, and a libc++ **with iostreams**, which Kosmos's is built
     without (`runtime/cxx/runtimes.cmake`: localisation off).
- **The estimate, labelled as one:**
  - **A headless Blender rendering a `.blend` with Cycles on the
    processor** - `blender -b scene.blend -f 1` writing a PNG - is about
    **10 to 14 weeks** of conventional work, with no GPU and no window.
  - **Blender in a window, its viewport on software GL** (Mesa's
    `llvmpipe`, or Zink over `lavapipe`), is **another 4 to 6 weeks on top,
    after `gpu3d.md` G1 to G4 exist** - which is its own work, weeks, and
    is not counted here.
  - **The viewport on the M700's GPU** follows `gpu3d.md` G6 to G8 and adds
    little of Blender's own.
  - **Together, about four to six months of a person's time.** This project
    has moved faster than that measure before - JetSki went from its design
    on 11 November to WebKit2's three processes on 14 November - because the
    runtime under it had been built over months. Blender would stand on that
    runtime too, and the first half of the estimate is the part of it that
    runtime does not cover yet.
- **The honest comparison is with Cafesa3D**, which already does on Kosmos
  what Diego asked for on 25 September ("a simple 3d modeling and animation
  tool like blender3d"), on Kosmos's kits, in a window that is fast today.
  Blender would bring its whole toolset, `.blend` files and the manual
  everyone knows, at the price of being the opposite of *kits supply, apps
  orchestrate*: a monolith with its own UI toolkit, file browser, colour
  management and image IO inside one process. **Recommended: not now**;
  grow Cafesa3D as the everyday tool, decide `gpu3d.md` first, and if
  Blender is wanted, start with Cycles on its own (B1 below) - one week of
  porting that answers half the dependency question and is useful by itself.

---

## 1. What Kosmos already has that Blender stands on

| Blender needs | Kosmos has | Where |
|---|---|---|
| C++20, clang 17 or later | clang from Homebrew's `llvm`, C++23 used by WTF and JavaScriptCore | `docs/setup.md`, `Makefile` |
| libc++, exceptions, RTTI | libc++, libc++abi, libunwind, static; an exception thrown three frames down and caught is tested | `runtime/cxx/`, `user/kits/cxxtest/` |
| `std::thread`, `std::mutex`, `thread_local`, `std::filesystem` | on, on the POSIX layer's pthreads and W2's futex | `runtime/cxx/runtimes.cmake`, `docs/w3.md` W3a |
| pthreads, semaphores | `runtime/posix/pthread.c`, `semaphore.h` | `docs/w3.md` W3a |
| `mmap`, `mprotect`, `madvise`, a file mapped privately for reading | yes; `MAP_SHARED` of a file refused | `runtime/posix/mman.c`, W3b |
| files, directories, `glob`, `fnmatch`, `stat` | the descriptor layer over the Storage Kit | W3d |
| `clock_gettime`, `sysconf(_SC_NPROCESSORS_ONLN)`, `uname` | yes | W3c |
| sockets | yes (Blender needs them only for its online extensions) | W3e |
| a temporary file | `tmpfile` and friends in `/Temporary` | `runtime/posix/stdio.c` |
| zlib, libpng, libjpeg-turbo, libwebp | built, held to their own pictures | `docs/w5.md` W5a |
| FreeType, HarfBuzz, Brotli | built, the Text Kit's engines | W5b, W5f |
| SQLite, libxml2, expat, OpenSSL | built | W5c, W5d |
| FFmpeg | **9.0.2** (Blender pins **8.1**) | `runtime/upstream/ffmpeg/` |
| a window from C | the Window Kit: a shared surface, two buffers, keys, the pointer, the wheel, resize, close | `user/kits/window/kosmos_window.h`, `docs/windowkit.md` |
| a crash said as a function's name | the Debug Kit | `docs/debug.md` |
| an installed application with its own image | `doom.elf`, `quake.elf`, `snes.elf` in `/Home/Apps` | `docs/elf.md` step 5 |
| many processors, threads spread across them | yes, four on QEMU, eight on the ThinkPad | `docs/smp.md` |
| 64-bit, room to map | a 39-bit space on AArch64, processes from 256 GB; pools that grow | `CLAUDE.md` |

**What this table says**: the bottom of the stack - the part that took
WebKit's W1 to W3 - is done. Blender would not need Nebula to change at all.

---

## 2. What Blender needs, layer by layer

Effort is in weeks of one person's work, an estimate; risk is how likely
the estimate is to be wrong.

### 2.1 The build: Blender's CMake for a new system

Blender is CMake; Kosmos already cross-builds the C++ runtimes and ICU with
CMake and a toolchain file (`runtime/cxx/kosmos.cmake`). Blender wants a
`platform_kosmos.cmake` beside `platform_unix.cmake` - Haiku has its own
branches in the same file (`if(HAIKU)`, four of them in `CMakeLists.txt`),
which is the precedent for a small system with a port of its own. Static
everything, as every Kosmos image is; Blender builds its dependencies
statically on Linux already.

- **Missing**: the platform file, the cache of options (§3), a Makefile
  rule that builds it once and caches it as WebKit's is.
- **Effort**: 1 week. **Risk**: low.

### 2.2 The C++ runtime: iostreams

**This is the one change to something shared.** Kosmos's libc++ is built
with `LIBCXX_ENABLE_LOCALIZATION OFF`, so there is no `<iostream>`,
`<fstream>` or `<sstream>`; WebKit and Skia were patched around it
(`docs/w4.md` patch 0003, `docs/w5.md`). Blender's dependencies cannot
reasonably be: OpenColorIO parses its configuration through streams and
yaml-cpp, OpenEXR's file IO is `std::ifstream`, OpenImageIO and Blender's
own importers use string streams throughout.

- **Missing**: libc++ with localisation on and wide characters still off,
  over the one "C" locale `runtime/posix/locale.c` already answers with.
  Either the one runtime is rebuilt that way - WebKit and ICU then link
  the same libc++ with more in it, and their suites prove nothing broke -
  or a second build of libc++ for ported code that wants streams. One is
  better than two (*a second copy of anything is a defect*).
- **Effort**: 1 week. **Risk**: medium, because it touches every C++ image
  - a full gate.

### 2.3 The dependencies Blender cannot start without

From `platform_unix.cmake` (`REQUIRED`), and what each needs here:

| Library (Blender's pin) | On Kosmos | Work |
|---|---|---|
| zlib, libpng 1.6.58, libjpeg-turbo, FreeType 2.13.3 | built (W5) | link Kosmos's builds; versions are close enough |
| zstd 1.5.7 | no | small, plain C |
| fmt 12.1.0 | no | header-mostly C++ |
| Eigen | no | headers only |
| libepoxy 1.5.10 | no | small, but it finds GL by `dlopen` - patched to a static table of Mesa's entry points |
| OpenEXR 3.4.10, Imath 3.2.2, libdeflate 1.18 | no | moderate C++; threads and streams |
| OpenImageIO 3.1.16 | no | large C++; image IO for almost every format Blender reads and writes |
| OpenColorIO 2.5.0, with yaml-cpp, pystring, minizip-ng, expat | expat only | moderate C++; Blender's colour management |
| oneTBB 2022.3.0 | no | **an OS port**: TBB knows Linux, macOS, Windows and FreeBSD; its waiting is a futex on Linux. Kosmos has a futex, so this is a few files, plus `TBB_MALLOC_PROXY` off (it replaces the allocator through the dynamic linker) |
| CPython 3.13.13 | no | §2.4 |

Technically optional, but needed for what a person expects: **TBB** (Cycles,
USD and OpenVDB refuse to build without it - `set_and_warn_dependency`),
**FFmpeg** (rendering an animation to a film), **Audaspace** (sound in the
sequencer), **OpenSubdiv**, **OpenImageDenoise**.

- **Effort**: 3 to 4 weeks for the required set and TBB, each held to its
  own tests as the W5 libraries were. **Risk**: medium - TBB's port and the
  amount of C++ more than any one library.

### 2.4 Python: CPython 3.13, embedded, with numpy

Kosmos has no Python at all - `grep -rni python user/ runtime/` finds only
comments and vendored build scripts. Blender links CPython into its own
image and runs its UI in it.

What a CPython on Kosmos is:

- **Static, with its C modules in the image** (the "inittab"): no
  `dlopen`, so `_struct`, `math`, `_json`, `zlib`, `_sha2` and the rest are
  linked in, as CPython's own WASI and iOS builds already do it - CPython
  3.13 lists WASI as a supported platform, and it has no `dlopen`, no
  `fork` and no signals either, which is the precedent that matters.
- **The standard library as `.py` files** in the application's folder.
- **No `fork`, no `subprocess`, no signals**: `os.fork` is not there,
  `subprocess` fails cleanly, `signal` exists with no handlers. What that
  loses in Blender: installing extensions from the online repository with
  `pip`, and the few add-ons that run another program. Nothing in the core
  UI calls them.
- **No environment**: Blender sets Python's home itself through
  `PyConfig`, so the missing `PYTHONHOME` does not matter.
- **numpy 2.3.4**: optional (`WITH_PYTHON_NUMPY`), but the glTF add-on
  imports it, and glTF is the format Cafesa3D and Blender share. A static
  numpy is done by Pyodide; it is a Meson build and a second set of C
  modules in the inittab.

- **Effort**: 3 to 4 weeks, CPython 2 of them and numpy the rest. **Risk**:
  medium to high - not because any piece is unknown, but because CPython's
  `configure` probes several hundred functions, and each one that is
  missing is found one at a time.

### 2.5 Blender itself, compiled

About three million lines, compiled for Kosmos with these changes, each a
patch in the port's own directory, never an edit to the vendored tree:

- **`GHOST_SystemPathsKosmos`** - where Blender keeps what, since `$HOME`
  and `$TMPDIR` do not exist (`getenv` answers NULL, `w3.md` W3c): its
  preferences in `/Home/Preferences/Blender`, its caches in
  `/Home/Cache/Blender`, temporary files in `/Temporary`, its own data -
  fonts, icons, `startup.blend`, the scripts - beside its image in
  `/Home/Apps/Blender`. This is an interface GHOST already has, one class
  per system; nothing else changes.
- **The crash handler** (`creator_signals.cc`, `signal(SIGSEGV, ...)`):
  `<signal.h>` is refused here (`runtime/posix/refused/`), so it is left
  out and the Debug Kit says where it stopped.
- **`system()` and "open the folder"**: Blender calls out to `xdg-open`
  and friends; here it asks Tracker, or does nothing.
- **Threads**: `BLI_threads` on pthreads and TBB, as on Linux.
- **x86-64's floor is SSE4.2**: Blender compiles everything for it
  (`build_files/cmake/macros.cmake`). The M700 and the T14 have it; **QEMU's
  default x86 processor (`qemu64`) does not**, and `X86_ACCEL` is empty
  under TCG, so Blender's suites need `-cpu max`. AArch64 goes through
  `sse2neon`, as Blender's own ARM builds do.

- **Effort**: 2 to 3 weeks of building until it links and starts. **Risk**:
  medium; the size makes every build long, so this is a step to cache
  carefully.

### 2.6 GHOST: Blender's window, keys and pointer

GHOST is Blender's platform layer: a `System` (the event loop, the
clipboard, the cursor, time), a `Window`, and a `Context` (the GPU's
connection to the window). It has one per system - Win32, Cocoa, X11,
Wayland - and **an SDL one, which is the template**: `GHOST_SystemSDL.cc`,
`GHOST_WindowSDL.cc` and `GHOST_ContextSDL.cc` are 28 KB, 24 KB and 4.5 KB,
about 1,600 lines (Wayland's `System` alone is 414 KB). There is also
`GHOST_SystemHeadless` with `GHOST_ContextNone`, for a Blender with no
window at all, which is what B2 below uses.

A `GHOST_SystemKosmos` over the Window Kit is the right door. **But the
Window Kit and the window manager's protocol do not yet say enough for
Blender** (`user/include/wmproto.h`):

| Blender needs | `wm_frame_event` today |
|---|---|
| a key's release, not only its press (modal tools end on release) | a key is one event, `a: the code` |
| Shift, Control, Alt and Super held, on every event | not carried |
| the **middle button** - Blender orbits the view with it | left and right only |
| the pointer warped and held (continuous grab: rotating without the pointer stopping at the screen's edge) | no |
| the cursor's shape (Blender has about twenty) | no |
| the clipboard, text | in `ui.lua`, not in the Window Kit |
| a tablet's pressure (painting, Grease Pencil) | no; later |

Each of those is a field in a declared shape and a line in `wm.lua`, and
each is useful to every C application, Doom and Quake among them - which is
where they belong. **It is the window manager, so it is a full gate.**

- **Effort**: 2 to 3 weeks: the protocol and the kit 1, GHOST 1 to 2.
  **Risk**: low to medium.

### 2.7 The GPU: Blender's viewport

Blender draws its **whole interface** with the GPU module, not only the 3D
view: buttons, text and panels are GPU batches. So there is no Blender
window without GL 4.3 or Vulkan. `source/blender/gpu/` has `opengl/`,
`vulkan/`, `metal/` and `dummy/` backends; Linux builds both GL and Vulkan.

- **OpenGL 4.3 core** through libepoxy: Mesa's `llvmpipe` gives 4.5, Zink
  over `lavapipe` gives 4.6, `iris` on the M700 4.6. Blender recognises
  `llvmpipe` by name and marks it a software device (`gl_backend.cc`).
- **Vulkan 1.1 with** `VK_KHR_swapchain`, `dynamic_rendering`,
  `timeline_semaphore`, `buffer_device_address`, `depth_stencil_resolve`,
  `create_renderpass2`, `separate_depth_stencil_layouts` required, and many
  more used where present (`vk_backend.cc`). `lavapipe` and `anv` on Gen9
  have all of them. The Vulkan backend also needs **shaderc** (glslang) to
  compile Blender's GLSL to SPIR-V at run time - a sizeable C++ library,
  but no OS calls to speak of.

**Kosmos has none of this today**, and `gpu3d.md` is the plan that would
give it: G1 (Mesa and LLVM build), G2 (`lavapipe`), G3 (a Vulkan surface in
a window), G4 (GL by Zink, EGL with a display). Blender would add only a
`GHOST_ContextKosmos` - an EGL context on the EGL Kit, or a Vulkan surface
from `VK_KHR_kosmos_surface` - and libepoxy's static table. §4 has what it
would feel like.

- **Effort, Blender's own share**: 1 to 2 weeks, after G1 to G4.
  **Risk**: high, and almost all of it inherited from `gpu3d.md` - LLVM on
  Kosmos is that document's largest unknown.

### 2.8 Sound

Audaspace, Blender's audio library, speaks OpenAL, SDL, JACK, PulseAudio,
PipeWire or CoreAudio. Here it would be **an Audaspace device over the
Playback Kit** (`docs/playback.md`) - a sound on a thread of its own in the
audio band, the samples in a region - about the size of Audaspace's SDL
device. **Off at first** (`WITH_AUDASPACE=OFF`); the sequencer and the
timeline play silent.

- **Effort**: 1 week, later. **Risk**: low.

### 2.9 The total

| Layer | Weeks (estimate) | Risk |
|---|---|---|
| 2.1 the build | 1 | low |
| 2.2 libc++ with iostreams | 1 | medium |
| 2.3 required libraries and TBB | 3-4 | medium |
| 2.4 CPython and numpy | 3-4 | medium-high |
| 2.5 Blender compiled, paths, patches | 2-3 | medium |
| **headless Cycles render (B2)** | **10-14** | |
| 2.6 the window protocol and GHOST | 2-3 | low-medium |
| 2.7 Blender's share of the GPU | 1-2, after `gpu3d.md` G1-G4 | high (inherited) |
| 2.8 sound | 1 | low |
| **interactive, software GL (B4)** | **+4-6** | |

---

## 3. The minimum Blender worth having

**What can be turned off** (`CMakeLists.txt`'s `WITH_*`), for the first
builds, and what each costs a person:

| Off | Lost |
|---|---|
| `WITH_CYCLES_DEVICE_CUDA`, `_OPTIX`, `_HIP`, `_ONEAPI`, `_METAL` | rendering on a GPU with Cycles - none of those exist here |
| `WITH_CYCLES_OSL`, `WITH_LLVM` | OSL shaders (the script node); everything else in Cycles stays |
| `WITH_CYCLES_PATH_GUIDING` (OpenPGL) | a faster convergence in some interiors |
| `WITH_EMBREE` | Embree's BVH; Cycles falls back to its own, slower to build |
| `WITH_OPENIMAGEDENOISE` | the denoiser, until OIDN is ported (it loads its devices as modules, and needs a static build) |
| `WITH_USD`, `WITH_HYDRA`, `WITH_MATERIALX`, `WITH_ALEMBIC` | USD and Alembic exchange - USD alone is about as big as the rest of this list |
| `WITH_OPENVDB`, `WITH_NANOVDB`, `WITH_MOD_FLUID` | volumes, smoke and fluid simulation |
| `WITH_OPENSUBDIV` | GPU-evaluated subdivision; the modifier still works on the processor |
| `WITH_XR_OPENXR`, `WITH_INPUT_NDOF`, `WITH_INPUT_IME` | VR, 3D mice, Asian input methods |
| `WITH_CODEC_FFMPEG`, `WITH_CODEC_SNDFILE`, `WITH_AUDASPACE` | films and sound, until 2.8 |
| `WITH_LIBMV` (Ceres) | camera tracking |
| `WITH_GMP`, `WITH_MANIFOLD`, `WITH_JOLT`, `WITH_BULLET` | exact booleans, physics - each a C++ library, cheap to bring later |
| `WITH_INTERNATIONAL` | translations |
| `WITH_TBB_MALLOC_PROXY` | nothing: it replaces the allocator through the dynamic linker |

**What cannot be turned off**: Python, for the reasons in *In short*. Built
`WITH_PYTHON=OFF`, Blender starts and renders with Workbench or EEVEE - both
GPU engines - but has **no Cycles** (Cycles is registered by its Python
add-on: `intern/cycles/blender/addon/engine.py` does `import _cycles`), no
menus or panels (they are Python classes in `scripts/startup/bl_ui/`) and
no default keymap (`blender_default.py`). `CMakeLists.txt` says it plainly:
"only disable for development".

**So the milestones are**:

1. **Cycles on its own** (`WITH_CYCLES_STANDALONE`): Cycles' own
   executable reading its XML scene format and writing a picture through
   OpenImageIO. No Python, no GPU, no window, no `.blend`. It needs TBB,
   OpenImageIO, OpenEXR, Imath and the image libraries - half of §2.3 - and
   is held to the same scene rendered on this Mac.
2. **`blender -b scene.blend -f 1`**: the whole program, headless
   (`WITH_HEADLESS`, `GHOST_SystemHeadless`), Python in it, Cycles on the
   processor. This is the render farm's Blender, and the first one that
   reads Diego's files.
3. **Blender in a window**, on software GL - the interface and the
   viewport, slow.
4. **Blender on the M700's GPU**.

---

## 4. The GPU question

**Blender asks for exactly what `gpu3d.md` recommends and nothing beyond
it.** That document's choice - Vulkan as the contract from Mesa, GL and GL
ES by Zink, software first with `lavapipe` - gives Blender both of its
backends: Vulkan from `lavapipe` or `anv`, GL 4.6 from Zink. Blender is in
fact a good argument *for* that recommendation over its alternatives: "GL
ES first by `iris`" would not run Blender at all (it needs desktop GL 4.3
core), and "Vulkan with GL ES by ANGLE only" would leave Blender on its
Vulkan backend alone, which Blender itself still treats as the newer of the
two on Linux.

**Software GL as the stopgap, and what it costs.**

- **To port**: nothing beyond `gpu3d.md` G1 and G4. `llvmpipe` (GL 4.5
  directly, without Zink) could be built first if Blender were wanted
  before Vulkan - the same LLVM, the same Mesa build, gallium's `llvmpipe`
  in place of `lavapipe`. That is the path Haiku took: its Blender port
  draws through SDL into Haiku's GL view on Mesa's `llvmpipe` (a crash log
  in the Haiku forum, September 2024, shows `GHOST_ContextSDL` over
  `BGLView` and `llvmpipe` threads on LLVM 12).
- **How fast**: not measured, and it cannot be until it exists. What is
  known: `llvmpipe` runs Blender's interface and the Workbench viewport at
  an interactive rate on a modern multi-core processor for simple scenes,
  and EEVEE slowly; **under TCG it would be seconds a frame**, so QEMU
  proves it works and only `make fast`, KVM or a real machine says how it
  feels. On the M700's four cores it would be usable for modelling the kind
  of scene Cafesa3D makes, and not for EEVEE.
- **A caution from Haiku**: its port is Blender **3.3 LTS**, the last
  release that asked for OpenGL 3.3; a 4.2 build there crashed reaching the
  Shading workspace through the GL context path (the same forum thread,
  November 2024). The difference between "it starts" and "every workspace
  works" is where the time goes in a software-GL port.

**On the M700's GPU**: `anv` (Vulkan) or Zink over it (GL), from
`gpu3d.md` G7 and G8, and Blender's viewport moves from the processor to
the HD 530 with nothing of Blender's own changing. Intel's Gen9 is inside
what Blender supports on Linux.

---

## 5. Principles in the way, and how each is answered

**Compatibility inside a process, never at system level.** Blender, CPython,
TBB and every library link `runtime/posix/`, as WebKit does - ported code,
inside one process. Nothing POSIX crosses into the system. This holds.

**`fork` and `exec`.** Blender does not fork to render. CPython's
`subprocess` and `os.fork` are absent; what is lost is installing
extensions with `pip` and the add-ons that run other programs. A **render
farm** - several machines on one animation, `renderfarm.md`'s question for
Cafesa3D - is several Blenders each started by its own machine's launcher
on a range of frames (Blender's `-s`, `-e`), never a fork. Answered without
bending anything.

**Signals.** Refused here (`runtime/posix/refused/signal.h`). Blender's crash
handler is left out and the Debug Kit names the function; Python's
`signal` module is built with no handlers, as on WASI. Nothing a person
uses depends on a signal.

**`dlopen`.** None, everything static, as every Kosmos image is.
Consequences: Python's C modules and numpy in CPython's inittab; libepoxy
given a static table; OpenImageDenoise built with its devices inside;
**no add-ons with compiled parts** (Python-only add-ons still work, being
source); CUDA, HIP and the Wayland and JACK "dynload" options off. A cost,
and a small one.

**No environment, no `/tmp`, no `$HOME`.** `GHOST_SystemPathsKosmos` says
where things live (§2.5). This is the interface Blender has for exactly
this question, so it is not a patch against Blender's grain.

**No precompiled bytecode.** That rule is written for Lua - the bytecode
loader verifies nothing - and CPython's `.pyc` files are the same thing in
another language: `marshal` trusts what it reads, and a planted `.pyc` is
code run. **So the same answer: source only**, `sys.dont_write_bytecode`,
no `.pyc` shipped or written. What it costs is that every start compiles
the few hundred modules Blender imports; how long that takes is a number to
measure at B2, and it is the one place this principle has a price here.

**Lua runs for a person.** Blender's Python runs inside Blender, for
Blender, as JavaScript runs inside JetSki; it is not a second system
language, no Kosmos kit or application calls it, and nothing in
`/Kosmos/Libraries` grows a Python door. Cafesa3D's scripting stays Lua
(agreed 26 September).

**Kits supply, applications orchestrate.** This is the principle Blender
genuinely does not fit, and it is better said than hidden. Blender is a
whole desktop inside one process: its own interface kit, its own file
browser, its own font drawing (BLF over FreeType), its own image IO
(OpenImageIO), its own colour management (OpenColorIO). A port cannot
change that, any more than JetSki changes WebKit. What the port *can* do,
and should: **link Kosmos's own builds** of zlib, libpng, libjpeg-turbo,
FreeType, HarfBuzz, Brotli, expat, SQLite, OpenSSL and FFmpeg rather than
bringing second copies (FFmpeg is 9.0.2 here against Blender's 8.1 pin -
Blender's FFmpeg code tracks the API, and whether 9.0 removed anything it
calls is found by building). And ask of each new library whether another
application wants it: **OpenEXR and OpenImageIO, yes** - HDR pictures that
Cafesa3D could light a scene with or save a render as, Paint, a viewer - so they would be a kit's
engine with one door, not Blender's private copy; TBB and CPython, no.

**An installed application carries its own runtime.** Blender would be
`/Home/Apps/Blender` with `blender.elf` and its data. **Its size is a
question for the stick**: Blender's own data (fonts, icons, scripts,
`startup.blend`) with Python's library is of the order of 150-250 MB and
the image itself likely as much again (estimate) - against `/Home`'s 512 MB
on a stick (`STICK_HOME_MB`). The M700's `/Home` on its SATA disk has room.

**Responsiveness.** Cycles starts a thread a processor. They must run below
the desktop's band, as a render should, so the pointer and the window keep
their frame while every core renders - which Kosmos's strict bands already
guarantee if the threads are put there. Blender's own interface thread is
one thread, like any application's.

**The tests take five to ten minutes.** A render in TCG is slow; the suites
render 64 by 64 at a few samples, and Blender's build is cached and its
suites skipped when nothing under them changed, as the W5 libraries are
(`make test LIBS=1`).

---

## 6. A staged plan

Each step is a revision with a test that stays. Only x86-64 suites run for
now; the AArch64 build must still compile.

1. **B0 - libc++ with iostreams.** The runtime rebuilt with localisation
   on. *Test*: `cxxtest` grows a string stream and a file stream written to
   `/Temporary` and read back; WebKit's, ICU's and JavaScriptCore's suites
   unchanged. Full gate.
2. **B1 - Cycles on its own.** TBB ported, OpenEXR, Imath, OpenImageIO
   (with Kosmos's image libraries), Cycles standalone. *Test*
   `x86-cycles`: a small XML scene rendered at 64 by 64, 16 samples, fixed
   seed, compared with the same build on this Mac within a stated
   tolerance (floating point across compilers is not bit-exact); TBB's own
   tests that need no shell. *Measure*: the render's time on the M700,
   against Cafesa3D's tracer on the same scene.
3. **B2 - `blender -b`.** CPython 3.13 and numpy, OpenColorIO, the rest of
   §2.3, Blender with `WITH_HEADLESS`. *Test* `x86-blender`: a `.blend`
   from `/Home` rendered one frame by Cycles to a PNG and held to the Mac's;
   `blender -b --python-expr` printing `bpy.app.version`; a glTF written by
   Cafesa3D imported and rendered. *Measure*: time to start, the Python
   compile share of it, peak memory.
4. **B3 - the window protocol.** Key release, modifiers, the middle button,
   pointer warp and grab, cursor shapes, the clipboard in the Window Kit -
   useful to every C application. *Test*: the Window Kit's suite gains each
   event, driven by `kosmos_vnc`-style input. Full gate.
5. **B4 - Blender in a window, software GL.** Needs `gpu3d.md` G1 and G4
   (or `llvmpipe` alone). `GHOST_SystemKosmos`, `GHOST_WindowKosmos`,
   `GHOST_ContextKosmos`. *Test*: Blender opened on `startup.blend`, its
   picture held to a reference as `make screenshot` holds the desktop's;
   the default cube rotated by a scripted drag and the picture changed.
   *Measure*: frames a second of the viewport on the M700 under
   `llvmpipe`.
6. **B5 - sound and film.** Audaspace over the Playback Kit; FFmpeg on.
   *Test*: a ten-frame animation rendered to an MP4 the Video app plays.
7. **B6 - the M700's GPU.** After `gpu3d.md` G7 and G8: Blender on `anv` or
   Zink. *Measure*: viewport frames a second, EEVEE's frame time.
8. **Later, each its own decision**: OpenImageDenoise, OpenSubdiv,
   OpenVDB, USD, physics.

---

## 7. Yours to decide

**DECIDED on 17 November 2026: every recommendation, as written.** Diego: "Yes to all recommendations" - Cafesa3D grows now and Blender is not ported; the later questions wait for a Blender that is.

1. **Port Blender, or grow Cafesa3D.** *(Recommended: grow Cafesa3D now;
   keep Blender as a later project, started only after `gpu3d.md` is
   decided.)*
   - **Cafesa3D** buys: a modeller that is already Kosmos's own - its kits,
     its Lua, its window, its GPU tracer on the M700 - fast on this
     system today, scriptable in Lua, and every piece of it reusable by the
     next application. Its cost is that every feature Blender has is one
     to write: modifiers, sculpting, UV editing, nodes, animation (held
     since 25 September as version two).
   - **Blender** buys: the whole of Blender, `.blend` files, add-ons
     written in Python, the manual and every tutorial on the internet - the
     tool, rather than one like it. Its cost is §2's four to six months,
     an application that is a monolith beside Kosmos's kits, a Python
     runtime that nothing else uses, and a viewport that waits on the GPU
     work anyway.
   - **They do not compete for the same weeks evenly**: Blender's
     interactive half waits on `gpu3d.md`, which Cafesa3D does not need.
2. **If Blender: Cycles on its own first (B1)** *(recommended)* - about a
   week of porting once TBB and OpenImageIO are in, no Python, no GPU, and
   it answers the dependency question before the expensive part. Or
   straight to B2.
3. **libc++ with iostreams, one runtime for every C++ image** *(recommended)*
   - or a second libc++ build for ports that want streams, which is a
   second copy.
4. **OpenEXR and OpenImageIO as a kit's engine** that Cafesa3D and Paint
   can use too *(recommended)*, rather than Blender's private libraries.
5. **Python source only, no `.pyc`** *(recommended)*, as Lua is; measured
   at B2, and revisited only if the start is too slow to live with.
6. **Software GL for Blender by Zink over `lavapipe`**, as `gpu3d.md` has
   it *(recommended)*, or `llvmpipe` built first if Blender's window is
   wanted before Vulkan.
7. **Which Blender**: the release current when B1 starts, pinned and moved
   deliberately *(recommended)* - `main` today is 5.4 alpha; or an LTS,
   which changes less under a port.

---

## Doubts

- **Speed under software GL is a guess** until it is measured; it is the
  number most likely to make Blender in a window not worth it before the
  GPU.
- **CPython's breadth**: each missing POSIX function is found by building,
  one at a time; WASI's port is the reason to expect it ends.
- **The allocator.** Blender and Cycles allocate from many threads at once;
  `runtime/libc/malloc.c` is a segregated-fit heap designed around Lua, and
  how it holds up under eight threads of C++ is a measurement for B1.
- **FFmpeg 9.0.2 against Blender's 8.1**, unknown until built.
- **Size on a stick**, as §5 says.

---

Sources outside the tree: Blender's source, `main` on 17 November 2026,
<https://projects.blender.org/blender/blender> (read through its GitHub
mirror); its pinned dependencies,
<https://projects.blender.org/blender/blender/src/branch/main/build_files/build_environment/cmake/versions.cmake>;
Haiku's Blender port, <https://discuss.haiku-os.org/t/blender-port/15541>
and <https://depot.haiku-os.org/__multipage/pkg/blender3>.
