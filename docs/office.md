<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# The office kits - what Mail, Write, Sheets and Present share

Written on 17 November 2026, before any of it is built. Diego, the same
day: "i want to work on the email and office productivity tools as they
are too basic now". This document is the part of that work that belongs to
no single application: the pieces that Mail, Write, Sheets and Present each
need. The premise of 4 October decides where they go (`CLAUDE.md`, *Kits,
servers and drivers supply; applications orchestrate*). Each one is a kit
with one door, and the first application to need it builds it for all of
them.

The applications' own documents name what they take from here:
`docs/mail.md`, `docs/write.md`, `docs/sheets.md`, `docs/present.md`. What
it all stands on is drawn in `docs/office-architecture.png`, rendered from
`docs/office-architecture.html`.

Nothing here is built and nothing here is agreed until Diego says so. The
questions that are his are at the end.

---

## Why a document of its own

Three things happened, and each one is a reason.

- **Mail and Write grew up apart.** Mail's composer types into Text
  Editor's `docview`, Write types into its own code inside `writer.lua`,
  and neither has spelling, find and replace, a template or a recovered
  draft. Ask "does another application want this?" of any one of those and
  the answer is all four applications, every time.
- **Present and Sheets were designed on 5 October and never started.**
  Both begin by moving pieces out of `writer.lua` (Present's P0, Sheets'
  S0). Those moves are the same moves Mail's rich composer needs. So they
  stop being one application's first step and become everyone's first
  step, done once.
- **What is missing is the same list everywhere.** Next to Apple Mail,
  Pages, Numbers and Keynote, the gaps are spelling, find and replace,
  undo that survives a crash, templates, opening the other side's files,
  and contacts. None of them is about mail or words or cells.

---

## The kits

*Exists* is used as it is. *Moved* is code inside an application today that
becomes a kit. *Grows* is a kit that gains something. *New* is new. The
**users** column is what makes each one a kit rather than an application's
own code: two names or more, from the start.

| Kit | What it is | Users | |
|---|---|---|---|
| **`textedit.lua`** | a body of rich text with a caret in it: typing, selection, the keys, copy and paste, undo | Write, Mail's composer, Present's boxes and notes, Sheets' text boxes | **moved**, out of `writer.lua` (Present's P0) |
| **`inspector.lua`** | the Format panel's parts | Write, Present, Sheets | **moved** (P0) |
| **`pagedesk.lua`** | pages on a desk and their thumbnails | Write, Present, Sheets' Print Preview, the PDF Viewer | **moved** (P0) |
| `pk.toolbar`, **`pk.findbar`** | the tools row; **the find bar** | all four, Text Editor, the PDF Viewer | toolbar moved (P0); **find bar new** |
| **the Spelling Kit**, `/Kosmos/Kits/spell` | is this word spelled right, and what might it be | Write, Mail, Present, Sheets, Text Editor, Notes later | **new, C** |
| **`richhtml.lua`** | a `richtext` body written as HTML and as plain text | Mail's composer (sending), Write (export HTML and Markdown), the clipboard | **new, Lua** |
| **`people.lua`**, the People Kit | contacts as files with attributes, and vCard | Mail, Calendar, Sharing, Write's letters later | **new, Lua** |
| **`templates.lua`** | the templates a new document can start from, and the window that chooses one | Write, Present (its themes), Sheets, Mail's signatures and stationery later | **new, Lua** |
| **`docfile.lua`** | a document's zip | `.write`, `.present`, `.sheets` | exists; **grows** a kept copy for recovery, and a preview for Tracker |
| **`ooxml.lua`** | Office's package: parts, relationships, DrawingML | DOCX, PPTX, XLSX, out and in | **moved**, out of `docxwrite.lua` (P0) |
| **the XML Kit**, `/Kosmos/Kits/xml` | XML read, by expat | DOCX in, XLSX in, PPTX in | **moved** (Sheets' S11, brought forward) |
| **`chart.lua`** | a chart's plan, frame and Office part | Write, Present, Sheets | **moved** (agreed 5 October) |
| `canvas.lua` | things placed, chosen, moved, sized | Present, Sheets | new (designed 5 October) |
| `recentdocs.lua` | the documents opened last | the PDF Viewer and Text Editor today; **Write, Sheets, Present** | exists |
| `pdf.lua`, `pdfwrite.lua` | PDF out | all four | exists; **grows** links |

**No new server and no new system call.** Every kit here runs in its
user's own process. That is why most of them are Lua: they are policy and
structure, called when a person does something. The one that is C, the
Spelling Kit, is C because it is a loop over a dictionary's bytes, called
on every word.

---

## O1 - the moves out of Write, done once

Present's design already names the doors (`present.md`, *What moves out of
Kosmos Write*): `textedit.lua`, `inspector.lua`, `pagedesk.lua`,
`pk.toolbar`, `ooxml.lua`, then `chart.lua` (`sheets.md` S0). **They are
taken as written there.** This document changes three things about them.

1. **They are first for everybody**, not Present's step. Mail's rich
   composer needs `textedit`, Write's own next version is built on all of
   them, and Sheets and Present start on them. Whoever is built first does
   the moves, and the proof is the same: Write's suites passing
   unchanged.
2. **Mail is `textedit`'s fourth user**, and it asks one thing the other
   three do not: **a body with no pages**. Mail's text is set on a page as
   wide as the pane and as tall as the text, which `pageset` already does
   for reading a plain message (`mail.md`, *Plain text*). `textedit` is
   handed `set`, a function, so it never knew what a page was. Nothing
   in its door changes for Mail.
3. **`textedit` grows find**, below, because "where does this text occur"
   is a question about a body of text and not about a window.

**First, measure a keystroke in Write**, by stage and by what each stage
allocates, as Present's P4 already asks. A 100-page document is where the
kit's cost would show. The number is kept, and every later step is held to
it.

**Status, 17 November 2026: done, in 0.11.463**, with Write's two suites
unchanged. `textedit.lua`,
`pagedesk.lua`, `inspector.lua`, `toolbar.lua` (which puts `pk.toolbar` and
`pk.tool_at` into a `pixelkit`) and `chart.lua` are in `user/lib/`, and
`writer.lua` stands on them, 2797 lines down to 1652. Proved so far by
`tools/test_textedit.lua` on the Mac (63 checks), which also times a
keystroke in 100 pages there: 1.5 ms, 0.03 ms of it the edit and the rest
the pages set again, with about 1 MB allocated, nearly all by the setting.
Still to come: Write's two suites, and the keystroke measured in QEMU.
`chart.lua` is the door, and the kinds have moved into it. The plan and
Word's part are still in `pageset` and `docxwrite`, and the door hands
calls on to them until they move.

---

## Find and replace: `textedit:find` and `pk.findbar`

**What a person gets**: Control-F opens a bar across the top of the text,
and Control-H opens it with Replace. Matches are tinted as they are typed,
with "3 of 17" beside them. Return and Shift-Return move through them.
There are three switches: match case, whole words, and in a selection
only. Replace and Replace All are each one step to undo. Escape closes the
bar and leaves the caret on the match.

**Two halves, and they are different kinds of thing.**

- **The search is the body's.** `textedit:find(needle, opts)` returns
  places as `{ para, at, row, col }`, the same places a caret has, so a
  match in a table cell is found and shown like any other. It walks each
  paragraph's text, which `richtext` already joins. That is a Lua
  `string.find` over about 300 KB for a hundred pages: a few milliseconds,
  done once when the needle changes, never per frame. **No C**: it is not a
  loop over bytes that Lua does badly, because `string.find` is already C.
- **The bar is a control.** `pk.findbar(s, b)` draws the field, the count,
  the arrows and the switches, and `pk.findbar_at` says which part a press
  hit. A window that has its own search uses the bar and supplies the
  search: Sheets over its cells (the Cells Kit's text), Text Editor over
  `textbuf:find` (which exists), the PDF Viewer over `pdf.text` (which
  exists). One bar, four searches, and nobody writes the field again.

**What crosses**: nothing. Both halves are in the window's process.

**Status, 17 November 2026: done, in 0.11.465** (`testing.md` 18.865).
`textedit:find`, `ed:replace` and `ed:replace_all` (one step to undo) are
in `textedit.lua`; the bar is `user/lib/findbar.lua`, which puts
`pk.findbar` and `pk.findbar_at` into a `pixelkit` as `toolbar.lua` does.
Write opens it with Control-F, and with Replace by **Control-Shift-F or
Control-Shift-H** - Control-H is Backspace's byte, 8, and cannot be told
from it. It searches as it is typed and says `writer: found N`; the
matches are tinted on the pages, the first 2000 of them.
`tools/test_findreplace.lua` holds the search, the replacements and the
undo on the Mac, and `arm-writeapp` presses it in QEMU. A bar opened
again holds its words chosen, so a word typed replaces them.

---

## The Spelling Kit, in C

**What a person gets**: a word misspelled is underlined with a red wave as
soon as it is finished. Pressing it with the right button offers up to six
suggestions, then Ignore, then Add to Dictionary. A document says which
language it is in (the Document panel, and Mail's composer), and English
and Spanish come with it. The words a person adds are one list, kept in
`/Home/Preferences/spelling`, and every application reads the same list.

### Why it is a kit, and why it is C

Every application with a text field wants it. That is five today, six
with Notes, and JetSki's forms too, since WebKit asks a platform for
spelling through one interface. So it is a kit from the start.

**It is C because it is a loop over bytes on every word**, and because of
what the alternative would cost. A dictionary is about 300,000 word forms
in Spanish once the affixes are expanded. As a Lua table that is 300,000
strings for the collector to walk, in every process that checks spelling,
which is the same mistake as pixels in a Lua table (`gfx.md` §19.1), made
with words. In C it is bytes that are never copied.

### The dictionaries, and what the build does to them

**Vendored as their authors ship them**, beside their licences, as
`assets/hyphenation/` holds Liang's patterns: LibreOffice's Hunspell
dictionaries, `en_US` and `es`, each a `.dic` (the stems) and an `.aff`
(the rules that make *walked* and *walking* from *walk*). They go in
`assets/dictionaries/` with a `README.kosmos.md` giving each file's SHA-256,
the way the hyphenation folder's README does.

**The build expands them once and packs them**, by a tool somebody can
read (`tools/dic2kd.py`, as `bdf2c.py` turns a BDF into C): every form the
affix rules make is generated on the Mac, and the whole list is packed
into **a DAWG**, a word graph in which words that share an ending share
nodes. Spanish's million-odd forms then come to a few megabytes. The DAWG
is an asset of the image, and the kit reads it **where it lies, in the
image's read-only copy**, as a font program is deflated straight from the
image into a PDF (`write.md` W3). No process copies a dictionary, and two
processes checking spelling share one set of pages.

**Why not port Hunspell itself**: it is C++ and reads its files at run
time, so every process would expand the affixes itself, in memory of its
own. Applying the rules at build time is what lets the kit be a small
piece of C over read-only bytes. What that costs, and it is real:
Hunspell's compound words and its rules for suggestions, which matter for
German or Hungarian, are not here. For English and Spanish an expanded list
loses nothing. *Diego's to decide* (question 3).

### The door

```lua
local spell = use("/Kosmos/Kits/spell")
local d = spell.open("es")            -- the language's DAWG, from the image
d:check("caminando")                  -- true
d:words(text)                         -- { { at =, len = } ... } of the words
                                      --   in text that are not in it
d:suggest("caminadno", 6)             -- { "caminando", ... }, nearest first
spell.languages()                     -- { "en-US", "es" }
```

`words` takes a whole paragraph's text and gives back only the misspelled
ones, so one call does a paragraph: the splitting into words, the
apostrophes and the hyphens, the lookups. Apart from Lua's own string, no
byte comes back through the interpreter. **The person's own words** are read by
`spelling.lua`, a few lines of Lua over the kit, which keeps them in a
small table beside it, because there are tens of them and not hundreds of
thousands.

**Suggestions** are the words in the graph within two edits of the
misspelled one (a letter added, dropped, changed or swapped with its
neighbour). They are found by walking the graph with the edits as a budget,
then ranked: an edit between letters next to each other on the keyboard,
or a missing accent (*cancion* for *canción*), costs less than another.
Suggestions are asked for only when a person presses a word, so a few
milliseconds is fine.

### The busiest path: typing

**A key that finishes a word checks that word**: one lookup, a walk of
the graph as long as the word, microseconds. Nothing else is checked on a
keystroke. **A document opened is checked in the background**, a few
paragraphs a frame in the window's idle time, and what is found is kept
**by paragraph and version**, the way `pageset.cache` keeps lines. An edit
makes a new table for the paragraph it touched and leaves the rest
(`richtext`), so only that paragraph is checked again. The wave under a
word is drawn by `pagedraw` as one more mark, beside the selection's
rectangles.

**Its tests are on the Mac**: `tools/test_spell.c`, holding the kit to
word lists made for it, every form an affix rule makes found, suggestions
for misspellings worked by hand, accents and apostrophes, a word at the
end of a paragraph with no space after it. It also checks `dic2kd.py`'s
output against Hunspell's own expansion on the Mac (`unmunch`), once, when
a dictionary changes.

---

## `richhtml.lua` - rich text as HTML, and as plain text

**What it is for**: Mail sends a styled message as HTML with a plain-text
part beside it. Write exports HTML and Markdown. Text copied from Write
and pasted into JetSki, or into Mail, should keep its bold. All three ask
the same question: *write this `richtext` body as HTML*. So one door
answers it.

```lua
local richhtml = use("/Kosmos/Libraries/richhtml.lua")
richhtml.html(body, { pictures = function(name) return "cid:" .. name end })
richhtml.text(body)        -- plain text: lists as "- " and "1. ", quotes as "> "
richhtml.markdown(body)
```

It is a serialiser and nothing more. Paragraphs become `<p>`, `<h1>`,
`<blockquote>` and `<li>`. A run's look becomes `<b>`, `<i>`, `<u>`, `<s>`
and an inline `style` for its face, size and colour. A picture becomes
`<img>` with whatever its caller says its source is: `cid:` for Mail, a
file name for an export. **Every character is escaped by `markup.lua`'s
escaper**, which `docxwrite.lua` already uses, so there is no second
escaper. Lua, because writing a document out is a walk over paragraphs,
done when a person presses Send or Export.

**The other way, HTML into `richtext`, is not in it**, and that is a
decision rather than a gap. Reading HTML well needs a browser's parser.
NetSurf's goes away when Mail moves to WebKit (`mailweb.md` W6), and
WebKit's lives in another image. What would want it is pasting from
JetSki into Write, and quoting an HTML message in a reply. For a reply,
the message's text is quoted, which is what Mail does now. For a paste,
the clipboard's plain text is used. *Diego's to decide* (question 5).

---

## The People Kit - contacts, as BeOS kept them

**What a person gets, through Mail first**: a sender pressed in a message
offers Add to Contacts. A contact's name and picture are what Mail shows
for an address. Completion in To prefers the people a person keeps over
the addresses merely seen. A group (*Family*, *Cycling club*) can be typed
as one recipient. A `.vcf` arriving in a message opens as a card, and a
card can be sent as one.

**A person is a file with attributes**, as BeOS's People application kept
them: `/Home/People/Ana Kowalski.person`, with `name`, `email` (one line an
address, labelled), `phone`, `company`, `groups`, `birthday` and a picture
beside it. `fs.query` finds them, and Tracker shows them as columns, as it
shows mail. This is the system's search doing a contact book's job, which
is the same choice Mail made for messages (`mail.md`, *Where mail is
kept*). The tutorial's lesson 4 (`people`) already teaches exactly this
shape, so the system's own contacts would be the lesson made real.

```lua
local people = use("/Kosmos/Libraries/people.lua")
people.find("ana")                 -- by name or address, as typed
people.of_address("ana@example.org")
people.add{ name =, email = { { label = "home", address = } }, ... }
people.vcard_read(text)  people.vcard_write(person)     -- RFC 6350
people.groups()  people.in_group("Family")
```

**`addresses.lua` stays, and becomes the kit's other half.** It answers
"whom has this person written to and heard from", read from the mail
itself. `people` answers "whom does this person keep". Completion asks
both, and people come first. **vCard is Lua**: a few lines of text a
card, read when a card is opened.

**What it is not, yet**: a Contacts application. A window for browsing
and editing people is an application, so it has its own four documents.
The roadmap puts "Calendar and contacts" side by side, so it is designed
with Calendar, which needs the same kit for its guests. Until then, Mail
shows and edits a card in a sheet of its own, and Tracker shows the files.
*Diego's to decide* (question 4).

**Google's contacts** (CardDAV) come with Google Calendar's CalDAV,
through the same sign-in. That is later.

---

## Templates: `templates.lua`

**What a person gets**: File, New from Template opens a chooser of
pictures. Blank, Letter, Report, Essay and CV for Write. Present's two
themes and their layouts. Budget, Invoice and Checklist for Sheets.
Templates of one's own appear beside them as soon as they are saved there.

**A template is an ordinary document**, in `/Kosmos/Templates/<app>/`
(the system's, read only) or `/Home/Templates/<app>/` (a person's): a
`.write` file is a template because of where it is kept. Opening one
makes a new, untitled copy. Saving a document into `/Home/Templates`
makes it a template. Nothing new to parse and no format of its own.

The chooser is one window for the three applications: a grid of previews
(`docfile`'s `preview.png`, below), the name under each, the Blank one
first. `templates.list("write")`, `templates.choose(app, on_chosen)`.
Present's themes are templates in this sense, so `presentdoc`'s two built-in
themes become two files, and a third is a file a person makes.

---

## `docfile.lua` grows: kept copies, and previews

### Nothing typed is lost

**What a person gets**: a document with changes not saved has its copy
kept every thirty seconds of quiet, and on every switch away from its
window. If Kosmos stops, the next start of the application offers what
was kept: "Kosmos Write kept *Letter* from 18:42. Open it?" Mail's
composer already does this for a draft, within two seconds of the last
key (`mailcompose.lua`). This makes the same promise for every document.

`docfile.keep(path, doc)` writes the document's zip, whole, to
`/Home/System/Recovery/<app>/<name>`, in one request, so a power cut leaves
the old copy or the new one (`write.md`, *Saved whole, and safely*). Saving
the document properly takes the kept copy away. `docfile.kept(app)` lists
what is waiting. **The application decides when**, on its own clock, never
in a repaint. The kit only writes.

**Not yet: versions** (every saved state browsable, as macOS's Revert To
does). That is a store of its own, and the kept copy is a smaller promise
that is kept first.

**Status, 17 November 2026: kept copies written, waiting for their gate;
previews not begun.** `docfile.keep(app, path, doc, pictures, name)`,
`docfile.kept(app)` and `docfile.forget(app, path)`: the copy is the
document's own zip with a `kept` entry saying its path, its name, when,
and when the machine had started. Write keeps one after thirty seconds of
quiet, on its window's clock, and says `writer: kept ...`; a save forgets
it. Opening a document with a copy kept for it offers the copy in a list
under the tools; a copy of a document never saved is offered to a new
window only when it was kept before the machine last started, since one
kept since may be another window's. **Not yet: a copy kept on switching
away** - the window has no event for losing the keyboard to read.

### A document shows what it is in Tracker

`docfile.save` writes `preview.png` beside `document`. It is the first page
or the first slide or the top of the first sheet, small, drawn by
`pagedraw` into a surface and written with `gfx.encode_png`, which exists.
Tracker and the template chooser show it. `write.md` W1 left this as
"later", and the chooser is what makes it needed now.

---

## Opening the other side's files: `ooxml.lua` and the XML Kit

**What a person gets**: a Word document opened in Write, a Numbers or
Excel workbook in Sheets (as XLSX), a PowerPoint deck in Present. Each one
opens with a short note saying what came across simplified: "2 floating
pictures placed in the text; 1 tracked change accepted".

**Reading is the larger half**, which is why every design here put
"out" first. Writing chooses every part, and reading has to forgive
whatever another program chose. What lets reading be done once is that the
three formats are one package: a zip of XML parts joined by relationships,
which `ooxml.lua` already writes for DOCX. So `ooxml` reads the package
(the content types, the relationships, a part's bytes into a region),
and each application's reader (`docxread.lua`, `xlsx.lua`, later
`pptxread.lua`) maps what is in the parts to its own document.

**The XML Kit is expat**, which is already in the tree
(`runtime/upstream/expat/`) and today compiled only into NetSurf, for its
DOM. Sheets' design moves it into a kit every image has (S11). **This
brings that move forward**, to be the first thing DOCX reading does,
before XLSX. And it settles where expat goes when NetSurf leaves Mail
(`mailweb.md` W6): to the XML Kit, which becomes its owner.

```lua
local xml = use("/Kosmos/Kits/xml")
xml.parse(at, bytes, { open = function(name, attrs) end,
                       text = function(s) end, close = function(name) end })
```

expat's events become Lua calls. A 100-page DOCX is about 2 MB of XML and
some tens of thousands of elements, a few tens of milliseconds of calls, once,
when it is opened. If a measurement says the calls are the cost, the kit
gains a filter (only these element names) before anything else is
considered.

**Not here: ODT** (LibreOffice's OpenDocument). It is the same idea, a zip
of XML parts, with other names. DOCX is what arrives in mail. *Diego's to
decide* (question 6).

---

## PDF grows links

A link in Write (and later in Present) is a run with a `link` field. The
PDF writer writes it as a link annotation over the run's rectangle
(`/Annot /Link` with a `URI` action), and the PDF Kit's reader, which the
PDF Viewer uses, opens it in JetSki through `weblink.lua`. DOCX writes it
as `w:hyperlink`. A small addition, listed here because three writers and
one reader meet in it.

---

## What crosses in a region, and what in a message

| What | How | Why |
|---|---|---|
| A dictionary | read where it lies, in the image's read-only copy | a megabyte-sized table of bytes; never copied, shared by every process |
| A paragraph checked | a Lua string in, positions out | one paragraph, because a word was finished |
| A DOCX's parts | the zip's entry into a region, expat over the region | bytes between C stages stay in regions (`zip.lua`'s `read_region`) |
| A kept copy, a preview | written whole, one request | a power cut leaves one or the other |
| Contacts | files and their attributes, `fs.query` | the system's search; nothing new travels |

Every kit runs in its user's process, so nothing here is a message between
two processes.

## C or Lua, and why

- **C**: the Spelling Kit, a lookup over a dictionary's bytes on every word;
  the XML Kit, expat, a loop over every byte of a file.
- **Lua**: everything else. `textedit`, `richhtml`, `people`, `templates`
  and `docfile`'s additions are structure and policy, called when a person
  does something, and none is a loop over bytes that Lua's own C does not
  already do (`string.find`, `table.concat`).

---

## The order, and what each step leaves

Each step is a revision of its own and a test that stays.

1. **O1 - the moves out of Write**, as Present's P0 names them, plus
   `chart.lua` (Sheets' S0). Write's suites unchanged are the proof. A
   keystroke in a 100-page document measured first.
2. **O2 - find and replace**: `textedit:find`, `pk.findbar`, Write first.
3. **O3 - the Spelling Kit**: the dictionaries vendored, `dic2kd.py`,
   `test_spell.c`, `spelling.lua`; Write first, then Mail's composer.
4. **O4 - `docfile` grows**: kept copies and recovery, previews; then
   `templates.lua` and the chooser.
5. **O5 - `richhtml.lua`**: needed by Mail's rich composer (`mail.md`
   N2) and Write's HTML export.
6. **O6 - the People Kit**: with Mail's N3.
7. **O7 - the XML Kit and `ooxml` reading**, with Write's DOCX opening.

The applications' own steps interleave with these. Each says which O-step
it waits for (`mail.md`, `write.md`, `sheets.md`, `present.md`).

---

## Diego's to decide

**Decided on 17 November 2026**: every recommendation below, as
recommended - Diego, "yes to all, start with write". The order is
`roadmap.md`'s, *Mail and the office applications, less basic*.


1. **The moves out of Write first, for every application (O1).**
   *Recommended: yes*, as Present's P0 recommended. The difference now is
   that Mail and Write's next version wait on them too, so they are done
   before any of the four applications grows.
2. **`pk.findbar` and `textedit:find` as the one find**, with each window
   supplying its own search (cells, PDF text, plain text). *Recommended:
   yes.*
3. **Spelling: Kosmos's own kit, with Hunspell's dictionaries expanded at
   build time**, or Hunspell ported as it is. *Recommended: our own.* It is
   a small C kit over read-only bytes that every process shares, and for
   English and Spanish nothing is lost. Hunspell is the answer if a
   language with compound words is ever wanted.
4. **Which dictionaries**: US English and Spanish, the two that
   hyphenation already has, to begin with. *Recommended: yes*, with one
   general Spanish dictionary rather than a country's.
5. **`richhtml` writes HTML and does not read it**: a reply to an HTML
   message quotes its text, and a paste from JetSki pastes text.
   *Recommended: yes for now.* Reading HTML into `richtext` is its own step
   once WebKit is Mail's (`mailweb.md`), with WebKit as the parser.
6. **DOCX first, ODT later**, for opening. *Recommended: yes*, because
   DOCX is what arrives in mail.
7. **Contacts as files with attributes in `/Home/People`**, BeOS's way,
   with vCard in and out, and a Contacts window designed with Calendar.
   *Recommended: yes.*
8. **Kept copies every thirty seconds of quiet**, in
   `/Home/System/Recovery`. *Recommended: yes*, and versions after.
9. **Templates are ordinary documents in a folder**, the system's and a
   person's. *Recommended: yes.*
