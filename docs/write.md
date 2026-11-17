<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos Write, its kits, before they are built

Written on 4 October 2026, before any of it is built, as `diskfs.md` was.
**All of it was built by 5 October** (`roadmap.md` W1-W7). On 17 November
Diego asked for more ("i want to work on the email and office productivity
tools as they are too basic now"), and the next version is *The next
Write*, at the end. What it shares with Mail, Sheets and Present is in
`docs/office.md`, and what it stands on is drawn in
`docs/write-architecture.png`, from `docs/write-architecture.html`.
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

## W7 - text boxes, shapes, charts and comments

**Each is a thing in the text**, set in its paragraph's place as a picture
and a table are, rather than floating over the page. So the setting, the
caret, the PDF and the DOCX each met one more kind of paragraph rather
than a second layout:
- **a text box** is a table of one cell drawn as a box, with a width, a
  fill and a border;
- **a shape** is a paragraph whose line's `art` is the shape, a polygon
  fanned from a centre every point is seen from;
- **a chart** is a table whose numbers are drawn - its first row the
  series, its first column the categories - with Edit Data showing the
  table above it;
- **a comment** is a mark on the runs it is about, not a look, with its
  words in a list the document keeps, and only those its text refers to.

What that costs, and it is real: nothing floats, so text does not wrap
round a shape, and a box cannot sit beside a paragraph. Pages does both.
They are a layout of their own - a second flow round obstacles - and they
come after the core, if at all.

## Not here yet

Floating boxes and shapes with text wrapping round them; comments beside
the page; columns of their own widths; a picture cropped; kerning; opening
a DOCX, which is later than writing one.

---

## The next Write - 17 November, before it is built

### What it does today, read from the code

Read from `user/bin/apps/writer.lua` (2,797 lines) and its kits as they are
on 17 November, not from the plan above:

- **Pages as they print**: A4 or Letter, either way round, margins, a
  header of one line and a footer of page numbers, facing pages,
  hyphenation in English and Spanish, ligatures, Add Page, a page break.
- **Text by style**: seven fixed styles; on top of them a face, weight,
  size, bold, italic, underline, strike-through and colour; four
  alignments, indents, spacing, lists and a drop cap.
- **Things in the text**: pictures with captions, tables, text boxes,
  shapes, charts with Edit Data, and comments shown in the Format panel.
- **Files**: `.write` saved and opened; PDF and DOCX out.
- **Keys**: typing, the arrows, Home and End, selection by Shift and by
  dragging, copy, cut, paste, undo, redo, Control-B and Control-U, save,
  and Control-E for a PDF.

### What makes it too basic

Next to Pages and Word, and checked in the code:

- **A Word document cannot be opened.** Write sends DOCX and cannot read
  one back, so the first `.docx` that arrives by mail is a dead end.
- **No find, and no replace.** Not a key, a menu or a bar.
- **No spelling.** Nothing in the system checks a word.
- **No word count.**
- **Editing keys that every writer reaches for are missing**: Control-I
  (only B and U exist), a word at a time with Control and the arrows,
  Control-Backspace, a double press for a word and a triple press for a
  paragraph, and dragging a selection to move it.
- **Styles cannot be changed or added.** The seven are fixed, so a
  document cannot be made to look like itself except paragraph by
  paragraph. The drawing showed "Character style", and there are none.
- **No links** in the text.
- **The header and footer hold almost nothing**: one line of words, and
  page numbers. There is no "page 3 of 12", no date and no different first
  page.
- **No recent documents, no recovery, no templates.** `recentdocs.lua`
  exists for the PDF Viewer and is not used here; a crash loses whatever
  was not saved; every document starts empty.
- **Nothing for a long document**: no outline of its headings, no table
  of contents, no footnotes, no sections.

### The next version, in the order a person needs it

New is marked **new**. Each says what supplies it.

**R1 - A writer's everyday.** Waits for `office.md` O1 (the moves out of
Write) and comes with O2-O4.

- **new - Find and replace**: Control-F and Control-H, the find bar across
  the top of the desk, the matches tinted on the pages, the count, match
  case and whole words, Replace All as one step to undo. `textedit:find`
  and `pk.findbar` (`office.md`).
- **new - Spelling as it is typed**, the red wave, suggestions and Add to
  Dictionary under the right button, and the document's language in the
  Document panel, which hyphenation already reads. The Spelling Kit
  (`office.md` O3).
- **new - Word count** in the foot of the window, of the document or of
  what is chosen, words and characters. `richtext` counts it, by version,
  as the setting is cached.
- **new - The editing keys**: Control-I, Control-arrows, Control-Backspace
  and Delete, double and triple presses, and a selection dragged to a new
  place (Alt to copy). All of these belong to `textedit`, so Mail, Present
  and Sheets get them too.
- **new - Recent documents** in File (`recentdocs.lua`, as the PDF Viewer
  has it), a mark in the title when there are changes not saved, and **the
  kept copy offered after a crash** (`docfile.keep`, `office.md` O4).
- **new - New from Template**: Blank, Letter, Report, Essay, CV, and one's
  own, in the chooser (`templates.lua`, `office.md` O4).

**R2 - Documents that look like themselves.**

- **new - Styles changed and added**: Update Style from what is chosen, New
  Style, Rename, Delete. Every paragraph that wears a style changes with
  it, which is what a style is for. A style stays whole, every field
  present (W1), so a new one is a copy of the one it was made from.
- **new - Character styles** (Emphasis, Strong, Code, Link), runs whose
  look is named rather than spelled out, so changing Emphasis changes
  every emphasised word. A run gains one field, `char_style`, and its own
  fields still win over it, as they win over a paragraph's style.
- **new - Links**: a run with a `link`. Control-K makes one, a press with
  Control follows it in JetSki (`weblink.lua`), and it is kept in the PDF
  as a link and in DOCX as `w:hyperlink` (`office.md`, *PDF grows links*).
- **new - Headers and footers with words**, each with a left, a middle and a
  right; the page number, "page *n* of *m*", the date, the document's
  title; a different first page; on facing pages, the outsides mirrored.

**R3 - Word's files, opened.** Waits for the XML Kit and `ooxml`'s reading
(`office.md` O7).

- **new - Opening a DOCX**: paragraphs, runs and their looks; styles (each
  made whole, as W1 requires, from Word's chain of parents); lists and their
  numbering; tables; pictures in the text; the page's size and margins; the
  header and footer's words; comments; links. What has nowhere to go here
  is simplified and said in a note under the title: a floating picture
  placed where it is anchored, a tracked change accepted, a field other
  than the page number shown as its last result, a text box anchored to
  the page set in the text. **Held by going round**: Write to DOCX and
  back, equal to itself; then made-up documents from Word, Pages and
  LibreOffice, each opened and its text checked word for word.
- **new - Plain text and Markdown opened**, as a document of Body
  paragraphs, and of headings, lists and quotes for Markdown (`markdown.lua`
  reads exactly these); **HTML, Markdown and plain text out**
  (`richhtml.lua`, `office.md` O5).

**R4 - Long documents.**

- **new - The outline**: View's sidebar gains a second tab, the headings as
  a tree, pressed to go there and dragged to move a section, a heading with
  everything under it.
- **new - A table of contents**, a paragraph of its own kind made from the
  headings and their pages, set again as the pages move. It is a thing in
  the text, like a picture or a table (W5), so the setting, the PDF and
  DOCX each meet one more kind of paragraph and not a second layout.
- **new - Footnotes**, numbered, set at the foot of their page above the
  footer, with the page's text shortened to make room. This is the one
  item in this document that changes how `pageset` fills a page, and it is
  said here so it is decided deliberately: a page's height becomes its
  text's and its notes', found by setting both.
- **new - Sections**: a section break, after which the paper, its
  orientation, the margins and the header may change (a landscape table in
  a portrait report), and **text in columns**, two or three, balanced on a
  section's last page.

**R5 - Later, each its own step**: comments beside the page rather than in
the panel; **tracked changes**, insertions and deletions kept with who and
when, accepted and refused; pictures and shapes floating over the page
with the text running round them (W7's "a layout of its own"); a picture
cropped; columns of a table at their own widths; kerning; mail merge from
Sheets or the People Kit; equations; ODT; printing when Kosmos prints.

### What is new under it

| Piece | Supplied by | |
|---|---|---|
| The caret, keys, selection, drag, undo | **`textedit.lua`** (`office.md` O1) | **moved** |
| Find | `textedit:find`, **`pk.findbar`** | **new** |
| Spelling | **the Spelling Kit** | **new, C** |
| The wave under a word | `pagedraw` marks, as the selection's rectangles are | **grows** |
| Word count | `richtext`, by version | **grows** |
| Kept copies, previews | `docfile.keep`, `preview.png` | **grows** |
| Templates | **`templates.lua`** | **new** |
| Recent documents | `recentdocs.lua` | exists |
| Styles changed, character styles | `writedoc.lua` (its styles' list and checks), `richtext` (`char_style`) | **grows** |
| Links | `richtext` (`link`), `pdfwrite.lua` (link annotations), `docxwrite.lua` (`w:hyperlink`) | **grows** |
| Headers and footers | `writedoc.lua` (their fields), `pageset` (their pieces) | **grows** |
| DOCX in | **`docxread.lua`** over `ooxml.lua` and **the XML Kit** | **new** |
| HTML, Markdown, text out | **`richhtml.lua`** | **new** |
| Markdown in | `markdown.lua` | exists |
| The outline, contents, footnotes, sections, columns | `pageset` and `richtext`, each a kind of paragraph or a field | **grows** |

**No new C in Write itself, and none in its kits except the Spelling Kit
and the XML Kit**, which are `office.md`'s. If the O1 measurement says
setting a long document is the cost, that is where C would go, in
`pageset`'s line breaking, and only with the number in hand.

### The busiest path: a key in a long document

A key goes into `textedit`, which makes a new body sharing every paragraph
it did not touch. `pageset` sets that paragraph again, and the paragraphs
after it only as far as the page where the lines come back into step;
`pagedraw` draws the pages on the screen. R1 adds a spelling lookup of the
word just finished, in microseconds, and a word count by paragraph
version, which only counts the paragraph that changed. R4's footnotes are
the one addition that could push the setting further, since a note moving
to the next page moves text with it, and they are measured against O1's
number when they arrive.

**The first number, on the Mac, 17 November** (`tools/test_textedit.lua`,
a keystroke in the middle of 100 pages, against the host's half-an-em
measure): **1.5 ms. The edit takes 0.03 ms of it and setting the pages
again takes the rest**, with about 1 MB allocated a key, nearly all of it
by the setting. So the sentence above, that the setting stops where the
lines come back into step, is the intention and not yet the code. The
cache keeps each paragraph's lines, but every page is put together again
on every key. That is where a long document's cost is, ahead of anything
the window draws, and it is `pageset`'s to fix, not `textedit`'s. The QEMU
number, with the drawing in it, is O1's record.

### The order

1. **R1** - with `office.md` O1-O4.
2. **R2** - styles, character styles, links, headers and footers.
3. **R3** - DOCX, plain text and Markdown opened; HTML, Markdown and text
   out.
4. **R4** - the outline, contents, footnotes, sections and columns.

Drawn in `docs/write.html`, *The next Write*.

### Yours to decide, 17 November

**Decided on 17 November 2026**: every recommendation below, as
recommended - Diego, "yes to all, start with write" (`roadmap.md`, *Mail
and the office applications, less basic*).


1. **R1 before anything else in Write**, the moves out of Write first
   (`office.md`, question 1). *Recommended: yes.*
2. **Opening DOCX in R3**, after the everyday and the styles, rather than
   first. *Recommended: R3*, because a DOCX opened into a Write that cannot
   change its styles cannot be kept looking like itself. If a DOCX arriving
   by mail matters more than that today, it moves to R1.
3. **Footnotes change how a page is filled.** *Recommended: yes, in R4*,
   because a report and an essay need them and Pages and Word both have
   them.
4. **Tracked changes later (R5).** *Recommended: later*: they touch every
   edit, and a person writing alone does not need them.
5. **Floating pictures with text round them, still later**, as decided on
   4 October. *Recommended: still later.*
