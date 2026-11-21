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

12. **Real airports** (F8): take off from and land on any airport's runway,
    found by its name or code in the pause sheet - its runways drawn where
    they are, the ground flat under them, the wheels rolling, braking and
    touching down, and a landing told from a crash by how fast it came
    down.
13. **Air traffic control on the radio** (F9): the live audio of the
    nearest airport's tower or approach, where a public feed exists,
    tuned in the pause sheet.

Not in it, on purpose: other traffic, navigation radios, a choice of
aircraft. One aircraft flown well; the rest is a model file and a table of
numbers later, if it is ever wanted. **This said runways as real geometry
and air traffic control were left out too**, until Diego, 20 November
2026: "Make sure we can takeoff and land in real airport locations", and
"some real ATC radio could be nice if there is any stream of audio public
we can tune into".

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
  - **F5e - the chase view's strip** (built, 0.12.31, `testing.md`
    18.951): speed in knots, altitude in feet, heading, climb in feet a
    minute, throttle and flaps, drawn with the UI kit's font into a gfx
    surface and laid over the frame by the 3D Kit - `w:overlay(surface, x,
    y)`, a screen-space quad blended over the scene with no depth test,
    its texture refreshed in place only when the words change. Hidden in
    the cockpit, which will have its own instruments.
  - **F5f - the cockpit's panel** (built, 0.12.32, `testing.md` 18.952):
    the six round instruments in a trainer's order - airspeed with its
    green and yellow arcs and red line, the attitude indicator, the
    altimeter's two hands; the turn coordinator, the heading indicator's
    turning card, the vertical speed - on a dark panel at the foot of the
    view, sized from the window. The mockup drew them as part of the 3D
    scene; they are the strip's kind of thing instead, a gfx surface laid
    over the frame by the same `w:overlay`, redrawn only when a needle
    would move. Lua says where each needle points, gfx's discs, lines and
    polygons draw it.
  - **F5g - the graphics panel** (built, 0.12.33, `testing.md` 18.953):
    top right, over the strip or the cockpit's panel, in the overlay's
    second place (`w:overlay(surface, x, y, place)`, four places). Frames
    a second, a sparkline of the last 120 frames' times against a 60th of
    a second, the device's time for a frame from Vulkan's timestamps (the
    Vulkan Kit's `kvk_time_*`), the processor's part - the frame less the
    device's, which it waits for - the draw calls and triangles the 3D Kit
    recorded (`w:stats()`), the terrain's chunks drawn of those chosen,
    the device and how a frame reaches the screen. Redrawn four times a
    second; `F3` hides it and that is remembered (`prefs`, `graphics`).
    A window under 480 wide shows the frames a second alone. The
    switches - shadows, water, haze, wireframe - come with the effects
    they turn off, in F6.
  - **F5h - the place, the credit and the tower** (built, 0.12.36,
    `testing.md` 18.956): the five cities in a table of their own - name,
    airfield, a line about the place - the place box top left saying the
    airfield and the line, as the mockup draws it; the credit bottom right
    naming whose imagery and whose elevation, from the sources' own hosts
    (Esri's and Mapzen's names for theirs, the host for any other), above
    the strip or beside the panel. Both in the overlay's third and fourth
    places, drawn once. The tower stands thirty metres over the city's
    airfield, placed in the terrain's own projection, and its lens follows
    the distance - a sixtieth of it, from 1.4 to 400 - so the Cessna stays
    about the same size from the circuit to 24 km.
  - **F5i - the pause sheet** (built, 0.12.37, `testing.md` 18.957): `P`
    opens it over the paused frame, in the overlay's fifth place (six now).
    Its rows are the search field, what the search found, Recent (the five
    cities first, then each place flown) and Start: over the city at 1,500
    ft, on the runway's threshold, or five kilometres out on a three-degree
    final. The last two take the runway's threshold, true heading and
    elevation from OurAirports' `runways.csv`, for the five cities. The
    buttons are Restart and Fly on, and Escape flies on. Restart somewhere
    else makes a terrain centred there, and the place box, the tower and
    Recent follow. It is driven by keys (the arrows, Return, typing) or the
    mouse, through two new doors on the 3D Kit's window: `w:typed()`, the
    characters since it was last asked, decoded by `keys.lua`'s decoder,
    since the arrows arrive as escape sequences; and `w:clicked()`, the
    last left press. The search is asked on Return, never a keystroke at a
    time, and its places are read by `tiles.places`. That reader moved out
    of Maps, so Flight is not its second copy. Weather, wind and the
    controls join the sheet with what they change (F6, F7). **Found on the
    M700**: stopped on a runway, the imagery at zoom 15, the terrain's
    deepest, is about 4.5 m a texel and blurred at wheel height. Deeper
    levels near the eye when it is low are wanted.
  - **F5j - the island**, **built in 0.12.38** (`testing.md` 18.958):
    with no network and nothing cached, an invented island, so the window
    always has something to fly over. The place box says "Offline: an
    invented island", the credit goes, and the flight starts again over the
    island's middle at the city's height, since a runway or an approach
    belonged to a field that is not there. Flight goes offline when it has
    no `/Tiles`, or when six seconds pass with fetches failing and nothing
    handed. The island is made in C, `user/kits/3d/k3d_island.c`, a tile at
    a time, six a frame, through the terrain's new `g:invent(kind, z, x, y)`:
    value noise for the shape and the ridges, a sea floor below the shore,
    and its colours - sea, sand, grass, rock - by height and slope. The
    heights are the same function the picture is drawn from, so they meet
    at every tile's edge. Restart asks the network again. **Found on the
    M700**: before the restart was added, a Cessna left on Rio's runway sat
    stopped on open sea past the island's shore.
  - **Flight in the dated screenshot**, **0.12.39** (`testing.md`
    18.959): `make shot` makes a `/Home` with Flight installed and its
    sources at a dead port, so the picture shows Flight on its island
    every time, the same in both looks.
  - **On the M700** (0.12.34, `testing.md` 18.954): flown there on
    lavapipe, at 85-89 ms a frame since Mesa counts Kosmos's processors.
    Mesa's Intel driver is linked as an image of its own, `k3dvk-anv.elf`,
    which init chooses on an Intel GPU when a program's header names it.
  - **On the M700's GPU** (0.12.35, `testing.md` 18.955): Flight's
    header names it, and anv draws Flight at about 10 ms a frame. The
    hang it had was Kosmos's Intel driver leaving out i915's Skylake
    registers; the whitelist is what the frame needed.
- **F6 - sound, weather, shadows.** The Synth Kit's engine and wind; haze
  and low cloud; the sun's shadow map. Then buildings from
  OpenStreetMap's outlines and heights, through the Map Kit that reads them
  for Maps, stood up as blocks with the photograph's roofs on top (agreed,
  *Questions* 2).
  - **Buildings, as they will be built** (written 21 November, before the
    code). Every piece is supplied by something that exists or is a kit
    other programs want:
    - *The outlines and heights*: OpenFreeMap's vector tiles, OpenMapTiles'
      schema, which Maps already reads through `tiles` from the same
      source. At zoom 14 its `building` layer carries each footprint and
      `render_height` and `render_min_height` in metres. **The Map Kit's
      MVT reader** (`mvt.c`) keeps those two numbers per feature, which it
      throws away today, and reads a value given as a float or a double
      as well as an integer.
    - *Footprints made solid*: **a new piece of the 3D Kit**,
      `k3d_extrude.c` - a polygon with holes, in metres, triangulated by
      ear clipping into a roof, and its rings stood up into walls between
      two heights - since Maps' 3D view and Cafesa3D's import want the
      same. C, held by a host test to areas and to a roof's triangles
      covering its footprint exactly.
    - *Where they stand*: inside the terrain (`k3d_terrain.c`), which
      knows the ground's height under every footprint and builds chunks
      on its own thread already. A building sits on the lowest ground under
      its footprint, so none floats on a slope. The vector tile is a third
      thing a chunk asks `tiles` for, beside its elevation and imagery,
      only at zoom 14 and only within a few kilometres of the eye.
    - *The roofs*: drawn by the ground's pipeline with the chunk's own
      imagery, a roof's corners given the picture's coordinates of where
      they stand - so the photograph's roof is on top, as agreed. The
      walls are lit plain, a light concrete, by the model pipeline's
      lighting.
    - *The busiest path*: a dense city's z14 tile is a few thousand
      buildings, some 60,000 triangles. Extruded on the builder's thread,
      uploaded once with the chunk, and drawn in one call - the frame
      pays the triangles and nothing else.
    - Steps: **B1** the MVT reader keeping heights, with its host test;
      **B2** `k3d_extrude.c` and its host test; **B3** the terrain asking
      for vector tiles and standing the buildings up, drawn plain; **B4**
      the roofs from the imagery.
      **B1 to B3 built in 0.12.45** (`testing.md` 18.965). Over the City
      of London the M700 held 46,906 buildings, 770,445 triangles, at
      11.6 ms a frame. The terrain also answers `g:top(x, z, r)`, the
      highest roof near a place, and the tower view stands above it.
      **B4 built in 0.12.46** (`testing.md` 18.966): the roofs wear the
      imagery at zoom 16, sixteen pieces a tile in one 1024 picture, and
      a piece that does not come is the walls' concrete. Over London the
      M700's 18 tiles were all roofed.
  - **Shadows, S1 and S2 built in 0.12.47** (`testing.md` 18.967;
    written 21 November, before the code). One shadow map from the sun,
    in the 3D Kit, so Cafesa3D's Solid view can have the same shadows by
    naming a radius:
    - *The map*: a 2048 by 2048 depth image, drawn from the sun before
      the frame. It is an orthographic box `shadow_radius` metres around
      a point the view names (`shadow_centre`), with a depth range deep
      enough to hold the tallest building. A view with no radius has no
      shadows, so every picture drawn before is drawn the same.
    - *What casts*: meshes and models, which means the aircraft and the
      buildings. The terrain does not cast at first. A hill's sunward and
      shaded sides are already lit by its normal, and a hill's shadow
      across a valley is a later step.
    - *What receives*: the ground, meshes and models. Each looks itself up
      in the map through a comparing sampler, nine taps, so an edge is
      soft over a texel or two. In shadow, the sun's part of the light
      goes and the ambient stays.
    - *Room*: the view's push constants gain the map's matrix as three
      rows (it is affine), so they are 192 bytes. A model's own matrix
      goes as three rows too. That keeps a model's push at 256, the most
      anv and lavapipe give.
    - *Flight*: the box is centred on the aircraft. Its radius grows
      with height above the ground, from 400 m on a runway to 2 km, so a
      2048 map is 0.4 to 2 m a texel. A graphics-panel switch turns
      shadows off, so their cost is seen.
    - *Checked*: `run_k3dvk.py` draws a shadowed scene. Its reference
      casts one more ray, from each pixel's surface towards the sun, and
      a pixel near a shadow's edge is left out as a silhouette's is.
    - Steps: **S1** the 3D Kit's map and its test; **S2** Flight's box,
      the switch and the M700's cost. Both built. On the M700 over
      London at dusk, the GPU went from 9.5 to 12.6 ms a frame. Where
      the device filters a depth format, the map is read linearly, which
      the runway's 0.4 m texels needed.
    - What a low sun does, seen on the M700: at dusk the sun is 3
      degrees up, so a level runway takes almost none of its light, and a
      shadow on it is barely darker than the ambient. That is right.
      Dusk's shadows are on the walls that face the sun, and a level
      shadow shows from morning to afternoon.
    - Later: the terrain casting (a hill across a valley), and cascades,
      so the buildings far off keep their shadows as well.
  - **Weather and wind, W1 and W2 built in 0.12.48** (`testing.md`
    18.968; written 21 November, before the code; the feature set's
    item 9). The deck went to 900 m (3,000 ft) when it was flown: at
    600 m, the "over the city" start was inside it, and the window was
    white. Two rows in the pause
    sheet, remembered like the time of day:
    - *Weather*: **Clear**, **Haze** or **Low cloud**.
      - Clear is the sky as it is now.
      - Haze brings the haze in from 4-40 km to 1.5-12 km, greys its
        colour, and takes a fifth of the sun's share.
      - Low cloud is an overcast deck 900 m over the field. Under it the
        sun's share is a quarter, so shadows go and the ambient rises. The
        sky is grey and the haze is 3-20 km. Above it the sky is clear,
        the sun lights the deck's top, and the ground is hidden.
      - The deck is a mesh, one grid 40 km wide that follows the aircraft
        in whole kilometres so it does not swim. Its points are lifted and
        shaded by a hash of where they stand, so the deck has lumps and
        lighter and darker patches. That is vertex data, not pixels, so it
        stays in the 3D Kit's existing mesh shape. The haze hides its edge.
      - Inside the deck, within 60 m of it, the haze closes to 80 m:
        flying through cloud is white.
    - *Wind*: **Calm**, **10 kt** or **25 kt**, from **N**, **E**, **S**
      or **W** (the sheet's Wind and From rows).
      - The flight model's speed is airspeed already. The wind is added
        to the ground track, so the aircraft drifts and crabs into the
        wind on final.
      - On the runway, the headwind's share is added to the airspeed the
        wings see, so a take-off into the wind is shorter.
      - Gusts are a slow sum of two sines on the strength, at ±20%.
      - The wind sound follows the airspeed, as it does now.
    - *What supplies it*: nothing new below Flight. The haze, the sun's
      share and the ambient are the view's existing fields. The deck is a
      mesh (`w:shape`). The wind is Flight's own physics.
    - *Checked*: `x86-flight-land` flies its final to SDU's 02R under
      the deck, into 25 kt from the north. It reads that the deck was
      made and the flight was under it, with the sun's share a quarter.
      And it reads the touchdown: 73 kt through the air and 45 kt over
      the ground. Two controls: `--clear` makes no deck, and `--calm`
      touches down at the same speed through the air and over the
      ground. Both must fail.
    - Steps: **W1** the weather row (haze and the deck); **W2** the wind.
- **Diego's evening list, 20 November 2026**, taken before the rest of F6
  and in this order: "Make sure we can takeoff and land in real airport
  locations", "Also don't know how to change cockpit to chase view",
  "Also engine sound and propeller animation is needed", "I love playing
  in dusk", "We neee to chose what time of day to play in", "Also some
  real ATC radio could be nice if there is any stream of audio public we
  can tune into".
  - **F6a - time of day**, **built in 0.12.40** (`testing.md` 18.960).
    Dawn, morning, noon, afternoon, dusk and night in the pause sheet,
    remembered, and `T` steps through them while flying. Each is an hour
    of the solar day at the equinox, so dusk is 17:45 in every city: the
    sun's direction from the hour and the latitude, and the zenith, the
    horizon, the haze, the sun's colour and the ambient share blended
    between five skies by how high the sun stands. The 3D Kit's view gained
    two fields every program on it can use: `light`, the sun's colour (a
    view without it is lit white, as before), and `glow`, the sun's disc
    and the light around it in the sky, strongest along the horizon under
    it. At night the moon, opposite the sun, lights the ground.
  - **F6b - the views, said**, **built in 0.12.40**. A chip in the top
    right corner names the view, "View: Chase" beside a `V` key cap,
    under the graphics panel when that is open, and a click on it changes
    the view. The pause sheet lists every key. (This said the strip would
    name it; the cockpit has no strip, and the chip is in every view.)
  - **F6h - the keys, all of them** - asked by Diego on 21 November
    2026: "we need a help screen to show all the commands". F1 opens a
    Keys page where the pause sheet sits, and the flight pauses under it.
    It lists every key in four groups - flying (the arrows, the throttle's
    keys, the flaps, the brakes), the view and the world (V, T, H, M, F3),
    the flight (P, R, F1), and a gamepad's (the left stick, the triggers,
    A and B) - each with what it does and, where it steps, what it steps
    through (the flaps' 0, 15 and 30 degrees, the views' three). F1 or Esc
    closes it, and the pause sheet's Keys button opens it. The sheet's two
    lines of keys become one, pointing at F1. **Built in 0.12.52.**
  - **F8d - stopping on the runway, and the tyres** - asked by Diego on
    21 November 2026, flying: "we need a way to hear the landing sound
    like wheels squeaking", "a way to stop the plane in the runway", "i
    am at 0% throttle and the plane is still running on the floor".
    **Built in 0.12.52.** The brakes are B or Space, held, and the strip
    says BRAKES while they are on. An idling propeller now has a
    windmill's drag on the ground (0.8 m², fading as the throttle opens),
    so a landing at idle stops: rolling on 0.04 g alone it had coasted
    about a kilometre. At touchdown the tyres chirp twice on a third
    Synth Kit track - a square and noise through a band-pass at 1.8 kHz,
    struck as hard as the sink rate (a quarter at 100 fpm, full at 500).
  - **F6c - the propeller and the engine**, **built in 0.12.41**
    (`testing.md` 18.961). The propeller turned at its true rate, up to 40
    turns a second, which frames at 10 to 60 a second sample into standing
    still or turning backwards. It is drawn as a camera shows one: three
    turns a second at idle to twelve at full power, and never more than a
    sixth of a turn a frame, so it always turns forwards. (This said a
    blurred disc above low power; the capped rate reads as turning at
    every frame rate, and a disc would need the model to carry one.) The
    engine and the wind are two held notes on the Synth Kit, into a
    stream called Flight in the Mixer. The engine's pitch is its firing
    rate, 23 a second at idle and 90 at full power, and its low-pass opens
    with the throttle. The wind is noise through a band-pass that rises and
    grows with the airspeed. `M` turns the sound off and on, remembered.
- **F8 - real airports.** OurAirports' `airports.csv` and `runways.csv`
  (public domain), the paved runways of every airport with a code, made
  small by a build step into a file Flight carries, and searched by the
  pause sheet beside Nominatim: "SBRJ", "Heathrow". The runway drawn as a
  strip with its number and centreline, the terrain flattened under it to
  the threshold's elevation, and the ground: wheels with rolling friction,
  `B` for the brakes, rotation at the trainer's speed, and a touchdown
  under about 600 feet a minute a landing and over it a crash.
  - **F8a, built in 0.12.42** (`testing.md` 18.962): everything above but
    the strip drawn.
    - **The data.** `tools/airports.py` fetches the two files at a pinned
      commit and checks their sums. It keeps 9,498 airports and 11,753
      runways in 1.7 MB: open, paved, at least 1,500 feet, both ends
      known.
    - **The Airports library** (`user/lib/airports.lua`) answers by code,
      by name or by nearness. It is a library because a map or the Radio
      Kit asks the same questions.
    - **The sheet.** It finds airports at once, the large ones first, and
      the finder's places after them. A found place gets the nearest
      field within 25 km, so "On the runway" works anywhere near one.
    - **The ground.** The 3D Kit's terrain takes `flats`, and every
      runway within 30 km of the centre is level in the chunks and in
      `g:height` alike: Santos Dumont's 02R read 106 m on the suite's
      hill without them.
    - **Final approach.** A final-approach start aims 300 m past the
      threshold, at the touchdown zone.
    - **The strip itself (F8b), built in 0.12.43.** It was the imagery's
      alone - a metre a texel at best, a grey smear from the cockpit on
      the M700 at Heathrow. Now each runway within 30 km is drawn over
      it: asphalt from its two ends at its width and its ends' heights,
      and its paint at ICAO's sizes - edge lines, the centre line's 30 m
      dashes, the threshold's piano keys and the aiming point's blocks.
      The 3D Kit draws it through a new door, **`w:shape{ part, ... }`**:
      a model made of numbers instead of read from a glTF file, a part
      each colour, made once per terrain. `w:draw` now takes a list of
      models - the aircraft and the runways.
      **A flat thing on flat ground fights it for every pixel**, and the
      runway lies exactly on the ground the flats level. So a shape's
      part has a **pull**: the share of its distance to the eye that it is
      drawn nearer by, applied as a scale about the eye. Every point stays
      on the ray it was seen along, so the picture is the same and only
      the depth comes forward: 0.4% for the asphalt, 0.6% for the paint
      over it. A 32-bit depth buffer with the near plane at half a metre
      tells about a metre apart at 3 km; the asphalt comes 12 m nearer
      there. The runway numbers are not painted yet, and the invented
      island's runway is still to use the same door.
    - **The runway's lights (F8c)** - asked by Diego on 21 November
      2026, with a photograph of a runway at night: "the runway should
      have lights as real runways". Written before the code:
      - *Which*: as ICAO's Annex 14 lays them out for a precision
        runway. Edge lights every 60 m along both sides, white, and
        yellow over the last 600 m. Centre-line lights every 30 m,
        white. A row of green across each threshold, seen from the
        approach. A row of red across each end, seen from the runway.
        And an approach light system: a line of white lights 900 m out
        from each threshold, every 30 m, with a crossbar 300 m out.
      - *How they glow*: a shape's part may be a **light** - drawn its
        own colour, not lit by the sun, and still hazed by distance. That
        is one new field in `w:shape` (`light = true`) and a branch in
        the model shader; nothing else in the 3D Kit changes.
      - *How they are seen*: a real light is a point, and a 30 cm fitting
        is under a pixel past 300 m. Each is drawn as three small quads
        crossed - two upright, one flat - 1.2 m a side, so a row of them
        reads as a line of light out to the far end.
      - *When*: from dusk to dawn and under the cloud deck, as a pilot
        sees them lit. In daylight the shape is left out of the frame.
      - *What it costs*: about a hundred lights a kilometre of runway, six
        triangles each. That is a few thousand triangles over a city's
        airports, and one draw.
      - **Built in 0.12.53.** The flag rides in the tint: `light = true`
        sets the part's alpha to 2, which no colour has, and
        `k3d_model.frag` draws such a part as its colour hazed, with no
        light from the sun and no new push constant. The edge lights
        are 1.5 m outside the edges. The threshold's green row is 1.5 m
        before it and the end's red row 1.5 m inside. Each end's last
        600 m of edge lights are yellow from either direction, since
        one set of lights serves both. 913 lights near Montevideo and
        1,487 near Rio, in four colours, one part each.
- **F5k - the minimap** - asked by Diego on 21 November 2026: "We
  should add a small map that shows where we are and heading so the user
  can orientate itself". Written before the code:
  - *What it shows*: a square of map, 220 points a side, in the lower
    left corner over the strip. North is up, with the aircraft at the
    centre as an arrow turned to its heading. Each runway known is
    drawn as a bar, and the scale is said under it in kilometres.
  - *What it is made of*: the Map Kit, not a copy of it. Flight already
    fetches OpenMapTiles' vector tiles for its buildings, and the Map
    Kit's `tile:draw` draws one into a surface in Maps' colours. **Maps'
    style moves into a library** (`user/lib/mapstyle.lua`), so Maps and
    Flight draw a map one way, light and dark. The minimap's tiles are
    zoom 12 at cruise and 13 below 600 m, asked of the same `tiles`
    source in the same list as the buildings'.
  - *What it costs*: it is drawn again twice a second, or when the
    aircraft has moved five points on it, into a gfx surface laid over
    the frame by the 3D Kit (`w:overlay`, a seventh place). A tile is
    decoded once and kept while it is under the map.
  - **Built in 0.12.54** (`testing.md` 18.974). The 3D Kit lays eight
    overlays now, not six, and the minimap is the seventh. It goes up
    beside the strip only when the window is at least 600 by 420; in a
    smaller one Flight says "minimap not laid". A tile is decoded once
    and up to 24 are kept. The runways are drawn at least 3 points wide,
    so one shows at zoom 12. Under the map is a scale bar, the longest
    of 100 m to 20 km that fits a third of its width.
- **F9 - air traffic control.** LiveATC's public feeds, each an MP3 over
  HTTP, the nearest to the airport chosen by its code. Played by a new
  **Radio Kit** - an HTTP stream into the MP3 Kit's decoder into a stream
  of the audio server - since Music's Radio source wants exactly that
  door; Flight is its first user, and the volume is a row in the sheet.
    - **The radio, built in 0.12.44** - and it is a library, not a new
      kit. Everything it needs was already supplied: the Network Kit's
      download (`download.c`) streams an HTTP or HTTPS body into a region
      with no byte of it a Lua string, and the Playback Kit's MP3 player,
      on its thread in the audio band, takes frames from an address. So
      `/Kosmos/Libraries/radio.lua` only says where: how far the download
      has filled the region and how far the player has taken it. It
      follows redirects, and asks a station that stopped again after five
      seconds. `radio <address> [seconds]` plays one at the prompt.
      `x86-radio` holds it to a station on the Mac, heard on the sound
      card.
    - **The source is Diego's to choose.** LiveATC says on its own pages
      that third-party use of its live streams is prohibited, and its
      playlists sit behind a Cloudflare check. Neither is to be got
      round, so Flight has no station yet. What is open: asking LiveATC
      for permission, another public source, or ATC spoken by Kosmos
      itself - Xavier's voice saying the tower's lines with the real
      airport's runway and the aircraft's state, which needs nobody's
      stream.
    - **A switch for it, by the airspace** - asked by Diego on 21
      November 2026: "I should be able to turn on and off the atc system
      to hear air traffic control conversations based on the air space I
      am geographically". The switch is a row in the sheet (Off, On) and
      a key; with it on, the station is the one for the airspace the
      aircraft is in - the nearest airport with a feed, chosen again as
      the flight crosses into another's - played by `radio.lua` as the
      radio station is. **It still waits on the source** above: a list
      of feeds by airport is what it needs, and none can be used yet.
- **F10 - flying itself, from one airport to another** - asked by Diego on
  21 November 2026: "Flight from Montevideo to Buenos Aires and it will
  takeoff in Carrasco, navigate to Buenos Aires and land in Ezeiza
  airport". Written before the code, in four steps:
  - **F10a - the world follows a long flight.** The terrain is anchored to
    one place (`k3d_place_at`) and a flight of 200 km leaves it. When the
    aircraft is 20 km from the anchor, the terrain is anchored again
    under it and every position - the aircraft, the runways, the tower,
    the deck - is moved by the same metres, so nothing on screen jumps.
    The tiles are Web Mercator's and do not move; only the frame's origin
    does.
    - **Built in 0.12.58** (`testing.md` 18.978). `ANCHOR` is the
      terrain's centre, apart from `settings.lon/lat`, which stay the place
      the flight began in. Once the aircraft is `anchor_after` metres
      (40 km) from it along either axis, `ANCHOR.move` does the move:
      - every kept point - the aircraft, where a roll or a roll-out
        began - goes to longitude and latitude under the old centre, then
        back into the frame under the new one;
      - the terrain is made again there, from tiles the cache holds;
      - the runways near the new centre are levelled, drawn and lit, the
        tower is placed again at its field, and the deck is set again.

      Restart returns the centre to the place. What the measurement showed
      first: the terrain picks its tiles around the eye, and its vertices
      are floats relative to the centre, so 230 km out they are placed to
      about 3 cm. Precision was never the reason to move it. The runways
      were: they are made only within 30 km of the centre.
  - **F10b - the autopilot.** Modes as a real one has them: hold a
    heading, hold an altitude, hold a speed with the throttle, and climb
    or descend at a rate - each a small controller over the yoke and the
    throttle the keys and the pad already drive, so the flight model is
    not touched. A key switches it on and off, and the strip says the
    modes held.
    - **Built in 0.12.59** (`testing.md` 18.979). The arithmetic of
      flying left `flight.lua` for `model.lua` beside it: an aircraft
      file's fields and checks, the wind, one step of the flight model,
      and the autopilot. It takes what it works on as arguments and uses
      nothing but `math`, so `tools/test_flightmodel.lua` flies it on the
      host. Three holds, each over the controls a pilot has:
      - **heading**: a bank towards it, never past 25 degrees;
      - **height**: a climb or descent towards it of at most 5 m/s, the
        yoke on that rate, proportional and integral;
      - **speed**: an acceleration asked of the throttle, a tenth of the
        error a second and at most 0.5 m/s2, turned into throttle by the
        aircraft's mass over its full thrust here and now - so the
        Cessna and the 747 are one loop with one set of gains. With the
        Cessna's gains in throttle per m/s the 747's six-second spool
        swung its throttle between 0 and 0.5 for good.

      Q engages it on what the aircraft is doing, and Q again lets go;
      with it on, the arrows turn the heading held 20 degrees a second
      and the height 300 ft, A and Z the speed 5 kt, a number key takes
      the throttle back and lets go, and the wheels on the ground let go.
      The strip's labels say what is held, in AUTOPILOT's amber -
      `HEADING 090` - and the state says AUTOPILOT; with "AP" in them
      as well, a 1280-point strip ran each into the next. The climb rate overshoots its 5 m/s by about
      a tenth as the aircraft trades speed for height; no gain removes
      it, and the check allows a fifth.
  - **F10c - the route.** A flight plan is two airports from the
    Airports library: the departure's runway into the wind, a climb to a
    cruise height for the distance, a great-circle track to the arrival,
    a descent so as to be 3,000 ft over the field 10 nm out, and the
    runway into the wind there.
    - **How it is built** (21 November, before the code). The plan and the
      guidance are arithmetic, so they go in `model.lua` beside the
      autopilot and are flown on the host. They take the Airports
      library's `bearing` and `km` as arguments rather than carrying
      copies (*One kit, one door*).
      - **`M.route(from, to, P, wind_from, geo)`** makes the plan:
        - each airport's runway end most nearly into the wind, or with
          no wind the end most nearly along the track;
        - a **final fix** 10 nm out on the arrival end's centre line;
        - a cruise height from the distance - 5,000 ft for the 124 nm
          from Carrasco to Ezeiza - never above the aircraft's
          `speeds.ceiling_ft` (8,000 for the Cessna, 35,000 for the
          747), and never under 3,000 ft over the higher field;
        - the **top of descent**: as far out from the fix as a 3 degree
          path (318 ft a nm) takes to come down from the cruise to
          3,000 ft over the arrival field.
      - **`M.guide(plan, AP, lon, lat, P, geo)`**, each frame, sets what
        the autopilot holds:
        - the heading, the great circle to the fix, then to the
          threshold once it is passed;
        - the height, the cruise until the top of descent, then the
          3 degree path's height for the distance;
        - the speed, the cruise and from the fix the approach.

        It says each phase as it starts.
      - **In Flight**: a route is `settings.fly_to`, an airport's code.
        With one set, Q engages the autopilot on the route rather than on
        what the aircraft is doing, and the strip's state says
        `NAV EZE 87 nm`. The sheet's "Fly to" row is F10d's.
      - **Held by** `tools/test_flightmodel.lua`, which flies Carrasco to
        Ezeiza on the real airports file, with the Cessna and with the
        747. Each must pass the fix within a nautical mile of the centre
        line, at 3,000 ft over the field give or take 300, heading
        within 20 degrees of the runway. **Its control** takes the top
        of descent away, so the aircraft arrives at its cruise height
        and the height check fails. `x86-flight` holds the key: with
        `fly_to` set, Q holds the heading to the fix.
  - **F10d - the take-off and the landing.** Lined up, full power,
    rotate at the aircraft's speed and climb on the runway's heading;
    and at the end a three-degree glide path to the touchdown zone, the
    flare, the throttle closed and the brakes - what `x86-flight-land`
    holds the hand-flown final to, flown by the autopilot. The sheet
    gains "Fly to", an airport typed as the place is, and Flight flies
    there by itself, the time faster if it is asked (F11).
- **F11 - the time faster** - asked by Diego on 21 November 2026: "I
  should be able to speed up the game by 2x, 3x, 4x". The flight model
  is stepped two, three or four times a frame with the frame's time, so
  it stays as stable as at 1x; the sound, the sky and the frame rate are
  not changed. A key steps 1x, 2x, 3x, 4x, and the strip says it. The
  tiles are asked for further ahead, since the ground goes by faster.
  - **Built in 0.12.55** (`testing.md` 18.975). X steps 1x, 2x, 3x, 4x
    and back to 1x, and the Keys page lists it. The strip shows
    `TIME 2x` in the state's place on the right while it is faster, or
    `2x  BRAKES` beside another state, and nothing at 1x. It was a
    seventh cell at first, which a 1280-point window had no room for
    (seen in 0.12.57's picture). Each step asks for
    the ground under the wheels again, and a crash ends the frame's
    steps. The tiles are asked for from a point ahead of the eye, as far
    as the extra steps carry it in two seconds.
- **F12 - aircraft of their own** - asked by Diego on 21 November 2026:
  "I will be adding more planes as well with different capabilities like
  jet engines, a Boeing 747 and other types so each plane has specific
  features like speed, weight etc". Today the Cessna's numbers are a table
  in `flight.lua` (`PLANE`). F12 makes **an aircraft a file of its own** in
  Flight's folder, `Aircraft/<name>.lua`, returning its numbers. The file
  holds:
  - mass, wing area, lift and drag;
  - its engines - a propeller's power, or a jet's thrust and how that
    falls with speed and height;
  - flap settings, the speeds to rotate and approach at, a ceiling;
  - its model (a glTF file beside it), the cockpit's eye, where the
    wheels are;
  - its sound: piston or jet, the engine's notes.

  The sheet gains an Aircraft row listing the files there, so a new
  aircraft is a file added and nothing in Flight changed. The Cessna
  becomes the first file, and a 747 the second, which needs a jet engine
  model in the flight model and a 747's glTF.
  - **F12a, built in 0.12.56** (`testing.md` 18.976). The Cessna is
    `Aircraft/cessna172.lua`, and nothing of it is left in `flight.lua`:
    the airframe's numbers, the engine (`kind = "propeller"`, its power,
    static thrust, idle drag and rpm range), the model's file and the
    names of its moving parts, and the sound's note. Flight reads a file
    as data: `load` in text mode, with only `math` in its environment.
    It then checks every field the flight model, the 3D Kit and the
    sound read, and refuses a file by the name of the first one missing
    ("nomass refused: no mass"). The sheet has an Aircraft row, read
    once each time the sheet opens. Choosing another aircraft loads its
    model in place of the old one and starts again, still paused. A
    file that won't fly leaves the current one flying.
    `tools/installed.py`'s `aircraft()` is the one list of the files,
    used by the install, Flight's suite and the gallery.
  - **F12b, built in 0.12.57** (`testing.md` 18.977): a jet engine in
    the flight model, as
    `kind = "jet"` - thrust that falls with the air's density and a
    little with speed, and spools up over seconds rather than at once.
    Then the 747 as a file: its numbers, a glTF of it under a licence
    that lets it be carried, and a jet's sound. The model found is Miha
    Lunar's low-poly Boeing 747 (Poly Pizza `49CLof4tP2V`, about 2,000
    triangles, OBJ/glTF, CC BY), to go in `assets/models/b747/` with its
    licence beside it. It has no moving parts, so an aircraft file's
    `moving` becomes optional, each part named only if its model has
    one.
    As built:
    - **The jet.** A first-order spool towards the throttle over
      `spool_s` (6 s for the 747). Its thrust is
      `(idle + (1 - idle) * spool) * static * (rho / 1.225)^0.75`, times
      `max(0.5, 1 - lapse * airspeed)`. A jet has no propeller drag on the
      ground; its idle thrust (6%) is smaller than the rolling friction.
    - **What a file may leave out**, with the Cessna's values standing
      in: `moving` (each part), `pose` (the model's nose, up, units and
      the point on the runway), `speeds` (the starts' speed, throttle and
      flaps), `chase` (the camera's distance and height) and `eye` (the
      pilot's, ahead and above). `sound.voice` names synth parameters
      over the piston's.
    - **The 747's pose** was found from its geometry: up is the airframe's
      flattest axis, ahead is towards the cockpit's glass, 41.13 m a unit
      for its 70.7 m, and its origin a metre under the belly, since the
      model has no wheels. It is 300 t on 525 m² of wing and four 282 kN
      jets, at 250 kt in the air and 144 kt on a final.
    - An aircraft that was chosen and has since gone or broken falls back
      to the Cessna, with the reason said.
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
  - **As it will be built** (written 21 November, before the code, from
    how keys and the pointer travel today):
    - *Today*: the xHCI driver turns a pad's changes into keys
      (`kosmos_key_push`), and the console server reads them
      (`SYS_KEY_EVENT`). The window manager reads the console once a pass
      (`con.wait`) and posts `rawkey` events to the focused window, which
      takes them in its per-frame poll (`struct wm_frame_reply`,
      `wmproto.h`). Nothing on the input path shares memory.
    - *The pad's region*: init makes one page and hands it to two
      processes. The xHCI driver gets it writable and the window manager
      read-only, so nobody else can name it. Its shape is `padring.h`:
      four pads, each `struct pad_state` (buttons, four 16-bit stick
      axes, two 8-bit triggers) with a sequence count. The driver bumps
      the count to odd, writes, and bumps it to even again; a reader
      keeps a copy only when the count was even and the same before and
      after. One writer, no lock, the latest values only, and a report
      costs no message.
    - *To the window*: the window manager reads the region once a pass,
      and `struct wm_frame_reply` gains a pad block, 20 bytes a pad (a
      slot as the page holds it), in the head ahead of the events, for
      the focused window only - every other window is answered zeros.
      The reply is 332 bytes of a 2,048-byte message, so it has room. It is a value read once a frame, as the
      pointer's position is.
    - *Kits*: the Window Kit's `kw_poll` keeps the latest pad state, and
      the 3D Kit's window gains `w:pad(n)`, which returns the sticks as
      -1 to 1, the triggers as 0 to 1, and the buttons. The Lua protocol
      the Super Nintendo, Doom and Quake poll with gains the same in its
      reply, so Quake's `IN_Move` can take the right stick later.
    - *Keys stay*: a pad still presses keys as now, so nothing that
      plays with one today changes.
    - *Flight*: the left stick is the yoke, proportional (the arrows are
      full deflection), and the triggers are the throttle (right more,
      left less). A is the brakes and B the flaps.
    - *Checked*: QEMU has no pad to plug in, so the driver half is held
      by `tools/test_paddecode.c` as now. Above the driver, a test mode
      lets the guest suite write the region the way the driver would:
      `opt/kosmos/pad=test`, which makes init hand the region to the
      shell's test program as well. `x86-flight` then holds the left
      stick full left (`padtest 0 -1 0 0 0 0 0`) and reads that Flight
      rolled left; the control (`--no-pad`) writes it centred.
    - *What is new below Flight*: `padring.h`, init's page, the window
      manager's read, a block in the frame reply, and two kit methods.
      None of it is in the kernel.
    - Steps: **F7a** the region, the driver and the window manager;
      **F7b** the kits, Flight and the suite - **both built in 0.12.49**
      (`testing.md` 18.969; the Lua protocol's pads for the Super
      Nintendo, Doom and Quake are still to come); **F7c** on the M700
      with the 8BitDo.

## Questions for Diego

Answered on 20 November 2026: "Yes to all", so each recommendation is the
decision. The mockup asks them too:

1. **The imagery**: Esri's World Imagery, sharp to about half a metre in
   cities, or Sentinel-2 cloudless, open data at 10 m? *Recommended*:
   Esri's, with Sentinel-2 where Esri has nothing; the source is one line.
2. **Buildings**: OpenStreetMap's outlines and heights - which the Map Kit
   already reads for Maps - stood up as blocks with the photograph's roofs
   on top? *Recommended*: yes, after the flat city flies (F6). **Yes -
   Diego, 20 November 2026: "for f6 yes do buildings".**
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
