<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# State

**Update at the end of every session.** This file is what keeps you from
starting over each time. It says where the work is and what comes next,
and nothing older: every session before 19 November 2026 is in
`docs/state-history.md`, moved there whole, and every revision is in
`testing.md` section 18 and `book/logbook.md`.

Last updated: 2026-11-19, at 0.12.8. Pushed to 0.11.507; 0.11.508 to
0.12.8 are committed on `main`, and go out with `make prepush`.

---

## Where it is

**The review before 0.12.0 is done** (`docs/cleanup.md`; Diego, 18
November 2026: "I think we can now do a full code cleanup and review
before we continue"). Eight readers found 205 findings
(`docs/cleanup-findings.md`), each checked again before anything changes.

- **Batch 1, the bugs** (0.11.508, `testing.md` 18.908): `/Running` keeps
  who registered a name and refuses anybody else's `unregister`; the three
  interrupt paths are one `irq_after`; Telnet's command split over two
  reads; Pewter's menu bar out of the applications; Video for `.mov` and
  `.m4v`.
- **Batch 2, the old browser's remains** (0.11.509, 18.909): about two
  thousand lines with no caller since 0.11.461.
- **Batch 3, dead code elsewhere** (0.11.510, 18.910).
- **The gate inside ten minutes again** (0.11.511, 18.911, finding 207):
  twelve suites at once rather than ten; 113 suites in 8:21 on a day the
  library suites are skipped, and `arm-sandbox`'s deadline test measured
  on the counter rather than in ticks.
- **Diego's seven decisions** ("Yea to all", 19 November): six in 0.11.512
  (18.912) - Paint in Demos, the web kit's own HTML layout gone, the
  Deskbar's seed migration gone, `run_site.py` and `record_site.py` gone,
  one release image, the three stray directories deleted; the seventh,
  `NR_CPUS` from the machine, in 0.11.513 (18.913): `CPU_MAX` 256 and
  `nr_cpus` asked of the board.
- **Batch 4, one copy** (0.11.514 the C half, 18.914; 0.11.515 the Lua
  half, 18.915): byte order, UTF-8, the opaque mix, CRC-32, PNG through
  the Compress Kit, sizes and rates, a monitor's heading, `theme.mix`,
  HTML as text, a program's grants.
- **Batch 5, comments and documents made true** (0.11.516, 18.916): this
  file cut to where the work is, the rest in `state-history.md`;
  `CLAUDE.md`, the README and a dozen documents brought to the tree;
  comments in about fifty files.
- **Batch 6, work nobody reads** (0.11.517, 18.917): a window's own
  surface only when it brought no region; Connect's clock out of its
  repaint; the Deskbar's strip compared field by field.
- **Batch 8, tests nothing ran** (0.11.518, 18.918): the layout checker
  and the PDF test in `host-check`, `run_smpshot.py` gone, eleven x86
  twins of ARM-only suites; five more need work (roadmap).
- **Batch 7, measured before anything changes** (0.11.519, 18.919): the
  image canary a word at a time (215 ms of a TCG boot), a thread's index
  without a square; the sleeper scan and the harness's pixel loops
  measured and left; five measurements need a Pi, the M700 or a new
  instrument (roadmap). **Every batch is done.**
- **Finding 206** (0.11.520, 18.920): `/Running` grows past its 24 names,
  and lists in pages.
- **0.12.0** marks the review done.

**3D, G1 done (0.12.1-0.12.2, `testing.md` 18.921-18.922):** LLVM 23.1.2
built for Kosmos on both architectures - seven patches in
`runtime/patches/llvm/`, 3:00 from nothing - and `x86-llvm` has it compile
and run two functions inside Kosmos and write an AArch64 object; its JIT
needs no double mapping (`gpu3d.md` §5.2 corrected). Mesa 26.2.5 by its own
Meson (`tools/mesa_build.py`), configured for `lavapipe`, two patches in
`runtime/patches/mesa/`, its trial links answered by
`tools/mesa_trial_ld.py`; `x86-mesa` has NIR build, fold, print and
serialize a shader. **G2 begun (0.12.3, `testing.md` 18.923):** Vulkan
on Kosmos - `lavapipe` linked into `vulkantest.elf` with no loader draws a
triangle the runner chose, every pixel where its edge functions say;
and in 0.12.4 (18.924) a textured cube with a depth buffer, every pixel
where a ray cast through it says; in 0.12.5 (18.925) libc++'s locales,
iostreams over the one "C" locale. **G2 done (0.12.6, 18.926):**
Khronos's conformance suite, dEQP-VK from VK-GL-CTS 1.4.7.1, built for
Kosmos (`tools/cts_build.py`, `runtime/patches/cts/`, `runtime/ports/cts/`)
and a minute of it in `x86-vulkan-cts` - 3565 cases, none failing - with
Mesa's patch 0004 doing what the loader does on Linux. It found three
things in Kosmos for the roadmap: stacks compiled in, llvmpipe without
SSE4.1, and a `malloc` that cannot say NULL. **G3 (0.12.7, 18.927):**
`VK_KOSMOS_surface` in Mesa's WSI (`wsi_common_kosmos.c`, patch 0005), a
swapchain over a Window Kit window, the cube presented into it and found
on the screen by `x86-vulkan`; presenting copies once until a window's
pixels start on a page, and `vkcube` as an application is a follow-up
(`roadmap.md`). **G6a-G6c (0.12.8, 18.928):** Gen9 context images and
48-bit page tables in `intelgpu_lrc.c`, held on the Mac by
`test_intelgpu_lrc`; on the M700 the video engine on execlists, one
context and then two at the same GPU address on pages of their own, each
holding only its own number (`gpu el`, `gpu el2`, the control `gpu
elshared`). **G6d (0.12.9, 18.929):** the render engine and the
blitter on execlists, in contexts of the driver's own through a page
table mirroring the global GTT; a hang watch that resets an engine whose
fence is still for 3 s (`gpu hang`, `gpu hang render`; the control, no
`GDRST`, stays stuck); the legacy ring gone. A submission is now 11-14 us
against 3-4 on the ring - a context loaded each time. **G6e (0.12.10,
18.930):** programs' own contexts in `/Devices/gpu`.
- **What a program can do**: bind regions at addresses it chooses, give
  batches in order, read a fence page or be pulsed.
- **The core** is `intelgpu_ctx.c`, held on the Mac by `test_intelgpu_ctx`.
- **On the M700**: execlists, each context with its own ring, image and
  page table. A context's image is restored only after a switch, and a new
  context begins from the engine's default state. The control showed one
  program's register reaching the next without it.
- **Under QEMU**: a stand-in runs `MI_` commands, held by `x86-gpumon`.
- **Checked** with `gpu contexts` and `gpu contexts hang`, two programs at
  once.

**G6 is done. G7a (0.12.11, 18.931):** Intel's Vulkan driver, `anv`,
built for Kosmos with a kernel-driver type of its own.
- **On the M700** it lists "Intel(R) HD Graphics 530 (SKL GT2)",
  `8086:1912`, Vulkan 1.4, a 6037 MB heap.
- **Under QEMU** it lists no device, and does not count that as an
  error (`x86-anv`).
- **`brw`**, Intel's compiler, turns a SPIR-V compute shader into 20
  Gen9 instructions inside a Kosmos process.
- **Where the code is**: Mesa's 28 edited files are
  `runtime/patches/mesa/0006-anv-on-kosmos.patch`, and the Kosmos half
  is `runtime/ports/mesa/src/intel/vulkan/kosmos/`.
- **The test image** is `anvtest.elf` (`user/kits/anvtest/`), kept apart
  from `lavapipe`'s.

**G7b (0.12.12, 18.932):** Vulkan's first work on the M700's GPU.
- **On the M700** `anv` makes a device in 4.4 ms, and a 64 KB buffer
  filled half by `vkCmdFillBuffer` and half by a compute shader reads back
  right: 16,384 of 16,384 words. The control, every batch made trivial,
  reads back nothing.
- **How**: one context a device, buffers bound into it, submissions
  with a point on its timeline; a slab's buffers share one mapping;
  unbinding waits for idle.
- **The C library** learned `%m`, names an unsupported conversion when
  it panics, and `strerror` knows sixteen codes, in `strerror.c`.
- **The M700 suite** has *Vulkan on the GPU* (`tools/run_m700.py`).

**G7c (0.12.13, 18.933):** a cube drawn by the M700's GPU, in a window.
- `vulkantest`'s cube on `anv` matches the reference pixel for pixel. 300
  frames of it are presented through `VK_KOSMOS_surface`, and the window is
  found on the screen by VNC.
- The swapchain is Mesa's buffer blit: the GPU's image is copied into a
  buffer the processor reads, behind the image's fence.
- `anvtest.elf` carries `vulkantest`'s kit too.
- **telnetd** took a big `put` in quadratic time and wedged on the M700.
  It is fixed, and `x86-servers` holds it, with the lines sent behind a
  running program.

**G7d (0.12.14, 18.934):** dEQP-VK on the M700's GPU.
- 3,565 cases: 1,441 passed, 2,124 not supported, none failed. The first
  run failed three, a queue at HIGH priority taken when the device offers
  MEDIUM; that is refused now.
- telnetd's put limit comes from free memory, not 64 MB.
- **Submit to fence is 8 ms**: `intelgpu` hears a fence on its next tick.

**G7e (0.12.15, 18.935):** the GPU's interrupt.
- The HAL gives the GPU an MSI, without touching its command register.
- `intelgpu` arms the GT's bank and each engine's `RING_IMR`, and waits on
  the line and its endpoint together.
- Submit to fence is 0.22 ms where it was 8, and the cube window runs at
  1,522 frames a second where it ran at 73.

**The GT's clock for Vulkan (0.12.17, 18.936).** Only the compute paths
asked for it, so Vulkan's frames ran at the GT's least. `engine_start`
asks now. A cube window 1024 pixels square runs at 388 frames a second
where it ran at 72, and `window` takes a side to measure it. At that size
the processor's copy into the window is now almost half a frame (roadmap,
owed from G7c).

**G7 is done. Flight is agreed** (20 November, `docs/flight.md`), with a
game controller's sticks added as F7. **F1 is built** (0.12.19,
`testing.md` 18.938): `/Tiles` keeps several sources at once, the
protocol is one library, `tiles.lua`, and a `tiles` program fetches from
the prompt. **F2 is built** (0.12.20, 18.940): the Map Kit's Web
Mercator and a terrarium tile's heights, kept in C, with Maps' tile and
scale arithmetic moved onto them. F3, the 3D Kit on Vulkan, is under way: **F3a** (0.12.21, 18.941) moved the
Vulkan setup into the Vulkan Kit's base, `user/kits/vulkan/kvk.c`, with the
cube's kit on it; **F3b** (0.12.22, 18.942) is the renderer on it,
`user/kits/3d/k3d_vk.c` - sky, depth, sun and haze - held pixel by pixel
to a ray cast on the host; **F3c**'s first half (0.12.23, 18.943) puts it
in a window through the swapchain, found on the screen and held to the
same reference; its second half, the same scene on the M700 by `anv`,
waits for the M700 to be booted. **F4a** (0.12.24, 18.944), the terrain's
chunks and meshes in C, held on the Mac; **F4b** (0.12.25, 18.945), the
chunks drawn with their imagery by the renderer; **F4c** (0.12.26,
18.946), the door Flight draws through - `k3dvk.elf`, a window and a terrain
fed the tiles it asks for, a fixed path flown and its last frame held to a
ray on the screen; **F4d** (0.12.27, 18.947), the chunks built on a thread
of the terrain's own, ancestors drawn while they are, and what the eye has
left given back; **F5a-F5d** (0.12.28, 18.948), Flight playable - held
keys, the flight model, real tiles, and the Cessna 172 (Sketchfab, CC BY)
drawn by the 3D Kit's GLB reader and textured models; **F5e** (0.12.31,
18.951), the chase view's strip of instruments, laid over the Vulkan frame
by the 3D Kit's new overlays (`w:overlay`); **F5f** (0.12.32, 18.952), the
cockpit's six round instruments on the same overlay; **F5g** (0.12.33,
18.953), the graphics panel - frames, the device's time by Vulkan's
timestamps, draw calls and triangles; **0.12.34** (18.954), Flight flown on
the M700 for Diego - on lavapipe at 85-89 ms a frame, all eight cores since
Mesa counts Kosmos's (patch 0007), and an image per GPU chosen by init,
`k3dvk-anv.elf`; **0.12.35** (18.955), **the M700's GPU draws Flight**,
about 10 ms a frame against lavapipe's 85-89: the hang was `intelgpu`
giving the render engine none of i915's Skylake registers - now in
`user/drivers/display/intelgpu_wa.h`, the whitelist being the one the frame
needed, held to a control - and the hang report says where the engine
stood and whether it faulted; **F5h** (0.12.36, 18.956), the place box,
the imagery's credit, and the tower on the city's airfield with a lens
that follows the distance; **F5i** (0.12.37, 18.957), the pause sheet -
search, Recent, the start on a runway or on final, Restart in another
city - and the 3D Kit's `w:typed` and `w:clicked`; **F5j** (0.12.38,
18.958), the island: with nothing fetched, Flight says it is offline and
flies over an island invented in C (`k3d_island.c`, `g:invent`);
**0.12.39** (18.959), Flight in the dated screenshot, on its island, with
`tile` laying out on paper first and the gallery's machine at 2 GB;
**0.12.40** (18.960), F6a and F6b - the time of day (T, or the sheet's Time
row; the sun from the hour and the latitude, skies keyed by its height, a
glow towards it) and the view named in a chip (V or a click), with
`tools/luaredefined.lua` in `make host-check` after a local declared twice
ended Flight; **0.12.41** (18.961), F6c - the propeller at a rate a frame
can show, and the engine and the wind as two held notes on the Synth Kit
(`M` for the sound); **0.12.42** (18.962), F8a - every paved runway from
OurAirports (`tools/airports.py`, the Airports library), found in the sheet
by code or name, the nearest field's runway for any place, the runways
levelled through the 3D Kit's `flats`, B for the brakes and a landing under
600 fpm (`x86-flight-land`); **0.12.43** (18.963), F8b - the runways drawn
over the imagery, asphalt and ICAO's paint, through the 3D Kit's new
`w:shape` and a part's pull towards the eye. **0.12.44** (18.964), F9's radio - `radio.lua` over the
Network Kit's download and the Playback Kit, `radio` at the prompt,
`x86-radio`. **0.12.45** (18.965), F6's buildings B1-B3 - the MVT reader keeps heights, the 3D Kit's `k3d_extrude.c`, the terrain stands the vector tiles' buildings up on the builder's thread (46,906 over London on the M700, 11.6 ms a frame); **0.12.46** (18.966), B4 - the roofs wear the satellite photograph, sixteen zoom-16 pieces a tile, and a missing piece is concrete; two racing checks in `x86-flight` fixed, the panel checks' failure kept as a screen when it next happens; **0.12.47** (18.967), F6's shadows S1-S2 - a 2048 sun map in the 3D Kit, read linearly where the device filters depth, the aircraft and buildings casting, H in Flight, 9.5 to 12.6 ms a frame on the M700 over London at dusk; the panel checks' failure was the Monitor's CPU graphs taken for Flight's window, fixed. The M700 netboots with `/Home` on its SATA partition 4 (B28F1C44) - a plain `make netboot` names the 0.12.30 stick's and `install` then cannot write. **0.12.48** (18.968), F6's weather and wind - Clear, Haze or Low cloud (a 900 m overcast deck, a 3D Kit shape) and 10 or 25 kt from N, E, S or W in the sheet; on the M700 under the deck 10.2 ms GPU, 3.9 ms processor. `open` over telnet starts another copy of an application each time: end the old ones (`kill <id>`) or numbers read three Flights' load. **0.12.49** (18.969), F7a-F7b - a pad's sticks, triggers and buttons in one page (`padring.h`) the xHCI driver writes and the window manager reads, handed to the focused window in `struct wm_frame_reply`, `kw_pad` and the 3D Kit's `w:pad(n)`; Flight's yoke on the left stick, throttle on the triggers, A brakes, B flaps; `padtest` under `opt/kosmos/pad=test` writes the page for `x86-flight` (19 degrees to -20; `--no-pad` fails). **0.12.50** (18.970), Diego's notes from his first flights - the throttle on Page Up/Down, +/- and 0-9, the wind without its sweep (it was "sea waves"), the chase camera along the nose, the deepest zoom set by height (17 under 150 m, 16 under 400, else 15; `g:deepest`) with a per-kind tile budget a frame, and GPU Monitor counting contexts' execs by `RING_TIMESTAMP`, which hung the M700's render engine on every Flight start; **0.12.51** (18.971) counts each context's saved `CTX_TIMESTAMP` from its image instead, no ring command added: Flight reads 37-42% busy, no hang in five starts. **0.12.52** (18.972), F1's Keys page (17 keys, 4 groups), the brakes on Space too and BRAKES on the strip, an idling propeller's drag on the runway (0.8 m²), and the tyres' squeak at touchdown. **0.12.53** (18.973), F8c's runway lights - edge, centre line, threshold, end and approach, `light = true` shape parts drawn unlit and hazed, on from dusk to dawn and under cloud. **0.12.54** (18.974), F5k's minimap - 220 points of OpenMapTiles in Maps' dark style (`user/lib/mapstyle.lua`, shared with Maps), north up, the aircraft's arrow, the runways and a scale, the 3D Kit's seventh of eight overlays; not yet seen on the M700. **0.12.55** (18.975), F11 - X steps the time 1x to 4x, the model stepped that many times a frame. **0.12.56** (18.976), F12a - the Cessna is `Aircraft/cessna172.lua`, read as data with only `math`, checked field by field, chosen in the sheet's Aircraft row. **0.12.57** (18.977), F12b - jets in the flight model (a six-second spool, thinning with the air) and the Boeing 747 as `Aircraft/b747.lua`, its CC BY 3.0 model posed from its geometry; `x86-flight-747` takes it off Rio's runway at 4x. **0.12.58** (18.978), F10a - the terrain centred again under the aircraft every 40 km (`ANCHOR`, `anchor_after`), kept points moved by their longitude and latitude, the runways the new centre's. **0.12.59** (18.979), F10b - the autopilot on Q, holding heading, height and speed through the yoke and throttle, the arrows and A/Z changing what it holds; the flight model moved into `model.lua`, flown on the host by `tools/test_flightmodel.lua` (both aircraft, 18 checks; its control freezes the integrators). **0.12.60** (18.980), F10c - a route to the airport in `fly_to`, made by `model.lua`'s `route` and flown by its `guide` on the Airports library's great circles (`airports.ahead` new); Carrasco to Ezeiza on the host, both aircraft at the final fix within 0.02 nm, 3 ft and 0 degrees; the strip says `NAV EZE 87 nm`. **0.12.61** (18.981), F10d-1 - the route flown from runway to runway: a take-off from the end the aircraft is on, a 3 degree glide path to 300 m past the threshold, the flare, the roll-out and the brakes; Carrasco to Ezeiza on the host, the Cessna down at 311 fpm and the 747 at 316, both stopped on the centre line; `x86-flight-747` takes off by Q. **0.12.62** (18.982), F10d-2 - the sheet's Fly to row: None, the airport chosen, and every airport the City field found, with its distance; F10 is done. **0.12.63** (18.983) - the Synth Kit's thread takes a waiting song before it applies commands, so a start's first note is no longer dropped: Flight's engine heard silent in one gate in three; `x86-sound`'s `synth_start` heard 1 of 30 starts before and 30 after. The M700 runs 0.12.63 with Flight installed (21 November). F9, ATC, set aside by Diego: "No atc for now". **0.12.64** (18.984) - Xavier's flake found: the kit's host builds fused multiply-adds (clang on ARM) and the guest's do not, so a long answer could part from the Mac's reference; `-ffp-contract=off`, and `tools/test_llmsame.py` holds this Mac's answer to an x86's to the byte; the follow-up's threshold was a guess. Next: F7c when the 8BitDo is plugged into the M700; AMD later. **Waiting on Diego**: what Flight's help screen should show - his
sentence "we need a help screen to show all the commands and" was cut off.
Next, in Diego's evening order (20 November): then the rest of F6, F7, and the imagery deeper near the ground. **The
Mac Pro's AMD GPU** (Diego, 20 November: "i will be running kosmos on the
mac pro later tonight so we need the gpu drivers done") has its driver
(0.12.29, 18.949): `docs/amdgpu.md`, Southern Islands - both cards found
and named, the display read back and a flip on demand (`gpu cursor 5`), and
each DMA engine's first fill, copy and fence - none of it run on the machine
yet; the Mac Pro's boot is its test, its `amdgpu:` lines read in the order
`amdgpu.md` gives. **Full 3D and Vulkan on the cards** was agreed the same
evening: `amdgpu.md`, *3D and Vulkan*, V1 to V10, with V1-V3 (the interrupt
ring, the GART and VMIDs, the command processor) built in 0.12.30, and the
Mac Pro's Ethernet (`tg3.c`, 18.950) with them, so the card's steps can be
tried over Telnet rather than a stick each. V4 onward next; V6 waits on
`amdgpu.md`'s proposal of one context core for both GPUs (`gpu_ctx.c`).
After
Flight, G4 and G5 (GL by Zink, WebGL in JetSki), Diego's order of 20
November, and then Quake3e on Vulkan (`docs/quake3e.md`, Q0 to Q6; Diego,
the same day: "add quake3e to the roadmap after flight").

**The gate is inside its ten minutes again** (0.12.18, 18.937): 548 s.
Its image step was 137 s, because every make rewrote AArch64's filter
list and the 10 MB table was compiled again. It is 43 s now.

**Flight, agreed** (designed 19 November, agreed 20: "Yes to all"): a flight simulator
over a real city - Esri's satellite imagery on Mapzen's elevation tiles,
any city searched by name - in `docs/flight.md`, `flight.html` and
`flight-architecture.png`, its six questions answered as recommended. F1 to F6.

**Before the review, 18 and 19 November (0.11.493-0.11.507):**

- **Astra's looks.** Starlight's S3, nebula glass (0.11.492, 18.891), and
  S4 - the roll on closing (0.11.497, 18.896) and the focus sweep
  (0.11.498, 18.897) - so Starlight's S1 to S5 are built. Liquid Glass's
  LG1 to LG6 are built; LG6, live glass, is off by default until its cost
  is measured on the M700. **A desktop's style is the desktop's**: the
  star, the plate and the cut left the looks in 0.11.496 (18.895), and
  `flat`, `title_bars` and `shadow` in 0.11.506 (18.906); a `.desktop`
  file says all six.
- **JetSki.** Save page as, MHTML by default (0.11.500, 18.899); the menu
  (0.11.502, 18.901); favorites and history (0.11.503, 18.902); JetSki in
  six files rather than one (0.11.504, 18.904). Print, Page source and
  History's list are greyed rows in the menu until each is built.
- **The dock**: a pin whose program is gone is drawn faded and says so
  (0.11.507, 18.907) - the M700's old `browser` pin.
- **Suites under the gate's load**: `x86-film` plays through `listen`, a
  program in the audio band (0.11.493, 18.892); `x86-profile` counts only
  the ticks that came (0.11.494, 18.893); a film's gap and the futex
  deadline (0.11.495, 18.894); `x86-sound`'s stretched tone (0.11.499,
  18.898) and `x86-mouse`'s drag (0.11.501, 18.900) explained; and
  `posixtest` dying in `malloc` under `x86-loader` (18.903), whose report
  now says which of three refusals it was (0.11.505, 18.905) - to be read
  the next time it fails.

**Still Diego's**: whether a program in `/Home` or `/Temporary` may ask
for the audio band (`roadmap.md`, *A daily driver*, 1); and LG6's cost,
which wants the M700's look switched to Liquid Glass for a minute.

---

## Next

**Reordered on 21 November 2026** (Diego: "Quake 3e port so I can play
tomorrow", "Then Xavier with gpu support with vulkan", "Then we need to
work on the daily driver requirements"): **Quake3e** (`docs/quake3e.md`,
Q1-Q4 to play on the M700; Q1, pointer capture, built in 0.12.65; Q2-Q3, Quake III in a Kosmos window on lavapipe with the game's code native, built in 0.12.66 - the demo's menu and The Longest Yard in QEMU; Q4's first half on 22 November: on the M700's HD 530 the demo runs at 125 fps, the cap, in a 1280x720 window; sound heard, play by a person and a timedemo next), **Xavier on Vulkan** (`xavier.md` X12, decided 22 November; V1-V3 built in 0.12.68-0.12.69, V4 on the Mac in 0.12.70 and in Kosmos in 0.12.71 - `llm-vk.elf`/`llm-anv.elf` in `/Home/System/LLM`, chosen by init, `x86-llm-vk`, 18.990; V5 built (the question as one batch on `mul_mm`, Compare's GPU row, 18.991), V6 built in 0.12.73 on the M700 - not yet faster than its processor, 0.5B 13.95 tok/s against 21.92, 18.992; Diego's two decisions built in 0.12.74 - the hold against an unrounded processor pass, the processor Xavier's default on Intel - and V7's first part: the L3 back for every Vulkan program and every GPU context restored at every run, 0.5B 21.84 tok/s against the processor's 22.0, 1.5B 10.33 against 12.77, 18.993-18.994; V7 goes on with 1.5B), then
**the daily-driver list** in item 3's order below. The order below stands
under that.

In this order, agreed on 18 November 2026 (`roadmap.md`, *A daily driver*,
*The order agreed on 18 November 2026*):

1. **The review's batches 5 to 8, then 0.12.0** - done.
**Reordered on 19 November 2026, GPU first** (Diego: "GPU first: G3, G6,
G7, then simulator"): **G3** (a window and `vkcube`), **G6** (`intelgpu`
for many programs), **G7** (`anv` on the M700), then **the flight
simulator**, designed first; then the list below from G4 on, G8 and G9
with the rest of 4.

2. **3D, G1 to G5, in QEMU** (`docs/gpu3d.md` section 6; Diego, 18
   November: "Yes do the 3d api work after the cleanup is done"): Mesa and
   LLVM built for Kosmos (G1 done, 0.12.2), `lavapipe` headless, a window and `vkcube`, GL
   and GL ES by Zink with TinyGL retired, WebGL in JetSki.
3. **The daily-driver list**, in order: (1) keys - Control-W given back,
   the desktop's keys on Super, press and release delivered, layouts and
   dead keys out of Nebula, Latin American first; (2) the clipboard
   redone; (3) JetSki's remaining basics - find, print, session restore,
   History's list, suggestions; (4) search on one door; (5) memory
   returned and a day's leak test; (6) restarting what dies; (7) booting
   from its own disk with two slots; (8) USB drives - exFAT, writing, NTFS
   read; (9) other people's files - DOCX, every Media Kit format, GIF,
   WebP, HEIC; (10) Telnet and VNC off in a stable build, then the lock
   screen.
4. **G6 to G9 on the M700's GPU**: `intelgpu` for many programs, `anv`,
   GL and WebGL there, sharing and presenting.

Owed along the way, not in that queue: LG6 measured on the M700; LG7, live glass for
windows on the GPU, with GPU composing (`m700-2d.md` 7d); Preferences' split header and Appearance's Theme row
(`astra.md`, S1); the amber glass plate's pixel check on Starlight wearing
a glass look; and the suites that fail only under the gate's load
(`x86-xavier`, `x86-smbd`, `x86-film`, `x86-sound`, `x86-loader`).
