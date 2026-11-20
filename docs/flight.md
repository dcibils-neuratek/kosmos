<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Flight

A small flight simulator of Kosmos's own: a light trainer over a real city
the person chooses, its actual terrain under its actual satellite images,
drawn by Vulkan on the GPU, with a graphics panel open to show what the 3D
API does every frame.

Diego, 19 November 2026: "after the 3d api is done and running on gpu, i
want to build a simple flight simulator app that shows the 3d api
capabilities"; asked how far it was, "GPU first: G3, G6, G7, then
simulator"; and that night, the mockup drawn, "Can we use real world maps
for the flight simulator?", "So i can choose q city to fly", "And see the
actual satellite images", "Of the city", "Really cool".

**Agreed on 20 November 2026, being built from F1** (Diego, shown the
design: "Yes to all" - every recommendation below taken). The four things every application
has before its code (`CLAUDE.md`, *How to work here*): this feature set and
architecture, the window in `docs/flight.html`, and the diagram in
`docs/flight-architecture.png`, drawn from `docs/flight-architecture.html`.
It comes after G7 (`gpu3d.md` section 6), and takes G9's full-screen flip
when that lands.

## In short

- **What it is**: one aircraft, three views, any city on Earth, its hills
  from elevation tiles and its streets from satellite tiles, with the
  frame's cost shown beside it.
- **What it stands on**: the `tiles` server Maps already uses, grown to
  hold several sources at once; gfx's JPEG and PNG decoders; the 3D Kit,
  which gains a Vulkan renderer and a terrain - both of which Cafesa3D and
  Maps want too; the Vulkan Kit and `anv` from G7; the Synth Kit for the
  engine's note.
- **What is new**: the terrain (C, in the 3D Kit), the 3D Kit's Vulkan
  raster path (C, wanted by Cafesa3D's Solid view anyway), several sources
  in `tiles`, and `flight.lua` - a few hundred lines of Lua that fly the
  aircraft and say what is drawn.
- **Its busiest path** is a frame: input, the flight model in Lua, the
  terrain choosing and recording its chunks in C, the GPU drawing, and the
  frame reaching the window - which, until G9, is one copy, measured.

## What a person can do

1. **Choose a city and fly over it.** The pause sheet searches by name or
   airport (Nominatim, through `tiles`' `FIND`, as Maps searches), lists the
   recent ones, and starts on the runway, over the bay or on final
   approach. Five are offered on the first start: Rio de Janeiro, San
   Francisco, Cape Town, Hong Kong and Montevideo.
2. **See the real place.** The ground's shape from elevation tiles, its
   colours from satellite imagery, the sea where the tiles say water, and
   the sky and the sun. Far hills fade into haze. A city flown once is in
   the cache and flies offline.
3. **Fly offline anyway.** With no network and nothing cached, an invented
   island, so the window always has something to fly over.
4. **Three views**: Chase, behind and above the aircraft; Cockpit, the six
   round instruments under the windscreen; Tower, the airfield's camera
   following it. `V` steps through them.
5. **The instruments**: speed, altitude, heading, climb, throttle and flaps
   in a strip in the chase and tower views; the six round ones in the
   cockpit.
6. **Time of day**: dawn, noon and dusk move the sun, and with it the sky,
   the light on the slopes and the shadows.
7. **The graphics panel**, the reason the application exists: frames a
   second and a sparkline of frame times; GPU and processor time a frame;
   draw calls, triangles and terrain tiles; the device (`anv` on HD
   Graphics 530, or `lavapipe`) and how the frame reaches the screen; and
   switches for shadows, water reflections, haze, wireframe and
   anti-aliasing, so each effect's cost is seen by turning it off. `F3`
   hides it.
8. **Controls**: arrows for pitch and roll, `A` and `Z` for the throttle,
   `F` for flaps, `V` for the view, `P` to pause; the mouse as the yoke
   when the pause sheet says so; and **a game controller** - the left stick
   the yoke, the triggers the throttle, the face buttons flaps, view and
   pause - used as soon as one is plugged in (F7).
9. **Weather and wind** in the pause sheet: clear, haze or low cloud, and
   a wind's direction and strength.
10. **Sound**: the engine's note following the throttle and the wind
    rising with speed.
11. **Credit**: whose imagery and whose elevation, in the window's corner,
    as their terms ask.

Not in it, on purpose: airports' runways as real geometry, other traffic,
air traffic control, navigation radios, a choice of aircraft. One aircraft
flown well; the rest is a model file and a table of numbers later, if it is
ever wanted.

## Where each piece comes from

The premise (`CLAUDE.md`, *Kits, servers and drivers supply*): each piece
is asked "does another application want this?" and "does something already
supply this?".

| Piece | Supplied by | Status |
|---|---|---|
| Tiles over HTTPS, cached in `/Home/Cache/Maps` | **`tiles`** server (`user/servers/tiles.c`, `tileproto.h`) | exists; grows several sources |
| A place by name | **`tiles`**' `FIND` and `json.lua` | exists (Maps M6e) |
| Decoding a JPEG (imagery) and a PNG (elevation) | **gfx**: `jpeg.c`, `png.c` | exists |
| Elevation from a terrarium PNG, metres a sample | **Map Kit**: `map.elevation(png)` | new, small, C |
| Web Mercator: a tile from a latitude, metres a pixel | **Map Kit** | exists in Lua in Maps; moves into the kit |
| A terrain: chunks of a height field, levels of detail, imagery as textures | **3D Kit**: `k3d_terrain.c` | new; Maps' 3D view and Cafesa3D want it |
| Drawing triangles with Vulkan: pipelines, a depth buffer, a sky, haze, shadows | **3D Kit**: `k3d_vk.c` | new; Cafesa3D's Solid view wants it (`gpu3d.md` 5.1) |
| The Vulkan API, `anv` on the M700, `lavapipe` elsewhere | **Vulkan Kit**, Mesa | G7 |
| A swapchain into a window | **`VK_KOSMOS_surface`** | built in G3 |
| The aircraft's model | **3D Kit**'s formats (`k3d_formats.c`, glTF) | exists |
| The engine's note and the wind | **Synth Kit** into `/Devices/audio` | exists |
| The window, its header, the pause sheet, the panel | **UI kit** (`ui.lua`) | exists |
| The instruments' faces | **gfx** paths, into a texture the 3D Kit samples | exists |
| Frame times and GPU time | **Vulkan** timestamp queries; `gpumon` for the device | exists |
| Flying: forces, attitude, the camera | `flight.lua` | the application |

Nothing in that table is the application's own except the last line. A
city's tiles are the same squares Maps draws, from the same server and the
same cache; a terrain is a 3D Kit object Cafesa3D could import a real place
as; and Maps gains a satellite layer and a tilted 3D view from the same two
doors at no further cost.

### Why `tiles` grows several sources

`tiles` holds one source today: a `SOURCE` replaces the last, and what was
under way for the old one is let go (`tiles.c`). The simulator wants two at
once - imagery and elevation - and Maps, open beside it, a third. So
`SOURCE` answers with a **source number**, `WANT` and `ARRIVED` name one,
and each source keeps its own connection, queue, arrivals and cache
folder. **Built in F1 so that the caller names the format** - `SOURCE`
carries "pbf", "png" or "jpg", and a tile is kept as `<x>-<y>.<format>` -
rather than the extension being read from the bytes as this first said: a
caller looks for a tile in the cache before asking, on the next run too,
and has to know its name without the server, and a tile there is none of
is an empty file with no bytes to read a format from. The caller chose the
source, so it knows what the source sends. The same address and format
asked again is the same source, so Maps and Flight wanting the same
imagery share one cache. A declared shape still: a number and a format
added to the request, a number to the reply, both sides rebuilt.

**The sources**, each one line, changeable without code:

- imagery: Esri World Imagery,
  `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`,
  with Sentinel-2 cloudless (EOX) where Esri has nothing - Diego's to
  decide (*Questions*);
- elevation: Mapzen's terrain tiles on AWS, terrarium encoding,
  `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png`:
  height = R x 256 + G + B / 256 - 32768 metres, about 5 m at zoom 14 in
  cities, worse elsewhere.

`tileproto.h`'s source URL has `{z}`, `{x}` and `{y}` in it already; Esri's
order of `{y}` before `{x}` needs nothing new.

## The architecture

```
flight.lua                     (Lua: input, the flight model, the camera,
  |                              the panel, the pause sheet)
  | terrain:view(camera)  terrain:draw(frame)   once a frame
  v
3D Kit  k3d_terrain.c          which chunks, at what detail; their tiles
        k3d_vk.c               pipelines, depth, sky, haze, shadows, MSAA
  |                    \
  | vkQueueSubmit       \  map.elevation(png), gfx.jpeg / gfx.png
  v                      v     (a decoding thread, never the frame's)
Vulkan Kit - anv           tiles server - /Home/Cache/Maps
  |                          |  HTTPS: Esri, Mapzen, Nominatim
  v                          v
intelgpu (G6)              network
  |
  v  VK_KOSMOS_surface: the window's buffer       window manager
```

**What is C and what is Lua.** `flight.lua` is a few dozen floating-point
operations a frame - forces, attitude, the camera - and the panel's
numbers; none of it is a loop over bytes, and the collector question is
answered by allocating nothing a frame (the frame's tables made once and
reused, as `con.wait` learned). Everything that touches a vertex, a sample
or a pixel is C: decoding tiles, turning a terrarium PNG into heights,
building chunks, recording commands. The flight model moves to C only if a
profile asks.

**What travels in a region and what in a message.** A tile's bytes go from
the network to the cache by `tiles` and from the cache into the process by
the disk server's read, once; decoded heights and imagery go straight into
GPU buffers and images (`GPU_OP_BUFFER_NEW`), never through Lua. A frame is
a swapchain image the GPU drew into, and presenting it is a message.
Messages carry only what is wanted (`WANT`, a list of tile numbers), what
arrived (`ARRIVED`), and "this image is live".

**The terrain.** Each chunk is a square of the height field, 65 by 65
samples, at a level of detail from the camera's distance - a quadtree over
the Web Mercator tiles, nearer chunks from deeper zooms. A chunk's imagery
is the satellite tile (or tiles) over it, uploaded as a texture; a chunk
whose tiles are not in yet draws its parent's, larger, as Maps draws an
ancestor. Skirts hide the cracks between levels. Elevation from zoom 12 to
14 and imagery from 12 to 17, nearer is deeper; about 150 chunks in view at
cruise, which is about 600,000 triangles - the mockup's panel says 1.42 M
for a generous view.

**Decoding off the frame.** A JPEG of 256 by 256 takes about a millisecond
in gfx on the M700's cores; a burst after a turn could be dozens. So
decoding runs on a thread of the process's own (`threads.md`), with
finished tiles handed to the frame's thread through a ring, and the frame
uploads at most a few a frame. A frame never waits on a tile: it draws
what it has.

**Without a GPU.** `lavapipe` draws the same scene in QEMU, slower, which
is what the suites run; the frame's path is the same code.

## The busiest path: a frame

At 60 frames a second, 16.7 ms:

| Stage | Where | Estimate |
|---|---|---|
| Keys and the mouse, the flight model, the camera | `flight.lua` | 0.2 ms |
| Choosing chunks, uploading arrived tiles (a few) | 3D Kit, C | 0.5 ms |
| Recording the command buffer: ~150 chunks, the aircraft, sky, instruments | 3D Kit, C, `anv` | 0.5-1 ms |
| The GPU drawing: depth, terrain, shadows, haze, MSAA 4x at 1600 by 900 | HD 530 | 5-8 ms |
| Reaching the screen: a linear image copied into the window's surface | blitter (G7c), then the GPU compositor or a flip (G9) | ~1-2 ms, to measure |

Estimates, labelled as estimates: the GPU's from the HD 530's fill rate and
what similar terrain renderers report, the copy's from the blitter's
measured rate in G6d. The graphics panel is how the real figures are read,
and they go into `testing.md` when the application is built. The risk is
the last row: a copy of a 1600 by 900 frame every frame is the price of a
window until G9, and the panel says "a copy to the window" so the cost is
named rather than hidden.

## Steps

Each leaves a permanent test, x86-64 under QEMU with `lavapipe` and tiles
served from the Mac (the tests never depend on the internet), and each
ends on the M700:

- **F1 - `tiles` with several sources.** **Built in 0.12.19**
  (`testing.md` 18.938): source numbers, a connection, queue and arrivals
  each, the format named by the caller; the protocol in one library,
  `tiles.lua`, with Maps moved onto it and its suite unchanged, and a
  `tiles` program; `x86-tiles` serving PNG and slow JPEG tiles from the
  Mac. On the M700 with Esri and Mapzen next.
- **F2 - the Map Kit's elevation and Mercator.** **Built in 0.12.20**
  (`testing.md` 18.940): `mercator.c` (a place's tile, a tile's edges,
  metres a pixel) and `elevation.c` (a terrarium tile's pixels to heights
  kept in C, sampled between the four around a point), as
  `map.tile_of`, `map.tile_bounds`, `map.metres_per_pixel` and
  `map.elevation(surface)`; Maps' tile range and scale bar moved onto the
  kit; `test_map` holds both to places and heights worked out by hand, and
  `x86-tiles` reads every served elevation tile through the kit to the
  metre.
- **F3 - the 3D Kit on Vulkan.** **F3a built in 0.12.21** (`testing.md`
  18.941): the Vulkan Kit's base, `user/kits/vulkan/kvk.c` - the device,
  memory, recording, a pipeline and a window's swapchain - moved out of
  `vulkantest.c`, which now stands on it, so the 3D Kit's renderer does not
  start as a second copy. **F3b built in 0.12.22** (18.942): the
  renderer, `user/kits/3d/k3d_vk.c` - a sky from horizon to zenith, then
  meshes with a depth buffer, lit by the sun and hazed by distance - drawn
  by `lavapipe` in QEMU and held, pixel by pixel, to a ray the suite casts
  through each pixel's centre (`x86-k3d-vulkan`). **F3c, its first half,
  in 0.12.23** (18.943): the same renderer into a Kosmos window through the
  swapchain, the eye going round the scene, the last frame found on the
  screen and held to the same reference. Its second half: the scene on the
  M700 by `anv`, the same check on Intel's GPU. Cafesa3D's
  Solid view is its second user later.
- **F4 - the terrain.** `k3d_terrain.c`: chunks, levels, skirts, imagery
  as textures, decoding on its own thread; a suite flying a fixed path over
  a served city and checking the picture and the frame time. In four:
  **F4a built in 0.12.24** (`testing.md` 18.944) - the frame (metres from
  an origin, x east, y up, z south), the chunks chosen for an eye (a
  quadtree over the map's tiles, split while the eye is nearer than
  `split` sides, the roots covered once whatever the room), and a chunk's
  mesh (65 by 65 samples, normals from the ring beyond its edges, skirts),
  held on the Mac by `test_terrain`; **F4b built in 0.12.25** (18.945),
  imagery as textures in `k3d_vk.c` and chunks drawn - 262 chunks from 2500
  m up, held pixel by pixel to a ray through a tilted plane and the chunk
  the guest said holds it (`x86-k3d-ground`), the Vulkan base gaining the
  texture upload the cube had kept to itself; **F4c built in 0.12.26**
  (18.946), the 3D Kit's door for Lua, `k3d_vk_kosmos.c` in an image of
  its own, `k3dvk.elf`, which Flight names: `k.window`, `k.terrain`,
  `g:wanted(eye)` for the tiles its chunks still need, `g:elevation` and
  `g:imagery` taking the gfx surfaces they were decoded into - read where
  they lie, so a tile's pixels never cross into Lua - and `w:draw(g,
  view)`, which builds a chunk the first frame both its tiles are there
  and keeps it. Its suite flies a fixed path north over Montevideo, the
  tiles made in the guest as Flight's loop will be handed them, and holds
  the last frame on the screen to a ray (`x86-flightpath`); the tiles are
  fetched in F5, with the tiles server. **F4d built in 0.12.27** (18.947):
  a chunk's mesh made by the terrain's builder, a thread of its own, the
  frame uploading at most `uploads` finished ones; a chunk not built yet
  drawn by its nearest built ancestor, which covers its siblings until it
  is; chunks past `keep` given back, the least lately drawn first, and
  heights no job is reading. The path's frames 2.4 times quicker under TCG
  (1669 ms to 702 on average). Decoding stays on the Lua thread until the
  tiles arrive as bytes, in F5: gfx's PNG and JPEG readers are bound to
  Lua, and one of each with no Lua in it is the piece that moves it.
- **F5 - Flight.** `flight.lua`: the aircraft, the three views, the strip
  and the cockpit, the pause sheet with search, the graphics panel, the
  island offline; in the dated screenshot, as every new application is.
  **Playable first** (Diego, 20 November, asking what was left for it to
  be playable), in this order, each with its suite:
  - **F5a - the keys.** `WM_EV_RAWKEY` in `wmproto.h` and `KW_RAWKEY` in
    the Window Kit, a key's going down and coming up - Quake III's Q1
    wants the same (`quake3e.md`) - and the door's `w:held(code)` and
    `w:pressed(code)`, a bit a key, read with no allocation a frame.
  - **F5b - the flight model.** `flight.lua`, in `user/installed/Flight`
    on `k3dvk.elf`: a point mass with an attitude - lift, drag, thrust and
    weight in the path's frame, the yoke asking for an angle of attack,
    a bank turning it - and the ground from the terrain's own heights,
    `g:height(x, z)`, so it lands on and flies into what is drawn.
  - **F5c - real tiles.** Esri's imagery and Mapzen's terrarium elevation
    asked of `tiles` as the terrain wants them, read from the cache when
    they arrive; a tile there is none of is the sea. `x86-flight` serves
    hills from the Mac and holds the keys down through QEMU's keyboard.
  - **F5d - the aircraft.** The Cessna's GLB read by the 3D Kit
    (`k3d_glb.c`), drawn by the renderer's textured models with a matrix
    each, its control surfaces and propeller turned about their hinges.
  - **F5e - the chase view's strip**: speed, altitude, heading, climb,
    throttle and flaps.
  Then the cockpit and tower views' detail, the pause sheet, the panel and
  the island.
- **F6 - sound, weather, shadows.** The Synth Kit's engine and wind; haze
  and low cloud; the sun's shadow map. Then, if Diego agrees, buildings
  from OpenStreetMap's outlines (*Questions*).
- **F7 - a game controller's sticks and triggers.** Today a pad reaches a
  program as keys only. The driver already keeps the analogue values
  (`pad_decode.h`); F7 hands them on: the pad's state - two sticks, two
  triggers, the buttons - kept by the driver as the latest values and read
  by the focused window once a frame, never a message per report (a pad
  reports hundreds of times a second, so it is a stream: *Control by
  message, data by shared memory*). A door every application reaches the
  same way, so the Super Nintendo, Doom and Quake gain it too; Flight maps
  the left stick to the yoke, the triggers to the throttle. HID-class pads
  (PlayStation's, generic ones) and Bluetooth pads after X-input, held by
  `tools/test_paddecode.c` with recorded reports, since QEMU has no
  controller to plug in, and on the M700 with the 8BitDo.

## Questions for Diego

Answered on 20 November 2026: "Yes to all", so each recommendation is the
decision. The mockup asks them too:

1. **The imagery**: Esri's World Imagery, sharp to about half a metre in
   cities, or Sentinel-2 cloudless, open data at 10 m? *Recommended*:
   Esri's, with Sentinel-2 where Esri has nothing; the source is one line.
2. **Buildings**: OpenStreetMap's outlines and heights - which the Map Kit
   already reads for Maps - stood up as blocks with the photograph's roofs
   on top? *Recommended*: yes, after the flat city flies (F6).
3. **One aircraft or two?** *Recommended*: one light trainer, flown well.
4. **Controls**: keyboard, and the mouse as a yoke? *Recommended*: both,
   keyboard by default. **And a game controller** (Diego, 20 November: "I
   would add gamepad support as well"). This said there was no gamepad
   driver, which was wrong: the USB driver reads X-input pads, the 8BitDo
   SN30 Pro among them (`pad_decode.h`), and the Super Nintendo, Doom and
   Quake play with one - but only as keys, the sticks past a threshold.
   Flying wants the sticks themselves, so that is step F7.
5. **Sound** through the Synth Kit? *Recommended*: yes.
6. **The graphics panel open on the first start?** *Recommended*: yes,
   open until it is closed, and remembered.
