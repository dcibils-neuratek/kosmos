<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Quake III on Kosmos: Quake3e and its Vulkan renderer

Written on 20 November 2026 as an analysis, for Diego to decide on. **No
port has been started.** It was written by reading Quake3e's source
(`github.com/ec-/quake3e`, a shallow clone of `master` taken that day) next
to Kosmos's own tree. Every claim about either tree names the file it comes
from. Where something is a guess, it says so.

---

## In short

- **Yes, it can be ported, and nothing in it goes against a principle.**
  Quake3e is plain C with no C++ in it. Its renderer needs only **Vulkan 1.0,
  `VK_KHR_surface` and `VK_KHR_swapchain`**. It finds the platform's
  surface extension by itself: any instance extension whose name ends in
  `_surface` is turned on, and `VK_KOSMOS_surface` ends that way. The
  platform has to supply exactly two Vulkan functions,
  `VK_GetInstanceProcAddr` and `VK_CreateSurface`. On Kosmos those are
  `vk_icdGetInstanceProcAddr`, linked in as `vulkantest.elf` already does,
  and `vkCreateKosmosSurfaceKOSMOS` on a Window Kit window.
- **It builds statically as it is.** The renderer can be linked in
  (`USE_RENDERER_DLOPEN=0`, which is what its own macOS build does). The
  game, cgame and ui modules run as QVM bytecode, and they are compiled by
  default (`vm_game`, `vm_cgame` and `vm_ui` default to 2). Its x86-64 QVM
  JIT writes code into `mmap(PROT_READ|PROT_WRITE)` memory and then calls
  `mprotect(PROT_READ|PROT_EXEC)`. Kosmos's POSIX layer and Nebula allow
  exactly that: written first, then made executable, never both at once
  (`runtime/posix/mman.c`, `kernel/reserve.c`). The interpreter is there
  as a fallback. **Built, it went otherwise** (21 November): the demo's
  QVMs are from 1999 and older than the engine's interface, so the three
  modules are ioquake3's game code, compiled native into the image (§4,
  *What happened*).
- **The platform layer is Kosmos's own, about a thousand to fifteen hundred
  lines**, written the way Quake 1's is (`user/installed/Quake/quake_kosmos.c`,
  994 lines). It covers `Sys_*`, the Vulkan glue, input, and `SNDDMA_*`
  onto the audio ring. Porting SDL2 instead would be a larger job than
  writing the platform, and there is no SDL in Kosmos. The only "SDL" in
  the tree is the stub headers Chocolate Quake's port carries.
- **The biggest obstacles are Kosmos's, not Quake3e's.** Each of them is a
  gain other applications would reuse:
  1. **Mouselook needs pointer capture with relative motion**, and Kosmos
     has neither. Quake 1 here looks around by dragging with the button
     held (`quake.lua`, line 200), because the window manager reports the
     pointer's position only, and the board clamps that position at the
     edges (`hal/pc/pointer.c`).
  2. **The Window Kit's frame path carries characters, not key
     transitions.** `WM_EV_KEY` is a code (`user/include/wmproto.h`). The
     `rawkey` press and release a game needs reaches Lua's `wmproto.poll`
     but not `kw_poll`. A Vulkan program's window is a Window Kit window.
  3. **One image cannot carry both `lavapipe` and `anv`** (`gpu3d.md` G7a).
     So without a loader there are two images: one for QEMU and one for
     the M700.
  4. **Multiplayer needs UDP in the POSIX layer.** The network stack has
     datagrams (`NET_OP_UDP_BIND`, `user/include/netproto.h`), but
     `socket(SOCK_DGRAM)` is refused (`runtime/posix/socket.c`, line 233).
     Single player needs none of it, because Quake III's local game talks
     to itself over loopback.
- **Speed on the M700: probably far above 60 frames a second at 1920 by
  1080.** This is a guess; no published HD 530 figure for Quake3e was
  found. Each frame also pays about 2 ms to reach the screen: the
  processor's copy into the window and the commit, scaled from the cube's
  measurement (`testing.md` 18.936). That sets a ceiling of a few hundred
  frames a second, not a problem at 60 Hz. G9's flips remove it.
- **Content to test with.** Quake III's retail paks are commercial. Two
  free sets exist: **OpenArena**, whose data and QVMs are GPL, and **id's
  Quake III demo** (`pak0.pk3`), which Quake3e recognises by checksum
  (`DEMO_PAK0_CHECKSUM`, `code/qcommon/files.c`). The gate does the same
  as for Doom and Quake 1: starts the engine with nothing to play, and
  runs the real game outside the tree.
- **Size, a guess:** five to eight sessions to a playable single-player
  game on the M700. Pointer capture and the key path come first, because
  every game wants them.

---

## 1. What Quake3e is, as code

The parts a client needs, counted from the clone (lines of C):

| Part | Lines | Kept as it is? |
|---|---|---|
| `code/qcommon` | 49,588 (about 14,000 of it the JITs for other processors) | yes; `vm_x86.c` and `vm_aarch64.c` per architecture |
| `code/client` (sound mixing included) | 21,489 | yes |
| `code/server` | 12,789 | yes; a local game is a server |
| `code/botlib` | 32,836 | yes; bots are the single-player game |
| `code/renderervk` | 36,938, plus 1.4 MB of SPIR-V in `shaders/spirv/shader_data.c` | yes |
| `code/renderercommon` | 3,912 | yes; image loaders |
| `code/unix`, `code/sdl`, `code/win32` | - | **no**: Kosmos writes its own platform |

The shaders arrive already compiled, as C arrays. Nothing has to run
`glslangValidator` to build it, though the Mac has it if the shaders are
ever changed.

Of what Quake3e bundles, Kosmos already has some, and those are linked
rather than copied, which follows *does something already supply this?*:

- **JPEG**: libjpeg-turbo from W5 (`build/w5/<arch>/install/lib/libjpeg.a`),
  used with `USE_SYSTEM_JPEG=1`, instead of `code/libjpeg`.
- **curl**: present, but only used for downloads from servers; start with
  `USE_CURL=0`.
- **Ogg Vorbis**: used only for music in mods. Leave it off
  (`USE_OGG_VORBIS=0`); Quake III's own sounds and music are WAV.
- **FreeType**: off by default (`BUILD_FREETYPE`, `renderercommon/tr_font.c`).
- **SDL2**: not used.

**Licence.** Quake3e is GPLv2, as Quake 1 and Doom are, and like them it
would be an image of its own in `/Home/Apps`. It is a line in `LICENSE`, nothing more.

## 2. The platform layer

A Linux build of Quake3e uses one of two platforms:

- **Native**: `code/unix/unix_main.c` and `unix_shared.c`, then X11 for the
  window (`linux_glimp.c`, `x11_*.c`), `linux_qvk.c` for Vulkan, which
  `dlopen`s `libvulkan.so.1`, and `linux_snd.c` for sound (ALSA or OSS,
  through a mapped buffer).
- **SDL** (`USE_SDL=1`, the default): `code/sdl/sdl_glimp.c`,
  `sdl_input.c` and `sdl_snd.c`, on top of the same `unix_main.c` and
  `unix_shared.c`.

**Neither fits as it is, and porting SDL2 would cost more than it saves.**
SDL2 is a large library with its own platform layer: video, events,
audio, threads, timers and its own Vulkan loading. All of that would have
to be written for Kosmos too, before Quake3e could use any of it. Quake3e
uses perhaps five percent of SDL. The interface Quake3e expects from a
platform is small and well defined: `qcommon.h` lists the `Sys_*`
functions, `client.h` lists `GLimp_*`/`VKimp_*`, and there are `IN_*` and
`SNDDMA_*`. So the port is a Kosmos platform in four files, written beside
the engine. This is how Chocolate Quake was ported
(`user/installed/Quake/quake_kosmos.c`).

**Would SDL2 be worth having for its own sake?** Possibly, later, for
other ports. `gpu3d.md` §1 counts SDL among the things a real GL brings in
reach. But a game port should not be what decides it. If SDL2 is ported
one day, it would be its own kit, with Quake3e as one of its users.

### What the platform files are

| File (Kosmos's) | Replaces | What it does | Size, a guess |
|---|---|---|---|
| `q3_kosmos.c` | `unix_main.c` | `main`'s work split into `q3.start` (`Com_Init`) and `q3.frame` (one `Com_Frame`), as `quake.start` and `quake.frame` are; `Sys_Error` and `Sys_Quit` returning to Lua by `kosmos_exit_arm`, as Quake 1's do; console output to the spill ring; a stack of its own if one is needed | 300 |
| `q3_shared.c` | `unix_shared.c` | time (`Sys_Milliseconds`, `Sys_Microseconds` on the counter), files (`opendir`, `readdir`, `stat`, `mkdir` from the POSIX layer, all present in `runtime/posix/fd.c`), the home and base paths as `/Home/Apps/Quake3`, random bytes (`getentropy`), `Sys_LoadLibrary` answering "no" | 300 |
| `q3_vk.c` | `sdl_glimp.c` / `linux_qvk.c` | `VKimp_Init`: `kw_open`, the window's size into `glconfig`; `VK_GetInstanceProcAddr` = `vk_icdGetInstanceProcAddr`; `VK_CreateSurface` = `vkCreateKosmosSurfaceKOSMOS`; `GLimp_*` stubs, since there is no OpenGL renderer in this image | 150 |
| `q3_input.c` | `sdl_input.c` | `IN_Init`, `IN_Frame`, `Sys_SendKeyEvents`: `kw_poll`'s events into `Sys_QueEvent` as `SE_KEY` (transitions, in Quake III's numbering), `SE_CHAR` (the console's text) and `SE_MOUSE` (relative motion); pointer capture taken and released as the game asks (`in_nograb`, the menu, the console) | 250 |
| `q3_snd.c` | `sdl_snd.c` | `SNDDMA_*` onto the audio server's ring, as `quake_kosmos.c` lines 420-525 do for Quake 1 | 200 |
| `quake3.lua` | - | the installed application's file: `-- kosmos: application`, `image quake3.elf`, the loop, the pak path, the sound stream from `audio.lua` | 150 |

**What Quake3e reaches for that Kosmos refuses, and the patch each needs**
(in `runtime/patches/quake3e/`, against a fetched release held to its
sum, as Mesa's are):

- `qcommon/files.c` `FS_PipeOpenWrite`: `popen`, used only for
  `\video-pipe`'s external ffmpeg. Compiled out.
- `qcommon/net_ip.c`: `ioctl(FIONBIO)` (line 986), which is refused here
  (`runtime/posix/refused/sys/ioctl.h`). It should be `fcntl(O_NONBLOCK)`.
  It also includes `<net/if.h>` and `<ifaddrs.h>`, which do not exist here,
  and uses `select`. For single player, the simpler path is a `q3_net.c`
  of Kosmos's, with loopback only and every `NET_*` and `Sys_SendPacket`
  answering "no network". This is the network table Quake 1's port has. The real `net_ip.c` comes with multiplayer (step Q6).
- Signals (`InitSig` in `VKimp_Init`), `termios` and `pwd.h` are all in
  `unix_main.c`/`unix_shared.c`, which Kosmos does not compile, so they
  need no patch.
- `dlfcn.h`: only in the platform's files. With `USE_RENDERER_DLOPEN=0`
  and QVMs, nothing in the engine calls it.

There is no `fork`, `exec`, signal or `dlopen` that the engine itself
needs. Nothing crosses the principle in `CLAUDE.md`, *Compatibility inside
a process yes, at system level never*.

## 3. The Vulkan renderer

From `code/renderervk/vk.c`:

- **The API version asked for is 1.0** (`appInfo.apiVersion =
  VK_API_VERSION_1_0`, 1.1 only in a debug build, line 1348).
- **Instance extensions**: every `VK_*_surface` it is offered, plus
  `VK_KHR_get_physical_device_properties2`, `VK_EXT_debug_utils` and
  `VK_KHR_portability_enumeration` if they are there
  (`used_instance_extension`, line 1259). `VK_KOSMOS_surface` qualifies
  without any change.
- **Device extensions**: `VK_KHR_swapchain` is the only one required (line
  1724). Dedicated allocation, memory requirements 2, timeline
  semaphores, the memory model, buffer device address and 8-bit storage
  are used if offered. `anv` on Gen9 offers all of them; they are core in
  1.2.
- **Device features**: `fillModeNonSolid` is required (line 1767);
  `shaderInt64`, `wideLines`, stores and atomics, and anisotropy are used
  if present. `anv` on Gen9 and `lavapipe` both have `fillModeNonSolid`.
  I am confident of this, but I did not read it from this tree's dEQP
  run.
- **The swapchain**: it needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT` (line 510;
  screenshots read the image back) and would like `TRANSFER_DST`. Kosmos's
  WSI answers `wsi_caps_get_image_usage()`
  (`runtime/ports/mesa/src/vulkan/wsi/wsi_common_kosmos.c`), which in
  Mesa includes both. Formats `B8G8R8A8_UNORM`/`SRGB` are in its list
  (line 1468). It prefers MAILBOX, then IMMEDIATE, then FIFO. Kosmos offers
  FIFO and IMMEDIATE, so it takes IMMEDIATE unless `r_swapInterval` is set.
  A resize returns `VK_SUBOPTIMAL_KHR` or `OUT_OF_DATE`, and Quake3e
  recreates the swapchain on either (`vk_restart_swapchain`, line 3864).
  That matches how the Kosmos WSI handles a `KW_RESIZE`.
- **Where the surface is made**: in the platform, through
  `ri.VK_CreateSurface` (`tr_public.h` line 229, called at `vk.c` line
  1970). The renderer never names a window system.

**`VKimp_Init` on Kosmos**, as the cube already does it
(`user/kits/vulkantest/vulkantest.c` lines 1105-1130):

```c
void VKimp_Init(glconfig_t *config)
{
    window = kw_open("Quake III Arena", width, height, KW_RESIZABLE | KW_CENTRE);
    /* fail with kw_why() if NULL */
    struct kw_surface s = kw_surface(window);
    config->vidWidth = s.width;  config->vidHeight = s.height;
    config->isFullscreen = qfalse;
    config->driverType = GLDRV_ICD;  config->hardwareType = GLHW_GENERIC;
    IN_Init();
}

void *VK_GetInstanceProcAddr(VkInstance i, const char *name)
{
    return (void *)vk_icdGetInstanceProcAddr(i, name);
}

qboolean VK_CreateSurface(VkInstance i, VkSurfaceKHR *out)
{
    VkKosmosSurfaceCreateInfoKOSMOS info = {
        .sType = VK_STRUCTURE_TYPE_KOSMOS_SURFACE_CREATE_INFO_KOSMOS,
        .window = window,
    };
    PFN_vkCreateKosmosSurfaceKOSMOS create =
        (PFN_vkCreateKosmosSurfaceKOSMOS)vk_icdGetInstanceProcAddr(i, "vkCreateKosmosSurfaceKOSMOS");
    return create && create(i, &info, NULL, out) == VK_SUCCESS;
}
```

**Linking.** As `anvtest.elf` is linked (`Makefile`, around line 4736):
the engine's objects, `@$(MESA_DIR)/anv.txt`, zlib, and the C++ runtime
that Mesa's Intel compiler needs (`CXXRT_LIBS`). Quake3e does not need
C++ itself, but its image carries the runtime because of Mesa. For QEMU it
is the same engine with `@$(MESA_DIR)/lavapipe.txt` and LLVM, as
`vulkantest.elf`. That makes two images today, `quake3e.elf` and
`quake3e-lvp.elf`, and `quake3.lua` would choose between them by whether
`/Devices/gpu` exists. This is G7a's open point: *"a loader choosing
between them is what G7c's program will need"*. A static Vulkan Kit
holding both drivers in one image is better solved once, as a kit, than by
two images per game.

I did not find how an installed application is granted `/Devices/gpu`.
`anv_kosmos.c` says a program has it "only when it was granted the GPU",
and `jetski.lua` uses `-- kosmos: needs network`. I did not confirm the
GPU's equivalent. Check this before Q3.

## 4. The QVM: JIT or interpreter

Quake III's game logic is in three QVMs, inside the paks. By default
Quake3e compiles them (`vm.c` line 290). `vm_x86.c` reserves code with
`mmap(PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS)` (line 4494) and
then makes it executable with `mprotect(PROT_READ|PROT_EXEC)` (line 4453).
On Kosmos:

- Anonymous `MAP_SHARED` is accepted. Only a *file* shared is refused,
  `ENODEV` (`runtime/posix/mman.c` line 140).
- RW then RX is what `kernel/reserve.c` documents: *"A page may be written
  and then made read and execute, which is how a JIT that flips works"*.
  Only both at once is refused.

So **the JIT should work with no change at all**. That conclusion comes
from reading the code; nothing was run. JavaScriptCore's double-mapped
pool (`docs/jit.md`) is not needed, because Quake3e writes each QVM's code
once, at load, and never again. `vm_aarch64.c` does the same and calls
`__clear_cache`, which matters for the AArch64 build later.

**The first step should still run with the interpreter**
(`vm_game 1`, `vm_cgame 1`, `vm_ui 1`), so that a crash in the JIT and a
crash in the platform are never confused. The interpreter costs a few
times the JIT on the game's logic, which is a small part of a Quake III
frame. That is a guess from how the engine is usually described, not a
measurement here. Then switch to 2 and measure the difference.

### What happened: the game's code is native (21 November 2026)

**None of the above survived the demo.** Its three QVMs are from 1999,
and the user interface's says it is version 3 where every 1.32 engine,
Quake3e included, expects 6 - so Quake3e refuses it before anything is
drawn: `ERROR: User Interface is version 3, expected 6`. No engine setting
changes that; the demo's code is older than the engine's interface.

**So the game's code is compiled from source, native, into `quake3.elf`.**
ioquake3 keeps id's GPL game, cgame and q3_ui building against the 1.32
interface, and its own README says the same thing: the demo's QVMs will not
work, use the project's. It is fetched at a commit (`tools/upstream.py`,
`ioq3`), and each module is compiled as released, from upstream's own list
(`cmake/basegame.cmake`), with its define (`QAGAME`, `CGAME`, `UI`).

**Three modules in one image** each have a `vmMain`, a `dllEntry`, a
`q_shared.c` and a `bg_misc.c`, so linked as they are they would meet. Each
is made one object (`ld -r`) in which everything is local but its two
doors, renamed for it - `q3_game_vmMain`, `q3_game_dllEntry` and so on
(`objcopy`, which renames before it decides what stays global, so `-G`
names the new names). The engine loads a "library" the way it always does,
asking `Sys_LoadLibrary` for `qagamex86_64.so` in its game folders, and
`q3_kosmos.c` answers from a table of the three; `Sys_LoadFunction` hands
back the doors. Nothing is linked dynamically, and nothing upstream is
edited.

**Two things a native game needs said** (`quake3.lua`): `vm_* 0`, and
`sv_pure 0`, since a pure server allows only QVMs - which is right for a
server strangers connect to and means nothing for a game on one machine.

**And the demo needs no key.** id's 1.32 q3_ui recognised the demo (four
arenas and one special) and never asked for a CD key; ioquake3's took that
test out, so with the demo's paks it opened on a key screen the demo never
had. The user interface is handed the engine's calls through
`ui_calls`, which answers the key's question yes while the game is the
demo (`fs_basegame demoq3`) - what the demo's own code did - and passes
every other call, and every call for the full game, through unchanged.

**What it gains**: the game's logic runs at the engine's speed rather
than the interpreter's, which under TCG is the difference between a map
loading and not, and no JIT (Q5) is needed for the demo at all. Q5 stays
for a mod whose code exists only as QVMs.

## 5. Sound, input, files, network

**Sound.** Quake III mixes into a DMA-style buffer it asks the platform
about (`SNDDMA_Init`, `SNDDMA_GetDMAPos`, `SNDDMA_BeginPainting`,
`SNDDMA_Submit`; `code/sdl/sdl_snd.c` is the model). This is the same
interface as Quake 1's. `quake_kosmos.c` already feeds the audio server's
ring from it, *AHEAD* periods at a time, from inside `GetDMAPos`, with no
thread. `q3_snd.c` would be that code, ported. Capture
(`SNDDMA_StartCapture`, VoIP) answers "none". Whether the stream asks for
the AUDIO band (`kosmos: needs audio`) is a later question. Quake 1 does
not ask.

**Input. This is where most of Kosmos's new work is.**

- *Keys*: Quake III needs transitions (`SE_KEY`, down and up) and
  characters (`SE_CHAR`, for the console and the menus' text fields). The
  window manager sends both to a window's Lua (`rawkey` and `key`,
  `user/bin/programs/wm.lua` around line 7475). The Window Kit's frame
  path, `WM_FRAME_POLL`, carries only `WM_EV_KEY` (`wmproto.h` line 48;
  `user/kits/window/window.c` line 354). There are two ways to fix this.
  The first is a `WM_EV_RAWKEY` in the frame protocol and a `KW_RAWKEY` in
  `kosmos_window.h`, which Flight, Cafesa3D and every C game would use too.
  The second keeps the Lua side owning the events, as `quake.lua` does,
  and passes them into the C kit. The first is better: the second puts
  the keys on Lua's path for no gain.
- *Mouselook*: Quake III turns relative motion into view angles without
  limit, so you can turn round and round. Kosmos gives a window only the
  pointer's position, and only within the window. The board clamps the
  position at the edges of its range (`hal/pc/pointer.c` lines 92-96,
  128). Quake 1 gets round this by looking only while the button is held
  (`quake.lua` line 200). Pointer capture needs three things, none of
  which exists today:
  1. **The board reports motion as well as position**: a count of
     relative movement since the last look, kept beside the position as
     the wheel's notches are (`struct pointer_state`, `hal/hal.h` line
     621). Under QEMU the tablet is absolute, so there the window manager
     derives the motion from positions, which is exact until the host's
     pointer leaves the window. A QEMU relative mouse (`-device
     virtio-mouse` or `usb-mouse`) would test the real path. This changes
     `hal.h`, so it is done on purpose, and `hal.h` should say why.
  2. **The window manager grants capture to the focused window**: the
     cursor hidden, motion sent as deltas (`WM_EV_MOTION`), and released
     on Super, on Super Tab, on losing focus, and when the program asks.
     The rule *a release goes where its press went* (`wm.lua` around line
     7420) has a counterpart here: capture never survives losing focus.
  3. **The Window Kit's door**: `kw_capture(w, on)`.
- *Controller*: Quake III's joystick axes would come with Flight's F7
  analogue sticks (`docs/flight.md`). Until then a pad reaches Quake III as
  keys (`user/drivers/usb/pad_decode.h`).

**Files.** The base game would be in `/Home/Apps/Quake3/baseq3/*.pk3`, and
the writable home (config, screenshots, demos) in a folder of its own
beside it. Quake3e reads paks through its own `unzip.c` and `fopen`, so
the POSIX layer's files are enough. Unlike Quake 1's port, nothing has to
be passed in a region by Lua: `pak0.pk3` is 457 MB on retail and Quake3e
seeks through it rather than holding it. Whether `diskfs` serves random
reads in a half-gigabyte file quickly enough is worth measuring. Loading a
map reads many small entries.

**Network.** A local game is a client and a server in one process,
talking over loopback (`net_chan.c`). For single player and bots,
`q3_net.c` returns "no network" and nothing more is needed. Multiplayer
needs `SOCK_DGRAM` in `runtime/posix/socket.c`, on the stack's
`NET_OP_UDP_BIND` rings: `sendto`, `recvfrom`, non-blocking, and `select`
or `poll` over datagram sockets, plus broadcast for finding servers on a
LAN. I did not check whether the stack sends to 255.255.255.255. IPv6 is
optional (`net_enabled`). This is one door that other ports would use as
well: an RTP stream, a game, NTP from ported code.

## 6. How fast, on the M700

**Nothing here has been measured for Quake III. What follows is reasoning
from Kosmos's own numbers.**

- *The GPU's share.* Quake III is a 1999 renderer: tens of thousands of
  triangles, lightmaps and a few passes. Quake3e's Vulkan renderer is
  described as low-overhead (multiple command buffers, VBO-cached world).
  An HD 530 is a ~400 GFLOP part that runs much heavier games. My guess is
  a few hundred frames a second at 1920 by 1080 on `anv`, with r_fbo's
  post effects off. I found no published Quake3e figure for HD 530 or any
  Gen9 part. On the Raspberry Pi 4's v3dv, Igalia found Vulkan Quake III
  up to 60% faster than the GL1 renderer, which shows the Vulkan path
  works on a modest GPU.
- *The processor's share.* A Quake III frame's game, bots and sound on a
  Skylake core should be a millisecond or two with the JIT. This is a
  guess.
- *Reaching the screen, which is measured.* At 1024 by 1024 the cube's
  present was: fence 1,048 µs, the processor's copy 815 µs, commit 280 µs
  (`testing.md` 18.936). The copy is linear in pixels. A 1920 by 1080
  window is about 2.0 times as many pixels, so about 1.6 ms of copy and
  0.3 ms of commit, about **2 ms a frame**, plus the GPU's own blit into
  the linear buffer, which is in the fence. That caps Quake III at
  something like 300 to 400 frames a second, whatever the GPU does. At 60
  Hz it is 12% of the frame, and it adds about 2 ms between a frame drawn
  and a frame shown.
- *Composing.* After the commit, the window manager composes the window's
  damage. A full-window game damages everything, every frame. I found no
  M700 figure for composing a 1920 by 1080 window that changes every
  frame. **That is the first thing Q4 should measure**, because it could
  be larger than the copy.
- *What G9 changes.* A full-screen flip (`docs/m700-2d.md` 4c) shows the
  swapchain image itself, so the copy and the composing both go. G7c's
  owed item, rendering straight into the window's buffer (`roadmap.md`,
  *Owed from G7c*), removes the copy for a game in a window. Quake III
  would be a good first user of both, and a good measure of them.
- *Under QEMU*, `lavapipe` under TCG is for correctness, not speed.
  Expect a few frames a second at 640 by 480. That is a guess, but the
  number does not matter, because the suite checks pictures.

## 7. The steps

**Started on 21 November 2026**, first in the queue (Diego: "Quake 3e
port so I can play tomorrow"). **The content is id's Quake III demo**
(Diego chose it over his own paks and OpenArena): `demoq3/pak0.pk3`,
46,853,694 bytes, sha256 `e77abad2...0754b08`, taken out of id's
`linuxq3ademo-1.11-6.x86.gz.sh` (sha256 `64dee3f6...6150a4ec`) by
reading its archive past the script's 165 lines, never running it; it
lives in `build/downloads/quake3e`, not the tree. Quake3e is pinned at
`dec793de9677bc9c16c7027e8905c2d4f4643a19` (the `latest` tag that day,
15 November; tarball sha256 `6ecfef08...7274cd`).

Each step leaves a permanent test, x86-64 first, under QEMU with
`lavapipe`, then on the M700 with `anv`. A step that changes the window
manager, the Window Kit or the HAL runs the whole gate. The others run
only their own suites.

- **Q0 - this document agreed, and the design an app needs.** Quake III
  is a port, not a Kosmos application with a window of its own design, so
  a mockup is mostly the window and its first screen. The feature list and
  the architecture are this document. A diagram,
  `quake3e-architecture.png`, would be drawn if Diego wants one.
- **Q1 - the key path and pointer capture, as kits.** `WM_EV_RAWKEY` and
  `WM_EV_MOTION` in `wmproto.h`; capture in `wm.lua`, released on Super,
  on focus change and on close; `kw_capture` and `KW_RAWKEY` in the Window
  Kit; motion in `hal_pointer_poll` (both boards). Quake 1's mouselook is
  moved onto capture, so the new path has a second user from day one.
  *Test*: a harness program in QEMU holds W through a focus change and
  sees its release; it gets motion past the screen's edge with a relative
  QEMU mouse attached; and capture is gone after Super Tab. *Control*:
  without capture, motion stops at the edge.
- **Q2 and Q3 - built on 21 November 2026, together** (`testing.md`
  18.986). The dedicated server was not built first: the engine's whole
  file list compiled unedited against the POSIX layer at once, so the
  client went straight into a window. The platform is `q3_kosmos.c`,
  `q3_vk.c`, `q3_input.c`, `q3_snd.c` and `q3_net.c`, with `quake3.lua` as
  the loop; the game's code is ioquake3's, native (§4, *What happened*),
  not QVMs. In QEMU on `lavapipe` it draws the demo's menu and plays The
  Longest Yard. *Test* (`x86-quake3`, in the gate): over a pak the suite
  makes - a `default.cfg` alone - the engine starts, finds the pak, opens
  Vulkan (`VK_RENDERER:`), loads the native user interface, does not stop,
  and the window holds a picture. *Control* (`--qvm`): the user interface
  asked for as a QVM fails both the native check and the not-stopping one.
  What was planned for these two steps follows, as it was written.
- **Q2 (as planned) - Quake3e built, dedicated server first.** The release fetched and
  held to its sum, patches applied, `quake3e-ded`'s objects (no client, no
  renderer) linked with `q3_shared.c` and a loopback `q3_net.c` into an
  image. *Test* (`x86-q3-nogame`, as `x86-nogame` does for Doom and Quake 1): started with an empty `baseq3`, it reaches "no pak found",
  then `exit`, and is back in Lua with nothing left dead.
- **Q3 (as planned) - the client in a window on `lavapipe`.** `q3_vk.c`, `q3_input.c`,
  `q3_snd.c`, and `quake3.lua`. QVMs interpreted. *Test*: with
  OpenArena's paks (outside the gate, as `make quake-check` is), the menu
  is drawn. Then a `timedemo` of a demo in OpenArena's paks runs to its
  end, and a `\screenshot` taken at a fixed frame is compared with one
  agreed reference. *In the gate*: the no-game start, plus the renderer
  brought up on an empty pak as far as `vk_initialize`, which needs no
  content. That checks the instance, the device, the surface and the
  swapchain.
- **Q4 - first half done on 22 November 2026** (`testing.md` 18.986): on
  the M700 the package installs, init chooses `quake3-anv.elf`, and the
  demo's menu and The Longest Yard run on the HD 530 at 125 fps, the
  frame cap, at 1280 by 720 in a window. Still to come: sound heard, mouse
  and keys played by a person, `timedemo`, and the present's split. The
  plan as written:
- **Q4 (as planned) - on the M700, on `anv`.** The second image, or the loader. Sound
  on. *Measure*: `timedemo` frames a second at 1280 by 720 and 1920 by
  1080; the present split into fence, copy and commit as in 18.936; and
  composing's cost for a full-window game (§6). *Test on the M700*: the
  same screenshot against the `lavapipe` reference, within a tolerance,
  since two drivers' rasterisers are not bit-identical.
- **Q5 - the JIT.** `vm_* 2`. *Test*: the same timedemo, with the same
  frame count and the same screenshot. *Measure*: interpreted against
  compiled.
- **Q6 - multiplayer.** `SOCK_DGRAM` in the POSIX layer, and the real
  `net_ip.c` with the `ioctl` patch. *Test*: a dedicated server in one
  QEMU guest and a client in another, or the host's Quake3e against the
  guest. Bots on a LAN game.
- **Q7 - full screen. Asked on 22 November 2026** (Diego, after playing:
  "Add full screen to the roadmap yes"). Today the game is always a
  window - `r_fullscreen` is forced to 0, since Kosmos has no full-screen
  mode for anything. What it wants: `\r_fullscreen 1` (and Alt+Return)
  gives the game the whole screen at the screen's resolution, with no
  frame, no Deskbar and nothing composed over it; Super, focus moving
  away or the game ending gives the desktop back. **It is the window
  manager's and the Window Kit's, not the game's** - one door,
  `kw_fullscreen`, with Quake III its first user and Video, Flight, Doom
  and Quake 1 its next, so a second copy is never written. Designed
  before it is built (`wm.lua`'s window states, what a full-screen window
  is told of the screen's size, the pointer capture of Q1 kept across
  it). *Test*: a harness window asks for full screen in QEMU and covers
  every pixel of the screen, the Deskbar gone, then Super gives the
  desktop back as it was. G9's flips are what make it cost nothing; until
  then it is composed like any window, which at 125 fps on the M700 is
  already enough.
- **Later**: full-screen flips (G9) with Quake III as the measure; AArch64
  on `lavapipe` with `vm_aarch64.c` (when ARM's suites are back); and the
  Pi 5's v3dv, which already runs Quake III on Linux.

## 8. Risks

- **Pointer capture touches the HAL and the window manager.** Both are
  shared, so mistakes there show everywhere. Capture that does not let go
  is the worst kind, because it takes the pointer from the person. It
  must be released by the window manager, never only by the program.
- **Two drivers' images.** Without a loader, every Vulkan application
  needs two images. That cost grows with every application, which is why
  the loader belongs in the Vulkan Kit rather than in any one port.
- **Memory.** `com_hunkMegs` defaults to 128 MB and the zone to 25 MB
  (`common.c` lines 49-55), plus the GPU's buffers. Kosmos's pools grow
  now, so this should fit. Quake 1's port comment about "48 MB a process
  may map" is older than that change.
- **Stack depth.** Quake 1 needed a stack of its own, a megabyte
  (`quake_kosmos.c`, `kosmos_call_on_stack`). Quake III's botlib and
  renderer have large frames as well. Measure the depth rather than guess.
- **`diskfs` and a half-gigabyte pak**, read at random: unmeasured.
- **Content in tests.** OpenArena is large, about half a gigabyte, so it
  should be fetched to `build/downloads` and held to a sum, not committed.
  The disk on this Mac is tight (`MEMORY.md`, *Full disk breaks the gate*).
  The demo pak is about 47 MB but is id's, and I did not check its terms
  for redistribution. Neither belongs in the tree.
- **Gen9's future in Mesa** (`gpu3d.md` §7) applies here as it does to
  everything on `anv`.

## 9. What Kosmos gains that other applications reuse

By the premise *kits supply, applications orchestrate*, most of this work
is not Quake III's:

| Gain | Where it lives | Who else wants it |
|---|---|---|
| Key transitions on the Window Kit's frame path | `wmproto.h`, `window.c`, `kosmos_window.h` | Flight, every C game, Cafesa3D's fly-through |
| Pointer capture and relative motion | `hal.h` (both boards), `wm.lua`, the Window Kit | Quake 1 (no more drag to look), Flight's yoke, Cafesa3D's camera, Remote Screen and Linux on Kosmos passing a mouse through |
| One Vulkan image for every driver: a static loader | the Vulkan Kit, `tools/mesa_build.py` | every Vulkan application, the cube and Flight first |
| Datagrams in the POSIX layer | `runtime/posix/socket.c` | any ported program that speaks UDP |
| A measured full-window game on the present path | `testing.md`, and G9's motive | the GPU compositor, flips |

The things that are Quake III's own and stay with it are the four
platform files, `quake3.lua` and the patches.

---

## What was not verified

- Nothing was built or run, on either side.
- That `anv` on Gen9 and `lavapipe` report `fillModeNonSolid`. Both should;
  it was not read from this tree's runs.
- How an installed application is granted `/Devices/gpu`.
- Whether the network stack sends broadcast datagrams.
- Every frame-rate figure for Quake III on the HD 530. No published
  number was found, and §6's figures are estimates.
- The terms for redistributing the Quake III demo.

Source on the Pi: Igalia, *V3DV Vulkan driver update: VkQuake1-3 now
working* (blogs.igalia.com/itoral/2020/07/23/v3dv_vulkan_driver_update).
