# Maps

Kosmos's own map: OpenStreetMap's world as shapes, drawn by Kosmos at any
zoom, sharp on any screen, with no browser underneath. Diego, 6 October
2026, with a picture of Apple Maps: "add a maps app to the roadmap taking
inspiration from the apple maps one", "The app should be lightweight and
cant use the browser engine, i want a native app that draws directly so its
speedy and clean". Drawn on 8 October in `docs/maps.html` and agreed -
"mockup is great", "but i do want a close/open sidebar button always
visible", "go ahead and build it".

This document is the feature set and the architecture; `maps.html` is the
window and `maps-architecture.png` the picture of what it stands on.

## What a person can do

**First, from a region on the disk** - the steps M1 to M5 below:

- **Look around.** Drag to move the map; the wheel to zoom in and out
  about the pointer; the + and - buttons; the arrow keys to move and
  `+` and `-` to zoom. A scale bar that says how far 100 points is.
- **See a city drawn.** Land, water, parks and woods, buildings at close
  zoom, roads by kind - motorway, main road, street, path - each at its
  width for the zoom, with their casings; names of places, roads, water and
  parks placed so they do not overlap.
- **Find a place.** Type in the sidebar's search: places, streets and
  points of interest whose names match, A to Z by how well they match;
  Return or a press goes to the first and opens its card.
- **A place's card**, over the map at its top left: its name, what it is,
  where it is, and Save.
- **Keep places.** Pinned - Home, Work and any added - along the top of the
  sidebar, Saved below, Recently viewed below that. Kept by the settings
  kit as `maps`, on this machine only.
- **The sidebar shown or hidden** by a button in the header that is always
  there (Diego's addition to the drawing), and by Ctrl+B.
- **Night and light**: the map takes the window's look.
- **Without any network.** A region is one PMTiles file. The image carries a
  made-up one - Port Alder, a city invented for the drawing and the tests,
  standing on Null Island at 0° 0° - and any PMTiles of OpenStreetMap
  vector tiles in `/Home/Maps` is offered beside it.

**Then, from the network** - M6 and after, each its own step:

- The world's map from OpenFreeMap's vector tiles over HTTPS, kept in a
  cache under `/Home` - fetched by a helper that answers when it has them,
  so the window never waits.
- Search the world (Nominatim or Photon), and directions (OSRM or
  Valhalla), within their terms.
- The map turned and tilted; the compass putting north back up.

**Not in it**: satellite pictures (no free source allows an application's
use), and anything that sends where a person is to anyone.

## Where each piece comes from

The premise (`CLAUDE.md`, *Kits supply, applications orchestrate*): every
piece that is not about this one application is supplied, and Maps is the
thinnest layer that puts them together. Asked of each piece: does another
application want this, and does something already supply it?

| Piece | Supplied by | New? |
|---|---|---|
| A filled shape and a wide line, anti-aliased | **gfx**: `path.c`, `s:polygon`, `s:polyline` and their C door `gfx_path_*` | new, in gfx - the 3D Kit's outlines, Write's shapes, Cafesa3D's guides and Paint's vectors want it too |
| Reading a PMTiles archive | **Map Kit**: `pmtiles.c` | new kit |
| Filling and stroking without seams where tiles meet | **gfx**: `raster.c`, the browser's SVG rasteriser, moved into gfx and made exact at a picture's sides | existed in the browser; now gfx's |
| Decoding a vector tile (MVT protobuf) | **Map Kit**: `mvt.c` | new kit |
| Drawing a tile in a style | **Map Kit**: `mapdraw.c`, on gfx's paths | new kit |
| A tile's names, for labels and search | **Map Kit**: `tile:labels()`, `archive:names()` | new kit |
| gzip, for tiles stored compressed | **compress kit**: `gzip.h` | exists |
| Text, its width, its face | **gfx**: `gfx_draw_text`, `gfx.measure` | exists |
| The header, buttons, field, lists in a window that draws its own pixels | **pixelkit.lua** | exists |
| A direct window, its band and its events | **ui.lua**, the Window Kit | exists |
| Pinned, Saved, Recently viewed | **prefs.lua**, as `maps` | exists |
| Tiles over HTTPS (M6) | **http.lua** and the TLS Kit, in a helper process | exists; the helper is new |
| Positions: longitude and latitude to the map and back | **Map Kit**: `map.project`, `map.unproject` (Web Mercator) | new kit |

The Map Kit is one kit with one door, `use("/Kosmos/Kits/map")`. Anything
else that wants a map - a photo's place, the Weather, a run's track - asks
it rather than copying it.

## The architecture

```
   maps.lua (Lua: what a person does)
     the window, the sidebar, search, the card, pins - pixelkit
     pan and zoom: which tiles, where on the screen
     labels: placed, kept from overlapping, drawn as text
         |
         |  archive:tile(z, x, y)  ->  a decoded tile, kept
         |  tile:draw(surface, x, y, size, style)   (C, every frame)
         |  tile:labels()  archive:names()          (once a tile)
         v
   Map Kit (C)  user/kits/map/
     pmtiles.c   header, directories, the Hilbert tile id, gzip by compress
     mvt.c       protobuf: layers, features, keys and values, geometry
     mapdraw.c   the style: which layer and class, what colour, how wide,
                 in what order; casings; each rule one pass of gfx paths
         |
         v
   gfx (C)  path.c - polygons and wide lines through one coverage buffer
            surfaces, text
         |
         v
   the window's region (a direct window), the window manager composes
```

**What is C and what is Lua.** A tile holds thousands of points and is
drawn every frame of a drag, so decoding it and drawing it are C - *a loop
over bytes, or something a collector pause would be felt in*. Which tiles,
where they go, which labels win, the sidebar and search are Lua, a few
dozen things a frame.

**What crosses where.** Nothing crosses between processes in M1-M5: the
archive is read from the disk once into the window's own memory, and the
pixels are the window's region. In M6 the helper fetches tiles and writes
them into the cache; what it tells the window is which tile arrived - a
message - and the bytes stay on the disk, where the window reads them.

**A tile is decoded once.** `archive:tile` returns a decoded tile -
geometry as points in the tile's own units, each feature with its layer,
class and name - kept in a table of the last few dozen, so a frame of a
drag draws and never decodes, reads or fetches.

**The style is data.** Colours, widths and order are a table in Lua - one
for Night and one for a light look - handed to the kit once
(`map.style{...}`); the kit keeps it as a C array of rules. Changing a
colour is not a C change.

## The busiest path: a drag

Each frame: the visible tiles at the zoom (on a 1720 by 1440 window at 512
points a tile, about twelve), each `tile:draw` filling its land, water and
parks, then its roads in two passes - casing, then fill - each rule a pass
of `gfx_path_fill` or `gfx_path_stroke` into one coverage buffer the size
of the tile on the screen, and one blend of it. Then the labels: their
places are kept from the last frame and only moved by the drag, so a frame
is a few dozen `text` calls.

Where the time goes, expected and to be measured at M3: coverage
accumulation is a few operations a pixel of each edge, and the blend one a
pixel per rule - so a frame is about rules x tile area, around ten passes
over the window's pixels, against gfx's blit at 42 ns a pixel. If it is
too slow, a tile is drawn once into a surface of its own and a drag blits
those, redrawing only when the zoom changes - the cache that would come
next, written down so it is not mistaken for the design.

## Steps

- **M1** - gfx paths: `s:polygon(xy, colour)`, `s:polyline(xy, width,
  colour)` and the C door, anti-aliased, held on the Mac to pixels by a
  host test, and by a control.
- **M2** - the Map Kit's PMTiles and MVT: an archive read, a tile found by
  its z/x/y, decoded; held on the Mac to `tools/mapcity.py`'s own region.
- **M3** - drawing a tile in a style; Port Alder carried in the image as
  the asset `maps/port-alder.pmtiles` (`sys.asset`, as the wallpapers and
  icons are - the programs' store holds text, not bytes), made at build
  time by `tools/mapcity.py`.
- **M4** - the window: header, sidebar and its button, pan, zoom, scale.
- **M5** - labels, search, the card, Pinned, Saved and Recently viewed.
- **M6** - tiles from the network, by a helper, cached; then search and
  directions from the network.

**Where it stands, 8 October**: M1 to M5 built in one night (0.11.61 to
0.11.64, `testing.md` 18.454 to 18.458) - Maps opens Port Alder, drags,
zooms, names its streets and places, searches them, opens a place's card
and keeps Pinned, Saved and Recently viewed. M6, the network, is next.
Drawn differently from the mockup, and why: Satellite and Transit are left
out until they exist (the drawing's fourth question); Directions is not on
the card yet, for the same reason; street names are level, so only along
streets within thirty degrees of it, until gfx can turn text.

Each step leaves a permanent test - the host's for C, `run_maps.py` for
the window - and only x86-64 runs them for now (`CLAUDE.md`).

## Decided along the way

- **Port Alder on Null Island.** A made-up city has to stand somewhere on
  a Mercator map; 0° 0° is the point every mapper knows is nowhere, so it
  cannot be mistaken for a real place.
- **The OpenMapTiles schema**, which OpenFreeMap serves: layers `water`,
  `landcover`, `landuse`, `park`, `building`, `transportation`,
  `transportation_name`, `place`, `water_name`, `poi`. Port Alder is
  written in it, so a real region and the made-up one draw through the
  same rules.
- **512 points a tile**, as vector tiles are made to be drawn.
