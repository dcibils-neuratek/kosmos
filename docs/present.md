<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos Present, before it is built

Written on 5 October 2026, before any of it is built, as `write.md` was.
Diego, 4 October: "our Kosmos Present presentation software like apple
keynote", and the day before, of Write's kits: "We will reuse most of this
technology for our Kosmos Present presentation software ... and Kosmos
Sheets". This is the first application to be designed under the rule of 5
October (`CLAUDE.md`, *An app is designed before it is written*; `design.md`
9.7), so it is the four things that rule asks for:

- **what a person can do** with it, below;
- **the mockup**, `docs/present.html` - the editing window, the slide shown
  full screen, and the presenter's view;
- **the architecture**, the rest of this page: every piece and what supplies
  it, what crosses in a region and what in a message, what is C and what
  is Lua, and the busiest paths with where their time goes;
- **the diagram**, `docs/present-architecture.png`, drawn from
  `docs/present-architecture.html`.

Nothing here is built, and nothing here is agreed until Diego says so. The
questions that are his are collected at the end.

---

## What it does

Keynote's main features, taken one at a time, with what Present does with
each. **The first version** is what the mockup draws; **later** is said
beside it so the first version is not mistaken for all of it.

### In the first version

- **A deck from a theme.** A new presentation starts from a theme - two to
  begin with, *Night* (the desktop's own dark blue) and *Paper* (light) -
  and a theme is its styles, its backgrounds and its **layouts**: Title,
  Title & Subtitle, Title & Bullets, Section, Statement, Big Fact, Photo,
  Blank. A layout is the master a slide is made from: where its title, its
  body and its picture stand, in which style.
- **Slides down the side**: the navigator, each slide small with its number;
  a slide added after the one chosen (Add Slide, with its layout), duplicated,
  deleted, dragged to a new place, or skipped - shown, kept, and passed over
  when presenting.
- **A slide on the desk**: the chosen slide large on the dark desk, as
  Write's page is, fitted to the window or at a zoom; under it the **notes
  for the person speaking**, typed there.
- **Things on a slide, placed where they are put**: a slide's own title and
  body from its layout, text boxes, pictures, shapes, tables and charts -
  **Write's six**, each the same thing it is in Write - chosen with a press,
  moved by dragging, sized by their handles, snapped to the slide's middle
  and edges and to each other, brought forward and sent back.
- **Text as Write sets it**: styles (Title, Subtitle, Body, Bullets,
  Caption, Quote, Big Fact, Notes), and on top of a style a face, weight,
  size, bold, italic, underline, strike-through, colour, the four
  alignments, line spacing, bullets and numbers - and, because a box has a
  height a page's column does not, **top, middle or bottom** in the box.
  Typing, selection, copy and paste, undo and redo exactly as in Write.
- **The Format panel at the right, in four parts as Keynote has them**:
  *Slide* (its layout, its background, whether it shows its title, body and
  number, skipped), *Text* (Write's), *Arrange* (where a thing is and how
  large, in points; front and back; aligned to the slide), *Animate* (the
  transition into the slide). And *Document*, as Write's: the slide's shape
  - 16:9 or 4:3 - the theme, and whether the show loops.
- **Presenting**: Play shows the deck full screen from the chosen slide;
  a press, a key or the arrows move on and back; Escape ends. **The
  presenter's view** - the slide being shown, the next one, the notes large
  enough to read standing, the time of day and the time since the start, the
  slide's number, and a bar that is green when the next slide is ready -
  on the same screen, swapped with the audience's view by a key, until
  Kosmos drives two screens (below).
- **Transitions between slides**: Dissolve, Push, Wipe, Move In, Reveal and
  Fade Through Colour, each with a direction where it has one and a length
  in seconds; started by a press or after a time.
- **Files**: `.present`, its own - a zip, as `.write` is; **PDF out**, a page
  a slide, the faces embedded, from the same setting the screen shows; and
  **PowerPoint's PPTX out**, so a deck goes to somebody without Kosmos and
  stays a deck they can edit.

### Later, each its own step

- **Builds**: a slide's things arriving one at a time - bullets one by one,
  a picture dissolving in - and leaving. Keynote's Build In, Action and
  Build Out; the Animate part shows where they will be, greyed.
- **Magic Move and the transitions that turn or scale** - Cube, Flip, Zoom:
  they need the scaled blit fast (below, *Transitions*).
- **A second screen**: the presenter's view on the laptop and the slide on
  the projector. What it needs is beneath Present, not in it (below).
- **Editing the layouts themselves**, a theme of one's own, a picture as a
  slide's background stretched or tiled.
- **A film or a sound on a slide**, through the Media Kit's `video.lua` and
  `/Devices/audio`.
- **Things turned** at an angle; **groups**; **locked** things.
- **The light table** (every slide in a grid) and **the outline** (the
  deck as its titles and bullets).
- **Rehearsing** with the time each slide took, a **self-playing** deck.
- **Opening PPTX**, which is the larger half, as opening DOCX is for Write.
- **A movie or pictures out.**

---

## The architecture

### At a glance

Each piece of the application and what supplies it. *Kept* is a kit used as
it is; *changed* is a kit that gains a thing; *moved* is code that is in
`writer.lua` or `docxwrite.lua` today and becomes a kit both applications
use; *new* is written for Present and, where another application could
want it, written as a kit from the start.

| Piece | What supplies it | |
|---|---|---|
| Text: paragraphs, runs, styles, editing | `richtext.lua` | kept |
| A text box's lines | `pageset.lua` - **`pageset.frame`** | changed |
| Faces and their measure | `faces.lua`, `gfx.typefaces`, `gfx.typeface` (`face.c`) | kept |
| Hyphenation | `hyphen.lua` | kept |
| A slide drawn | `pagedraw.lua` - frames, pictures scaled once | changed |
| Pictures, shapes, tables | Write's: `richtext.new_shape`, `new_table`, `new_box`, `pageset.shape_art` | kept |
| Charts | **`chart.lua`** - the plan, the frame and Office's part, out of `pageset.chart_plan` and `docxwrite.chart_part` (agreed with Sheets) | moved |
| The file | `docfile.lua` over `zip.lua` - a picture read into a region | changed |
| The deck as data | **`presentdoc.lua`** | new |
| Things placed on a slide | **`canvas.lua`** | new |
| The caret, selection, typing, undo | **`textedit.lua`**, out of `writer.lua` | moved |
| The Format panel's parts | **`inspector.lua`**, out of `writer.lua` | moved |
| Pages on a desk, and their thumbnails | **`pagedesk.lua`**, out of `writer.lua` | moved |
| The tools row | `pixelkit.lua` - **`pk.toolbar`**, out of `writer.lua` | moved |
| PDF out | `pdf.lua` (`pdf.write`) over `pdfwrite.lua` - frames | changed |
| Office's package, DrawingML, charts | **`ooxml.lua`**, out of `docxwrite.lua` | moved |
| PPTX out | **`pptxwrite.lua`** | new |
| Transitions | **`transition.lua`** over `gfx` `blit`, `blend`, `fill` | new |
| The window, full screen | `ui.lua` (`direct`, `header`, `fullscreen`), `wm` | kept |
| Pixels | the gfx Kit (`gfx.c`, `docfont.c`, `face.c`, `png.c`, `jpeg.c`) | kept |
| Deflate, inflate | the Compression Kit | kept |

**There is no new C in the first version.** Every loop over pixels, glyphs
and bytes Present needs is one the gfx Kit and the Compression Kit already
have. Where a measurement might later ask for one - a cross-fade in one
pass, a scaled blit in vector lanes - it is said below, with the number
that would decide it.

### Write's kits, used as they are

What Present is made of is mostly what Write is made of, and that was the
point of building Write's pieces as kits (`write.md`, *Why the kits come
first*):

- **`richtext.lua`** is every word on every slide: a box's body is a list of
  paragraphs exactly as Write's body is, checked by `richtext.paragraph`,
  edited by `richtext.type`, `split`, `delete`, `format`, `arrange`,
  `restyle`, and read at a place by `richtext.look_at`. Bullets are
  `layout.list = "bullet"`. Nothing in it is about a page, so nothing in it
  changes.
- **`faces.lua`** picks a look's face and measures it in points, and
  **`hyphen.lua`** breaks words, as for Write.
- **Pictures, shapes, tables and charts** are Write's paragraphs - a
  picture paragraph, `richtext.new_shape`, `new_table`, `new_box`,
  `new_chart` - set by `pageset` (`pageset.shape_art`, and a chart's plan
  through **`chart.lua`**, below) and drawn by `pagedraw` as `art`, which
  the PDF writer already writes as paths. A chart on a slide is a table
  whose numbers are drawn, with Edit Data, because that is what a chart is
  in Write.
- **`docfile.lua`** is the file: a zip with the document as `tabletext` and
  its pictures under `pictures/`. Its own header already names `.present`
  as its second user.
- **`pdf.lua`** is the one door to PDF, and `pdf.write` is how Present
  exports, as Write does.
- **`pixelkit.lua`** draws every control a direct window has - `pk.tool`,
  `pk.segments`, `pk.chooser`, `pk.stepper`, `pk.check`, `pk.swatch`,
  `pk.menu`, `pk.header` - and **`panel.lua`** is the Open window that
  Media opens at Pictures.

### What moves out of Kosmos Write

`writer.lua` is nearly 2,800 lines, and about half of it is not about Write. It is
the first place a second copy would come from: Present needs the same
caret, the same Format panel and the same desk, and copying them into
`present.lua` is exactly the defect the premise was written the day of. So
**before Present has a line of its own, four pieces leave `writer.lua`**,
Write is moved onto them, and Write's suites are what prove nothing changed
(step P0 below). The door of each is named here so the move can be checked
against it.

**`textedit.lua` - a body of text with a caret in it.** What a key does to
text is not Write's: it is the same in a slide's box and in a spreadsheet's
cell. Out of `writer.lua` come `edited` and `swap` (an undo list in which a
run of typing is one step, `UNDO_MOST` of them), `move` and `clamp` (a
caret and an anchor, a selection kept inside its cell), `type_text`,
`line_break`, `back_or_forward`, `apply_char`, `apply_para`, `apply_style`,
`selected`, `range` and `shown_look`, the text half of `sink:key` - the
arrows, Home and End, Shift, Delete and Backspace, Return, Tab in a table,
UTF-8 put together a byte at a time - and `sink:edit`'s copy, cut and paste
through `wmproto`. The door:

```lua
local textedit = use("/Kosmos/Libraries/textedit.lua")
local ed = textedit.new{ body = body, by_name = by_name, measure = measure,
                         set = function(body) return pageset.frame(...) end,
                         keep = function() return snapshot end,      -- what an undo puts back
                         put_back = function(snapshot) ... end }
ed:key(c)              -- true when it was the text's
ed:edit(kind)          -- "copy", "cut", "paste", "selectall"
ed:format(fields)      -- apply_char;   ed:arrange(fields), ed:restyle(name)
ed:caret()             -- pageset.locate's answer: x, baseline, height, in points
ed:marks()             -- the selection's rectangles, for pagedraw
ed:undo(), ed:redo()
```

The application keeps what an undo puts back beyond the text - Write's
paper and margins, Present's slide - through `keep` and `put_back`, which
is what `snapshot` and `swap` already do in Write with Write's fields
spelled out.

**`inspector.lua` - the Format panel's parts.** `draw_panel` is 540 lines,
and nearly all of it maps a look or an object to `pixelkit` controls and a
change: the Text part's Style, Layout and More; a picture's width; a
shape's kind, size and fill; a text box's width, fill and border; a table's
rows, columns and header; a chart's kind, height and data. Out with it come
what it stands on - `control`, `say_where` (the log line a harness presses
by), `open_menu` and the menu it opens, `weight_names`, `colour_name` - and
the lists a person picks from: `COLOURS`, `FILLS`, `SIZES`, `SPACINGS`,
`SHAPE_KINDS`, `CHART_KINDS`. **The Document part stays in Write**: paper,
margins and a header are Write's. The door:

```lua
local inspector = use("/Kosmos/Libraries/inspector.lua")
local ins = inspector.new(pk, { name = "present" })   -- "present: control size at ..."
ins:begin(s, x, y, w)                                   -- a frame's controls start again
ins:text(look, layout, style, styles, catalogue, part, on)   -- on.char(fields), on.para, on.style
ins:shape(shape, on)   ins:picture(picture, on)   ins:table(t, on)   ins:chart(t, open, on)
ins:control(key, box, act)                              -- an application's own control
ins:press(x, y)        ins:key(c)                       -- a control pressed; a menu's keys
ins:draw_menu(s)
```

Present adds its own parts through `ins:control` - Slide, Arrange, Animate,
its Document - and those stay in `present.lua` until a second application
wants one. Arrange is the likeliest: Sheets places things too (*Shared with
Kosmos Sheets*).

**`pagedesk.lua` - pages on a desk.** `page_at`, `page_px`, `desk_height`,
`page_surface` with its cache of each page drawn by version and zoom,
`forget_far`, the desk part of `frame` (each page blitted with its shadow,
the parts scrolled off cut), `scroll_to`, `follow`, `zoom_to`, and
`draw_thumbs` with its own cache. A page here is anything with a size in
points and a way to be drawn - `pagedraw` for Write's pages and Present's
slides, `pdf.render` for a PDF's - so the PDF viewer is the third user to
be read for whether this serves it as it is, as `regions.lua`'s users were.
The door: `pagedesk.new{ pages = n, size = function(i) end, draw =
function(i, surface, scale) end }` and `desk:draw(s, x, y, w, h)`,
`desk:point(x, y)` (a page and a place in points), `desk:follow(page, x_pt,
y_pt)`, `desk:zoom(z)` with "fit" among its zooms, `desk:thumbs(s, x, y, w,
h, chosen)`, and `desk:changed(i)` when a page's version moves.

**`pk.toolbar` - the row of tools.** `draw_tools` lays out a row of
`pk.tool`s from the left, a gap, the rest pushed to the right, and says
where each one is; `frame` puts the note between the two groups, and
`sink:mouse` finds the tool under a press. That is `pixelkit`'s next
control rather than a kit of its own: `pk.toolbar(s, x, y, w, tools, note)`
and `pk.tool_at(tools, x, y)`.

**Not moved**: `doc_edit` and the Document panel, the header's field,
`save`, `export`, `export_docx`, Write's `TOOL_ACTS` - each is about a page
that prints.

### A slide is a page of frames

The honest difference between Write and Present is this: **in Write
everything is set in the text**, and W7 kept it so on purpose - a text box,
a shape and a chart are paragraphs in the flow, "nothing floats" (`write.md`
W7). **On a slide everything floats.** A title stands where its layout puts
it, a picture where it was dragged, and nothing flows round anything.

The reconciliation is not to make `pageset` a second layout engine. It is
that **every thing on a slide is a small Write document**: a body of
paragraphs, set by `pageset` into a box as wide as the thing, from the box's
own top left, never broken onto a second page - a **frame** - and the slide
is a page whose content is its frames, each at its place:

- a **text box** or a layout's title or body is a frame of the paragraphs
  typed into it;
- a **picture**, a **shape**, a **table** or a **chart** is a frame of one
  paragraph - Write's picture, shape, table or chart paragraph - as wide as
  the thing, so it is set exactly as Write sets it and drawn and written
  exactly as Write draws and writes it;
- a frame's **fill and border** (a text box's) is `art` under its lines, the
  same rectangles a table's tint is;
- the slide's **background** is the paper `pagedraw` fills first, and a
  background picture is a frame at the back.

What that asks of the kits is small, and each is one thing:

- **`pageset.frame(body, styles, measure, cache, width_pt, opts)`**: the
  body set as `pageset.set` sets a page's paragraphs, `width_pt` wide, with no
  margins, no header, no footer and no foot - one page as tall as its lines.
  It is `pageset.set`'s own paragraph loop (`text_lines`, `set_table`,
  `set_chart`, the shape and picture cases), with the page taken away, so
  there is still one place a line breaks. A deck's frames share one list of
  looks (`opts.looks`), so a deck's setting is a set as `pagedraw` and
  `pdfwrite` already know one: `set.looks`, `set.pages`.
- **`page.frames`** in a set's page: `{ x_pt, y_pt, page }` each, in the
  order they are drawn. **`pagedraw`** draws each at its place - its
  `Drawer:page` already takes an origin - and **`pdfwrite`** writes each
  inside `q 1 0 0 1 x -y cm ... Q`, which is what a PDF's matrix is for.
- **The caret's functions take a frame**: `pageset.locate`, `hit`,
  `vertical`, `line_ends` and `selection` work on a frame's set unchanged,
  since a frame is a set of one page; the application adds the frame's
  origin. `pageset.lua`'s own comment already says "a slide's text box".
- **`pagedraw`'s paper may be nothing** (`false`), for a frame drawn over a
  slide that has already been filled.

What it costs, said: **a box does not grow sideways**, it wraps at its width
as a column does; a title longer than its placeholder **runs past its foot**
in the first version, marked on the desk, where Keynote shrinks the text to
fit - shrinking is a later step, a sum over the frame's lines, not a second
layout. And a frame is drawn upright: turning one needs a rotated blit the
gfx Kit does not have, which is why turned things are later.

**Units.** A slide is measured in **points**, as Keynote's inspector and a
PDF and a PPTX all measure it (12,700 EMU to the point): `x_pt`, `y_pt`,
`w_pt`, `h_pt`, and a 16:9 slide is **960 by 540 points** - PowerPoint's
widescreen, 13.33 by 7.5 inches, so a PPTX says the same size Present does.
Write's pictures, shapes and charts are in millimetres inside their
paragraphs; a frame converts at one place, `presentdoc.mm(pt)` beside
`writedoc.pt(mm)`, and nowhere else (*a number says its unit in its name*,
`write.md` W1).

### The deck as data, and its file

**`presentdoc.lua`** is Present's own document, as `writedoc.lua` is Write's:
what a deck is beyond its words, and its check. It is not shared - a slide's
layout and transition are nobody else's - but it is a library rather than a
part of `present.lua`, so the Mac can test it without a window, as
`test_writedoc.lua` tests Write's.

```lua
-- kosmos: table
{
  format = "kosmos-present", version = 1,
  size = { name = "Wide", width_pt = 960, height_pt = 540 },
  theme = "Night", loop = false,
  styles = {
    { name = "Title", face = "IBM Plex Sans", weight = "SemiBold", size_pt = 54,
      colour = "#e6eaf2", align = "left", ... },
    { name = "Bullets", face = "IBM Plex Sans", weight = "Regular", size_pt = 28,
      colour = "#c9d2e3", list = "bullet", spacing_lines = 1.15, after_pt = 10, ... },
    ...
  },
  layouts = {
    { name = "Title & Bullets", background = "#0b1630",
      places = {
        { place = "title", x_pt = 64, y_pt = 48, w_pt = 832, h_pt = 92, style = "Title" },
        { place = "body",  x_pt = 64, y_pt = 156, w_pt = 832, h_pt = 330, style = "Bullets" },
        { place = "number", x_pt = 864, y_pt = 500, w_pt = 64, h_pt = 24, style = "Caption",
          align = "right" },
      } },
    ...
  },
  slides = {
    { layout = "Title & Bullets",
      things = {
        { place = "title", body = { { style = "Title", runs = { { text = "Kits supply" } } } } },
        { place = "body",  body = { { style = "Bullets", runs = { { text = "A kit is code you run" } } },
                                    { style = "Bullets", runs = { { text = "A server is someone you ask" } } } } },
        { x_pt = 640, y_pt = 300, w_pt = 256, h_pt = 160,
          body = { { style = "Body", shape = { kind = "rounded", fill = "#2a55c9",
                                               width_mm = 90.3, height_mm = 56.4 } } } },
      },
      notes = { { style = "Notes", runs = { { text = "Diego's words of 4 October first." } } } },
      transition = { kind = "push", direction = "left", seconds = 0.8, after_s = nil },
      skip = false },
  },
}
```

- **A deck carries its layouts**, copied from the theme when it was made, as
  Keynote's does. So a deck opens the same on a machine whose themes have
  moved on, and editing a layout later edits the deck's own (*explicit over
  hidden*: what a slide looks like is in its file).
- **A layout's place is where a thing stands until it is moved.** A thing
  with `place = "title"` and no `x_pt` stands where the layout says; dragged,
  it carries its own. Changing a slide's layout moves the things that have
  not been moved.
- **Read as somebody else's file**, as a `.write` is: `presentdoc.check`
  takes the declared fields of the declared types, each number to its range
  (a thing on the slide or partly off it, never ten thousand points away), a
  transition one of the six, a body through `richtext.paragraph` - and a deck
  saved and opened again is equal to itself, which is the test.
- **`.present` is `docfile`'s zip**: `document` first, `pictures/<n>.<ext>`
  as they came. Saved whole, in one write: under `/Home` that is one
  journalled transaction, so a power cut leaves the old deck or the new one.

**What `zip.lua` gains, and Write with it.** Write reads every picture in
its file into a Lua string when it opens (`zip.read(path, name, 64 MB)` in
`writer.lua`), and holds those strings until it closes. A Write document has
a few pictures; a deck of photographs has fifty, at several megabytes each,
in a heap that starts at 2 MB - and a picture's bytes crossing the
interpreter between two C stages, inflate and decode, is what `CLAUDE.md`
says bytes must not do. Both `gfx.png` and `gfx.jpeg` already take an
address and a length as well as a string, for the wallpaper's sake. So:

- `zip.read_region(path, name, most)` - an entry inflated into a region and
  handed back as one (`regions.lua`'s `{ cap, at, size }`), never a string;
- an entry for `zip.write` that is **another archive's entry**, copied as it
  is stored - so saving a deck copies its pictures from the file it was
  opened from without inflating, decoding or holding them;
- a picture kept by the deck as **where its bytes are** - this file's entry,
  or the file it was put in from - and as the surfaces it is drawn at, never
  as bytes in Lua.

And a picture is **kept at the sizes it is shown at**: a twelve-megapixel
photograph is 48 MB decoded, so it is decoded once, scaled to the largest
size it appears at - the screen's, when presenting - and the decode let go.
That is `pagedraw`'s change: it scales a picture once per size and keeps the
copy, where today it stretches with smoothing on every page it draws. The
browser measured the same thing (`testing.md` 18.317): a band of Wikipedia
spent 4.1 of its 12.6 ms scaling three pictures again on each paint, and
none once they were kept. Write gains it too: a page with a photograph on it
is drawn again on every keystroke.

### Things placed: `canvas.lua`

What a slide does that a page does not is let things be **chosen, moved and
sized with the pointer**. That is not Present's alone - Sheets places
tables, charts and text boxes on a sheet as Numbers does, and Write's
floating boxes, when they come (`write.md`, *Not here yet*), are the same -
so it is a kit from the start:

```lua
local canvas = use("/Kosmos/Libraries/canvas.lua")
local c = canvas.new{ snap_pt = 6 }
c:things(list)               -- { x_pt, y_pt, w_pt, h_pt, locked } each, back to front
c:at(x_pt, y_pt)             -- the front-most thing under a point, and which handle
c:press(x_pt, y_pt, mods)    -- chosen; Shift adds to the choice
c:drag(x_pt, y_pt)           -- moved, or sized by a handle, held to its proportions with Shift
c:release()                  -- the change, for the application to make an edit of
c:guides()                   -- the lines it snapped to, in points: the page's middle and
                             -- edges, and other things' edges and middles
c:draw(s, scale, x, y)       -- the chosen things' outlines and eight handles, and the guides
c:order(how)                 -- "front", "forward", "backward", "back"
c:align(how, page)           -- "left", "centre", "right", "top", "middle", "bottom"
```

It knows rectangles in points and nothing about what is in them - which is
why a table on a sheet and a picture on a slide can share it. The
application turns a release into an edit, so a move is undone as a
keystroke is. Arrange's controls are drawn by `inspector.lua`'s caller from
what `canvas` says.

### PDF out

`pdf.write(path, set, measure, info)` with a set of slides: **a page a
slide**, its `MediaBox` the slide in points, each frame inside its `q ...
cm ... Q`, the faces embedded as subsets and every glyph shown in `Identity-H`
with its `ToUnicode` map, the pictures as Write's are (`pdfwrite.image`: a
JPEG as it came). Nothing about PDF is written again: the one thing
`pdfwrite` learns is a frame. **Held as W3 is held** - read back by Kosmos's
own reader drawing every glyph, by this Mac object by object, and by macOS
(`sips`) - and **the slide on the screen is held to the PDF line by line**,
as `arm-writeapp` holds Write's page.

Skipped slides are left out; a PDF with the notes under each slide - a
handout - is a later choice in the Export list.

### PPTX out, and one kit for Office's files

A PPTX is the same package a DOCX is: a zip of XML parts, each named in
`[Content_Types].xml` and joined by relationships, with `docProps/core.xml`
beside them. And much of what is inside is not Word's or PowerPoint's at
all but **DrawingML**, shared by all three of Office's formats: preset
shapes (`a:prstGeom`), fills (`a:solidFill` / `a:srgbClr`), pictures
(`pic:pic` and an `a:blip` by relationship), sizes in EMU, and **charts**,
whose part (`c:chartSpace`) is identical in a DOCX, a PPTX and an XLSX.

`docxwrite.lua` has all of that today, in Word's file - its header says
"Present's and Sheets' Office formats are the same parts and the same zip,
and borrow this file's XML helpers", and `docxwrite.chart_part` is already
exported. Borrowing from an application's writer is how a
second copy starts, so **`ooxml.lua`** takes them out, and DOCX, PPTX and
Sheets' XLSX each stand on it:

- `ooxml.package(path, { parts, core })` - the content types, the package's
  relationships, a part's relationships, the core properties, written
  through `zip.write`; the pictures' bytes copied from where they are, never
  through Lua (above);
- `ooxml.emu_mm(mm)`, `ooxml.emu_pt(pt)` - 36,000 to the millimetre, 12,700
  to the point;
- `ooxml.PRESET` - Write's six shapes as Office's presets (`rect`,
  `roundRect`, `ellipse`, `triangle`, `star5`, `rightArrow`);
- `ooxml.fill(colour)`, `ooxml.picture(rid, w_emu, h_emu)`;
- **and nothing about charts**: a chart's part goes to **`chart.lua`**,
  with its plan out of `pageset.chart_plan` and the one list of series
  colours both files repeat today - the door for what a chart is, on the
  screen, on paper and in Office's files, which Sheets grows (area,
  scatter, an axis's figures, references to cells). Settled with Sheets'
  design on 5 October (`sheets.md`, *Shared with Kosmos Present*);
- `ooxml.text_body(body, by_name)` - a richtext body as DrawingML's
  `a:txBody`: `a:p` with its alignment and bullet, `a:r` with `a:rPr` - face
  as `a:latin`, size in hundredths of a point, `b`, `i`, `u`, `strike`, a
  fill. PowerPoint's every text and a text box on an Excel sheet are the
  same element.

WordprocessingML (`w:p`, `w:r`, the section) stays in `docxwrite.lua`;
PresentationML is **`pptxwrite.lua`**: `ppt/presentation.xml` with the slide
size, a slide master and the deck's layouts as `slideLayoutN.xml`, a theme
part (a slide master must have one), each slide as
`ppt/slides/slideN.xml` - a `p:sp` with its text body for each text frame,
`p:pic` for a picture, `p:graphicFrame` for a table (`a:tbl`) and for a
chart - its notes as a notes slide, and its transition as `p:transition`:

| Present | PowerPoint |
|---|---|
| Dissolve | `p:fade` |
| Fade Through Colour | `p:fade thruBlk="1"` |
| Push | `p:push dir` |
| Wipe | `p:wipe dir` |
| Move In | `p:cover dir` |
| Reveal | `p:pull dir` |

with the length as PowerPoint 2010's `p14:dur` beside the base schema's
`spd`. **What does not map, said**: a frame's top, middle or bottom is
`a:bodyPr anchor`, and fits; the faces travel by name, as a DOCX's do. Held
as DOCX is (`test_docx.lua`): the parts read on the Mac, part by part, and
the file opened by Diego in Keynote and PowerPoint.

### Presenting

**Full screen is the window manager's already** (`wm.lua`, *Full screen:
the window is the screen*; `README.md`, September). A window asks for it as
it opens, having made its buffers the screen's size, and gets no tab, no
border, the origin, and a place in front of everything. **A second window,
not a bigger one**, as Cafesa3D does it (`cafesa3d.lua`, `FULL.toggle`):
Play reads `/Devices/screen`, opens `ui.window{ direct = true, fullscreen =
true, w = width, h = height }` in the same process, and **runs it as the
Open window runs** - a loop inside the editing window's loop
(`panel.lua`, as `writer.lua`'s Media uses it). Escape closes it and the
editing window goes on where it was, with the deck never having left the
process. The editing window is not drawn and not asked anything while the
show runs.

**The slide is drawn ahead.** When a slide is shown, the next one is drawn
at the screen's size into a surface of its own - its frames set, its
glyphs rasterised at that size, its pictures scaled - so a press starts a
transition at once and nothing in the transition sets, rasterises or
scales. The previous one is kept, for going back. The presenter's bar is
green while the next slide is ready and red while it is being drawn: Keynote's
"ready to advance", and here it is also the truth about the one place the
show can make a person wait (*The busiest paths*, below).

**The presenter's view** is a second drawing, not a second program: the
slide being shown, the next one smaller, the notes in a large face, the
time of day, the time since the first slide, and *4 of 8*. One key - P -
swaps the screen between the audience's slide and the presenter's view. Its
clock is drawn once a second, and only its rectangle is committed; the time
zone is read once, when the show starts, not on every tick (the review
before 0.11 found `clock.now` reading the settings file inside paints).

**Keys**: Space, Return, the right and down arrows, Page Down and a press:
on. The left and up arrows, Page Up, Backspace: back. Home and End: the
first and last. A number and Return: that slide. B: the screen black and
back. P: the presenter's view. Escape: the end.

**A second screen, later, and what it needs.** The presenter's view on the
laptop and the slide on the projector is how a talk is given. Present's
side of it is already the shape it needs to be - two drawings, either of
which can be its own full-screen window - and what is missing is beneath
it: the board scanning out a second buffer (virtio-gpu has scanouts beyond
the first; `hal_fb_init` hands over one), the window manager composing two
screens rather than one `W` by `H`, `/Devices/screen` listing both, and a
window asking to be full screen on the second. That is a roadmap item of
its own and **its first user is this**. Until then the projector shows what
the one screen shows.

### Transitions

**Two slides drawn, and a moment between them composed in C each frame.**
Both slides are already surfaces at the screen's size when a transition
starts (above). Each frame, **`transition.lua`** works out from the counter
how far through the transition it is - `sys.ticks()` against `counter_hz`
from `/Devices/cpu`, so a one-second dissolve takes one second on a slow
machine and shows fewer frames, rather than taking longer - and asks the gfx
Kit for at most three calls into the window's buffer:

| Transition | Each frame | Pixels touched |
|---|---|---|
| Dissolve | `blit` the old slide, `blend` the new one over it at the moment's alpha | the screen twice |
| Fade Through Colour | `fill`, then `blend` the old or the new at its alpha | the screen twice |
| Push | `blit` the old moved out, `blit` the new moved in | the screen once |
| Wipe | `blit` the old where the new has not reached, `blit` the new where it has | the screen once |
| Move In | `blit` the old, `blit` the new over it at its place | up to twice |
| Reveal | `blit` the new, `blit` the old over it at its place | up to twice |

then commits. Lua decides **positions and an alpha**, a dozen sums a frame;
it never computes an address, and every call writes a million pixels or
two - three orders past the two thousand at which a crossing pays for
itself (`gfx.md` 19.11). **A slide that does not fill the screen** - a 16:9
deck on Diego's 3440 by 1440, a 4:3 deck anywhere - has its bars filled
black once in both buffers and is never drawn again: only the slide's
rectangle is composed and committed.

**`transition.lua` is a kit** because it knows two surfaces, a rectangle, a
kind, a direction and a moment, and nothing about slides: the Photo
Viewer's slideshow, or a film's chapter change, would use it as it is. It
is Lua, because what it holds is choreography; the pixels are the gfx Kit's.

**What the first version leaves out, and why.** Cube, Flip, Zoom and Magic
Move scale or turn a picture every frame. The gfx Kit's `stretch` scales a
pixel at a time - its vector path is only the unscaled row (`testing.md`
18.317) - and has never been measured scaling a whole screen; nothing turns.
They come when a measurement of `stretch` at 1920 by 1080 says they fit, or
after the scaled path is moved into lanes.

**One primitive that is not proposed.** A dissolve is two passes - a copy,
then a blend - where one pass reading both slides and writing the screen
would do. That would be `dst:mix(a, b, alpha)`, a new gfx call. It is not
proposed, because the two passes fit in the frame with room (below) and
`CLAUDE.md` asks for a profile before a loop is moved; if the M700's number
says otherwise, it is a dozen lines beside `blend_row`, and the window
manager's fades would use it too.

### What crosses in a region, and what in a message

**In regions:**

- **the editing window's pixels and the presenting window's**: two surfaces
  each in one region the window manager can see (`ui.lua`'s
  `direct_region`, made by `regions.make`), handed over as a capability when
  the window opens. A transition's sixty frames a second never leave it;
- **the file being saved**: the archive made whole in regions by `zip.write`
  and written from them in one request; the PDF made in a region by
  `pdfwrite` and written from it; the PPTX through `zip.write`;
- **pictures**, inflated from the deck into a region and decoded from it
  (above), and copied into a saved deck as they are stored;
- **fonts**, which are never copied at all: `face:program()` is the
  address and length of the image's own read-only copy, rasterised by
  `gfx.docfont` and subset into a PDF from there.

**In messages:**

- to **`/Running/wm`**: `open` (with the region's capability), `commit` -
  which buffer is now live and the rectangle that changed - `poll` and its
  events (a key, a press, a look changed), `lights` for the three in the
  title band, and the clipboard's text through `wmproto.copy` and `paste`;
- to **`/Home`** (`diskfs`): a file's attributes, a write from a region;
- to **`/Devices`**: the screen's size and the counter's rate, read once.

**Nothing that recurs because a clock says so crosses as a message.** A
frame is pixels in the region and a commit that names the buffer, which is
the rule `cube3d` has held sixty frames a second by since the window
manager was written.

### What is C and what is Lua

- **Lua**: `present.lua`, which orchestrates; `presentdoc.lua`, a document's
  fields and checks; `canvas.lua`, rectangles and handles; `textedit.lua`,
  `inspector.lua`, `pagedesk.lua`, which decide what a key or a press means
  and what is drawn where; `transition.lua`, positions and an alpha;
  `pptxwrite.lua` and `ooxml.lua`, structure - a few dozen bytes of XML an
  element and a decision about each, as `docxwrite.lua` is; and the changes
  to `pageset`, `pagedraw`, `pdfwrite` and `zip`, which stay Lua.
- **C, all of it already written**: every pixel (`fill`, `blit`, `blend`,
  `stretch`, `triangle`, `fill_round` in `gfx.c`, the blend in vector
  lanes), every glyph (`docfont.c` rasterising, `face.c` measuring,
  placing, mapping to glyph numbers and subsetting), every picture decoded
  (`png.c`, `jpeg.c`), every byte deflated and inflated (the Compression
  Kit).
- **Why that line holds here**: the busiest Lua on either path below is a
  handful of calls and sums a frame, and the question `CLAUDE.md` asks of a
  Lua process on a deadline - what does it allocate? - is answered by a
  table for the commit and its reply, which is measured in step P5 rather
  than assumed.

### The busiest paths

Two paths decide whether Present feels like Keynote: **a transition while
presenting**, and **typing in a box while editing**. Numbers are the ones
the repository has; where it has none, that is said, and the step that
measures it is named. QEMU's are not performance numbers and are given only
where they say what QEMU will show.

**A transition, one frame, at 1920 by 1080 - 2,073,600 pixels.** A dissolve,
the most expensive of the six:

| | Where | Cost |
|---|---|---|
| 1 | The moment: the counter read, an alpha and positions worked out | a few microseconds of Lua |
| 2 | `blit` the old slide into the window's buffer | 0.49 ms at the M700's 4,250 Mpx/s (`testing.md` 18.354) |
| 3 | `blend` the new slide over it at that alpha | 1.3 to 1.6 ms: with an alpha under 255 every pixel takes the arithmetic, 1,293 to 1,546 Mpx/s on this Mac's cores (`gfx.md` 19.14); **not measured on the M700** |
| 4 | `commit` to the window manager, answered | 0.31 ms (`gfx.md` 19.4) |
| 5 | The window manager copies the window into its frame - a full-screen window is a `blit`, not a blend (`wm/drawwindow.lua`) | 0.49 ms, the M700's blit again |
| 6 | The frame copied to the screen and flushed | **not measured on real hardware** - the M700's copy into its framebuffer is step 1 of `m700-2d.md` |

**About three milliseconds of a 16.7 ms frame, before step 6** on a
machine like the M700; Push, Wipe, Move In and Reveal are about half of it,
since they touch the screen once. At 3440 by 1440 the 16:9 slide is 2560 by
1440 - 3.7 million pixels, 1.8 times as many - and about five;
at 3840 by 2160, four times as many, and about eleven, which still fits and
is the case to measure first. What is not on the path, by construction:
setting, rasterising, scaling a picture, the disk, a message carrying
pixels, and a collection's worth of garbage.

**Under QEMU** the window manager composes at 42 ns a pixel (`CLAUDE.md`,
the window manager measured), so step 5 alone is 87 ms and a transition
shows about ten frames a second. That is TCG, and it is what the suite will
see; it is not what the ThinkPad or the M700 will.

**The one wait on this path is a slide not ready**: a slide whose next one
is still being drawn when the key comes. Drawing a slide at the screen's
size is a fill of the background (0.4 ms at the M700's fill rate), its glyphs
- rasterised once a size and kept by `pagedraw`'s rasteriser - and its
pictures scaled once at that size: **the scaling is not measured**, and a
full-screen photograph through `stretch`'s smoothed path, a pixel at a time,
is likely the longest single thing Present does. It happens once a slide is
shown, never inside a transition, and the bar says when it is not done.
When Lua threads arrive (`threads.md` step 8), drawing the next slide is
the first work to give one.

**Typing in a box on a slide.** A key, from the window manager's event to
the commit:

| | Where | What is known |
|---|---|---|
| 1 | `poll` answers with the key | the round trip is 0.31 ms (`gfx.md` 19.4) |
| 2 | `textedit`: `richtext.type` makes a new body - the box's few paragraphs, the untouched ones the same tables | unmeasured; a box is a handful of paragraphs |
| 3 | `pageset.frame`: the changed paragraph set again, the others from the cache | a hundred pages set in 89 ms of Lua on this Mac (`testing.md` 18.378), so a paragraph is far under a millisecond; 120 pages in 1,288 ms under TCG |
| 4 | The slide drawn again on the desk: the background (about a third of a million pixels at the desk's size), pictures blitted from their kept copies, shapes as triangles, each frame's glyphs - one call into C per face, size and colour | unmeasured |
| 5 | The slide blitted into the window, the caret drawn over it, the navigator's thumbnail of this slide drawn again on a tick rather than on every key | unmeasured |
| 6 | `commit` with the rectangle that changed - the slide, and the panel only when the look at the caret changed | 0.31 ms |

**Nothing on this path has ever been measured in Write either.** Write
draws the whole window on every key - its tools, its panel, its
thumbnails, the page from nothing - and commits all of it. Groove's lesson
is the one that applies (`testing.md` 18.266): drawing only what changed
took it from 72 ms a frame to 10.1. So **step P4 starts by measuring a
keystroke** in Write as it is, from event to commit, split by stage as
the window manager's `frames` splits its pass and with what each stage
allocates; and the design's choice - draw the slide, the caret and the
part of the panel that changed, and commit only that rectangle - is held
to that number rather than to this paragraph. The window's two buffers mean
a part drawn into one is stale in the other, so drawing only what changed
keeps a damaged rectangle for each buffer, as Groove does.

### Shared with Kosmos Sheets

Kosmos Sheets is being designed beside this (`docs/sheets.*`). Where the two
would share a kit, it is named here so the two designs can be put side by
side and given one name before either is built:

| Kit | What it is | Present's use | Sheets' likely use |
|---|---|---|---|
| `docfile.lua` (exists) | a document's zip: `document` and `pictures/` | `.present` | `.sheets` |
| `<app>doc.lua` (the pattern) | an application's document as data and its check | `presentdoc.lua` | its own, as `writedoc.lua` is Write's |
| `ooxml.lua` | Office's package, DrawingML, a text body - nothing about charts | under `pptxwrite.lua` | under `xlsx.lua`, with `docxwrite.lua` |
| `inspector.lua` | the Format panel's parts | Text, shape, picture, table, chart | a cell's text, a chart, a shape |
| `textedit.lua` | a body of text with a caret | a box's text, the notes | a text box's words; a cell is edited with `textbuf.lua`, one line of plain text |
| `canvas.lua` | things placed, chosen, moved, sized | a slide's things | a sheet's tables, charts and boxes, as Numbers places them |
| `pageset.frame`, `page.frames` | a body set in a box; a page of placed boxes | every thing on a slide | a text box on a sheet; a sheet as printed |
| `pagedesk.lua` | pages on a desk, and their thumbnails | the slide and the navigator | Print Preview's pages |
| `pk.toolbar` | the row of tools | the tools | the tools |
| `chart.lua` | a chart's plan, its frame and Office's part, one list of colours - out of `pageset` and `docxwrite` | a chart on a slide | a chart drawn from cells, with area and scatter |

Reconciled with Sheets' design on 5 October: the names above are both
documents' now. Sheets adds `pk.tabs`, a scale on a frame, a zip entry
written from a region, the XML Kit and the Cells Kit (`sheets.md`).

### What is new code, and the order to build it in

New code, as a list:

- `user/bin/apps/present.lua` - the application (`wm present`;
  `-- kosmos: icon File_Image_1`, `-- kosmos: opens present`; `.present` a
  "Kosmos Present presentation" in Documents, in `filetypes.lua`);
- `user/lib/presentdoc.lua`, `canvas.lua`, `transition.lua`,
  `pptxwrite.lua` - new;
- `user/lib/textedit.lua`, `inspector.lua`, `pagedesk.lua`, `ooxml.lua`,
  `chart.lua`, and `pk.toolbar` in `pixelkit.lua` - moved, out of
  `writer.lua`, `pageset.lua` and `docxwrite.lua`;
- `pageset.frame` and shared looks; `page.frames` in `pagedraw.lua` and
  `pdfwrite.lua`; pictures scaled once in `pagedraw.lua`;
  `zip.read_region` and an entry copied from another archive in `zip.lua`;
- `tools/test_presentdoc.lua`, `test_canvas.lua`, `test_transition.lua`,
  `test_pptx.lua` on the Mac; `tools/run_present.py` in the machine.

Each step its own revision and its own test, as Write's were:

- **P0 - Write's pieces out, and Write on them.** `ooxml.lua` out of
  `docxwrite.lua` first, since it is the smallest: `test_docx.lua` and
  `arm-interchange` unchanged. Then `pk.toolbar`, `pagedesk.lua`,
  `textedit.lua` and `inspector.lua`, Write moved onto each in turn, and
  **Write's suites passing unchanged are the proof** - `arm-writeapp` presses
  every control by the name the log gives it, so a control that moved
  without its name fails there. The whole gate, since `pixelkit` is shared.
  No Present yet.
- **P1 - the deck as data, and its file.** `presentdoc.lua`: `new` from a
  theme, `check`, `save`, `open` over `docfile`; the two themes and their
  layouts. `zip.read_region` and the copied entry, Write's pictures moved
  onto them. On the Mac: a deck out and back equal to itself; every refused
  shape refused with its reason; a deck of fifty pictures saved without one
  of them inflated.
- **P2 - a slide set and drawn.** `pageset.frame`, shared looks,
  `page.frames` in `pagedraw`, pictures kept at their sizes. On the Mac:
  a frame breaks lines exactly as the same paragraph in a column of that
  width does; the caret's functions on a frame. In the machine: a slide
  drawn.
- **P3 - PDF out.** `page.frames` in `pdfwrite`. Read back three ways as W3,
  and the slide on the screen held to the PDF line by line, as W4a.
- **P4 - the window.** `present.lua` as drawn: the title band, the tools,
  the navigator, the slide on the desk with its notes, `canvas.lua` choosing,
  moving and sizing, typing through `textedit`, the Format panel's Slide,
  Text and Arrange, Document, Add Slide and its layouts, reorder, skip,
  undo. **First, a keystroke measured in Write**, by stage and by what each
  stage allocates, and Present's held to it as each part arrives.
- **P5 - presenting.** Play, the full-screen window, the next slide drawn
  ahead, the keys, the presenter's view with its clock, its timer, its
  notes and its bar; `transition.lua` and the Animate part. **A transition
  frame measured on the M700 and the ThinkPad**, every stage of the table
  above, at the screen's own size; under QEMU, `-icount`'s count kept as the
  regression number.
- **P6 - the six things on a slide.** Table, Chart, Text, Shape, Media and
  Comment from the tools, each Write's paragraph in a frame, its part of the
  panel from `inspector.lua`.
- **P7 - PPTX out.** `pptxwrite.lua` on `ooxml.lua`; read on the Mac part
  by part; opened in Keynote and PowerPoint by Diego.
- **Then, each when its time comes**: builds, the light table, text shrunk
  to its box, Magic Move and the scaled transitions after `stretch` is
  measured, the second screen after the window manager has two.

---

## What is Diego's to decide

1. **The kits out of Write first (P0).** `textedit.lua`, `inspector.lua`,
   `pagedesk.lua` and `pk.toolbar` moved out of `writer.lua`, and
   `ooxml.lua` out of `docxwrite.lua`, with Write moved onto them before
   Present is begun. *Recommended*: yes - it is the premise applied before
   the second copy rather than after, and Write's suites make it checkable.
2. **What a slide is.** A page of frames, each a small Write document set by
   `pageset` - with what that costs: text wraps at a box's width, and a long
   title runs past its box until shrinking comes. *Recommended*: yes, rather
   than a second layout engine for slides.
3. **The slide's size.** 16:9 at 960 by 540 points, PowerPoint's
   widescreen, with 4:3 one choice away in Document. *Recommended*: yes.
4. **What the first version holds.** Transitions, and builds later;
   Dissolve, Push, Wipe, Move In, Reveal and Fade Through Colour first, the
   ones that turn or scale after `stretch` is measured. *Recommended*: yes.
5. **The presenter's view on one screen** - swapped with the slide by a key
   - until Kosmos drives two screens, which is its own roadmap item with
   Present as its first user. *Recommended*: yes, and the second screen
   written into the roadmap now.
6. **The file.** `.present` a zip like `.write`, through `docfile.lua`, the
   deck carrying its own layouts. *Recommended*: yes.
7. **PPTX in the first version or after it.** *Recommended*: after PDF, as
   Write's DOCX came after its PDF - P7.
8. **The two themes**, *Night* and *Paper*, and the eight layouts the
   mockup shows. *Recommended*: as drawn, and changed on the page.
9. **The kits' names**, with Kosmos Sheets': `ooxml`, `inspector`,
   `textedit`, `canvas`, `pagedesk`.
