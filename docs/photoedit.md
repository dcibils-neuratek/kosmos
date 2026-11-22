<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos Paint: a photo editor, in a family with Kosmos Cut

Diego, 22 November 2026: "i want a new app for photo editing similar to
adobe photoshop and in the same line as the video eeditor, as it will be
part of a family of apps for photo and video editing like adobe family of
prodycts. After this we will do another app that is similar to adobe
illustrator which is a vector oriented design and editing tool."

This document has the feature set, what other editors do, the
architecture, the busiest paths, large images, the steps and the
decisions. `photoedit.html` is the window, and `photoedit-architecture.png`
is what it stands on, drawn from `photoedit-architecture.html`. It was
written on 22 November 2026, before any of it is built, and is **for Diego
to agree or change**. *Kosmos Paint* is a working name (decision 1).

It is the second of three applications in one family. **Kosmos Cut**
(`videoedit.md`) was agreed the same day, with every recommendation taken.
The vector editor comes next. Wherever this document says "shared", the
piece is one kit with one door that Cut and Paint both use, and it is
built once, by whichever of them needs it first.

It also answers an older request. On 7 October Diego asked for Paint to be
"redesigned as a serious image editor" (`roadmap.md`), after Pixelmator
Pro, with layers, blend modes, shape tools, and PSD planned "because we
want to store layers as well in a format". This design is that redesign,
widened to Photoshop and Lightroom. Today's Paint, in Demos since 0.11.512,
is replaced by it (decision 12).

---

## In short

- **One family, Kosmos Studio** (decision 1): *Cut* for film, *Paint* for
  pixels, *Draw* for vectors. MacPaint and MacDraw split the work this
  way. All three have the same window, the same header, the same
  inspector, the same colours, the same Looks and the same Export sheet. A
  person who knows Cut knows where everything in Paint is.
- **One application, two depths, as Cut has.** *Simple* is Lightroom's
  and Apple Photos' ground: one photograph, developed with sliders, a
  look, crop and straighten, spot healing, and a picture out. *Full* is
  Photoshop's: layers, groups, masks, adjustment and filter layers,
  selections, brushes, text and shapes. Both views show the same
  document. In it, Simple's development is the bottom of Full's stack.
- **The application is thin.** Every piece two applications need is a
  kit:
  - **Shared with Cut**, in the Graphics Kit: `grade.c`, which Cut
    proposed. Grown here: blend modes, scopes, effects and resampling.
  - **New, C**:
    - **the Canvas Kit**: tiles, layers, masks, compositing and undo by
      tiles;
    - **the Brush Kit**: dabs, strokes, clone and heal;
    - **the Selection Kit**: marquees, lassos, the wand, colour range
      and refining;
    - **the Picture Kit**: every picture format read and written,
      colour profiles, EXIF and RAW, behind one door.
  - **New, Lua**: `paintdoc.lua` (the document and its file),
    `undo.lua`, `params.lua` (the inspector's rows, declared once) and
    `exportsheet.lua`. Cut is the second user of all four.
- **No pixel passes through Lua.** Lua says what the document is. C keeps
  its pixels in tiles of 128 by 128, composites only what changed and only
  at the size it is seen, and hands the window the newest view in a ring,
  as Cut's viewer has it.
- **Large images are tiles.** A 24-megapixel photograph is about 1,500
  tiles a layer. A tile that was never painted takes no memory. A tile of
  one colour takes one word. Tiles are copied when written, so undo keeps
  only the tiles a step changed. Past a memory budget taken from the
  machine, cold tiles go to a file on the disk.
- **A photo moves between the family as a file.** A still from Cut opens
  in Paint as a `.paint` document. A `.paint` placed in Cut's timeline is
  drawn by the same Canvas Kit. When it is saved in Paint, Cut sees the
  folder change and draws it again.
- **CPU first, held on the Mac; then Vulkan compute**, on lavapipe in
  QEMU and `anv` on the M700, held to the CPU path. This is Cut's
  decision 7, kept.

---

## 1. What other editors do

Researched on 22 November 2026 from each product's own documentation and
release notes, with secondary reviews where the vendor's pages were
navigation only (see *Sources*). This is a summary in my own words.

### The six, in a line each

- **Adobe Photoshop** sets the vocabulary:
  - **layers**: pixel, adjustment, fill, shape and type layers; groups;
    *clipping masks* (a layer shown only where the one beneath it is);
    layer masks and vector masks;
  - **Smart Objects**: a layer that keeps its source, embedded or
    linked, so transforms and *smart filters* stay editable;
  - about **27 blend modes**, with opacity and fill;
  - **selection**: marquee, lasso, magic wand, quick selection, and
    *Select Subject* and *Sky*, which are learned models;
  - **Select and Mask**: refine-edge brush, smooth, feather, contrast,
    shift edge and decontaminate colours;
  - **retouching**: brushes; the healing brush, spot healing, the patch
    tool and clone stamp; content-aware fill and generative fill;
  - **transforms**: free transform, warp, perspective and puppet warp;
    crop with straighten and content-aware edges;
  - **adjustments**: levels, curves, hue and saturation, colour balance,
    and more;
  - **filters**, many of them live as smart filters;
  - **files**: Camera Raw for RAW development, ICC colour management at
    8, 16 and 32 bits, PSD and PSB, actions and batch processing.
- **Adobe Lightroom** is the development side. Its Develop module stacks
  panels that are worked down in order:
  - **Basic**: white balance, exposure, contrast, highlights, shadows,
    whites, blacks, texture, clarity, dehaze, vibrance and saturation;
  - **Tone Curve** and **HSL / colour mixer**, then colour grading;
  - **Detail** (sharpening and noise reduction), **Lens Corrections**,
    **Transform** and **Effects**;
  - **Masking**: automatic (subject, sky, background, people) or manual
    (brush, linear gradient, radial gradient, colour or luminance
    range).

  Every edit is a set of instructions over the untouched RAW. Presets
  carry a look from photograph to photograph.
- **GIMP** (3.0, March 2025, and 3.2) is GEGL underneath, with high bit
  depth and babl for pixel formats and colour spaces. 3.0 made most
  filters **non-destructive**: they stay live on a layer and can be
  edited, hidden or merged later, and they are saved in XCF. It added
  layer multi-selection and locks, imports PSD with its layer order, and
  reads and writes CMYK files, although it does not yet edit in CMYK.
- **Affinity Photo** divides the work into **personas**: Photo, Develop
  (RAW), Liquify, Tone Mapping and Export. A RAW file opens in Develop and
  is committed into Photo. **Live filter layers** are filters as layers,
  with a blend mode and a place in the stack. It also has adjustment
  layers, inpainting, frequency separation, and panorama, HDR and focus
  merges.
- **Pixelmator Pro** is the one the October design was after. It has
  non-destructive colour adjustment and effects layers, levels, curves,
  colour-wheel balance, replace colour, and a set of LUTs. Its learned
  tools are ML Super Resolution, Enhance, Denoise, Match Colors and Crop.
  Shape and text layers stay editable. Everything is built on the
  machine's GPU.
- **Photopea** is Photoshop's model in a browser. PSD is its native format
  and opens and saves back whole. It has smart objects, smart filters,
  layer styles, adjustment layers, channels and paths.

### What every one of them has

| Area | What it means |
|---|---|
| **Document** | A size, a resolution, a depth (8, 16 or 32 bits a channel), a colour profile, and layers. Open from any common format; save in the editor's own format with everything kept. |
| **Layers** | Pixel layers, groups, opacity, blend mode, visibility, lock, order, link. Masks on any layer or group. Clipping to the layer below. Adjustment layers (a correction over what is under it), fill layers, text and shape layers that stay editable. |
| **Selection** | Rectangle and ellipse marquees, lasso and polygonal lasso, magic wand by tolerance, colour range, select subject (learned), add, subtract and intersect, feather, grow and shrink, refine edge for hair, a selection saved as a mask, quick mask painting. |
| **Painting** | A brush with size, hardness, spacing, opacity and flow; pressure on a tablet; eraser; fill and gradient; clone stamp; healing brush and spot healing; patch; content-aware fill; smudge, dodge and burn; colour picker. |
| **Transform** | Move, free transform (scale, rotate, skew), perspective, warp, liquify; crop with straighten, rotate and flip the image; image size with resampling; canvas size. |
| **Adjustments** | Levels, curves, exposure, brightness and contrast, hue and saturation, vibrance, colour balance, black and white, channel mixer, selective colour, photo filter, gradient map, LUTs, invert, posterise, threshold. |
| **Filters** | Gaussian, motion and lens blur; unsharp mask and smart sharpen; noise reduction and added grain; high pass; distortions; render (clouds, lighting). Live as smart or non-destructive filters in the better ones. |
| **Text and shapes** | Point and paragraph text with faces, sizes, tracking and leading; rectangles, ellipses, polygons, lines and paths with fill and stroke; layer styles (drop shadow, stroke, glow, bevel). |
| **RAW** | Demosaicing, white balance by temperature and tint, exposure and the tone sliders, lens profiles, noise, a profile per camera, every edit a recipe over the RAW. |
| **Colour** | ICC profiles embedded and converted; a working space; soft proofing; 16-bit and 32-bit float documents; a histogram always to hand. |
| **Safety** | Unlimited undo while the document is open, a history list to step back to, snapshots, autosave and recovery. |
| **Output** | Save as PNG, JPEG, WebP, AVIF, HEIF, TIFF, PSD; export for screens at sizes, by quality, with or without metadata; batch actions over a folder. |

### What makes a first editor worth using

A person who opens a photograph wants to:

- crop and straighten it;
- correct its light and colour;
- take out a spot;
- add a word or a shape;
- put two pictures together with a mask;
- send it on as a JPEG.

That is Apple Photos' and Lightroom's sliders plus Pixelmator Pro's
layers. It needs five things to work: layers with masks and blend modes,
adjustment layers, a few selections, a brush with clone and spot heal, and
text. Photoshop's depth can wait. What cannot wait is that every change can
be undone and that nothing is lost.

### What makes Photoshop "fully equipped"

- **Non-destructive everything**: adjustment layers, filter layers, Smart
  Objects that keep their source, masks rather than erasing.
- **Selections as a discipline**: refine edge, colour range, channels,
  quick mask, and learned subject selection.
- **Retouching that understands the picture**: the healing brush's blend
  of texture and colour, content-aware fill.
- **Geometry**: perspective, warp, liquify.
- **Precision and colour**: 16 and 32 bits, ICC end to end, soft proof.
- **Scale**: documents of hundreds of megapixels, actions and batch, PSD
  and PSB.

So the tiers below put Lightroom's and Pixelmator's ground first, as a
picture a person can finish, and Photoshop's on top, in steps.

---

## 2. What a person can do (the feature set)

### Tier 1: a first editor worth using

**Simple, the Develop view.** One photograph, as Lightroom's Basic panel
and Apple Photos have it:

- **open** a picture by dropping it from Tracker or through Open... (the
  Open window, `panel.lua`): PNG, JPEG, WebP, AVIF, GIF, BMP and TIFF.
  Pictures from `/Home/Photos` and `/Home/Captures` are offered first;
- **crop and straighten**: free or a ratio (original, 1:1, 4:5, 3:2, 16:9),
  rotate by degrees with a grid, flip, and rotate by 90;
- **light**: exposure, contrast, highlights, shadows, whites and blacks;
- **colour**: temperature and tint, vibrance and saturation;
- **a look**: one of Cut's eight Looks with a strength slider. These are
  the same files, so a film and its stills match (*Looks*, below);
- **detail**: sharpen (amount, radius) and noise reduction (amount);
- **effects**: vignette, grain;
- **spot heal**: click or brush over a spot, and it is filled from around
  it;
- **auto**: one button that sets exposure, contrast and white balance from
  the histogram;
- **before and after**: hold `\` to see the original;
- the **histogram**, live, over the inspector.

**Full, the Layers view.** The same document, with its stack:

- **layers**: pixel layers, groups, opacity, blend mode (*Blend modes*,
  below), show and hide, lock, reorder by dragging, duplicate, merge down,
  flatten. A layer can be any size and anywhere, and can reach past the
  canvas;
- **masks**: a mask on any layer or group, painted with the brush or made
  from a selection; shown, hidden, inverted, applied;
- **adjustment layers**:
  - levels, curves (RGB and each channel), exposure, brightness and
    contrast;
  - hue and saturation (all, or one hue range), vibrance, colour
    balance (shadows, midtones, highlights), black and white;
  - invert, a LUT from a `.cube` file, and a Look.

  Each has a mask, a blend mode and an opacity. Simple's development is
  itself an adjustment layer, named *Develop*, at the bottom of the stack;
- **selection**:
  - rectangle and ellipse marquees, the lasso and the polygonal lasso;
  - the magic wand by tolerance, contiguous or not, and colour range by
    a picked colour and a fuzziness;
  - select all, deselect, invert, feather, grow and shrink, and a
    selection made from a layer's transparency;
  - shift adds, option subtracts, and both intersect. The selection is
    shown by marching ants;
- **painting**:
  - the brush (size, hardness, opacity, flow, spacing) and the eraser;
  - fill (bucket) and gradient (linear, radial; two colours or a
    preset);
  - the clone stamp, the spot healing brush, and the colour picker;
  - a foreground and background colour (X swaps them, D resets them);
- **transform**: move, free transform of a layer or a selection (scale,
  rotate, with the handles), flip; image size with resampling (Lanczos
  or bilinear); canvas size;
- **text**: a text layer of one or more lines - face, size, weight,
  colour, alignment, tracking, leading. It stays editable until it is
  rasterised;
- **shapes**: rectangle, rounded rectangle, ellipse, line and polygon,
  with a fill and a stroke. They stay editable as shape layers until
  rasterised (the October decision);
- **filters**: Gaussian blur, unsharp mask, noise reduction, add noise,
  high pass. They are applied to a layer or a selection, and in Tier 1
  they are applied for good, as undoable steps;
- **undo everything**: Command Z and Shift Command Z, without limit while
  the document is open and inside a memory budget (*Large images*). The
  History panel lists every step, and any of them can be returned to;
- **files**:
  - save as a `.paint` document, with everything kept and only what
    changed written;
  - autosave every minute while changes come, and recovery after a
    crash;
- **export**, one sheet shared with Cut (`exportsheet.lua`):
  - PNG, JPEG or WebP, at the document's size or another;
  - quality Good, Better or Best;
  - with or without the photograph's metadata (location is off by
    default);
  - the facts the file will have, and its size;
- **zoom and pan**: fit, 100%, Command plus and minus, the scroll wheel,
  Space to drag. Rulers, a grid and guides are shown on request.

### Tier 2: the fully equipped editor

- **RAW development**, in the Develop view:
  - cameras' RAW files (DNG, CR2/CR3, NEF, ARW, RAF, ORF, RW2), decoded
    to 16 bits in the camera's own colour, then white balance from the
    camera or picked;
  - the whole Basic panel, the tone curve, the colour mixer (HSL by
    eight hues), and colour grading (shadows, midtones and highlights
    wheels, the same wheels as Cut's);
  - lens corrections (distortion, vignetting, chromatic aberration) by
    hand, and by profile later;
  - presets: a Look carries development settings as well as a grade.

  The RAW is never changed. Development is a recipe stored in the
  document, as Lightroom does.
- **16-bit documents** (decision 6), and **32-bit float** for HDR merges
  and linear work.
- **Colour management**:
  - an embedded ICC profile is read and kept;
  - a working space is chosen per document: sRGB, Display P3, Adobe RGB
    or ProPhoto;
  - assign or convert a profile;
  - the screen is drawn through a conversion to sRGB;
  - soft proofing against a printer profile.
- **Non-destructive, everywhere**:
  - **filter layers** (Affinity's live filters, Photoshop's smart
    filters): a blur, sharpen, noise or high pass that stays live in the
    stack with a mask;
  - **Smart Objects**: a placed picture, RAW or `.paint` kept with its
    source, embedded or linked, transformed and filtered without loss,
    and opened for editing in its own window;
  - **clipping masks** (Option-click between layers), vector masks from
    a shape, and fill layers (colour, gradient).
- **Blend modes, Photoshop's full set**, and *fill* opacity apart from
  layer opacity. **Layer styles**: drop shadow, inner shadow, stroke,
  outer and inner glow, colour overlay.
- **Selection, in depth**:
  - **Refine Edge**: a view of the mask on black, white or overlay;
    smooth, feather, contrast and shift edge; a refine brush for hair;
    decontaminate colours;
  - quick mask; selections saved and loaded as channels;
  - **select subject** and **select sky**, once a segmentation model
    runs here (*Tier 3* for the model, written here so the door is
    there).
- **Retouching, in depth**:
  - the **healing brush**, with a source chosen by Option-click, which
    keeps the destination's colour and light and the source's texture;
  - the **patch tool**;
  - **content-aware fill** of a selection, by PatchMatch-style
    nearest-neighbour fields;
  - smudge, dodge and burn, sponge.
- **Geometry**: perspective and skew in free transform, **warp** by a
  grid of handles, **liquify** (push, twirl, pucker, bloat), and
  perspective crop.
- **Adjustments, more**: channel mixer, selective colour, photo filter,
  gradient map, posterise, threshold, shadows and highlights.
- **Filters, more**: motion blur, lens blur with a depth from a mask,
  smart sharpen, clarity and dehaze, pixelate, emboss, and a lighting
  effect.
- **Brushes, more**: presets, a brush from a picture's tip, scattering
  and jitter, and pressure and tilt from a tablet. The kit takes pressure
  from the start, and a tablet's driver is a roadmap item of its own
  (decision 13).
- **Files, more**:
  - **PSD and PSB read** (decision 5): layers, groups, masks, blend modes
    and opacity, the levels, curves, hue and saturation adjustment
    layers, and text as its pixels with its words kept;
  - PSD written later;
  - TIFF at 8 and 16 bits read and written;
  - HEIF read; AVIF written when Cut's AV1 encoder arrives (V13);
  - export at several sizes at once (1x, 2x, a width);
  - a document copied as one file to send.
- **Batch**: a saved recipe - a Look, an export preset, a resize -
  applied to a folder. It is an Automations task, so it can run on a
  trigger (`automations.md`; every control is scriptable by its name
  already).
- **Snapshots**: the document's state saved by name, beside the history.
- **Large documents**: hundreds of megapixels, by tiles and the disk
  (*Large images*).

### Tier 3: later, written down so they are not forgotten

- **Learned tools**: select subject and sky, generative fill and super
  resolution. These need a segmentation or diffusion model run on the
  GPU, as Xavier's LLM engine runs on `ggml`'s Vulkan.
- **Merges**: panorama stitching, HDR merge with tone mapping, and focus
  stacking.
- **Print**: CMYK editing and output for print.
- **Others**: an animation timeline (Photoshop's), 3D layers, plug-ins,
  scripts in Lua beyond Automations, and collaboration.

### Left out, on purpose

- **A second application for Simple.** One application with two views, so
  a photograph never has to be moved from one to the other. This is Cut's
  decision 2, kept for the family.
- **A library of photographs** (Lightroom's catalogue). `/Home/Photos` is
  a folder, Tracker shows it, and photographs are found by their
  attributes (`roadmap.md`, *Music and photos as libraries*: BeOS's
  queries, not a database). A Photos application is its own design.
- **Plug-ins** (Photoshop's 8BF). The filters are the kits', in C.

---

## 3. One family

### The name and the naming

**Kosmos Studio** is the family (decision 1). Each member is a short verb,
as *Write* is:

| | What | Its file | Its own kits |
|---|---|---|---|
| **Kosmos Cut** | film | `Name.cut/` | Timeline library, Edit Kit |
| **Kosmos Paint** | pixels | `Name.paint/` | Canvas, Brush, Selection, Picture kits |
| **Kosmos Draw** | vectors, after this | `Name.draw` | paths and the pen, on gfx's `path.c` and the PDF Kit |

### One look

`photoedit.html` is drawn from `videoedit.html`'s styles, class for class:

- **the header**: the document's name and facts, the Simple / Full switch
  in the same place, Open... and Export... at the right;
- **the panes**:
  - three columns, as in Cut. The left column holds the document's
    structure, which is Cut's library and Paint's layers;
  - the centre column holds what is seen, which is Cut's viewer and
    Paint's canvas;
  - the right column is the inspector, with the same tabs, section
    heads, slider rows, values and dropdowns;
- **the toolbar**: Cut's tool buttons and chips, over the timeline in Cut
  and over the canvas in Paint, each tool with its key;
- **the colours**: Astra's dark look, the same tokens;
- **the sheets**: Export is one sheet, shared, and the "exporting" window
  is the same.

### Where the two applications share code

| What | The one copy | Paint | Cut |
|---|---|---|---|
| Grade: exposure, contrast, temperature and tint, saturation, lift / gamma / gain, curves, HSL secondaries, 3D LUTs | `grade.c`, the Graphics Kit | Develop, adjustment layers | its Colour tab |
| Blend modes | `blendmode.c`, the Graphics Kit (new) | layers | the effect stack's blend mode |
| Scopes: histogram, waveform, RGB parade, vectorscope | `scopes.c`, the Graphics Kit (new) | the histogram, scopes in Develop | under the viewer |
| Blur, sharpen, noise, vignette, shadow | `effects.c`, the Graphics Kit (new); `blur.c`'s box blur stays glass's | filters, Develop's detail and effects | the effect stack |
| Resampling | `resample.c`, the Graphics Kit (new) | image size, transform, the view | export at another size, photographs on a timeline |
| Looks | `.look` files: a grade's parameters and an optional `.cube` | Develop's looks | Simple's looks |
| Text | the Text Kit (FreeType and HarfBuzz) | text layers | titles |
| Inspector rows | `params.lua` (new) | every adjustment and tool option | every effect, with the keyframe diamond shown |
| Undo and history | `undo.lua` (new) | the document's steps | `timeline.lua`'s steps |
| Export | `exportsheet.lua` (new) | pictures | films and stills |
| A colour well and picker | `ui.colour`, the UI kit (new) | foreground, text, fills | titles, mattes |
| A layered picture | `paintdoc.lua` and the Canvas Kit (new) | everything | a `.paint` placed on a track |

### One file language

- **A still from Cut, into Paint.** Cut's viewer gets *Edit frame in
  Paint*. The Edit Kit renders the frame at the project's full size from
  the originals, and Paint opens it as a new document: one layer, named by
  the film and the timecode, with the project's grade carried as a
  *Develop* layer rather than baked in. Saved, it is a `.paint` beside the
  project.
- **A photograph, into Cut.** Cut's import accepts `.paint` (and PSD,
  through the same kit). The Edit Kit asks the Canvas Kit for the
  document's composite at the size the clip is drawn, and keeps it in
  Cut's cache. Cut watches the document's folder (`win:watch_folder`,
  `pulses.md`'s folder watch). When Paint saves, `document.json` changes,
  and Cut draws it again on the next frame. *Open in Paint* is on the
  clip's right click.
- **Layers stay layers.** A `.paint` on a timeline is drawn whole in Tier
  1. Tier 3 could show or animate its layers on Cut's tracks, a title
  designed in Paint.
- **Looks go both ways.** A grade saved as a Look in either application is
  the same file, offered in both.
- **Draw will place a `.paint`** the same way, and Paint will place a
  `.draw` as a Smart Object, which the vector kit rasterises at any size.

### The `.paint` document

A document goes where its person saves it, and `/Home/Photos` is offered
first. It is a folder, as Cut's project is (decision 4):

- `document.json`: the size, depth, colour profile, guides, and the layer
  tree. Each layer has its kind, name, opacity, blend mode, visibility,
  lock, bounds, its mask's id, and its parameters (an adjustment's
  values, a text's words and style, a shape's geometry). It is written by
  `json.lua` and read field by field, never run, because a document from
  another machine is untrusted. This is Cut's decision 8, kept;
- `Layers/<id>.tiles`: one file a pixel layer. It holds an index, then
  each tile that is not empty, deflated by the Compress Kit. A layer is
  rewritten only when one of its tiles changed since the last save, so
  saving after a brush stroke on one layer writes one file;
- `Masks/<id>.tiles`: masks and saved selections, one channel each, in the
  same form;
- `Sources/`: what Smart Objects keep (the original RAW, a placed
  picture), and the RAW a developed photograph came from, unless it is
  linked where it is;
- `preview.jpg`: the composite at 2048 pixels on its long side, written on
  every save. Tracker's Quick Look (`preview.lua`), Photo Viewer and the
  Open window show a `.paint` from this alone, with no Canvas Kit;
- `Autosave/`: the last ten `document.json` files and the layer files they
  name, one a minute while changes come;
- `Cache/`: the spill file for cold tiles (*Large images*), which is never
  needed to open the document.

*Copy as one file* zips the folder (`zip.lua`), for mail. PSD is read
(Tier 2) and is the format for handing a layered picture to another
program. It is never the native format, since it cannot hold a Look, a
develop recipe or a linked source.

### Looks

A **Look** is a small file: a grade's parameters (as `params.lua` declares
them, by name) and an optional `.cube` LUT beside it. The image carries
the eight Looks Cut's Simple view offers. A person's own are saved by
either application and kept with the family's settings (`prefs.lua`).
Camera's looks, when it has them, are the same files.

---

## 4. What it is made of

**The application is the thinnest layer** (`CLAUDE.md`, *Kits, servers and
drivers supply; applications orchestrate*). Each piece below was asked the
two questions: does another application want this, and does something
already supply it? Every file named as existing was checked in the tree
on 22 November.

| Piece | Supplied by | New, grows, or as it is |
|---|---|---|
| The window: header, panes, lists, tabs, sliders, dropdowns, segments, sheets, menus | the UI kit, `ui.lua` (`ui.slider`, `ui.segments`, `ui.tabs`, `ui.sheet`, `ui.list`, `ui.dropdown`) | as it is |
| The canvas shown in a kit window | `ui.lend_picture` and `gc:lent`, a surface lent to the painter and drawn with no copy, as Cut's viewer | as it is; measured at P3 (Cut's own open question) |
| **A colour well and its picker** (hue ring and square, hex, the eyedropper) | **the UI kit**, `ui.colour` | **new** - Cut's titles, Draw, Write and Present's text and shapes, Appearance |
| **Inspector rows declared once**: a parameter's name, range, default, unit and how it is shown, drawn as a slider row, read by C as a struct field | **`params.lua`** | **new** - Cut's effects (with a keyframe diamond), Paint's adjustments and tool options, Draw's, Automations reading names |
| **Undo**: named steps, grouped and coalesced (a slider dragged is one step), a budget, the History panel | **`undo.lua`** | **new** - Cut's `timeline.lua` stands on it; Write and Present could |
| **The Export sheet**: formats declared with their options, the facts, the progress window | **`exportsheet.lua`** | **new** - Cut's Export sheet, Draw, Camera's save, a screen recorder |
| Choosing files, dropping them | `panel.lua` (its `as` row for Save As), `ui.drag` and `ui.dropped` | as it is |
| Settings, recent documents, "export finished" | `prefs.lua`, `recentdocs.lua`, `notify.lua` | as it is |
| The document's file type and icon | `filetypes.lua` | grows: `.paint`, `.look`, and RAW kinds |
| A folder changed (Cut seeing a save) | `win:watch_folder` (`pulses.md`, built) | as it is |
| One file to send | `zip.lua` | as it is |
| **The document**: the layer tree, each layer's properties, the `.paint` file read and written, the stack described to the Canvas Kit as declared structs | **`paintdoc.lua`** | **new** - Cut (placing a `.paint`), Draw, Present (a picture with adjustments), Photo Viewer |
| **Pixels in tiles**: sparse, copy-on-write, 8 or 16 bits; layers, groups, masks, clipping; compositing what changed; a pyramid of smaller levels for the view; undo by tiles; spilling to disk | **the Canvas Kit**, `user/kits/canvas/`, C | **new** - Cut (a `.paint` on a track), Draw (its raster effects and placed pictures), Present and Write (a picture cropped and adjusted), Camera (a capture with a look) |
| **Brushes**: dabs (round, hardness, a picture tip), spacing, pressure, a stroke smoothed through the pointer's samples; eraser, clone, spot heal, healing, smudge, dodge and burn | **the Brush Kit**, `user/kits/brush/`, C | **new** - Cut (painting a mask on a clip), Present's pen, the PDF Viewer's markup, Draw's pencil (the same smoothing, fitted to a curve) |
| **Selections**: masks as tiles, marquees and lassos rasterised, the wand's flood, colour range, feather, grow and shrink, refine edge, marching ants as an outline | **the Selection Kit**, `user/kits/select/`, C | **new** - Cut (a painted or qualified mask for a secondary grade), Present (remove a picture's background, Keynote's *instant alpha*), Camera |
| **Every picture format, read and written**: PNG, JPEG, WebP, AVIF, GIF, BMP, TIFF, HEIF; PSD's layers and composite; EXIF (orientation, camera, date, location); ICC profiles and conversions; RAW | **the Picture Kit**, `user/kits/picture/`, C, over W5's libraries | **new** - Photo Viewer, Tracker's previews, Write's and Present's pictures, Mail's attachments, Wallpapers, Cut's stills, Camera's save (`webkit.md` already says "the image kits move onto them") |
| PNG, JPEG, WebP and AVIF codecs | **libpng, libjpeg-turbo** (SIMD required), **libwebp**, **libavif** over dav1d: W5a and W5f's builds in `tools/w5_build.py`, fetched and held already | as they are; libwebp's encoder and libpng's writer used for the first time |
| TIFF, HEIF, RAW | **libtiff**, **libheif** with **libde265**, **LibRaw** (decision 9) | **new**, fetched by `tools/upstream.py` as W5's are |
| PSD | the Picture Kit's own reader, `psd.c`, from Adobe's published specification | **new** |
| Colour profiles | **skcms**, Skia's colour management, already in the tree's WebKit source (`Source/ThirdParty/skia/modules/skcms`, BSD, one `.cc` file and libc's headers), compiled into the Picture Kit (decision 8) | **as it is in JetSki**: one source, two images |
| **Grade, curves, 3D LUT, HSL qualifier, masks with feather** | **the Graphics Kit**, `grade.c`, as Cut's design proposed | **shared with Cut**; grows to take 16-bit rows (*Changes Cut's design should make*) |
| **Blend modes** | **the Graphics Kit**, `blendmode.c` | **new, shared** - Cut, the Canvas Kit, Draw |
| **Scopes**: histogram, waveform, parade, vectorscope | **the Graphics Kit**, `scopes.c` | **new, shared** - Cut's scopes had no home in its design; Camera's histogram |
| **Filters**: exact Gaussian blur, unsharp mask, high pass, noise reduction, grain, vignette, shadow, motion blur | **the Graphics Kit**, `effects.c`. `blur.c`'s three box passes stay what glass is frosted with | **new, shared** - Cut's effect stack, Paint's filters, Draw's effects |
| **Resampling**: Lanczos-3, bilinear and box, in vector lanes | **the Graphics Kit**, `resample.c`; `stretch` stays for the desktop | **new, shared** - Cut's export at another size, Photo Viewer, thumbnails |
| Shapes, wide lines, polygons, anti-aliased | the Graphics Kit's `path.c` over `raster.c` (Maps' and the browser's SVG's) | as it is; Béziers flattened are Draw's growth and Paint uses them |
| Text, shaped and rasterised | **the Text Kit**, FreeType and HarfBuzz (W5b); typing in a box through `textedit.lua` (`office.md`) | as it is |
| A frame of a film (opening a still directly) | the Media Kit's `media_frame_at` (Cut's V1) | Cut's |
| Files read, the spill file written | the Storage Kit and `regions.lua`; the spill file and layer files written at an offset (`DISK_OP_WRITE_AT`) | as it is |
| Compositing and filters on the GPU | **the Vulkan Kit**, `kvk.c`'s compute pipelines (`kvk_compute_make`, `kvk_compute_dispatch`), as Xavier's | as it is; the shaders are new (P15) |
| Batch | Automations' `autod` and its tasks (`automations.md`, A1-A4 built) | as it is |

**Does something already supply it? Four near misses, so they are not
copied:**

- **`gfx.png` and `gfx.jpeg`**, the stb_image decoders behind
  `picture_decode.h`. They are the system image's decoders, and they are
  right for icons and the desktop. The Picture Kit is in the family image,
  over W5's libraries, which need the POSIX layer's build. Until the
  system's decoders move onto the same libraries (`webkit.md`'s row on
  pictures), the tree has two PNG decoders behind two doors. That is
  already a recorded defect, not a new one, and this design does not add
  a third. `picture.h` takes `picture_decode.h`'s shape - decode into rows
  with a pitch, with a `room` callback - so moving the system onto it
  later changes no caller.
- **`gfx.encode_png`** writes PNG through the Compress Kit's deflate, for
  screenshots. The Picture Kit writes PNG through libpng, because it needs
  16 bits, a profile and metadata. The screenshot keeps its encoder until
  the system's decoders move, and then both are one.
- **Paint, the demo** (`user/bin/apps/paint.lua`): `disc` calls into a
  direct window, with no file, no undo and no layers. It is the brush
  path's demonstration, and it retires when Kosmos Paint reaches P3
  (decision 12). `user/lib/paint.lua` has nothing to do with it: it is
  the kit's drawing commands, and keeps its name.
- **Skia**, in JetSki's image, rasterises paths, text and pictures, and
  its codecs read PNG, JPEG and WebP. It is not the canvas engine. A
  Skia-backed canvas would put WebKit's 220 MB image under a photo editor,
  and Skia has no tiles, no undo and no 16-bit blend modes of
  Photoshop's kind. Only skcms is taken from it, as a source file.

### The doors

```lua
-- The document: layers, properties, the file. No pixels.
local paintdoc = use("/Kosmos/Libraries/paintdoc.lua")
local doc = paintdoc.open("/Home/Photos/Harbour.paint")   -- or paintdoc.new{ w, h, depth = 16 }
local l = doc:add_layer{ kind = "curves", above = id, points = { ... } }
doc:set(l, "blend", "multiply")   doc:set(l, "opacity", 0.8)
doc:move(l, to)  doc:group{ a, b }  doc:mask(l, from_selection)
doc:save()       -- document.json, and only the changed layers' tiles
doc:describe()   -- the stack, flat, in canvas.h's structs

-- The kits, from the family image (decision 3).
local k = use("media.elf")
local c = k.canvas.open{ doc = doc:describe(), depth = 16, budget = bytes }
c:read_layer(id, "/Home/Photos/Harbour.paint/Layers/7.tiles")
c:place(id, k.picture.read("/Home/Photos/harbour.jpg"))   -- decoded into tiles
local v = c:view{ out = region, w = 1400, h = 900, frames = 3 }
v:look(x, y, zoom)        -- what is visible; newest wins
v:frame()                 -- the newest composited slot: lend it to the canvas view
c:set(doc:describe())     -- after every change: what changed is recomposited

local b = k.brush.new{ size = 120, hardness = 0.6, flow = 0.4, colour = 0xffc8402c }
local s = c:stroke(layer_or_mask, b)     -- a stroke begins; pixels go into tiles
s:to(x, y, pressure, us)  s:finish()     -- -> the step's tile set, for undo.lua

local sel = c:select{ kind = "lasso", points = pts, mode = "add", feather = 2 }
c:wand(x, y, { tolerance = 24, contiguous = true })
c:outline()               -- marching ants, as segments, at the view's zoom

c:undo_point()  c:restore(step)          -- tiles shared, nothing copied
k.picture.write(c, { path = ..., kind = "jpeg", quality = 88, metadata = false })
```

The Picture Kit's C door is `picture.h`:

- `picture_probe(bytes, n, facts)`: the kind, size, depth, orientation,
  profile and camera, without decoding;
- `picture_decode(bytes, n, room, user, out)`: into rows the caller owns,
  as `picture_decode.h` decodes now, at 8 or 16 bits;
- `picture_encode(rows, w, h, depth, kind, options, io)`: through a
  `struct picture_io` that appends, so a large picture is never held
  twice;
- `picture_psd_layers(...)`: PSD's layer records and channels, for the
  Canvas Kit to make layers of;
- `picture_raw_open(...)`, `picture_raw_develop(...)`: LibRaw behind it.
  The RAW is decoded once to linear 16-bit camera colour and kept. White
  balance and the camera matrix are the develop recipe's;
- `picture_profile(...)` and `picture_convert(...)`: skcms behind them.

### Where the code lives, and which process

- **An installed application in Cut's and Flight's shape**:
  `user/installed/Paint/` holds its Lua. Its C is the family's image,
  named with `-- kosmos: image media.elf`, and with `-- kosmos: image on
  intel-gpu media-anv.elf` when P15's shaders arrive, as Flight names
  `k3dvk-anv.elf`.
- **One image for the family** (decision 3): `media.elf` already carries
  the Media Kit, the Edit Kit, the Playback Kit and the POSIX layer for
  Video and Cut. It gains the Canvas, Brush, Selection and Picture kits
  and the Text Kit. An image is copied into memory when it starts
  (`elf.md`, "ELF copied yes"), so every member pays for every kit.
  *Estimated* at under 15 MB beyond what Cut's image is; P2 measures it.
- **One process.** The window is Lua on the main thread. The Canvas Kit
  runs:
  - a render thread that composites into the view's ring;
  - a pool of workers, one a core less one, for tile jobs: the
    full-resolution pass after a change, filters, decoding, and
    compressing tiles for a save;
  - a writer thread for the spill file and saves.

  Export and saving run at LOW, so the desktop and the canvas stay ahead
  of them. The pointer's events reach the window as they do now; a
  stroke's samples go to the render thread through a ring.

### C or Lua

- **Lua**: the window, the tools' rules (which handle is under the
  pointer, what a drag means), `paintdoc.lua`'s tree and its file, undo's
  list, and the export sheet. These are structure. `paintdoc.lua` and
  `undo.lua` are tested on the Mac's Lua, as `timeline.lua` will be
  (`tools/test_flightmodel.lua` is the model).
- **C**: everything per pixel. That is tiles, compositing, blend modes,
  adjustments, filters, dabs, the wand's flood, feathering, outlines,
  resampling, the pyramid, the histogram, decoding and encoding, and
  colour conversion. Loops over pixels use GCC's vector types, as
  `gfx.c`'s blend and `pack.c` are written, at 16 lanes of 16 bits in
  AVX2 and 8 in NEON. The scalar loop is kept as the reference a host test
  holds the vector one to, natively and through Rosetta.
- **GPU**: the same compositing, adjustments and filters again as Vulkan
  compute through `kvk.c`, with tiles as storage images, held to the C
  path on lavapipe and then on `anv` (decision 10).

### What travels in a region and what in a message

(`CLAUDE.md`, *Control by message, data by shared memory*.) Almost all of
it is inside one process, so "a message" is mostly a call into a kit. The
rule still decides the shape.

- **Tiles** live in memory the Canvas Kit maps, never in Lua. They are
  64 KB at 8 bits and 128 KB at 16 bits for RGBA, and 16 KB for a mask.
  They are reference-counted and copied only when written.
- **The view** is **a ring of three surfaces in a region**,
  single-producer and single-consumer, as Cut's viewer has it: the
  render thread writes, and the window lends the newest to the canvas
  view. Only what changed is redrawn into a slot, and the damage
  rectangle goes with it.
- **The stack** crosses from Lua into the Canvas Kit as **a declared
  shape**: `canvas.h`'s fixed structs. A struct holds a layer's kind, its
  bounds, its blend mode and opacity, its mask's id, its clip flag, and
  up to 32 parameters (an adjustment's values; a curve's points are in
  the struct, and its tables are built in C). `paintdoc:describe()`
  builds them into a buffer and `c:set` copies it whole. It is not a
  server boundary, but it keeps the C threads from walking Lua tables,
  which they may not.
- **A stroke's samples** - position, pressure and time - go into a ring
  the render thread reads. One writer and one reader, so nothing
  waits.
- **A file read** comes into pages (`regions.lua`) and is decoded from
  there, so its bytes never become a Lua string, as `pictures.lua` does
  now.
- **The spill file and the layer files** are written in pieces with
  `DISK_OP_WRITE_AT`, never assembled whole in memory.
- **Progress** (saving, exporting, a long filter) is a counter a worker
  writes and the window reads twice a second.

---

## 5. The busiest paths

**Numbers below that are not measured are labelled as estimates.** QEMU's
numbers are not speeds (`CLAUDE.md`), so the M700 measures each one at its
step.

The case for both paths:

- a photograph of 6000 by 4000 pixels (24 megapixels), 16 bits a channel;
- twelve layers: the photograph, a *Develop* layer, three retouching
  layers, a text layer, two shape layers, three adjustment layers (curves,
  hue and saturation, a Look) and a masked copy;
- a window of 1400 by 900, showing the whole photograph fitted, at 23%.

A frame is 16.7 ms.

### Painting a brush stroke

1. **A pointer event** arrives in the window's Lua, at about 125 a second
   from a USB mouse (*estimated*, the usual polling rate). Lua turns it
   into the document's coordinates (one division by the zoom) and puts
   `x, y, pressure, time` into the stroke's ring. That is all Lua does
   while painting.
2. **The render thread** takes the samples since the last frame and fits
   them with a centripetal Catmull-Rom curve, so a fast stroke is round
   rather than a polygon. It walks the curve at the brush's spacing. A
   brush of 120 pixels at 10% spacing makes a dab every 12 pixels, so a
   quick stroke of 2000 pixels a second is about 170 dabs a second.
3. **Each dab** is a coverage mask, made once for the size and hardness
   and cached. It is laid into the layer's tiles with opacity and flow:
   about 11,000 pixels a dab, touching one to four tiles. Each tile is
   copied first if the step's undo still shares it. *Estimated* 15 µs a
   dab with vector lanes, so well under a millisecond a frame.
4. **Compositing what changed, at the size it is seen.** The view shows
   pyramid level 2 (a quarter of each side), and the stroke's damage at
   that level is a few hundred pixels across. Two caches make the stack
   cheap:
   - **below**: the composite of every layer under the one being painted,
     kept while the stroke lasts;
   - **above**: the layers over it, composited again only in the damaged
     rectangle.

   So a damaged pixel costs the below cache, the painted layer with its
   mask, and the layers above. Here those are the three adjustments, as
   LUT lookups, and two shapes and the text, as blends: about eight
   operations a pixel. *Estimated* 300 by 200 pixels at 2 ns an operation,
   which is about 1 ms.
5. **The pyramid's levels** under the damage are made again by a 2-by-2
   average from level 0 up (*estimated* well under a millisecond). The
   view's slot gets the damaged rectangle, converted from the document's
   profile to sRGB (a 3D LUT, as `grade.c` has), and the ring takes it.
   The window lends it and only that rectangle is painted.
6. **Behind the view**, level 0 is composited at full size in the same
   rectangle by the workers. It is needed for zooming in, for saving the
   preview, and for the histogram's exact numbers. It is never waited
   for.
7. **The stroke ends**: the set of tiles it changed (with their old
   versions) is the undo step, and `undo.lua` gets one entry, *Brush on
   "Retouch"*. Nothing is copied.

**Where the time goes**: the dabs on a large soft brush, then the layers
above the painted one. A filter layer above (a blur of radius r) widens
the damage by r at each level, which is why filter layers come in Tier 2
with the measurement. **What is promised**: a dab on the screen in the
frame after its event, with the M700 measuring that from the event's time
to the slot's (P7). Under QEMU it is a regression number only.

### A curves slider, with the whole stack

The person drags the curves layer's midpoint. The layer is the ninth of
twelve.

1. **The slider's value** changes in Lua, and `params.lua` says which
   struct field it is. `c:set` copies the stack, about 12 by 300 bytes.
   The drag is coalesced: `undo.lua` gets one step when the button goes
   up, *Curves*.
2. **The curve's tables**: a monotone cubic through the points, sampled
   into a 4096-entry table a channel at 16 bits. That is microseconds.
3. **Below is cached**: the composite of layers 1 to 8 at the view's
   level, kept since the last change below the curves layer. The curves
   apply to it as one lookup a channel, then layers 10 to 12 again.
4. **Only the view's pixels**: level 2 of a fitted photograph is 1500 by
   1000, 1.5 megapixels rather than 24. At about five operations a pixel,
   split over four threads in bands, that is *estimated* 3-4 ms on the
   M700. So the photograph follows the pointer every frame.
5. **The histogram** is computed from the same view-level composite by
   `scopes.c` (*estimated* 0.5 ms) and drawn over the inspector.
6. **When the pointer rests**, for two frames, the workers composite level
   0, all 24 megapixels, in tiles, most visible first. *Estimated* 24M
   pixels at about 5 operations at 1.5 ns over four cores, about 50 ms.
   It never blocks the next movement: a new value cancels the tiles not
   yet done.

**Where the time goes**: the layers above the adjustment, and the move
from the view's level to full size. **The accuracy**: a curve applied at
level 2 and averaged is not exactly level 0 averaged, because a curve does
not commute with averaging. Photoshop and Affinity accept this while a
slider moves. The view swaps to the exact pixels when level 0 is done, and
export always uses level 0.

### Opening and saving

- **Opening a 24-megapixel JPEG**: libjpeg-turbo with its SIMD,
  *estimated* 150-250 ms on one core. Decoded straight into tiles, at 8
  bits, or widened to 16 if the document is. The pyramid is built by the
  workers. The fitted view shows level 3 first, as soon as the decode
  passes the first rows, then sharpens.
- **Saving**: only changed layers. Their tiles are deflated by the workers
  in parallel (*estimated* 20-40 MB a second a core for photographic
  tiles at deflate's fast level), and `preview.jpg` is written from level
  1. The window says *Saved* when the writer's last piece is
  acknowledged.

---

## 6. Large images

### Tiles

- **128 by 128 pixels.** The size is measured at P1 against 64 and 256. At
  64, undo and the dirty rectangles are finer, but the index and the
  per-tile cost are four times the size. At 256, the reverse.
- **Three kinds**:
  - **absent**: transparent, or the layer's fill, no memory;
  - **uniform**: one colour, one word;
  - **full**: pixels.

  A text layer over a photograph is mostly absent. A mask is mostly
  uniform.
- **Copy-on-write**: a tile has a reference count. An undo step, a
  duplicated layer or a snapshot shares it until somebody writes, and
  then the writer copies it.
- **The pyramid**: each pixel layer and each composite cache keeps levels
  1 to n, each half the last, made from the tiles below as they change.
  The view always works at the level nearest its zoom.

### Memory, on an 8 GB machine

The M700 has 8 GB. A budget for the document's tiles is taken from free
memory when the document opens: half of what `sysinfo` reports free, and
at most 4 GB (*recommended starting values*, measured at P14). The budget
holds the full tiles, the caches and undo's old tiles together.

| Case, 6000 by 4000 | 8 bits | 16 bits |
|---|---|---|
| One full layer | 96 MB | 192 MB |
| Its pyramid | +32 MB | +64 MB |
| Twelve layers as above (the photograph, Develop, three part-retouching layers, text, shapes, adjustments with no pixels, one masked copy) | *estimated* 350 MB | *estimated* 700 MB |
| The below and above caches at full size | 192 MB | 384 MB |
| A brush stroke's undo, a band across the picture | *about* 4-8 MB | 8-16 MB |

**Past the budget**, the least recently used tiles are written to
`Cache/spill` in the document's folder, deflated, at an offset, by the
writer thread. They come back when touched. A tile being written is never
spilled. The order is:

1. undo's oldest tiles go first;
2. then caches are dropped, since they can be made again;
3. then cold layer tiles are spilled.

Kosmos has no swap, so this is the application's own paging, and it is
bounded by the disk. A document of 200 megapixels at 16 bits works on the
M700, slowly where it pages, rather than refusing.

### Undo by tiles

- A step is a **set of tile references**: which tiles of which layer or
  mask it replaced, and their previous versions. Undo swaps them back.
  Redo swaps them again. Neither copies a pixel.
- A step that changes only the stack (an opacity, a blend mode, a curve,
  a reorder) holds only the old and new `document.json` fragment, and no
  tiles.
- `undo.lua` holds the list: names, groups, the budget in bytes as the
  Canvas Kit reports each step's size. History is unlimited until the
  budget, then the oldest steps are dropped, and the History panel says
  from where.
- **A snapshot** (Tier 2) is a named step that is never dropped.
- Undo is held by a host test that applies every operation and its undo
  and compares the tiles word for word (P1).

---

## 7. Steps

Each step ends in something checked. The gate runs on x86-64 QEMU (`make
test`). The Mac runs host tests of pure C and of Lua on its own
interpreter. Test pictures are made at the test's start:

- by the Mac's tools and Python from known pixels, as
  `tools/picture_fixtures.py` makes them;
- RAW fixtures are a DNG written by the test from a known Bayer pattern,
  and nothing of any camera's is committed.

No site's bytes and none of Diego's pictures go into the repository. Every
check has a control that must fail.

- **P0 - before any code: the family's shared pieces**, done once and by
  whichever of Cut and Paint reaches them first:
  - `grade.c`, `blendmode.c`, `scopes.c`, `effects.c` and `resample.c` in
    the Graphics Kit, each held on the Mac to a scalar reference
    (`tools/test_grade.c` and the rest, natively and through Rosetta, as
    `test_yuv.c` does);
  - `undo.lua`, `params.lua`, `exportsheet.lua` and `ui.colour`.

  Cut's V7 and V11 need most of these, so in the recommended order (decision
  11) they arrive with Cut.
  - Control: a blend mode with its two inputs swapped must fail; Lanczos
    held to a direct sum of the kernel; the histogram of a picture with a
    known count.
- **P1 - the Canvas Kit on the Mac.** `canvas_core.c`:
  - tiles of the three kinds, copy-on-write, layers, groups, masks and
    clipping;
  - normal compositing and every Tier 1 blend mode, at 8 and 16 bits;
  - the pyramid, the below and above caches, undo by tiles, the budget
    and the spill file.
  - Host test, `tools/test_canvas.c`:
    - a hundred random stacks, each composited and held to a whole-picture
      scalar compositor written plainly beside it;
    - every operation followed by its undo returns identical tiles;
    - a document past its budget composites the same as one within it.
  - Control: a tile written without being copied must fail the undo
    check.
- **P2 - the Picture Kit, and the family image.**
  - The kit, with these codecs:
    - read PNG, JPEG, WebP, AVIF, GIF, BMP and TIFF;
    - write PNG, JPEG and WebP;
    - EXIF orientation and facts, and ICC profiles through skcms;
  - libtiff fetched and built by `tools/w5_build.py`;
  - `media.elf` linked with it, its size measured.
  - Host test:
    - each format to the word against the library's own decode, called
      directly, and against macOS's for a second opinion, as
      `test_picture_decode.c` does;
    - every writer's file read back by the Mac's own tools;
    - a profile conversion held to skcms's own result.
  - Suite `x86-picture` (or `run_pictures.py` grown): the same pictures
    decoded inside Kosmos, each line the same as on the Mac.
  - Control: a JPEG decoded without its EXIF orientation must fail.
- **P3 - the window, showing.** `user/installed/Paint/` and
  `paintdoc.lua`:
  - open a picture; fit, zoom and pan through the pyramid;
  - the layers panel: add, hide, opacity, blend, reorder, group;
  - save and reopen a `.paint`.
  - Host test, `tools/test_paintdoc.lua`: a document round-trips through
    `document.json`, and a corrupt or hostile one is refused.
  - Suite `x86-paint`: a fixture's layers with a multiply layer are opened,
    and the canvas's pixels on the screen are held to the host
    compositor's.
  - Measured: the canvas's frame rate with a lent surface changing every
    frame (Cut's open question; if it does not hold, the canvas becomes a
    direct child window, and so does Cut's viewer).
  - The demo Paint retires (decision 12).
- **P4 - Simple, the Develop view**: crop and straighten, rotate and flip,
  the light and colour sliders, Looks and strength, sharpen and noise,
  vignette and grain, auto, before and after, and the histogram. These
  are on `grade.c` and `effects.c`, as the *Develop* layer.
  - Pixels held to the host reference at level 0; the drag measured at the
    view's level under `-icount` as a regression.
- **P5 - export and the file language**:
  - `exportsheet.lua`'s sheet with PNG, JPEG and WebP, sizes, quality and
    metadata;
  - *Edit frame in Paint* from Cut, and a `.paint` placed in Cut and
    updated when saved (needs Cut's V4).
  - Suite: an export read back inside Kosmos, its facts and pixels
    checked. A `.paint` saved while Cut shows it, and the new colour found
    in Cut's viewer.
- **P6 - adjustment layers**: levels, curves, exposure, brightness and
  contrast, hue and saturation, vibrance, colour balance, black and white,
  invert, LUT and Look, each with a mask, blend mode and opacity.
  - Host tests on `grade.c`'s references.
  - The curves drag measured **on the M700**: from the pointer's event to
    the view's slot, at a 24-megapixel, twelve-layer fixture.
- **P7 - brushes**: the Brush Kit with the brush, eraser, fill, gradient,
  clone stamp and spot heal, the colour picker, and pressure in the kit at
  1.0 from a mouse.
  - Host test, `tools/test_brush.c`: a stroke through known samples held
    to a scalar dab-by-dab reference. Spot heal held to its formula on a
    picture with a known blemish.
  - **On the M700**: a dab's latency from event to slot on the same
    fixture.
- **P8 - selections and masks**: the Selection Kit with the marquees,
  lassos, wand, colour range, add, subtract and intersect, feather, grow
  and shrink, invert, from transparency, marching ants, layer masks
  painted and made from a selection.
  - Host test: each tool's mask held to a reference (the wand's flood to a
    plain recursive fill on small pictures, a feather to an exact
    Gaussian).
- **P9 - text, shapes, transform and filters**:
  - text layers through the Text Kit and `textedit.lua`;
  - shape layers on `path.c`;
  - move and free transform (scale and rotate, Lanczos);
  - image and canvas size;
  - Gaussian blur, unsharp mask, noise reduction, add noise and high pass,
    applied as steps.
  - Pixels against host references. A text layer held to the Text Kit's
    own raster.

**P1 to P9 are Tier 1**, an editor a person can finish a picture in. Each
step's suites pass before the next. The whole gate runs when the Graphics
Kit, the UI kit or `media.elf`'s kits change, since others stand on them.

- **P10 - 16 bits and colour, end to end**: 16-bit documents from 16-bit
  PNG and TIFF, working spaces, assign and convert, the screen through
  sRGB, and soft proof.
- **P11 - RAW**: LibRaw in the Picture Kit, the Develop view's RAW panels,
  the tone curve, colour mixer and grading wheels (Cut's wheels), lens
  corrections by hand.
  - Host test: a DNG made from a known Bayer pattern and matrix, held to a
    plain demosaic and matrix written in the test.
- **P12 - PSD and TIFF**: PSD and PSB read into layers (groups, masks,
  modes, opacity, levels, curves, hue and saturation, text as pixels) by
  `psd.c`; TIFF 16-bit written; HEIF read through libheif and libde265.
  - Host test: PSDs written by a Python writer from known layers, read back
    layer for layer. A PSD made by the Mac's own tools, if one is at hand,
    as a second opinion.
- **P13 - non-destructive, fully**: filter layers, Smart Objects (embedded
  and linked, opened in their own window), clipping masks, vector masks,
  fill layers, layer styles, the rest of the blend modes and fill opacity.
- **P14 - retouching and geometry, in depth**: the healing brush, patch,
  content-aware fill, smudge, dodge, burn and sponge, perspective,
  warp and liquify, refine edge, quick mask, channels, snapshots, batch
  through Automations, and the memory budget measured on the M700 with a
  200-megapixel document.
- **P15 - the GPU**: compositing, the adjustments and the filters as Vulkan
  compute through `kvk.c`, tiles as storage images.
  - Held to the C path's output on lavapipe in QEMU, then on `anv` on the
    M700, where it is measured. Content-aware fill and liquify are the
    first to move.

**P10 to P15 are Tier 2.**

---

## 8. Decisions that are Diego's

1. **The names.** *Kosmos Studio* for the family, with **Kosmos Paint**
   for pixels and *Kosmos Draw* for vectors beside *Kosmos Cut*
   (recommended). They are short verbs, as *Write* and *Cut* are. *Paint*
   and *Draw* are MacPaint's and MacDraw's split, and *Paint* is the name
   the October redesign already had. *Or Kosmos Develop*, which says RAW
   and Lightroom but not layers. *Or Kosmos Retouch*. For the family, *or
   Atelier*.
2. **One application with Simple (Develop) and Full (Layers) views**, the
   same document in both, switched in the header where Cut's switch is
   (recommended). The labels read *Simple* and *Full*, as in Cut. *Or
   two applications*, a Lightroom and a Photoshop, which means two windows
   and a photograph moved between them.
3. **One image for the family, `media.elf`** (recommended): Video, Cut,
   Paint and later Draw, with every media and creative kit linked once. It
   costs each member the others' kits in memory, *estimated* under 15 MB.
   *Or a second image*, `studio.elf`, for Paint and Draw without FFmpeg:
   smaller, but *Edit frame in Paint* and a still from a film then go
   through a file.
4. **The `.paint` document is a folder**, written incrementally, with
   *Copy as one file* for mail (recommended), as Cut's `.cut` is. *Or one
   zip file*, as Write's `.write` is: easier to send, rewritten whole on
   every save, which is seconds for a large document.
5. **PSD read in Tier 2 (P12), written later** (recommended): layers,
   masks, groups, modes, the common adjustments, text as pixels. *Or PSD
   read in Tier 1*, which brings P12 forward before brushes. *Or PSD as
   the native format*, which cannot hold a Look, a develop recipe or a
   linked source.
6. **8- and 16-bit documents from P1, composited in 16-bit lanes; 32-bit
   float in Tier 2** (recommended). This asks Cut's `grade.c` to take
   16-bit rows from its start. *Or 8-bit first*, as Cut's decision 9 has
   it, which is half the memory and bands in a pushed curve or a smooth
   sky.
7. **Blending in the document's own encoding**, as Photoshop does by
   default, so a PSD looks the same here (recommended), with RAW
   development working in linear light inside. *Or linear blending
   always*, which is physically right and makes every PSD look different.
8. **Colour management by skcms**, Skia's, from the copy already in the
   tree for JetSki: one source compiled twice (recommended). *Or Little
   CMS 2*, more complete (CMYK, named colours, every ICC version), a
   second colour engine in the tree.
9. **RAW by LibRaw**, fetched as W5's libraries are (recommended): it is
   maintained, covers most cameras, and is one library. *Or dcraw*, one C
   file, unmaintained since 2018. *Or rawspeed*, darktable's, fast and
   C++, which wants more of the C++ runtime.
10. **CPU first, then Vulkan compute at P15** (recommended), so every
    operation has a C reference before it has a shader, as Cut's decision
    7. *Or the GPU from P1*, faster on the M700 sooner and with nothing to
    hold it to.
11. **Where it sits in the order.** After Cut's V1 to V7, which build
    `media.elf`, `grade.c` and the shared Lua pieces, then P1 to P9 as one
    run (recommended). *Or before Cut*, with P0 then built by Paint.
12. **The demo Paint retires** when Kosmos Paint reaches P3, and the
    October entry ("Paint, redesigned as a serious image editor") is
    marked answered by this design (recommended). *Or keep the demo in
    Demos* as the brush path's demonstration.
13. **A pen tablet's driver as a roadmap item of its own** (recommended).
    The Brush Kit takes pressure and tilt from the start, a mouse gives
    1.0, and a USB HID digitiser (a Wacom) is a driver in
    `user/drivers/usb/`. *Or leave pressure out* until a tablet is here.

---

## Changes Cut's design should make, to be a family

These are not made here. They are for the main session to fold into
`videoedit.md`, `videoedit.html` and `videoedit-architecture.html`, if
Diego agrees.

- **`scopes.c` in the Graphics Kit.** Cut's V11 draws four scopes "by C
  into surfaces", but its table gives them no home. They are a shared
  piece.
- **The effect stack's filters and blend modes come from the Graphics
  Kit** (`effects.c`, `blendmode.c`), not from code inside the Edit Kit.
- **`grade.c` takes 16-bit rows as well as `0xffRRGGBB`**: one source
  compiled for both widths. Cut keeps decision 9, 8-bit, and uses the
  8-bit entrance.
- **Titles through the Text Kit** (FreeType and HarfBuzz, in the image),
  rather than gfx's faces. Then Cut's titles, Paint's text layers and
  Draw's type are one renderer.
- **`timeline.lua`'s undo stands on `undo.lua`.** The inspector's rows are
  `params.lua`'s, with the keyframe diamond shown. The Export sheet is
  `exportsheet.lua`'s. The colour of a title is `ui.colour`.
- **Looks are files** (`.look`), shared with Paint.
- **Import accepts `.paint` and PSD** through the Canvas Kit and
  `paintdoc.lua`, watched with `win:watch_folder`. *Edit frame in Paint*
  is on the viewer's menu, and *Open in Paint* on a `.paint` clip's right
  click.
- **Scaling for export at another size by `resample.c`**. `gfx_i420` into
  a scaled target stays the viewer's fast path.
- **Decision 3 widens**: `media.elf` is the family's image, Paint's and
  Draw's as well as Video's and Cut's.
- **The header's facts** follow one shape in both: the document's name,
  then its size and rate or size and depth.

---

## What I could not check

- **Speeds.** Every dab, composite, curve, decode and save time above is
  an estimate. P6, P7 and P14 measure them on the M700.
- **Whether a lent surface holds 60 frames inside a kit window** while it
  changes every frame. That is Cut's open question too, measured once at
  P3 or Cut's V4, whichever is first.
- **Whether skcms builds unchanged in the family image.** It is one C++
  file over libc's headers with optional AVX2 and AVX-512 paths. It has
  only been built inside Skia here.
- **Whether LibRaw, libtiff, libheif and libde265 build for Kosmos's POSIX
  layer.** They are not in the tree. LibRaw and libheif are C++. None has
  been tried.
- **Whether W5's libraries could link into the system image** without the
  POSIX layer, which is what would let the system's decoders move onto
  them and leave one PNG decoder.
- **The image's size.** "Under 15 MB" for the new kits and libraries is an
  estimate. P2 measures it.
- **A tablet's events.** Kosmos's pointer path carries a position and
  buttons (`hal_pointer_move`, `usb.md`). Pressure and tilt would need a
  field, which is a decision for the tablet's driver.
- **PSD's adjustment layers beyond levels, curves and hue/saturation**,
  and its Smart Objects' internals. Adobe's specification documents their
  keys but says little of the latter.

---

## Sources

Read on 22 November 2026:

- Adobe, *Photoshop User Guide*: adjustment and fill layers, layer
  opacity and blending, create masks, create and manage layers -
  <https://helpx.adobe.com/photoshop/using/adjustment-fill-layers>,
  <https://helpx.adobe.com/photoshop/using/layer-opacity-blending.html>,
  <https://helpx.adobe.com/photoshop/desktop/create-masks.html>,
  <https://helpx.adobe.com/photoshop/desktop/create-manage-layers.html>
- Adobe, *Select and Mask* and *Refine Edge* -
  <https://www.adobe.com/learn/photoshop/web/make-precise-selections-in-select-mask>,
  <https://www.adobe.com/products/photoshop/refine-edge.html>
- Adobe, *Photoshop File Formats Specification* (PSD and PSB) -
  <https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/>
- Adobe, *Lightroom Classic: Develop module basics* and *Masking* -
  <https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/develop-module-tools.html>,
  <https://helpx.adobe.com/lightroom-classic/desktop/process-and-develop-photos/masking.html>
- GIMP, *GIMP 3.0 Release Notes* and *GIMP 3.0 Released* -
  <https://www.gimp.org/release-notes/gimp-3.0.html>,
  <https://gimp.org/news/2025/03/16/gimp-3-0-released>; *GIMP 3.2 RC1* -
  <https://gimp.org/news/2025/11/17/gimp-3-2-RC1-released/>; Libre Arts,
  *GIMP 3.0 released* - <https://librearts.org/2025/03/gimp-3-0-released/>
- Serif, *Affinity Photo 2 help: key features* -
  <https://affinity.help/photo2/en-US.lproj/pages/Introduction/keyFeatures.html>;
  iPhotography, *What is the persona system in Affinity Photo?* -
  <https://www.iphotography.com/blog/what-is-the-persona-system-in-affinity-photo/>
- Pixelmator, *Pixelmator Pro tech specs* -
  <https://pixelmator.com/pro/tech-specs>; Apple, *Pixelmator Pro* -
  <https://www.apple.com/ca/pixelmator-pro/>
- Photopea, *Photopea 1.0*, *2.1* and *3.8* release posts, and *Opening
  and saving files* - <https://blog.photopea.com/photopea-1-0.html>,
  <https://blog.photopea.com/photopea-3-8-live-shapes-and-new-filters.html>,
  <https://photopea.com/learn/opening-saving>
