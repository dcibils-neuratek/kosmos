<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# The review before 0.12.0

Diego, 18 November 2026: "I think we can now do a full code cleanup and
review before we continue", "There is for sure old code that we don't use
anymore and we can optimise the entire system". `CLAUDE.md` asks for this
before every minor version, and there had not been one since 0.11.0.

## In short

- **Eight readers, one per area**, read the tree in parallel on 18
  November: Nebula and below, the servers and drivers, the C kits and the
  runtime, Astra, the applications, the libraries and programs, the build
  and tests, and the documents. They changed nothing. They reported **205
  findings**, each with the grep or the reading that proves it.
- **Every finding is checked again before anything is removed**, and lands
  in a revision of its own batch with its suites passing.
- **The tree is in good shape at the level a compiler sees.** `-Werror`
  already catches unused statics, every syscall has a caller, every
  library is loaded by something. What is left is the shape of what the
  compiler cannot see: a public function whose last caller went, a comment
  describing a system that was replaced, a second copy of a helper.
- **Most of the dead code has one cause**: the old browser removed in
  0.11.461. Its C stayed in the web kit for Mail and about two thousand
  lines of it now have no caller; `http.lua`, `browserprefs.lua`,
  `longmenu.lua` and the clock and history libraries carry its options.
- **Six real bugs**, fixed first (batch 1).

## The batches, in the order they land

| # | Batch | Findings | Status |
|---|---|---|---|
| 1 | **Bugs**: `/Running` unregister by anybody and capabilities kept for ever; the interrupt paths' three copies merged (AArch64 woke neither the audio server from EL0 nor the input reader from EL1); Telnet's command split over two reads; Pewter's menu bar listed as an application; Video not opening `.mov` and `.m4v` | 113, 114, 155, 156, 202, 203, 32, 33, 34 | **0.11.508** (`testing.md` 18.908) |
| 2 | **The old browser's remains**: the web kit's forms, fetching, zoom, sheets, imports and cost timers; `http.lua`'s cache, streaming and refresh; `browserprefs`' agents and roots; `longmenu.lua`; `clock.days`, `history.lately`; the comments naming `browser.lua` and NetSurf as a browser | 40-44, 93, 95, 96, 107, 134, 144, 145, 151, 152, 176-183, 185, 194, 201 | **0.11.509** (`testing.md` 18.909) |
| 3 | **Dead code elsewhere**: functions, constants, bindings and fields nothing calls | 35-38, 57-61, 97-102, 115-121, 137, 138, 146-149, 157-159, 175, 184, 186-189, 205 | **0.11.510** (`testing.md` 18.910; 102, 159, 175, 189 and 205 kept, with reasons) |
| 4 | **One copy**: byte order through `bytes.h`, one UTF-8 header, one opaque mix, `strcasecmp`, one HTML-to-text, one size and rate formatter, one monitor heading, `theme.mix`, `pulse.name`, one place where `needs` become spawn flags, PNG through the Compress Kit, CRC-32 | 45-47, 62, 80-82, 103-106, 128-131, 190-193, 200 | C half (103-106, 128-131) **0.11.514** (`testing.md` 18.914), the rest **0.11.515** (18.915) |
| 5 | **Comments and documents made true**: every stale comment, `CLAUDE.md`'s Build, Layout and HAL sections, README, `state.md` brought to 0.11.507 and cut down, the logbook, the document map's statuses | 1-29, 31, 39, 49, 51, 52, 64-79, 90-92, 109-112, 122-127, 139-142, 150, 160-168, 195-199, 204 | **0.11.516** (`testing.md` 18.916) |
| 6 | **Work nobody reads**: per-pass allocation in `wm` and the Deskbar, a surface per region window, the clock read in a repaint, a busy poll | 53, 54, 56, 83-86 | - |
| 7 | **Measured before anything changes**: the image canary hashing 44 MB four times a boot, AArch64's broadcast TLB flush per switch, the sleeper scan over every thread slot, a thread index found in O(n²), the VMCS flushed per guest run, a table per drawing op, the display harness's Python pixel loops, the Deskbar's gradient | 87, 88, 154, 165, 169-173 | - |
| 8 | **Tests nothing runs**: `test_layout.py` (three documents say it fails the gate), `test_pdf.lua`, `run_smpshot.py`; sixteen ARM-only suites with no x86 twin | 132, 133, 135, 136 | - |

## Found while checking

- **206.** `/Running` holds `APP_MAX` = 24 names in a fixed table
  (`user/include/appproto.h`), against *no compiled-in limits*. With every
  application open a twenty-fifth is refused "too many applications
  registered". To grow as the other tables have.
- **207.** The gate ran 11:39 on 18 November (`testing.md` 18.909),
  past the ten minutes `CLAUDE.md` allows; `x86-dock` took 353 s and
  `x86-cafesa3d-2` 302 s. **Done in 0.11.511, ahead of the batches**
  (`CLAUDE.md`: a gate past ten minutes is fixed before anything else
  lands): the suites wait far more than they compute, so the gate runs
  twelve at once rather than ten - 9:48 with every suite (`testing.md`
  18.911).

## Diego's to decide

**Diego, 19 November 2026: "Yea to all".** One to six are 0.11.512
(`testing.md` 18.912); seven, `NR_CPUS` from the machine, is 0.11.513
(`testing.md` 18.913).

1. **Paint** (55) is a demonstration of the brush path with no Open, Save
   or undo, listed among the applications. *Recommended: move it to Demos*
   until *Paint, redesigned* is built.
2. **The web kit's own HTML layout** (94): about two thousand lines
   (`web_paint.c` and half of `web_style.c`) that only `make web`'s probe
   still runs. Mail draws with NetSurf, and `mailweb.md` replaces that with
   WebKit. *Recommended: remove it, and the probe.*
3. **The Deskbar's seed migration** (89), which moves September's seeded
   launchers to the Trash once and runs at every start. *Recommended:
   remove it*, if every `/Home` in use has started a Deskbar since 28
   September - the ThinkPad's stick and the M700's disk both have.
4. **`run_site.py` and `record_site.py`** (153), W6d's offline painting of a
   saved site, from before JetSki loaded pages itself. *Recommended:
   remove both.*
5. **`make release`** (143) links one image per screen size although any
   size is chosen at boot. *Recommended: one image.*
6. **The three stray directories at the top of the tree** (30) - `TEST=1`,
   `apps-web-ffmpeg` and `webkit_kit.d`, untracked build leftovers.
   *Recommended: delete them*, and find what wrote them.
7. **`NR_CPUS` = 8** (174), which the Mac Pro already exceeds.
   *Recommended: derive it from the machine at boot*, as the pools are.

## The findings

Every one, numbered as the batches cite them, is in
`docs/cleanup-findings.md`; each batch's revision says in `testing.md`
which it took and what checked it.
