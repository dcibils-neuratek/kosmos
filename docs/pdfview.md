# PDF Viewer, made again

The reader from the first builds, as a reader of today. Diego, 10 October
2026: "we need to revisit our pdf reader which hasnt been updated since the
first kosmos builds", "it needs a menu, zoom, better navigation, window
resizing, etc". `pdfview.lua` is 370 lines: a page at a time, no menu, zoom
by `+` and `-`, a window that does not resize, and pictures left blank.

Designed before it is built (`CLAUDE.md`, *How to work here*): this file is
its feature set and its architecture, `docs/pdfview.html` its mockup,
`docs/pdfview-architecture.png` its diagram, drawn from
`docs/pdfview-architecture.html`. **None of it is built until Diego has
agreed all four.**

## What it does

- **Opens a PDF** from Tracker, the Open window (`panel.lua`), or the File
  menu's recent documents.
- **The whole document as one column of pages**, scrolled by the wheel,
  the keys and the scroll bar; or **one page at a time**, for slides.
- **Zoom**: fit width (the default, following the window as it resizes),
  fit page, actual size, and steps from 50% to 400%; Command-plus and
  -minus.
- **Goes**: next and previous page, first and last, a page by number
  (Command-G, or the number in the header), and back to where it was before
  a jump.
- **A sidebar** of the pages as thumbnails, or the document's own
  **contents** when it has an outline; the page or part in view lit.
- **Find**: through the whole document, every match marked, the current
  one stronger, Return and Shift-Return between them, and the matches
  listed by page in the sidebar. A document with no readable text - a scan -
  says so.
- **Pictures in pages**, which today's reader leaves blank: JPEG first, the
  rest of a PDF's image filters after.
- **Copies selected text.**
- **Resizes**, laying the pages out again; below a width the sidebar hides
  and the header keeps the page, the zoom and Find.
- **Opens each document where it was left**: its page and its zoom.

Not in the first version: printing, which Kosmos does not do; forms and
annotations; two pages side by side.

## What it is made of

The premise (`CLAUDE.md`, *Kits, servers and drivers supply; applications
orchestrate*): what a PDF *is* - reading it, its pages, its text, its
outline, its pictures - belongs to the PDF Kit, behind its one door,
`pdf.lua` (*One kit, one door*, done 4 October). Present, Sheets, the
browser and Kosmos Write use that door too, and each thing added here is
there for them. The reader is the window: layout, scrolling, zoom, the
sidebar, the marks, the menus.

| Piece | What supplies it | New? |
|---|---|---|
| The window, its menu bar, header, sidebar, sheets | `ui.lua` (`ui.menubar`), `paint.lua`, `wm` | no |
| Opening a file; recent documents | `panel.lua`, the Open window; `recent.lua` | no |
| The document, its pages and their sizes | `pdf.lua`: `pdf.open`, `doc.pages` (each page's `MediaBox`) | no |
| A page drawn into a surface at a scale | `pdf.render` (`pdfpage.lua`), glyphs rasterised once a face a size in C | no |
| **A page's text as characters, with where each is** - for Find and Copy | `pdf.text(doc, page)` in the kit: `pdfpage.walk`'s glyphs mapped through each font's `ToUnicode` or its encoding | **new, in the kit** |
| **The outline** - the document's contents | `pdf.outline(doc)`: `/Outlines` read into titles and their pages | **new, in the kit** |
| **Pictures in a page**: an image XObject drawn at its place and size | `pdfpage.render` calling `gfx.jpeg` (`DCTDecode`), as `pictures.lua` does; `FlateDecode` images through the Compression Kit | **new, in the kit** |
| Rendered pages kept, the ones in view and either side | a cache in the reader: a surface a page at the current zoom, in regions | new, in the app |
| Thumbnails | `pdf.render` at a small scale, made when the sidebar first shows them | no |
| Where each document was left | the settings kit, `prefs.open("pdfview")` | no |
| Find's matching | Lua over each page's text, which is short: a page is a few thousand characters | no |

**What is C and what is Lua.** C: rasterising glyphs, blitting them,
decoding a JPEG, scaling a picture into place, every blit of a rendered
page to the window - all already in the Graphics Kit. Lua: reading the
document's structure (`pdf.lua`, as now), laying the pages out, the cache's
bookkeeping, matching Find's words. A page's content stream is tokenised by
the PDF Kit's scanner in C (`pdftok.c`), as now.

**What travels where.** Nothing leaves the process: the document is read
from its file into regions (`pdf.file`), pages render into surfaces, and the
window draws itself and commits damage, as every window does since 9
October. Its settings are a text file in `/Home/Preferences`.

## Its busiest path, and where the time goes

**Scrolling**, which happens sixty times a second while it lasts: the pages
in view are already rendered at the current zoom, so a frame is a blit of
the visible band of each - C, one or two calls a page, nothing interpreted
and no glyph drawn. This is today's reader's one good idea, kept: "built so
that scrolling costs nothing".

**A page arriving in view** - scrolled to, jumped to - is a render: the
content stream walked (the scanner in C, the operators in Lua, as now) and
its glyphs drawn from the cache in C. Rendered one page ahead and one
behind while the window is idle, so scrolling down a document finds the
next page ready. The cache keeps the pages in view and either side, so a
377-page book never holds 377 rendered pages.

**A zoom changed** re-renders the pages in view at the new scale and drops
the rest; until a page is done, the old rendering is drawn scaled, so the
window answers at once and sharpens a moment later.

**Find** reads each page's text once, the first time Find is used in that
document, and keeps it: a few kilobytes a page. Matching is a string search
in Lua over that - not a loop over bytes that would want C.

Where the time is expected to go: rendering a page that arrives, and the
first Find over a long document. Both measured on the M700 as it is built,
with the 377 pages of the Odyssey.

## The steps, once agreed

1. **The kit's three additions**, each held by a test of its own: a page's
   text (held to the text a known PDF says it holds), the outline, and JPEG
   pictures drawn in a page.
2. **The window**: menu bar, header, the whole document scrolled, zoom,
   resize, the cache.
3. **The sidebar**: thumbnails, then contents.
4. **Find**, Copy, Go to page, back.
5. **Where it was left**, and recent documents.
