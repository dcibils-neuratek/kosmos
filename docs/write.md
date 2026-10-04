# Kosmos Write, its kits, before they are built

Written on 4 October 2026, before any of it is built, as `diskfs.md` was.
`docs/write.html` is the window as drawn and agreed; this is what stands
under it. Diego, 3 and 4 October: "an Apple pages inspired word writing and
editing app", "Kosmos Write should be able to export as DOCX as well as PDF.
The native format should be .write", "pdf export is key", and "We will reuse
most of this technology for our Kosmos Present presentation software ... and
Kosmos Sheets".

## Why the kits come first

Present and Sheets are coming after Write, and what they need is most of what
Write is under its window: text in paragraphs with styles, a file that holds
a document and its pictures, pages laid out as they print, and a PDF made of
them. **What is not about words on a page is a kit from the start**, so
Present is the second user of each one rather than a copy of it. These are
Lua libraries under `user/lib/`, reached as `/Kosmos/Libraries/...`, and C
where a measurement says a loop over glyphs or bytes is the cost.

| kit | what it is | first used by | then |
| --- | --- | --- | --- |
| `richtext.lua` | paragraphs, runs and styles, and their checks | Write's body | Present's text boxes, Sheets' cells |
| `docfile.lua` | a document file: a zip of the document as text and its pictures | `.write` | `.present`, `.sheets` |
| `writedoc.lua` | Write's own document: paper, margins, header and footer, a body | Write | - |
| `pageset.lua` (W2) | paragraphs set into lines, lines onto pages | Write | Present's slides |
| `faces.lua` (W2), `gfx.typefaces` | which face a look is set in, and its measure in points | Write | both |
| PDF writer (W3) | pages out as a PDF, faces embedded | Write | both |

## W1 - the document, as data

**A number says its unit in its name.** The Document panel speaks
millimetres, PDF points (1/72 inch), DOCX twentieths of a point, and a font
size is points everywhere. Four units in one document is exactly the shape
of the bug `CLAUDE.md` records twice for clocks - a number arriving naked
and read in the wrong one - so the rule is the same: `width_mm`, `size_pt`,
never `width`. Each conversion is one function, `writedoc.pt(mm)`, and DOCX's
when it arrives. A page's measures are kept in millimetres because that is
what a person typed, and A4 is 210 by 297 there and 595.28 by 841.89 in
points.

```lua
-- kosmos: table
{
  format = "kosmos-write", version = 1,
  paper = { name = "A4", width_mm = 210, height_mm = 297, landscape = false },
  margins_mm = { top = 25, bottom = 25, left = 25, right = 25 },
  header = { on = true, from_top_mm = 9 },
  footer = { on = true, from_bottom_mm = 6, page_numbers = true },
  facing = false, hyphenation = false, ligatures = true,
  styles = {
    { name = "Title", face = "IBM Plex Sans", weight = "Bold", size_pt = 30,
      colour = "#1b2330", align = "left", spacing_lines = 1.0,
      before_pt = 0, after_pt = 12, next = "Body" },
    { name = "Body", face = "IBM Plex Serif", weight = "Regular", size_pt = 11,
      ... },
  },
  body = {
    { style = "Title", runs = { { text = "Simple Home Styling" } } },
    { style = "Body", runs = { { text = "To get started, " },
                               { text = "write over this text", italic = true },
                               { text = "." } } },
  },
}
```

- **Styles are a list**, in the order the Format panel shows them: Title,
  Subtitle, Heading 1, Heading 2, Body, Caption, Quote. A style is whole -
  every field present - so a paragraph's look is its style and its own
  changes, never a chain of parents to follow. `next` is the style Return
  gives the paragraph after it.
- **A paragraph is a style and runs.** What the Layout tab changes for one
  paragraph without changing its style - alignment, spacing, indents, a
  drop cap, a list - is a field on the paragraph, present only when it
  differs. A run is text and only the character fields that differ from the
  style: `face`, `weight`, `italic`, `size_pt`, `colour`, `underline`,
  `strike`. `"\n"` inside a run is a line broken inside its paragraph
  (Shift+Return); a paragraph never holds its own end.
- **A colour is `"#rrggbb"`**, because a document is a text file somebody may
  read, and 1778480 says nothing there (*explicit over hidden*).
- **A face is named as a person names it** - `"IBM Plex Sans"`, weight
  `"SemiBold"` - and which file that is belongs to W2, where text is measured.
  A face this machine does not have is kept as written, so a document goes
  back to a machine that has it unchanged.

**Read as somebody else's file, always.** `tabletext` decodes values and
never runs anything, which is why it is the format. What comes out is then
taken field by field into a fresh document: the declared fields of the
declared types, each number held to its range (a size from 1 to 1000
points, a margin no more than half the paper), anything else left behind,
and runs that ended up alike joined into one. **That is the same promise as
a server's declared shape, made by a reader instead of a wire**: the editor
never meets a field it did not declare. A file that is not a document at all
- no body, another format, a version newer than this reader - is refused,
with the reason in words. And because a document is made canonical on the
way in, **a document saved and opened again is equal to itself**, which is
the test.

**What it costs, said, and measured** (`test_writedoc.lua`): a hundred
pages - a thousand paragraphs of three hundred characters in three runs - are
866 KB as tables and 502 KB as text, 1,368 KB both. A process's heap starts
at 2 MB and grows 256 KB at a time (`runtime/libc/malloc.c`), so that is not
a ceiling; the test holds the number so that a document's shape growing fat
is seen. A document file's text is refused over 4 MB, about eight hundred
pages, before anything is inflated, so a file claiming a gigabyte costs
nothing to turn away.

## W1 - the file

**A `.write` file is a zip**, as Pages' own is (agreed, 4 October):

- `document` - the document above, as text, first in the archive;
- `pictures/<n>.<ext>` - each picture's bytes as they arrived (W5);
- later, `preview.png`, the first page small, for Tracker.

`zip.lua` makes archives from files on a disk and opens them into a folder.
A document file is neither: its text is made in memory and a picture is a
file somewhere else. So `docfile.lua` asks `zip.lua` for the two things it
lacks - an archive written from named entries, each either a string or a
path, and one entry read into a string with a ceiling on its size - and the
bytes stay where `zip.lua` keeps them: deflated in C, from regions.

**Saved whole, and safely.** The archive is made whole in memory and
written in one request, and under `/Home` one write is one journalled
transaction (`diskfs.c`'s `ATOMIC` around `kfs_store`): after a power cut
there is the old document or the new one, never half of each. A rename would
not do it - `/Home` refuses one over a name that exists - and does not need
to.

## W2 - pages as they print

*Built 4 October* (`testing.md` 18.378). Paragraphs are set into lines and
lines onto pages: the paper, its margins, page numbers. **The setting is
handed its measure** - `width(look, text)` in points and `line(look)`, the
ascent, descent and gap - so the whole of it runs on the Mac against a
measure a test can do sums with, and inside the machine against the faces'
own.

**The faces' own is their advance widths, unhinted, in the font's units**
(`user/kits/gfx/face.c`): `gfx.typefaces()` lists every TrueType face the image
carries as the font names itself - its family, weight class and italic bit -
and `gfx.typeface(file)` measures with it. `gfx`'s outline fonts measure at a
pixel size, rounded per glyph, because they draw on a screen; a PDF's widths
are the font's own, and a page is set once for both. `faces.lua` picks a
look's face from that catalogue - the family it names, the same slant if
there is one, the nearest weight, the heavier of two equally near - and a
family this machine lacks is set in IBM Plex Sans and said to be, so a
document from elsewhere keeps its face names for the day it goes back.

What the setting does: a line breaks at a space, and a word in two looks is
still one word; a word longer than a line is broken between characters; a
line break inside a paragraph; tabs to every half inch; the four alignments,
justified by widening the spaces as a PDF's `Tw` does; the three indents;
line spacing; the space before and after a paragraph, and none before at the
head of a page; **no paragraph leaving one line alone at the foot of a page
or the head of the next**; a paragraph kept with the next when its style
says so - the title and the headings, a new paragraph field
(`keep_with_next`, the Format panel's More) - and a chain of them moving
together; page numbers, centred, in Caption. Not yet: hyphenation,
kerning, ligatures, lists and drop caps.

What it produces is the page as a list of placed pieces - a look, an x and
a baseline in points from the page's top left, the text, and where its
bytes start in its paragraph - and that list is what the window draws and
what the PDF writer writes. **One setting for both** is the point: the PDF
cannot disagree with the screen about where a line broke, because there is
one place a line breaks.

## W3 - PDF out

*Built 4 October* (`testing.md` 18.379). The PDF Kit had only ever read.
Writing is simpler than reading - the writer chooses every object and never
has to forgive anything - and it is the part that matters most, so it is
held hardest. **`pdfwrite.lua`** writes what `pageset` set and nothing else:

- a page per page set, its `MediaBox` the paper in points;
- each face the pages use **embedded whole** as a TrueType program
  (`FontFile2`) under a **Type 0 font in `Identity-H`** - its text glyph
  numbers, two bytes each, from `face:glyphs` - so a PDF shows every
  character its face can draw rather than the two hundred of a single-byte
  encoding; `/W` the widths the setting used, the font's own advances, for
  the glyphs shown; a **`ToUnicode`** map saying which character each glyph
  is, so the text can be searched and copied; its descriptor from the
  font's own tables (`face:descriptor()`). A character the face lacks is its
  missing glyph, counted, and the count comes back for the window to say;
- each piece one show at the baseline the setting chose; a justified
  line's widened spaces as **`TJ`** steps, since `Tw` applies only to a
  single-byte space; underline and strike-out as rules at the font's own
  positions; the page number;
- the streams deflated by the Compression Kit as zlib - a PDF's
  `FlateDecode` is the deflate with two bytes before it and an Adler-32
  after, where a zip's method 8 is the deflate alone, so the kit gained
  `adler32` - and the whole made in a region and written from it: a font
  program is deflated from the image's own read-only copy straight into the
  PDF, and no font byte passes through the interpreter.

**The first draft wrote single-byte WinAnsi fonts**, and Kosmos's own reader
drew none of their glyphs: it reads Type 0 fonts, which is what a PDF of
embedded TrueType is now. The reader was right and the writer out of date,
so the writer changed - and every character a face has came with it.

**Held by reading it back three ways** (`run_write.py`): Kosmos's own reader
inside the machine - `pdf.lua` opens it and `pdfpage.render` draws every
glyph of every page, no face refused; this Mac's reading of the file, object
by object - every offset in the cross-reference table where it says, each
piece of text, read back through its face's `ToUnicode`, at the place and in
the words the setting gave it, each glyph's width and each mapping the
font's own, each program the font's own bytes; and macOS's own renderer
(`sips`), which has never heard of Kosmos.

**Each face is a subset** (W3b, `testing.md` 18.380): `face:subset` keeps
glyph 0, the glyphs shown and what a composite among them is built from,
empties every other outline - keeping every glyph's number, which Identity-H
names - and leaves behind the tables a PDF reader never reads. Whole, a face
was about 93 KB deflated and the suite's three pages in seven faces were
664 KB; as subsets they are 56 KB.

Not yet: kerning, and pictures (W5).

## W5 - pictures, captions and tables

**A picture is a paragraph** that holds it and no text: a name inside the
document's `pictures/` and the size it is shown at, in millimetres. It is
set as one line as tall as the picture, scaled to the column when it is
wider, and the `.write` file keeps its bytes as they came.

**A table is a paragraph too**, and **its cells are paragraphs of their
own**. That one decision is what makes a table cheap:
- an edit in a cell is the same edit as on a page, done on a body of one
  paragraph - the cell - and put back into a new table that shares every
  row it did not touch;
- typing, formatting, the Format panel's look and undo needed no second
  copy;
- the setting's cache sets again only the cell that changed.

A place in a cell says which cell: `{ para, at, row, col }`.

**A row is one line of the page.** So a table breaks between rows and never
inside one, the rule about not leaving one line alone applies to rows, and a
header row is repeated at the head of each page as a copy that a caret
never stands in.

A row draws its rules and a header's tint as `art`, rectangles and rules in
points under its text, and the screen and the PDF draw the same list. That
is also where a shape or a chart will draw (W7).

**A picture or a table goes whole or not at all**: a range that reaches one
takes it. Nothing ever leaves half a table, or a picture's paragraph with
text in it.

## Not here yet

Text boxes, shapes, charts and comments are W7. Opening a DOCX is later
than writing one.
