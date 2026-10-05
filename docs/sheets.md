<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos Sheets, before it is built

Written on 5 October 2026, before any of it is built, under the rule of the
same day (`CLAUDE.md`, *And an app is designed before it is written*): its
feature set and its architecture here, its window in `docs/sheets.html`,
and what it stands on drawn in `docs/sheets-architecture.png`. Diego, 4
October: "Kosmos Sheets which is a apple numbers and excel inspirated app";
the roadmap's entry the same day: "Numbers' tables on a free canvas and
Excel's grid and formulas: cells, formulas and their functions, references
across tables and sheets, formats for numbers and dates, sorting and
filters, charts from a table's cells, and PDF and XLSX out. Reusing Write's
tables, charts, inspector and file kits. Its formula engine in C - a loop
over cells - and Lua for what a person does."

Kosmos Write is built and is what this stands on (`docs/write.md`).
Kosmos Present is being designed beside it (`docs/present.md`); where the
two would share a kit, *Shared with Kosmos Present* below names it so the
two can be given one name before either is built.

---

## What it does

**Numbers' window and Excel's arithmetic.** A document is a set of sheets,
each a free canvas on which tables, charts, text boxes, shapes and
pictures stand where they were put - Numbers' way, and what the roadmap
asked for. What happens inside a table is Excel's: the formula bar, the
grammar of a formula, the names of the functions and how they treat what
they are given. That second half is not taste: **a formula travels in an
XLSX**, and it should give the same answer at the other end.

### In the first version

- **Sheets**, as tabs across the top: added, named, reordered, deleted.
- **A canvas on each sheet**, white, in points as a page is: tables, charts,
  text boxes, shapes and pictures placed with the pointer, moved, sized,
  brought forward and sent back, snapped to each other's edges and
  middles.
- **Tables** with header rows and header columns that stay in view while
  the table scrolls under them, footer rows for totals, a name the
  references use, rows and columns added, removed and sized, six looks,
  gridlines, alternating rows. **No row or column count compiled in** - a
  table is as large as the machine's memory allows, and 100,000 rows scroll
  as 10 do.
- **Cells** holding a number, text, a date or a time, true or false, or a
  formula - recognised as a person types them: `4180`, `$1,200`, `12%`,
  `5 Oct 2026`, `18:30`, `=SUM(B2:B6)`.
- **Formulas**: `+ - * / ^ &`, comparisons, `%`; references relative and
  absolute (`B2`, `$B$2`, `B$2`), ranges (`B2:D6`), a column's body (`B:B`);
  **references to other tables by name** (`Costs!E7`), on this sheet or any
  other; about eighty functions (below); **errors as values** (`#DIV/0!`,
  `#REF!` and the rest); a cycle refused and marked.
- **The formula bar** above the canvas: the chosen cell's name and its
  formula, edited there or in the cell, **each reference in its own colour
  and outlined in that colour on the canvas** - in whichever table it is -
  and a function's name completed with its arguments as it is typed.
- **Quick formulas** from the tools' Formula (Σ): Sum, Average, Minimum,
  Maximum, Count and Product of the chosen cells, put in the cell below or
  beside them; and **the foot of the canvas says the sum, average, smallest,
  largest and count of whatever is chosen**, without a formula.
- **Fill**: the handle at a choice's corner dragged to copy a formula along
  - its references moving with it - or to carry a series on: 1, 2, 3;
  January, February; a date a week at a time.
- **Formats** (the Format panel's Cell): Automatic, Number (places, a
  thousands separator, how a negative is shown), Currency (its symbol, places,
  accounting alignment), Percentage, Scientific, Text, and Date and Time in
  a list of styles.
- **A cell's look**: face, size, bold, italic, colour, fill, its alignment
  across and down - the Format panel's Text, as Write's.
- **Sort and filter** (the tools' Organize): a table sorted by one or more of
  its columns, up or down; filters by rules - is, is not, contains, begins
  with, greater, less, between, is empty - all of them or any; footer rows
  and header rows never sorted or hidden.
- **Charts from a table's cells**: column, bar, line and pie as Write has
  them, and area and scatter, which a spreadsheet needs and a word
  processor did not. A chart is **live**: it reads a range of a table and is
  drawn again when a number in it changes. Series by rows or by columns.
- **Comments** on a cell: a mark in its corner and its words in the panel.
- **Undo and redo** of everything, a run of typing one step.
- **Copy and paste**: cells within Sheets with their formulas, the
  references moving; to another application as tab-separated text; tabbed
  text pasted into cells.
- **Files**: `.sheets`, its own; **CSV** in and out; **XLSX** out, and in;
  **PDF** out, the sheet's canvas laid on pages and fitted to their width.

### The functions first

About eighty, by Excel's names and argument order:

| | |
|---|---|
| Sums and arithmetic | `SUM` `PRODUCT` `ABS` `ROUND` `ROUNDUP` `ROUNDDOWN` `INT` `TRUNC` `MOD` `POWER` `SQRT` `EXP` `LN` `LOG10` `PI` `SIGN` `RAND` `RANDBETWEEN` |
| Statistics | `AVERAGE` `MEDIAN` `MIN` `MAX` `COUNT` `COUNTA` `COUNTBLANK` `LARGE` `SMALL` `STDEV` `VAR` |
| Conditions | `IF` `IFS` `IFERROR` `IFNA` `AND` `OR` `NOT` `SUMIF` `SUMIFS` `COUNTIF` `COUNTIFS` `AVERAGEIF` `AVERAGEIFS` `MAXIFS` `MINIFS` |
| Looking up | `VLOOKUP` `HLOOKUP` `XLOOKUP` `INDEX` `MATCH` `CHOOSE` |
| Text | `CONCAT` `CONCATENATE` `TEXTJOIN` `LEFT` `RIGHT` `MID` `LEN` `UPPER` `LOWER` `PROPER` `TRIM` `FIND` `SEARCH` `SUBSTITUTE` `REPLACE` `REPT` `TEXT` `VALUE` |
| Dates and times | `DATE` `TIME` `TODAY` `NOW` `YEAR` `MONTH` `DAY` `HOUR` `MINUTE` `SECOND` `WEEKDAY` `EDATE` `EOMONTH` `DAYS` `DATEDIF` `NETWORKDAYS` `WORKDAY` |
| Money | `PMT` `FV` `PV` `NPER` `RATE` `NPV` `IRR` |
| What a value is | `ISBLANK` `ISNUMBER` `ISTEXT` `ISERROR` `ISNA` `NA` |

**Where Excel and Numbers disagree, Excel is followed** - text in a range
passed over by `SUM`, text that reads as a number taken as one by `+`,
numbers before text before true and false in a comparison - for the same
reason as the grammar.

### Later, each its own step

Categories (Numbers' rows grouped by a column's values, with a summary row
each) and pivot tables; conditional highlighting; checkboxes, star ratings,
sliders, steppers and pop-up menus in cells; Fraction and Duration formats
and formats of a person's own; merged cells; text wrapped inside a cell;
borders; names for ranges, and a column called by its header's words;
statistics, engineering and the rest of Excel's five hundred functions, and
its formulas that return many cells (`FILTER`, `SORT`, `UNIQUE`); stacked,
doughnut and two-axis charts; find and replace; templates; a text box, a
shape or a picture taken to an XLSX; a sheet of thousands of pages to PDF
(*PDF out*, below); a recalculation on a thread of its own, if a measurement
asks for one.

---

## The architecture

### At a glance

Every piece, and what supplies it. *Exists* is a kit used as it is;
*grows* is one that gains a thing; *moved* is code in `writer.lua`,
`pageset.lua` or `docxwrite.lua` today that becomes a kit Write, Present and
Sheets stand on; *new* is new, and a kit wherever another application could
want it. Present's draft moves four of these first (its step P0); whichever
application is built first moves them, once.

| Piece | Supplied by | |
|---|---|---|
| **The cells, the formulas over them, their order, their formats, their files, and the grid drawn** | **the Cells Kit**, `user/kits/cells/`, `use("/Kosmos/Kits/cells")` | **new, C** |
| XML read: an XLSX's parts | **the XML Kit**, `user/kits/xml/` - expat 2.8.5, vendored already, compiled today only into the browser's images | **moved**, from the browser's build |
| The document: its sheets, what stands on each, its styles, print setup | **`sheetsdoc.lua`** | new, Sheets' own |
| Things placed, chosen, moved, sized, ordered | **`canvas.lua`** | new, shared with Present |
| A chart: its kinds, colours, the picture of it and its DrawingML | **`chart.lua`**, out of `pageset.chart_plan` and `docxwrite.chart_part` | moved (see *Charts*) |
| Office's package: content types, relationships, core properties | **`ooxml.lua`**, out of `docxwrite.lua` | moved, shared with Present |
| The workbook's own parts | **`xlsx.lua`** | new |
| The Format, Organize and Document panels' rows and controls | **`inspector.lua`**, out of `writer.lua`'s `draw_panel` | moved, shared with Present |
| The row of tools | `pixelkit.lua` - **`pk.toolbar`**, out of `writer.lua`'s `draw_tools` | moved, shared |
| The sheets' tabs | `pixelkit.lua` - **`pk.tabs`**, `ui.tabs`' look in a direct window | grows |
| Every other control: header, segments, chooser, stepper, check, swatch, field, menu | `pixelkit.lua` | exists |
| The window, its pixels, its commit | `ui.lua` (`direct = true`, `header = true`, `window:surface`, `window:commit`), the window manager | exists |
| Keys | `keys.lua` | exists |
| The formula being typed: a line, a caret, a selection, undo | `textbuf.lua` | exists |
| A text box's words, typed | `richtext.lua`; `textedit.lua`, out of `writer.lua` (Present's P0) | exists; moved |
| A text box, a shape, a picture set in its box | `pageset.frame` (Present's), `pageset.shape_art` | grows; exists |
| Drawing a box, a shape, a chart, a picture | `pagedraw.lua`, a page of frames (Present's) | grows |
| Faces and their measure, for the PDF | `faces.lua`, `gfx.typefaces`, `gfx.typeface` | exists |
| PDF out | `pdf.lua` (`pdf.write`) over `pdfwrite.lua`, with frames (Present's) | grows |
| The file | `docfile.lua` over `zip.lua`; **an entry written from a region**, and `zip.read_region` (Present's) | grows |
| Regions | `regions.lua` | exists |
| Copy and paste between applications | `wmproto.copy`, `wmproto.paste` | exists, 1,900 bytes |
| Today's date for `TODAY()` | `clock.lua` (`clock.now`, its offset) | exists |
| `.sheets`, `.csv`, `.xlsx` as types Tracker opens with Sheets | `filetypes.lua`, the header's `-- kosmos: opens` | grows by three words |
| Pixels, glyphs, pictures | the gfx Kit: `gfx_draw.h` for the Cells Kit, `gfx.face`, `docfont`, `png`, `jpeg` | exists |
| Deflate, inflate, CRC-32, Adler-32 | the Compression Kit | exists |

**No new server, no new protocol header, no new system call.** Sheets is a
kit and an application. The servers it reaches are the ones every
application reaches: the window manager, `/Home`, `/Kosmos`, `/Drives` for a
CSV or an XLSX on a stick, `/Devices/clock`.

### Why the cells are a kit's, in C

The question `CLAUDE.md` asks first - is it a loop over bytes, or does it
sit where a collector pause would be felt? - is answered yes twice.
Recalculating is a loop over cells, and so is drawing the grid, sorting,
filtering, and reading or writing a CSV or a worksheet. And all of them are
on the path from a key to the screen.

**The cells cannot be Lua values, and the repository has measured why.**
Clearing a 960 by 540 framebuffer ten times (`testing.md` 18.128): a Lua
loop into a Lua table, 24.81 ms; the same loop in C into that table,
**43.81 ms - 0.56 times, slower than the interpreter**; in C into a
surface, 1.32 ms. A store from C into a Lua table is the whole table API -
boxing, a write barrier - and a recalculation in C over cells kept in Lua
tables would pay that on every operand it read and every result it wrote.
*Moving a loop to C is worth nothing until the data it walks stops being a
Lua value*; for Sheets that means the cells live in the kit from the first
line, not in Lua with a fast loop added later.

**And not in Write's tables either.** A Write table is a `richtext`
paragraph whose every cell is a paragraph of its own, held to at most 1,000
rows by 20 columns (`richtext.TABLE_ROWS`, `TABLE_COLUMNS`) - limits chosen
for a page, and right there. A thousand paragraphs of three runs are 866 KB
as tables (`write.md`, measured); a sheet of 100,000 rows by six columns is
600,000 cells. As small Lua tables - a header of 56 bytes, two hash slots,
this heap's 32-byte header on each allocation (`runtime/libc/malloc.c`) -
that is roughly 180 bytes a cell by arithmetic, not measured: about 100 MB,
every table of it walked by the collector each cycle. In the kit a cell is
16 bytes and 600,000 of them are 9.6 MB, in pages the collector never sees.

**What "reusing Write's tables" means, then**: their look - the header's
tint `pageset.HEADER_TINT`, the rule `pageset.RULE`, a cell's padding - so a
table in Sheets and in Write are one look; their header row repeated at the
head of each printed page, as `pageset` does it; the Table part's controls
through `inspector.lua`; and the move between them, a range copied out of
Sheets arriving in Write as a table through tabbed text. Not their storage.

---

## The Cells Kit - the formula engine

`user/kits/cells/`, reached as `use("/Kosmos/Kits/cells")`, built by
`kosmos_cells_kit` and listed in `sys_user.c`'s `kits[]` beside the
others. **Pure where it can be**, as the Synth Kit's engine is: everything
but the door and the painter compiles on the Mac and is driven there by
`tools/test_cells.c`, natively and through Rosetta, as `test_rows` and
`test_pack` are.

| file | what |
|---|---|
| `cells.h` | the declared types: a cell, a value, a style, a view |
| `cells_store.c` | tables as columns of blocks; the strings; geometry |
| `cells_input.c` | what a person typed, recognised |
| `cells_parse.c` | a formula's text to its compiled form and back; the spans the editor colours |
| `cells_graph.c` | who depends on whom; what a change dirties; the order |
| `cells_eval.c`, `cells_fn.c` | the evaluator, and the functions by name |
| `cells_format.c` | a value as a format shows it; Excel's format codes in and out |
| `cells_order.c` | sorting, filtering |
| `cells_io.c` | the `.sheets` cells, CSV, an XLSX worksheet and its shared strings |
| `cells_draw.c` | the grid painted, through `gfx_draw.h` - the one file that knows the gfx Kit |
| `cells_kosmos.c` | the door |

### Where the cells live, and what a cell is

**A table is columns, and a column is blocks of 1,024 cells** - 16 KB, four
pages, asked of the kernel with `kosmos_map_bytes` as the gfx Kit asks for a
surface. A block never written is absent and reads as empty, so a sparse
table costs what it holds. Columns, because nearly everything a spreadsheet
does walks one: `SUM(B2:B100001)` reads a hundred blocks front to back, a
sort compares one column, a filter tests one, and the visible window is
thirty runs of a dozen columns rather than a scatter.

```c
struct cell {                   /* 16 bytes */
    union {
        double   number;        /* a number, a date, a time, true or false */
        uint32_t text;          /* a string in the book's pool */
        uint32_t error;         /* #DIV/0!, #REF!, ... */
    } v;
    uint32_t formula;           /* 0, or the compiled formula it computes */
    uint16_t style;             /* the document's style: its look and format */
    uint8_t  kind;              /* empty, number, text, logical, error */
    uint8_t  flags;             /* dirty, volatile, a comment on it */
};
```

- **Strings once**: a book's text is a pool, each string stored once and
  named by number - which is also what an XLSX's shared strings part is, so
  writing one is reading the pool out.
- **A formula filled down is one formula**: references are compiled as
  offsets from the cell where they are relative and as numbers where they
  are absolute, so `=B2*1.21` in C2 and `=B10001*1.21` in C10001 are the same
  compiled formula, held once and pointed at by 10,000 cells. Excel's
  shared formulas in an XLSX (`<f t="shared">`) are the same idea, and are
  written from it.
- **Geometry is the kit's**: column widths, a default row height and the
  rows that differ, which rows a filter hides - kept with a running total
  per block of 1,024 rows, so a point on the screen finds its row by a
  search over blocks and a walk inside one, never a walk from the top.
- **No limits compiled in** (`CLAUDE.md`, *4K and no hard limits*): a table
  grows as its cells are written. Two limits are kept, each Excel's and each
  said when refused - a cell's text at most 32,767 characters and a formula
  at most 8,192 - because a formula written here has to open in Excel, and
  because a file that claims a gigabyte in one cell should cost nothing to
  turn away.

### One grammar, one parser

**The parser is written once and used four ways**: what a person types into
a cell; the editor's colours while they type it (`book:spans` gives each
reference's place in the text and what it points at); the formulas of an
XLSX, in and out; and the formulas in a `.sheets` file. Excel's grammar,
because the last two are Excel's and a person who knows a spreadsheet knows
it: function names in English, arguments between commas, `$` for absolute,
`!` after a table's name.

It is a recursive descent with precedence climbing, in C, and **its depth
is held to 64 levels** - Excel's own limit for nested functions - because a
parser's recursion is the C stack, and a formula is text that arrived from
a file.

**A file holds a formula's text, never its compiled form.** The compiled
form is bytecode, and `CLAUDE.md`'s reason for refusing precompiled Lua -
a bytecode loader verifies nothing - is the same reason here: a `.sheets` or
an XLSX is somebody else's file, and the text goes through the parser and
its checks like anything typed. The formula bar shows the compiled form
spelt back out, so a reference moved by an inserted row reads as it now is;
what that costs, said: the kit's spelling replaces the person's - capitals
for a function's name, no spaces - as `=sum( b2:b6 )` comes back
`=SUM(B2:B6)`.

### References across tables and sheets

**A reference names a table, not a sheet**: `Costs!E7`, `Costs!B2:D6`. A
table's name is unique in its document - a second "Table 1" is made "Table
2" - and held to the rule Excel holds a worksheet's name to (at most 31
characters, none of `[ ] : * ? / \`).

- **A table moved to another sheet keeps every reference to it**, because
  no reference mentions a sheet. Numbers spells the same thing
  `Sheet 2::Costs::E7` and breaks it when the table moves.
- **An XLSX maps without renaming**: each table becomes a worksheet of its
  name (*XLSX*, below), and `Costs!E7` is already the reference Excel
  would write. The canvas does not go to Excel; the formulas go unchanged.
- **Compiled, a reference is the table's number**, not its name: renaming a
  table changes one entry in a map and no formula; deleting it turns every
  reference to it into `#REF!`, as deleting the row a reference names does.
- **`B:B` is a column's body** - the rows between the header and the footer
  - as Numbers means it, so a footer's `=SUM(B:B)` never counts itself or
  the header. Written to an XLSX it becomes the body's range as it is then
  (`B2:B6`), since Excel's `B:B` would take in the footer and go round.

Inserting rows moves the references below the point and widens a range they
fall inside, Excel's rule; deleting a row a reference names gives `#REF!`.
**Sorting is the same remapping**: rows are permuted, and a reference to a
sorted row follows the row, as Numbers' does - a total that pointed at
Rent still points at Rent. Excel's sort moves the values and leaves the
references where they were; that is the one place Numbers is followed, and
an XLSX is unaffected by it, since it is written after the sort.

### Who depends on whom, and the order of a recalculation

- **A formula records what it reads** when it is compiled: single cells,
  and ranges.
- **The reverse is kept as an index**: for each cell, the formulas that read
  it alone; for ranges, a list per block of 1,024 rows per column of the
  ranges that cross it - so a change to B7 finds `SUM(B2:B100001)` by looking
  in one block's list rather than in every range in the book, and a range
  is one entry, not 100,000 edges.
- **A change marks** the changed cell's dependents dirty, and theirs, until
  nothing new is marked - the dirty set.
- **The order is worked out over the dirty set alone** (Kahn's algorithm):
  each dirty formula counts how many of the dirty cells it reads, the ones
  with none are evaluated first, and each one evaluated lets the next go.
  What is left with a count when nothing more can go is a **cycle**, and
  every cell in it is the error `#CYCLE!` rather than a guess. Excel offers
  to iterate round a cycle; that is later, if ever.
- **Volatile functions** - `TODAY`, `NOW`, `RAND`, `RANDBETWEEN` - are dirty
  on every recalculation and when a book opens.
- **Bounded, not merely quick**: `book:recalc(budget)` stops when its budget
  is spent and says so; the window draws what is ready - a cell not yet
  computed in its old value, dimmed - and asks again on the next pass. A
  recalculation of a million cells is then many frames, each of them a
  frame, rather than one frozen window. Kahn's order is what makes stopping
  anywhere safe: whatever has been computed was computed from finished
  values.
- **Not in the first version, said**: stopping the spread where a value came
  out unchanged. It is the obvious saving, and it waits for S1's number to
  say whether it is needed.

### Evaluating

A compiled formula is postfix bytecode: push a number, a string, a
reference, a range; an operator; call a function with *n* arguments; and
jumps, so `IF` and `IFERROR` evaluate only the branch they take. The stack
is C's, its depth known when the formula is compiled. **A range reaches a
function as a range** - a table, a corner, a size - and `SUM` walks the
store's blocks as they lie in memory; no list of values is made. Strings a
formula makes along the way live in an arena emptied after each formula,
and only a result kept in a cell enters the pool.

### Errors are values

`#DIV/0!`, `#N/A`, `#NAME?`, `#NUM!`, `#REF!`, `#VALUE!` - Excel's - and
`#CYCLE!`. An error is a cell's value like a number is: an operation on an
error gives that error, `SUM` over a range with one in it gives it,
`IFERROR` and `ISERROR` look at it, and the cell shows it in the error
colour with what went wrong in the panel ("divides by B4, which is 0").
Nothing is thrown, nothing stops a recalculation, and one bad cell spoils
only what reads it. An XLSX gets Excel's errors as they are; a cycle's
cells are written as their formula with no value beside it, and the
workbook marked to calculate when it opens, so Excel says what Excel says
about a cycle.

### Numbers are doubles, and money is not exact

**Every number is a 64-bit binary double**, as in Excel and in Numbers. Said
plainly, because a spreadsheet is where people keep money:

- **Whole numbers are exact up to 2^53**, a little over nine thousand
  million million - every count, every amount in cents a household or a
  business will meet.
- **Most decimal fractions are not exact.** 0.1 is stored as the nearest
  double, which is a little more than a tenth, so `=0.1+0.2` computes
  0.30000000000000004. Each sum of prices carries an error about a
  ten-thousand-million-millionth of its size, and a long column of them can
  end a few of those away from the cent.
- **What is shown is rounded**: a format's places, and never more than
  fifteen significant digits, as both of the others show - so the column
  above reads `0.30`.
- **What is compared is rounded too**: `=`, `<>`, `<` and the rest, a
  lookup's match and a filter's rule treat two numbers as equal when they
  agree to fifteen significant digits - so `=0.1+0.2=0.3` is TRUE, as a
  person reading the screen expects. Arithmetic itself is never rounded
  behind anybody's back.
- **Where a cent must be exact**, `ROUND(x, 2)` makes it so at that step,
  which is the practice Excel's users already follow.

The other way - decimal arithmetic, every amount exact in cents - is real
and is not taken: every function from `SQRT` to `IRR` is defined over
doubles, an XLSX stores doubles, and a number would compute differently
here than in the Excel it was exported to. That is Diego's to confirm
(*Yours to decide*).

### Dates and times

**A date is a number with a date's format**: days since 30 December 1899,
with the time of day as the fraction - Excel's serial numbers, which is
what an XLSX holds, so dates go out and come back unchanged and
`=B2+7` is a week later in both. Two things Excel does are met at the door
rather than kept: its 1900 has a 29 February that never was, so serials
before 1 March 1900 are moved by one going in and out; and a workbook made
on an old Mac counts from 1904, said in its `workbookPr`, and is moved by
1,462 days.

**A date in a cell is a date on the wall**, with no time zone - as in both
others. `TODAY()` and `NOW()` are the local date and time, which the kit is
handed rather than reads: the window asks `clock.now()` - `/Devices/clock`
through the person's offset - when a book opens and before a recalculation
that has a volatile cell in it. The kit reads no device.

**The calendar in C is a second copy of Howard Hinnant's arithmetic**,
beside `clock.lua`'s. The review before 0.11 found the Lua one in two halves
in two files ("the calendar split between `clock` and `httpcache`",
`roadmap.md`), and the second-copies work under way as this is written
joins them in `clock.lua` - its `civil` one way, `clock.days` the other. The
kit's is deliberately on the other side of the language line: it is inside
the painter's per-cell loop, where a crossing into Lua for each date would
cost more than the date. It is held to `clock.lua`'s by `test_cells.c`, both
ways, over every day from 1900 to 2200.

### Formats

A style's format is a declared shape, not a string a person composes:

```c
struct format {
    uint8_t kind;         /* automatic, number, currency, percent, scientific, text, date, time, datetime */
    uint8_t places;       /* 0 to 15 */
    uint8_t thousands;    /* a separator every three digits */
    uint8_t negative;     /* -1,234   (1,234)   -1,234 in red */
    uint8_t date_style;   /* 5 October 2026, 5 Oct 2026, 05/10/2026, 2026-10-05, Monday 5 October 2026 */
    uint8_t time_style;   /* 18:42, 18:42:10, 6:42 PM */
    char    symbol[8];    /* $  EUR  GBP  JPY  UYU ... */
};
```

**Formatting happens in C**, in the painter's loop, a visible cell at a
time, into a buffer on the stack - so scrolling makes no strings.
**Automatic** is what the person typed: a number as it came, `$1,200` as
currency, `12%` as a percentage, a date in the style it was typed in -
Numbers' behaviour, decided once in `cells_input.c`.

**Excel's format codes are met at the door of an XLSX**:
`#,##0.00;[Red]-#,##0.00` read into the shape above and written back out of
it; a code the shape cannot say is kept as written, shown as Automatic, and
written back unchanged.

**And one way of writing a number, not two.** Tracker, Info, Machine and
Solar System each grouped digits in thousands themselves, and the
second-copies work under way as this is written makes that one function,
`text.grouped`. The kit's formatter is the same rule again on the C side of
the line, for the same reason as the calendar - it runs a cell at a time
inside the painter. So the two are held to each other: `test_cells.c`
formats every number `text.grouped`'s own test does and expects the same
digits. And the day the system has a region - a decimal comma in Spanish
- `text.grouped` and `cells.format` change together, or `text.grouped`
becomes a call to `cells.format`, which is the smaller of the two doors to
keep. That is a choice for that day, named here so it is not found later
as a disagreement.

### Sorting and filters

**In C, on the kit's cells.** A sort is a stable merge of the rows' numbers
by one or more keys, numbers before text before true and false before
errors, empty cells last whichever way the sort goes, as Excel orders
them; the permutation is then applied to the table's blocks and to the
references into them (*References*, above). 100,000 rows is one call.

A filter is rules on a table - a column, a test, a value; all of them or
any - kept in the document and applied by the kit as a set of hidden rows
with a count per block, which is what the painter and the hit test already
walk. Turning filters off clears the set and keeps the rules. **A footer's
`SUM` still counts the rows a filter hides**, as both others do; `SUBTOTAL`,
which does not, is later.

### The door

A handful of calls carry every frame; the rest are each one thing a person
did. **Each call is coarse**: one crossing per thing a person did, never one
per cell.

```lua
local cells = use("/Kosmos/Kits/cells")
local book = cells.book()                 -- an empty book, in this process's pages

-- every frame, or every key
book:set(t, row, col, "=SUM(B2:B6)")       -- what was typed: recognised, compiled,
                                           -- the old content kept for undo, dependents dirtied
book:get(t, row, col, into)               -- one cell into a reused table: shown, formula,
                                           -- value, kind, style, error's reason
book:spans(text, into)                    -- a formula's references: where, which table, which cells
book:recalc(budget_us)                    -- true when done; the tables whose shown cells changed
book:draw(surface, t, view)               -- the cells of `t` that `view` shows, painted
book:at(t, x, y, view)  book:rect(t, r1, c1, r2, c2, view)   -- a point to a cell; a range to pixels

-- what a person does
book:table(rows, columns)  book:name(t, "Costs")  book:drop(t)
book:insert(t, "row", at, n)  book:remove(t, "column", at, n)  book:size(t, "column", at, width_pt)
book:styles(list)  book:today(serial)
book:fill(t, from, to)  book:copy(t, range)  book:paste(t, row, col)  book:text(t, range, most)
book:sort(t, keys)  book:filter(t, rules)  book:series(t, range, by, most)
book:step()  book:undo()  book:redo()

-- files: into and out of regions
book:write(t, region)   book:read(t, region, size)            -- the .sheets cells
book:csv_in(t, region, size, separator)   book:csv_out(t, region, separator)
book:xlsx_out(t, region)  book:strings_out(region)  book:xlsx_in(t, region, size, strings)

cells.format(value, format)  cells.functions()
```

- **`get` fills a table it is handed** rather than making one - the lesson
  of `con.wait` (`CLAUDE.md`): a call made every pass that allocates is
  garbage made every pass.
- **`view` is a reused table of a dozen numbers** - where the table stands
  in the window, how far it is scrolled, the zoom, whether its headers are
  frozen - read once a call.
- **`styles` is handed whole when it changes**, each style's look resolved
  by the window to the gfx faces at the zoom (`gfx.face(name, px)`), so the
  painter never calls back into Lua.
- **What leaves the kit for Lua is what a person reads in Lua**: one cell
  for the formula bar and the panel, a chart's series (at most `most`
  points), tabbed text for the clipboard (at most `most` bytes). **Never a
  column, never a table.**
- **Undo is the kit's for cells**: a journal of what each step changed,
  bounded by memory rather than a count. What the canvas does - a table
  moved, a chart's kind - is the document's, kept as Write keeps its body,
  and one list of steps in `sheets.lua` says which is which.
- **A file's bytes never become a string** (`design.md` 8.3b2): a region
  the window made goes in; when it is too small the call says how large one
  must be, the shape `fs.read_into` already has.

### The grid, drawn

**`book:draw` paints a table's visible cells in one call**, through what the
gfx Kit already lends another kit (`gfx_draw.h`): `gfx_surface_check` for
the window's surface, `gfx_draw_fill`, `gfx_draw_text`,
`gfx_draw_measure`. It is the web kit's shape and Doom's - a frame drawn
inside one C loop and one crossing (`gfx.md` 19.11) - rather than a
call from Lua for each cell, which at the crossing's measured 9
microseconds under QEMU would be 2.7 ms for 300 cells before a pixel was
drawn.

What it draws, back to front: the table's fills (a header's tint, a
footer's, alternating rows, a cell's own); the gridlines, a fill a pixel
thick per line; each cell's value formatted into a buffer on the stack and
set at its alignment - numbers to the right and text to the left, as
Automatic has them; and the frozen header rows and columns last, over the
body that scrolled under them.

- **Text that does not fit is cut** at the last character that does. **A
  number that does not fit is `###`**, as Excel shows it - a number cut short
  would be a different number.
- **The screen uses the gfx Kit's outline faces at a pixel size; the PDF
  uses the same families measured unhinted** (`faces.lua`). Write would not
  accept that, because its lines break by measure and the PDF must break
  where the screen did. A cell does not break in the first version - it is
  cut - so the two can differ by a pixel without anything moving. When text
  wraps in cells (later), the wrap is measured with `faces.lua` and drawn
  with it.
- **Lua draws only what is few**: the choice's outline and its handle, the
  coloured outlines of the references being typed, a comment's corner - a
  handful of fills, at places `book:rect` gives.

**Scrolling 100,000 rows costs what scrolling 10 does.** The first row on the
screen is found by the block totals; after it the painter walks forward a
row at a time and stops at the window's foot. Nothing is proportional to
the table, so the 100,000th row is drawn exactly as the 10th.

---

## The canvas

**`canvas.lua`**, as Present's design names it: rectangles in points,
chosen, moved, sized by eight handles, snapped to each other's edges and
middles, ordered front to back - and nothing about what is inside them,
which is why a table on a sheet and a picture on a slide can share it
(`present.md`, *Things placed*). Sheets uses it as drawn there:
`canvas.new{ snap_pt = 6 }`, `c:things`, `c:at`, `c:press`, `c:drag`,
`c:release`, `c:guides`, `c:draw`, `c:order`, `c:align`.

What stands on a sheet, and what draws each:

- **a table**: its box is its visible size - the whole table, or the part a
  long one shows before the canvas scrolls it - drawn by `book:draw` every
  frame;
- **a text box, a shape, a picture**: Write's paragraphs set in a box by
  `pageset.frame` and drawn by `pagedraw` (Present's frames), each into a
  surface of its own at the zoom and blitted after that, as Write's pages
  are - drawn again only when it changes;
- **a chart**: `chart.lua`'s picture of `book:series`, a frame too, drawn
  again when `book:recalc` says a cell in its range changed.

A sheet is measured in points, as a page and a slide are; 100% shows a
point as Write's 100% does. A table taller than the window scrolls inside
the canvas with its header rows frozen at the canvas's top, as Numbers'
do, so the canvas and a long table are one scroll rather than two.

---

## Charts

**A chart kit of its own: `chart.lua`.** What a chart is lives in two
places today, and both are second homes: `pageset.chart_plan` - the bars,
lines and slices as `art`, and their words - in the library that sets
paragraphs onto pages, and `chart_part` - the same chart as DrawingML's
`c:chartSpace` - in Word's writer. And **the series' colours are written
twice already**: `pageset.CHART_COLOURS = { "#2a55c9", "#d35400", ... }` and
`docxwrite.lua`'s `CHART_COLOURS = { "2A55C9", "D35400", ... }`. Sheets is
the application charts are most for, and it grows them: two kinds, an
axis in the cells' own format, a range rather than a paragraph, and
references in the XLSX's part. Grown in two places, the kinds would drift
the way the colours already have.

```lua
local chart = use("/Kosmos/Libraries/chart.lua")
chart.KINDS     -- column, bar, line, pie, area, scatter
chart.COLOURS   -- the one list
chart.plan(data, kind, x, y, w, h, text_w, figure)   -- pageset.chart_plan, moved; `figure`
                                                     -- says a number as the axis shows it
chart.frame(data, kind, w_pt, h_pt, measure, look)   -- the plan as a frame: art and labels
chart.part(data, kind, refs)                         -- c:chartSpace; `refs`, when given,
                                                     -- the cells each series is read from
```

- `data` is what `richtext.chart_data` returns today - series, categories,
  values - so **Write's chart is unchanged**: `pageset`'s `set_chart` calls
  `chart.frame`, and `docxwrite` calls `chart.part(richtext.chart_data(t),
  kind)` with no references, the numbers written into the part as now.
- Sheets hands it `book:series(t, range, "rows" or "columns", most)`, and an
  XLSX's chart gets `refs` - `c:numRef` with `Sales!$B$2:$D$2` and the
  numbers cached beside it, so Excel's chart follows Excel's cells.
- **A chart over a long range is reduced before Lua sees it**: `most` is
  about the chart's width in pixels, and `series` gives a line or an area the
  smallest and largest value in each slice of the range rather than every
  value. A chart of 100,000 numbers is then a few hundred points of `art`,
  which is what it looks like anyway. Columns and pies over that many
  categories are refused with the reason - nobody can read them.

**Present's draft says it differently**: `pageset.chart_plan` kept, and
`chart_part` moved whole into `ooxml.lua` as `ooxml.chart_part(t)`. The
difference is one decision, and *Shared with Kosmos Present* puts it.
Whichever way it goes, Sheets needs the same three things: the kinds grown
in one place, a part that takes data and references rather than a Write
table, and one list of colours.

---

## The file

**A `.sheets` file is `docfile`'s zip**, as `.write` and `.present` are:

- `document` - **`sheetsdoc.lua`**'s document as `tabletext`, first: its
  sheets in order, what stands on each and where, each table's name,
  columns' widths, header and footer counts, look, filter and sort; the
  charts with the range each reads; the text boxes' bodies; the styles - each
  a look (`richtext`'s character fields: face, weight, size, bold, italic,
  colour) with a fill, an alignment and a format; the comments; the print
  setup. Read as somebody else's file, field by field, as `writedoc.check`
  reads Write's, and **saved and opened again equal to itself**, which is
  the test.

  ```lua
  -- kosmos: table
  {
    format = "kosmos-sheets", version = 1,
    paper = { name = "A4", width_mm = 210, height_mm = 297, landscape = true },
    fit_width = true,
    styles = {
      { name = "Body", face = "IBM Plex Sans", weight = "Regular", size_pt = 10,
        colour = "#1b2330", align = "auto", format = { kind = "automatic" } },
      { name = "Money", face = "IBM Plex Sans", weight = "Regular", size_pt = 10,
        colour = "#1b2330", align = "auto",
        format = { kind = "currency", symbol = "$", places = 2, thousands = true } },
    },
    sheets = {
      { name = "Q3 2026", things = {
          { kind = "table", name = "Sales", cells = "tables/1.cells",
            x_pt = 24, y_pt = 64, columns_pt = { 120, 64, 64, 64, 72 },
            header_rows = 1, header_columns = 1, footer_rows = 1, look = "Classic" },
          { kind = "chart", chart = "column", from = "Summary!A1:D3", by = "rows",
            x_pt = 420, y_pt = 220, w_pt = 330, h_pt = 190 },
      } },
    },
  }
  ```

- `tables/<n>.cells` - **each table's cells, written and read by the kit**,
  one line a cell that is not empty: where, what was typed, its value as last
  computed, its style. Text, because a document is a file somebody may read
  (`write.md`, W1), and a person who unzips one finds the formulas:

  ```
  B2	4180	4180	2
  B7	=SUM(B2:B6)	14490	3
  ```

  Tabs and line breaks inside a cell are escaped. Written from the store
  into a region in C and deflated from it; read back into a region,
  inflated, and parsed by the kit from there - every line checked, every
  formula through the parser. **The values are what is shown while the book
  recalculates on opening**, in slices like any recalculation, so a large
  book opens at once and settles; a value edited by hand in the file is
  corrected by the recalculation, never believed.
- `pictures/<n>.<ext>` - the pictures, as `docfile` keeps them.

**What `zip.lua` gains**: an entry written **from a region** (`{ name,
region, size }`), beside Present's `zip.read_region` and its entry copied
from another archive. 100,000 rows by six columns is about 12 MB of cells
as text and a few MB deflated; none of it is a Lua string at any point.
`docfile.MOST` - 4 MB of document text - stays what it is and applies to
`document` alone, which holds no cells. **Saved whole, in one write**, so
under `/Home` a save is one journalled transaction: after a power cut, the
old book or the new one.

---

## XLSX out, and in

**One table, one worksheet, named as the table is** - which is what makes
`Costs!E7` mean the same thing in both. The sheet a table stood on and where
on it are lost to Excel, which has no canvas; Numbers' own export makes the
same choice. What maps:

| Kosmos Sheets | Excel |
|---|---|
| a table | a worksheet of its name |
| its header rows and columns | the first rows and columns, bold, the panes frozen there |
| its footer rows | ordinary rows below the body, with their formulas |
| a value, a formula | `<c>` with `<v>`, and `<f>` - one `t="shared"` for a formula filled down |
| the strings | `xl/sharedStrings.xml`, the kit's pool read out |
| a style | `xl/styles.xml`: a font, a fill, the format's code, one `cellXfs` entry each |
| a column's width | `<col width>`, from points |
| a chart | `xl/charts/chartN.xml` from `chart.part` with references, anchored by `xl/drawings/drawingN.xml` in its first series' worksheet |
| a comment | later |
| a text box, a shape, a picture | later, said in the bar when it is left behind |

The package - `[Content_Types].xml`, the relationships, `docProps/core.xml` -
is **`ooxml.lua`**'s, taken out of `docxwrite.lua` as Present's design takes
it (`ooxml.package`), so DOCX, PPTX and XLSX are one package with three
vocabularies. SpreadsheetML is **`xlsx.lua`**: the workbook part, the styles
part, a drawing per worksheet with a chart. **The worksheets and the shared
strings are the kit's** - `book:xlsx_out`, `book:strings_out` - written in C
from the store into regions, because a worksheet of 100,000 rows is 600,000
`<c>` elements and that is a loop over cells. `<calcPr fullCalcOnLoad="1"/>`
asks Excel to compute everything itself on opening, so nothing rests on
Sheets' values being Excel's to the last digit.

**A table larger than Excel's grid** - 1,048,576 rows or 16,384 columns -
is refused, by name, before anything is written. Sheets has no such limit;
Excel does, and only the export meets it.

**In: the XML Kit.** Expat is in the tree already (`runtime/upstream/expat`,
2.8.5, MIT, its notice beside it) and built today only into the images
that carry the browser (`WEB_SRCS` in the `Makefile`), under libdom's XML
binding. It becomes a kit of its own, `user/kits/xml/`, linked into every
image, and the browser's libdom binding uses that copy - one expat, not a
second:

- `xml.tree(region, size, most)` - a small part (the workbook, the styles,
  a drawing's anchor) as Lua tables, with a ceiling on its size;
- `xml_kit.h` - events in C, for a kit that wants them without Lua between:
  the Cells Kit's `book:xlsx_in` reads a worksheet and its shared strings
  straight into the store.

What comes in: every worksheet as a table on a sheet of its own, its name
the worksheet's; values; formulas, through the parser; number formats by
their codes; columns' widths; the frozen rows as header rows. What does
not, said: charts, drawings, pictures, comments, merged cells and
conditional formats - left behind and counted in the bar. And the XML Kit
is the door Write needs to open a DOCX and Present a PPTX, which both
designs list as later.

## CSV out, and in

The Cells Kit's (`book:csv_in`, `book:csv_out`), a loop over bytes into
and out of the store: RFC 4180's quoting, commas or semicolons or tabs
chosen when opening and found by looking at the first lines, UTF-8 with or
without the mark Excel puts first. **Each field goes through the same
recogniser a person's typing does**, so `5/10/2026` and `$1,200` arrive as a
date and an amount. One table out, as what it shows - values formatted,
not formulas. **Pasting tabbed text into cells is the same reader** with a
tab between fields, so a range copied from a web page or from Write arrives
as cells.

## PDF out

**A sheet as printed is a page of frames** - Present's shape (`present.md`,
*A slide is a page of frames*): the text boxes, shapes, pictures and charts
as frames at their places, each table's cells as a frame of pieces and
`art` built from `book:get`'s formatted values and the table's look, its
header row repeated at the head of each page it runs on to, as Write's
tables are. Fitted to the paper's width by a scale on each frame, which is
one thing Sheets asks of Present's frames beyond a place: `q s 0 0 s x -y
cm`, the PDF's own matrix. Then `pdf.write` - faces as subsets, `Identity-H`,
`ToUnicode` - with nothing written twice. **Held as Write's PDF is held**:
read back by Kosmos's own reader drawing every glyph, by this Mac object by
object, and by macOS's `sips`.

**What it costs, said**: `pdfwrite.write` makes every page's operators
before it writes the first object, so the fonts are known first. A sheet of
a few hundred pages is that and fine; one of 100,000 rows is about 2,500
pages and is not. Pages written as they are made, with the fonts after
them - which a PDF allows, since an object is found through the
cross-reference table, not by its order - is `pdfwrite`'s change to make
when a sheet needs it, and until then such a sheet is refused with its page
count.

---

## The window

As drawn in `docs/sheets.html`: a direct window (`ui.window{ direct = true,
header = true }`), the kit's title band, then

- **the tools** (`pk.toolbar`): View, Zoom and Formula at the left; Insert,
  Table, Chart, Text, Shape, Media and Comment in the middle - Write's tools
  where they are Write's, doing in Sheets what they do there; Export,
  Format, Organize and Document at the right. A note between the groups, as
  Write's;
- **the sheets' tabs** (`pk.tabs`, at `ui.tabs`' numbers: 32 tall, the chosen
  one on the panel's colour with the accent two pixels deep along its foot),
  and `+`;
- **the formula bar**: the chosen cell's name (`Summary E4`), `fx`, and its
  formula in a `textbuf` - the same buffer the cell shows when it is edited
  in place, one line, its references coloured from `book:spans`;
- **the canvas**, white, and **the panel** at the right that Format,
  Organize and Document switch (`inspector.lua`):
  - **Format**: *Table* (its look, header rows, header columns and footer
    rows as steppers, its name shown, gridlines, alternating rows), *Cell*
    (the format and its places, separator, negatives and symbol; a fill),
    *Text* (`ins:text`, Write's part, handed the cell's style as a look), and
    *Arrange* (where and how large, front and back - Present's part, which
    moves into `inspector.lua` the day a second application wants it, which
    is this one);
  - **Organize**: *Sort* and *Filter*;
  - **Document**: *Document* - the paper and its side, fitted to width,
    page numbers - and *Sheet*, its name.

**What a key does is the window's**, and each one is a call or two into the
kit: arrows, Tab and Return move the choice and Shift widens it; typing
starts an edit; Return or Tab ends it with `book:set`; Escape abandons it;
Delete empties the chosen cells; Control-Z and Control-Y are the kit's or
the document's undo; Control-C, X and V copy, cut and paste.

---

## Region or message

**In regions**:

- **the window's pixels**: two surfaces in one region the window manager
  can see (`ui.lua`'s `direct_region`), handed over once;
- **the kit's cells**: pages of this process's own, never shared and never
  copied - the window draws from them in place;
- **every file's bytes**: a `.sheets`, a CSV, an XLSX's parts and a PDF are
  read into regions and written from them (`regions.read_whole`,
  `zip.read_region`, `fs.write_from`); the cells go from the store to a
  region to the deflater to the disk without being a Lua string.

**In messages**:

- to **`/Running/wm`**: the window's opening, `commit` with the rectangle
  that changed, its events, its lights, and the clipboard - `wmproto.copy`
  takes **at most 1,900 bytes** (`wmproto.CLIP_MAX`), which a range of cells
  is soon more than. Inside Sheets a copied range stays in the kit and is
  pasted whole; another application gets the tabbed text as far as it fits,
  and the bar says how much was taken. A clipboard in a region is the window
  manager's change and Diego's call (*Yours to decide*);
- to **`/Home`**, **`/Drives`**, **`/Kosmos`**: a file's attributes, a read
  into a region, a write from one;
- to **`/Devices/clock`**: the time, when a book opens or a volatile cell
  recalculates.

**Nothing that recurs because a clock says so is a message**: a frame is
pixels in the region and a commit that names the buffer.

## C or Lua, and why

- **C, the Cells Kit**: the store, recognising what was typed, the parser,
  the graph, recalculating, the functions, formatting, sorting, filtering,
  the `.sheets` cells, CSV, the worksheets and shared strings of an XLSX in
  and out, and the grid painted. Each is a loop over cells or bytes on the
  path to the screen, and the cells have to be where those loops are.
- **C, the XML Kit**: expat, as released.
- **C, already written**: the gfx Kit's pixels and glyphs, the Compression
  Kit's deflate, the PDF Kit's faces as subsets.
- **Lua**: `sheets.lua`, which orchestrates; `sheetsdoc.lua`, a document's
  fields and checks; `canvas.lua`, rectangles and handles; `chart.lua`, a
  few hundred points into `art`; `inspector.lua`, `pk.toolbar`, `pk.tabs`,
  what a press means and what is drawn where; `xlsx.lua` and `ooxml.lua`,
  the structure of a package - a few dozen parts, a decision about each.
- **Why the line holds**: what is left in Lua on a frame is a handful of
  calls, a view's dozen numbers and one cell read back - none of it grows
  with the sheet. And what does it allocate, which `CLAUDE.md` says to ask
  of a Lua process on a deadline before its language: the reused `into` and
  `view` tables, and the commit's message.

---

## The busiest path

### A change that 10,000 cells depend on

A person types `4500` into `Sales!B2` and presses Return. Ten thousand
formulas read it, directly or through others - a projection, row after row
of `=B2*growth^n` - and some of them are on the screen.

| | Where | Crossings | What the repository knows |
|---|---|---|---|
| 1 | The key reaches the window | a message | a round trip to the window manager, 0.31 ms (`gfx.md` 19.4); an IPC round trip on the M700, 19.0 us (`testing.md` 18.354) |
| 2 | `book:set`: recognised as a number, the old content into the undo journal, stored, the 10,000 dependents marked | 1 | the crossing, about 9 us under QEMU (`gfx.md` 19.11); **the marking is not measured** |
| 3 | `book:recalc`: Kahn's order over the dirty set, 10,000 compiled formulas evaluated, the tables whose shown cells changed returned | 1 | **not measured** - S1 times it on the Mac's own cores, and the budget it has to meet is below |
| 4 | `book:draw` for each table on the screen: about 300 cells formatted and set | 1 a table | **a grid's glyphs are not measured**; the browser's test page's text is 0.8 ms under QEMU (`testing.md` 18.316); a fill on the M700 is 4,914 Mpx/s, so the canvas's ground is 0.13 ms |
| 5 | The rest of the frame, in Lua: a chart whose range changed planned again; the formula bar, the choice's outline, the panel when what it shows changed | a few dozen | a chart is tens of `art` entries; **Write's own frame has never been timed** (`present.md` says so too) |
| 6 | `win:commit` with the rectangle that changed | a message | 0.31 ms; the window manager composes it - 9,865 Mpx/s for opaque pixels on this Mac's cores (`gfx.md` 19.14), so the canvas is under 0.1 ms; 42 ns a pixel under QEMU |

**The budget is a frame - 16.7 ms - on real cores.** Steps 1, 2 and 6 are
measured or bounded and come to well under a millisecond; step 4's ground is
a tenth of one. **What decides it is step 3 and step 4's glyphs**, both in
C, both unmeasured, and the design is built so that both can be measured on
the Mac before a window exists: S1 times 10,000 dependents and a `SUM` over
100,000 rows natively and through Rosetta, and S3 adds `sheets --bench`,
which prints each stage of a frame as the window manager's `frames` does,
with what each allocates. If step 3 does not fit in about 4 ms, the
recalculation is already sliced (`book:recalc(budget)`), so the frame is
kept and the last cells arrive a frame later - bounded, which is the
promise, rather than merely quick.

**What is not on the path, by construction**: a Lua table per cell, a
string per cell, a message per cell, and a crossing per cell. The last
would be 10,000 crossings - **90 ms under QEMU before any arithmetic** at the
measured 9 us each - and the first is the 0.56 times of `testing.md` 18.128
paid on every operand.

**Under QEMU** the composing alone is 42 ns a pixel, about 27 ms for the
canvas, so the suite will see a frame of two dozen milliseconds or more;
that is TCG, which detects a regression and claims no speed. `make fast`
does not boot on this Mac today (`state.md`), so the native numbers come
from the host test and from the M700.

### Scrolling 100,000 rows

A wheel turn, or the scroller dragged:

| | Where | What the repository knows |
|---|---|---|
| 1 | The event, and the view's offset moved in Lua | the round trip, 0.31 ms |
| 2 | `book:draw` for the tables in view: the first row found through the block totals, the rows after it walked, the frozen headers drawn over them | independent of the table's size by construction; the glyphs not measured |
| 3 | Things on the canvas that did not change blitted from their surfaces | the M700 blits 4,250 Mpx/s |
| 4 | `commit` | 0.31 ms |

**The nearest measured thing is the browser's scroll frame**, a direct
window of 884 by 584 pixels under `hvf` - 2.8 ms of blit, 2.8 of commit, 5.7
in all, and 1.02 KB allocated a frame (`state.md`, *The browser was
measured*). That commit has since dropped from a wait to 0.31 ms. Sheets'
frame swaps the blit of a page for the grid painted afresh; whether that is
cheaper or dearer is S3's measurement, and if it is dearer, the frame
changes rather than the promise: the body painted into a surface once and
moved by a blit as it scrolls, with only the newly shown rows painted -
which is Groove's lesson (`testing.md` 18.266, 72 ms a frame to 10.1 by
drawing only what changed). The window's two buffers mean a part drawn into
one is stale in the other, so drawing less keeps a damaged rectangle for
each, as Groove does.

---

## Shared with Kosmos Present

Present is designed beside this (`docs/present.md`, its *Shared with Kosmos
Sheets*). Where the two would share a kit, the name and the door are put
side by side here; where they differ, it is said, and one is chosen before
either is built.

| Kit | Present's draft | Sheets | Agreed? |
|---|---|---|---|
| `docfile.lua` | `.present` | `.sheets` | yes |
| `<app>doc.lua` | `presentdoc.lua` | `sheetsdoc.lua` | yes - the pattern |
| `canvas.lua` | `canvas.new{ snap_pt }`, `things`, `at`, `press`, `drag`, `release`, `guides`, `draw`, `order`, `align` | used as drawn there | yes |
| `ooxml.lua` | the package, EMU, presets, fills, pictures, a text body | the package, under `xlsx.lua` | yes |
| charts | **`chart.lua`** (was `pageset.chart_plan` kept and `ooxml.chart_part`) | **`chart.lua`**: plan, frame, part, the one colour list | yes - settled 5 October |
| `inspector.lua` | `inspector.new(pk, { name })`, `ins:text`, `ins:shape`, `ins:picture`, `ins:table`, `ins:chart`, `ins:control`, `ins:press` | the same; Sheets' Table, Cell, Sort and Filter parts through `ins:control`; **Arrange moves in as `ins:arrange`**, Sheets being the second user Present's draft waits for | yes; Arrange to move |
| `textedit.lua` | a box's text, the notes; "a cell edited in place" as Sheets' likely use | a text box's words. **A cell is edited with `textbuf.lua`**, which the IDE's editor stands on: one line of plain text, its colours from the kit's spans, no looks inside it in the first version | to note |
| `pageset.frame`, `page.frames` | every thing on a slide | text boxes, shapes, pictures, charts; a sheet as printed - **plus a scale on a frame**, for fitting to width | yes, with the scale |
| `pagedesk.lua` | the slide and the navigator | Print Preview, later | yes |
| `pk.toolbar`, `pk.tool_at` | the tools | the tools | yes |
| `pk.tabs` | - | the sheets' tabs | Sheets' |
| `zip.lua` | `zip.read_region`; an entry copied from another archive | both, and **an entry written from a region** | yes, with the third |
| the XML Kit | - | XLSX in | Present's PPTX in, later |
| the Cells Kit | - | everything in a cell | a table with formulas on a slide, later - and in Write |

**The chart was the one to settle, and it is `chart.lua`** (5 October;
`present.md` says the same). Present's draft kept the plan in
`pageset` and moves the part into `ooxml.lua`: the fewest moves. Sheets'
proposal is `chart.lua`: one door for what a chart is, on the screen, on
paper and in Office's files - because the colours are already two copies,
because Sheets grows the kinds (area, scatter), the axis's figures and the
references, and because neither a paragraph setter nor a package writer is
where somebody looks for a chart. Either way the same three things change.
**Recommended: `chart.lua`**, with `ooxml.lua` the package and nothing about
charts.

---

## What is new, and the order to build it in

New code, as a list:

- `user/kits/cells/` - **the Cells Kit**, about a dozen files (*The Cells
  Kit*, above), and `kosmos_cells_kit` in `sys_user.c`'s list;
- `user/kits/xml/` - **the XML Kit**: expat moved out of the browser's
  sources into a kit every image has, its door and `xml_kit.h`;
- `user/bin/apps/sheets.lua` - the application (`wm sheets`; `-- kosmos:
  name Kosmos Sheets`, `-- kosmos: opens sheets csv xlsx`; `.sheets` a
  "Kosmos Sheets document" in Documents in `filetypes.lua`);
- `user/lib/sheetsdoc.lua`, `xlsx.lua` - new;
- `user/lib/chart.lua` - moved, out of `pageset.lua` and `docxwrite.lua`;
- `user/lib/canvas.lua`, `inspector.lua`, `ooxml.lua`, `textedit.lua`,
  `pk.toolbar` - Present's P0, shared, done once by whichever comes first;
- `pk.tabs` in `pixelkit.lua`; an entry from a region in `zip.lua`; a scale
  on a frame in `pagedraw.lua` and `pdfwrite.lua`;
- `tools/test_cells.c` (and its x86 twin through Rosetta),
  `test_sheetsdoc.lua`, `test_chart.lua`, `test_xlsx.lua` on the Mac;
  `tools/run_sheets.py` in the machine, as `arm-sheets` (the files) and
  `arm-sheetsapp` (the window), each split in halves if it nears the ten
  minutes (`CLAUDE.md`, *The tests take five to ten minutes*).

Each step its own revision and its own test, as Write's W1-W7 were:

- **S0 - What Write lends, moved first.** If Present's P0 has not happened:
  `ooxml.lua`, `pk.toolbar`, `inspector.lua`, `textedit.lua`, Write moved
  onto each and its suites passing unchanged as the proof. Then
  **`chart.lua`** out of `pageset.lua` and `docxwrite.lua`, the one colour
  list with it: `test_pageset.lua` 117 to 128, `test_docx.lua` 44 to 51,
  `arm-write` and `arm-writeapp` unchanged. No Sheets yet.
- **S1 - The Cells Kit, on the Mac.** The store, the recogniser, the parser
  and its spelling back, the evaluator, the graph, Kahn's order, cycles,
  errors as values, twenty functions. `test_cells.c`, natively and through
  Rosetta: every function against values worked by hand and checked in
  Numbers; a formula filled down is one formula; inserting and deleting rows
  and the references that follow; a cycle found; a parser held to 64 levels.
  **And timed**: 10,000 dependents of one cell, a `SUM` over 100,000 rows -
  the number step 3 of the busiest path waits for. No window.
- **S2 - The door and the file.** `kosmos_cells_kit`; `sheetsdoc.lua`; the
  cells written and read through regions, and `zip.lua`'s region entry.
  On the Mac: a book out and back equal to itself, and every refused shape
  refused with its reason. In the machine: a book made, saved, opened.
- **S3 - The window, and one table.** `sheets.lua` as drawn: the header, the
  tools, the sheets' tabs, the formula bar, a canvas with tables drawn by
  `book:draw`; choosing, typing, Return and Tab and the arrows, the editor's
  coloured references, undo; **100,000 rows scrolled with the header
  frozen**. `arm-sheetsapp`; `sheets --bench`. **Held as Write's page is**:
  the cells on the screen read back against the kit's values.
- **S4 - The canvas.** Several tables, text boxes, shapes, pictures; moved,
  sized, Arrange; references across tables outlined in their colours on
  the canvas; sheets added, named, reordered.
- **S5 - Formats, and the panel.** Table, Cell, Text and Arrange; the
  formats; the rest of the eighty functions; the fill handle; Formula's
  quick sums; the foot's sum; copy and paste. `cells.format` held to
  `text.grouped`'s digits by `test_cells.c`.
- **S6 - Sort and filter**, Organize's two parts.
- **S7 - Charts** from a range, live; area and scatter added to `chart.lua`,
  and so to Write's Chart tool and Present's.
- **S8 - CSV in and out**, and tabbed text pasted.
- **S9 - XLSX out**: `xlsx.lua` on `ooxml.lua`, the kit's worksheets and
  shared strings, charts with references. Held as DOCX is: the parts read
  back on the Mac, cell by cell and formula by formula; and opened by Diego
  in Numbers or Excel.
- **S10 - PDF out**: a sheet as a page of frames, fitted to width. Held three
  ways, as Write's is.
- **S11 - XLSX in**: the XML Kit, the browser moved onto it (its suites
  unchanged); worksheets as tables, values, formulas, formats, widths. Held
  by S9's files read back in, equal to the book they came from.
- **S12 - Comments on cells, and the Document panel's print setup.**

PDF before XLSX is the other order and as good; Write put PDF first because
Diego said it mattered most, and for Sheets he has said XLSX and PDF in one
breath.

---

## Yours to decide

1. **The arithmetic.** Binary doubles, as Excel and Numbers, with what is
   shown and what is compared rounded to fifteen digits and `ROUND` for an
   exact cent - or decimal arithmetic, exact in cents and different from
   Excel. **Recommended: doubles.**
2. **How a reference names another table.** `Costs!E7`, a table's name
   unique in the document, no sheet in it - or Numbers' `Sheet 2::Costs::E7`.
   **Recommended: the table's name**: a table moves between sheets without
   breaking anything, and an XLSX needs no renaming.
3. **XLSX in, in the first version** (S11) or after it. Write left opening
   a DOCX for later because reading Word's layout well is the larger half;
   a worksheet's cells are not a layout, and people's spreadsheets arrive as
   XLSX. **Recommended: in the first version, after XLSX out.**
4. **How numbers and dates are written.** 1,234.56 and day, month, year
   (`5 October 2026`, `05/10/2026`) to begin with, the document recording
   the convention so that a region chosen later - 1.234,56 in Spanish - is a
   setting rather than a format change. Or the decimal comma from the first
   day. **Recommended: the point and day-first, recorded.**
5. **The clipboard's 1,900 bytes.** A range copied to another application
   is cut short there, and said to be; within Sheets nothing is lost. A
   clipboard held in a region is the window manager's change. **Recommended:
   later, as its own step, since every application gains from it.**
6. **The chart kit** (*Shared with Kosmos Present*): `chart.lua`, or
   Present's draft - the plan kept in `pageset`, the part in `ooxml.lua`.
   **Recommended: `chart.lua`.**
7. **The icon.** None of the fifty Haiku icons in `assets/icons` is a
   spreadsheet; whether Haiku's artwork at the pinned commit has one to
   fetch is to be looked at. The drawing uses a sheet drawn for it.
   **Recommended: one more Haiku icon if there is one, as the others came.**
8. **A sheet of thousands of pages to PDF**: refused with its page count
   until `pdfwrite` writes pages as it makes them, or that change made now.
   **Recommended: refused for now**, the change made when a real sheet
   needs it.
