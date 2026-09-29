# Video in hardware on the M700: a study

Diego, 27 September 2026: "study the effort to integrate gpu decoding and
encoding capabilities on m700 using intel hd graphics driver *might take it
from linux*". And on 29 September, the order it sits in: virtio-gpu under
QEMU first (`roadmap.md` 4h a, built), "after that do the m700 gpu codec and
driver". **This is the document to decide from, not a start on the driver**,
as the roadmap asked. Everything below with a number has a source; what
could not be checked says so.

Collected on 29 September 2026. The line counts are physical lines, measured
on shallow clones: Linux v7.2, intel/media-driver of 24 September 2026,
intel-vaapi-driver at its last commit, Mesa, Fuchsia and Genode of the same
week. Intel's Skylake manuals (the PRMs) are public, at
<https://www.intel.com/content/www/us/en/docs/graphics-for-linux/developer-reference/1-0/skylake.html>.

---

## The answer, short

- **The M700 can decode H.264 in hardware** - Baseline, Main and High, up
  to 4096x4096 - and HEVC 8-bit, and encode H.264. It cannot do VP9 or
  10-bit HEVC in hardware. Decode needs **no firmware**.
- **Taking Linux's driver as it is does not fit Kosmos.** What Linux uses is
  two programs: the kernel's i915 driver, 418,774 lines, and Intel's
  media-driver, 2.5 million lines of C++, joined by i915's buffer and
  submission interface. Kosmos has neither that interface nor a place for
  either program.
- **What does fit is a driver of Kosmos's own, written from Intel's manuals
  with three open programs as the reference**: Fuchsia's Intel GPU driver
  (about 9,000 lines, a userland driver on a microkernel, and it already
  drives the video engine on this exact GPU); Mesa's H.264 decode (364
  lines of command building); and Linux's i915 for the register-level
  details neither has.
- **Before any of that, measure what software decode does on the M700.**
  Kosmos builds FFmpeg as plain C on one thread, and nobody has timed it at
  1080p on that machine. If it keeps up, the next step is not a GPU driver.
- **Nothing here runs under QEMU**: there is no emulated Intel GPU. Every
  step is tried on the M700, which means a stick per step, and the checks
  that can live in `make test` are the ones that are pure C.

---

## 1. The machine

The M700 is a 6th-generation Core i7 with **HD Graphics 530** (`boot.md`, 22
September). That is Skylake, graphics generation 9, GT2: PCI device
`8086:1912`, 24 execution units (Linux `pciids.h`; Intel's PRM Vol 4).
**The device ID is still to be read off the machine itself** - `pci` at the
prompt will show 00:02.0 - since HD 510 (`1902`, GT1) is the other part the
M700 was sold with.

It has one video engine (VCS0, the "VDBOX"), one video enhancement engine
(VECS0, the "VEBOX"), the render engine and the blitter; and on Skylake a
scaler and format converter (SFC) the decoder can feed directly (PRM Vol 15).

| | Hardware on Gen9 | Firmware needed |
|---|---|---|
| H.264 decode | Constrained Baseline, Main, High; to 4096x4096 | none |
| HEVC decode | 8-bit only | none |
| VP9 decode | no (only a discontinued CPU-and-GPU hybrid) | - |
| H.264 encode, low-power (VDEnc) | Main, High, Baseline | none for constant quality; **HuC** for bitrate control |
| H.264 encode, shader (VME + PAK) | yes | Intel's precompiled GPU kernels |

The decoder writes **NV12 in a tiled layout (Tile-Y)**, so a frame has to be
untiled and converted before the compositor can use it - on the CPU, or by
the VEBOX or the SFC on the way out.

HuC (the media microcontroller's firmware) is needed only for the low-power
encoder's rate control, and on Gen9 loading it means loading the GuC's too,
which authenticates it. Linux itself loads neither on Gen9 by default. No
H.264 decode path in media-driver, i965 or Mesa waits on it.

## 2. What software decode costs today, and why that comes first

Kosmos builds FFmpeg with `--disable-asm --disable-inline-asm
--disable-pthreads --disable-runtime-cpudetect`, one configuration for both
boards (`user/kits/ffmpeg/config/config.h`): **plain C on one thread**. The
H.264 SIMD it leaves out is 17 files and 11,155 lines of FFmpeg's x86 code -
the transforms, motion compensation, deblocking and CABAC that are most of
decoding.

The one measured figure is 5.0 ms a frame at 640x360 under QEMU (`testing.md`
18.182). Scaled by pixels to 1080p that is about 45 ms, over the 33 ms a
frame at 30 fps allows - **an extrapolation from an emulator, and so worth
nothing as a figure about the M700**. No published benchmark of FFmpeg on
an i5-6500T or i7-6700T class core without SIMD was found.

So the first step is a number from the machine: the Video app playing a
1080p H.264 file on the M700, with the decode time it already reports. It
decides between three paths, cheapest first:

1. **Software keeps up.** Then hardware decode is for power and heat, not
   for being able to play, and it waits.
2. **Software is close.** Then FFmpeg's own SIMD and threads are the cheaper
   step: enabling x86 assembly for the x86 build (it needs NASM and a
   second configuration) and FFmpeg's frame threads over Kosmos's threads.
   Typically several times faster, and a build change rather than a driver.
3. **Software is far off.** Then the driver below.

What hardware decode takes off the processor is entropy decoding, the
inverse transform, motion compensation and deblocking. What stays is parsing
the stream down to the slice headers and keeping the reference frames, which
FFmpeg already does in Kosmos.

## 3. What Linux does, and why it does not port

| Linux part | Lines | What it is for here |
|---|---|---|
| i915, all of it | 418,774 | |
| of which the display | 201,207 | nothing (Kosmos keeps the firmware's screen) |
| of which what a video-engine-only path cannot avoid | about 24,000 | register access and power wells, the GPU's page tables, submission, contexts, interrupts, cache settings, workarounds, reset |
| GuC / HuC loading | 8,346 | encode rate control only |
| media-driver (iHD), all of it | 3,710,234 | |
| of which H.264 decode, hand-written | 16,802 | plus 15,270 generated, on its OS layer, gmmlib (115,301) and libva (34,709) |
| intel-vaapi-driver (i965), H.264 decode | about 1,230 | plus about 2,100 of helpers and libdrm's 4,100; discontinued in October 2024 |

Both of Intel's user-level drivers sit on libva above and i915's GEM and
`execbuffer2` below. A port would mean either Kosmos growing an
i915-shaped interface - buffer objects, tiling, relocations, contexts, some
twenty ioctls - or rewriting their OS layer. **Fuchsia did the second**:
its fork of media-driver runs on its own GPU driver through a 2,051-line
backend. That is proof it can be done and a measure of what it takes, but
it brings 2.5 million lines of C++ into a system whose whole userland is
smaller than that.

## 4. What a driver of Kosmos's own needs from the hardware

From the PRMs (Vol 3 *GPU Overview*, Vol 6 *Command Stream Programming*,
Vol 8 *Media VDBOX*) and i915 v7.2 for the details:

- **The BAR.** BAR0 (GTTMMADR) is 16 MB: the registers in the lower half,
  the global GTT's entries from 8 MB. Kosmos already finds this device's
  BAR0 for the backlight (`hal/pc/devices.c`).
- **Forcewake.** The video engine's registers are in the media power well;
  it is woken through `FORCEWAKE_MEDIA_GEN9` (0xa270) and acknowledged at
  0xd88 before they answer. (Offsets from i915's `intel_uncore.c`; to be
  checked against Vol 2c before use.)
- **The global GTT**: the ring, the context and every buffer the engine
  reads or writes has to be mapped there, by writing entries into BAR0. A
  first driver can put everything in the global GTT and leave per-process
  page tables (PPGTT) for later.
- **Submission.** The PRM documents two ways on Gen9. **Execlists**, which
  every open driver uses - Linux has forced it on Gen9 since 3.19, and
  Fuchsia and Genode do the same - needs a two-page context image for the
  video engine and a submission port (ELSP at engine base + 0x230). **The
  legacy ring buffer**, which Vol 3 says works on the video engine with no
  context image at all, is simpler and **untested territory**: nobody's
  driver on Gen9 uses it.
- **Completion.** `MI_FLUSH_DW` with a value written to memory, and
  `MI_USER_INTERRUPT` for the interrupt (bit 0 of the video engine's bits in
  `GEN8_GT_IIR(1)`), which arrives as an MSI - the PC HAL has MSI already.
- **The decode itself**, Vol 8. Two formats. The *long* one is what i965
  emits: 8 commands a picture and 5 a slice, the host having parsed every
  slice header. The *short* one is what Mesa uses: the hardware parses the
  slice headers, and the host gives it the sequence and picture parameters
  and the reference frames. Either way the host parses SPS and PPS, which
  FFmpeg's parser already does. The engine is stateless: every picture sends
  its whole state.

## 5. What there is to learn from

| Project | Video engine | Size | Why it matters here |
|---|---|---|---|
| **Fuchsia**, `msd-intel-gen` | yes: VCS0, execlists, forcewake, no GuC/HuC | about 9,000 lines, BSD | a GPU driver outside Linux, on a microkernel, that drives this engine and lists `1912` |
| **Mesa anv**, Vulkan Video | yes: H.264 decode compiled for Gen9, short format, no HuC | the decode is 364 lines; the command layouts are `genxml/gen90.xml` | the smallest maintained description of the commands, and the layouts as data a build step can read |
| Linux i915 | yes | see above | the register-level truth where the others are silent |
| i965 | yes: long format | about 1,230 lines for H.264 | a second opinion on each command |
| Genode | no, render engine only | 9,505 lines | a userland Intel GPU driver on a microkernel, for its structure |
| Haiku, SerenityOS | no | | modesetting only |

No hobby operating system or unikernel was found with a video engine of its
own. This would be the first.

## 6. What Kosmos has for it, and what it lacks

**Has**: drivers as processes (`xhci.c`, `e1000.c`), device registers
mapped by `SYS_DEV_MAP`, physical addresses of contiguous regions by
`SYS_MEM_PHYS`, interrupts as capabilities, MSI on the PC, and this
device's BAR already found. And FFmpeg's H.264 parser, which gives the
hardware what it needs, and FFmpeg's software decoder, which gives a
decoded frame to compare the hardware's with bit for bit.

**Lacks**:

- **`SYS_DEV_MAP` stops at 4 MB** (`DEV_MAP_PAGES_MAX`), a bound set to catch
  a wrong number when the largest BAR was xHCI's 64 KB. This one is 16 MB,
  or at least the 8 MB of the global GTT's entries.
- **No IOMMU fencing.** A GPU reads and writes wherever its GTT says; a bug
  in the driver's GTT is a device writing anywhere in memory. The same open
  question as storage's (`roadmap.md`, *storage at full speed* 4).
- **Frames that stay on the GPU side.** Decoded NV12 has to be untiled and
  converted before the compositor blits it - a new kit function in C, or
  the VEBOX or SFC, which Vol 9 and 15 describe.
- **A test that runs without the machine.** None of this runs under QEMU.
  What can be checked on the Mac is what is pure C: building each command
  into its words, untiling NV12, the GTT's entries - held to the PRM and to
  `gen90.xml` the way `fat_decode.c` is held to a disk. What needs the GPU is
  checked on the M700, by a program that says what it found.

## 7. The steps, if it goes ahead

Each ends in something the M700 can show, and each is a stick:

1. **Measure software decode at 1080p on the M700** (section 2). Needs no
   code, only a stick and a file.
2. **Find and wake it**: read the device ID, map BAR0, wake the media well,
   read an engine register back; `diagnose` says what it found.
3. **Run a batch**: `MI_STORE_DATA_IMM` then `MI_BATCH_BUFFER_END` on VCS0,
   the value appearing in memory - intel-gpu-tools' `gem_exec_basic` is the
   model. Ring buffer first, as the simpler path the PRM documents;
   execlists if it will not run.
4. **Its interrupt**, by MSI.
5. **One picture**: an IDR frame of the test clip decoded, NV12 out,
   compared with FFmpeg's software decode of the same frame.
6. **A stream**: reference frames, P and B, and the Video app choosing the
   engine with software as the fallback.
7. **On the screen**: untiled and converted, into the surface the Video app
   already hands the window manager.
8. **Encode**: VDEnc at constant quality, no firmware, for the recorder and
   the camera.
9. **Later, 2D**: the blitter engine for copies and fills - the accelerated
   2D the roadmap wants - on the same submission path.

**Effort.** Steps 2 to 4 are the driver's foundation and the riskiest part,
because a GPU that ignores a command says nothing, and there is no emulator
to watch it in. Steps 5 and 6 are the codec. What was built here before is
the measure, and it cuts both ways: USB went from its first step - every
xHCI controller found (`b1272ce`, 13 September) - to a stick holding its own
`/Home` (`8e10174`) in two days. But xHCI runs under QEMU, and every one of
those steps was tried over and over on the Mac before the ThinkPad saw it.
Here each try is a stick, the manuals are larger, and a wrong bit in a
context image hangs the engine rather than returning an error. So the time
is set by how many sticks steps 2 to 4 take, which nobody can know in
advance: **one to three weeks of sessions, each ending at the M700**, is an
estimate and is labelled as one.

## 8. What is Diego's to decide

1. **Whether to measure first** (step 1), and to let that number decide
   between software, FFmpeg's SIMD and threads, and the driver.
2. **If the driver: Kosmos's own**, written from the PRMs with Fuchsia, Mesa
   and i915 as references - rather than a port of Linux's pair.
3. **A stick for each step**, since none of it can be tried under QEMU.
4. **Raising `SYS_DEV_MAP`'s bound** for this device, and living without an
   IOMMU for now, as storage does.
