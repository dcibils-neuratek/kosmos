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
| `pageset` (W2) | paragraphs set into lines, lines onto pages | Write | Present's slides |
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

Paragraphs are set into lines and lines onto pages: the paper, its margins,
a header and a footer, page numbers. **The setting is handed its measure**
- a function from a face, a size and a string to a width in points - so the
whole of it runs on the Mac against a fixed measure, and inside the machine
against the faces' own. A line breaks at a space or after a hyphen, a word
longer than the line is broken where it must be, `spacing_lines` and the
face's height give the line's, `before_pt` and `after_pt` stand between
paragraphs, a paragraph's last line may not stand alone at the top of a page
(a widow) when two can move together. Hyphenation, ligatures and facing
pages are the Document panel's switches and come after the rest works.

What it produces is the page as a list of placed runs - a face, a size, a
colour, an x and a baseline in points, the text - and that list is what the
window draws and what the PDF writer writes. **One setting for both** is the
point: the PDF cannot disagree with the screen about where a line broke,
because there is one place a line breaks.

## W3 - PDF out

The PDF Kit has only ever read. Writing is simpler than reading - the writer
chooses every object and never has to forgive anything - and it is the part
that matters most, so it is held hardest:

- a page per W2 page, `MediaBox` from the paper in points;
- each face used embedded as a TrueType font program (`FontFile2`), with its
  widths, so the PDF looks the same on a machine that has never heard of IBM
  Plex; the text written in WinAnsi, with what WinAnsi cannot say through a
  `ToUnicode` map so it can be searched and copied;
- each line one `Tj` at the baseline W2 set;
- the streams deflated by the Compression Kit, and the bytes made in a
  region and written from it, never through the interpreter.

**Held by reading it back** with Kosmos's own reader, `pdf.lua` and
`pdfpage.lua`: the number of pages, their size, and each line's text and
place against what W2 set. On the Mac first, then the PDF viewer opening it
inside the machine.

## Not here yet

Pictures, captions and tables are W5, DOCX out is W6, and shapes, charts and
comments come after the core (Diego's second answer). Opening a DOCX is
later than writing one.
