# Shortcuts

An application, in the dock and the Kosmos menu, that shows every key
Kosmos answers to: the desktop's, the windows', text editing's, and each
application's own. They are grouped, searchable, and drawn as keys rather
than written as words. Diego, 10 October 2026: "a better shortcut app to show all available
shortcuts in a nice way", "right now is very raw", "a real app that can be
launched from the dock, called Shortcuts", "can be also extended as a help,
cheatsheet window in the future", "make it look nice".

Drawn the same day in `docs/shortcuts.html` and agreed ("All mockups are
perfect"), with the drawing's three questions answered as recommended:
- the dark window, as drawn;
- menu items get a shortcut field and each application declares its keys,
  as part of this;
- Print as a cheat sheet now.

This document is the feature set and the architecture; `shortcuts.html` is
the window and `shortcuts-architecture.png` the picture of what it stands
on.

## What a person can do

- **Open it** from the dock, the Kosmos menu, or Super+/. Super+º opens the
  same window over everything, closed by Escape, as it does today.
- **See every key, grouped.**
  - The sidebar lists the system's groups first:
    - All;
    - The desktop (Super's keys);
    - Windows (Alt+Tab and Control-W's);
    - The screen (captures);
    - Text (what every field and editor answers).
  - Then comes one row per application that declares keys, each with its
    icon and how many keys it has.
  - Choosing a row shows its keys on the main side.
- **Read a key as a key.** Each shortcut is drawn as keycaps - Super, then
  Tab - with what it does in words beside it. A sequence reads as one:
  Control-W, *then* Q.
- **Search everything at once.**
  - Typing searches what a key does and the keys themselves. "ctrl w"
    finds the Control-W rows; "close" finds every key that closes
    something.
  - The matches are shown under their group or application, with the
    matching words lit.
  - Escape clears the search.
- **Print as a cheat sheet.** The whole list, as a PDF in
  `/Home/Documents/Shortcuts.pdf`, to keep beside the keyboard. It opens in
  the PDF viewer once written.
- **Always the keys in force.** Nothing in the window is a list of its own:
  - the desktop's keys come from the window manager, which decides them;
  - text's come from the UI kit, which answers them;
  - an application's come from that application's declaration, which the
    kit also uses to draw its menus and answer its keys.

  So the window cannot drift from what the keys do (*One list* below).

**Not in it yet**: changing a key (that is Preferences' Keyboard pane, when
keys can be rebound), and Help - a page for each application with its keys
at the end, the "help, cheatsheet window in the future". The sidebar has
room for Help to become another group.

## Where each piece comes from

The premise (`CLAUDE.md`, *Kits supply, applications orchestrate*): we ask
of every piece whether another application wants it, and whether something
already supplies it.

| Piece | Supplied by | New? |
|---|---|---|
| The desktop's, windows' and screen's keys, with their words | **window manager**: `handlers.shortcuts` in `wm/keys.lua`, from the same arrays it dispatches from | exists; split into the drawing's groups |
| Text editing's keys | **UI kit**: `ui.TEXT_KEYS`, declared beside the text field's key handling, which then dispatches from it | new, in the kit; the field's comparisons become a table |
| A chord as words and back: "Ctrl+Shift+S" to a key number, a number to its name, "ctrl"/"control", "cmd"/"win"/"super" as one | **keys.lua**: `keys.chord`, `keys.spell` | new, in `keys.lua`; menus, Shortcuts, Preferences' Keyboard and the IDE want it |
| Keycaps drawn: a chord as raised keys, a sequence with "then" | **UI kit**: `g:keycaps(x, y, chord)` and `ui.keycaps_width` | new, in the kit; menus draw their shortcuts with it, and so do Preferences, the IDE's lessons and the switcher's hint |
| A menu item's shortcut, drawn in the menu and answered | **UI kit**: a menu item's `keys` field | new, in the kit; every application with a menu |
| An application's keys, readable without starting it | the application's header, `kosmos: key <chord> <words>`, read by **appkeys.lua** | new library; Shortcuts first, the Kosmos menu's tooltips and the IDE after |
| An application's icon and name | **binfs** / **diskfs** `getattr`: `icon`, `name` | exists |
| The sidebar, the search field, the scrolling list of keys | **UI kit**: `ui.sidebar`, `ui.field`, `ui.list` | exists |
| The cheat sheet as a document | **writedoc.lua**, the Write document | exists |
| Its pages and its PDF | **pageset.lua** and **pdfwrite.lua**, the PDF Kit writing | exists |
| Opening the PDF | the file type's application, through **filetypes.lua** | exists |

**Two copies this removes rather than adds.** The current window's "Editing"
group is a list the window manager keeps of keys the UI kit answers
(`EDIT_BINDINGS`). The kit's own table replaces it. `docs/cheatsheet.html`
becomes what Print writes, from the same lists, so it can never again be
silently missing the Super keys, as it was on 3 October.

## One list: how an application declares its keys

The question the drawing left open: an application's own keys are not
declared anywhere today. Each one is a comparison in its key handler, and
its menus do not carry them. So Shortcuts could not list them without
starting every application, and a copy written into Shortcuts would be
wrong the first time an application changed.

**The header declares them**, as it already declares the icon, the section
and the authorities an application needs:

```lua
-- kosmos: application
-- kosmos: icon App_Mail
-- kosmos: key Ctrl+N Write a new message
-- kosmos: key Ctrl+R Reply
-- kosmos: key Ctrl+Shift+R Reply to everyone
-- kosmos: key Ctrl+F Find in the mailbox
```

The first word is the chord, with no spaces; a sequence is two chords with
a comma between them (`Ctrl+W,Q`). The rest of the line is what it does, in
the words a person reads.

**A menu item names its key by the chord, and the kit does the rest:**

```lua
{ title = "Reply", keys = "Ctrl+R", run = reply }
```

The kit draws the keycaps at the item's right in the menu. While the window
has the keyboard, it answers Ctrl+R by running the item, before the
application's `on_key` sees it. So the dispatch, the menu and the list in
Shortcuts all come from one declaration.

**A check holds the two together.** `tools/check_appkeys.py`, in the host
suite, beside `check_needs.py`, which does the same for `needs`. It checks
that:
- every `keys =` in an application is declared in its header;
- no chord is declared twice in one application;
- no application declares a key the window manager takes first: Super's
  chords, Alt+Tab, Control-W. Those never reach a window, so a declaration
  of one would be a key that does nothing.

A key an application answers outside any menu, such as Maps' arrows or a
game's, is declared in the header and answered in `on_key` as now. The
check cannot see that comparison, and does not need to: the header is the
promise and the window is where a person finds it.

**Why the header and not a table in the program.** Shortcuts lists
applications that are not running, and reading a header is reading text.
Running a program to ask it its keys would be starting thirty processes to
draw a list. `binfs` already finds the header for `icon` and `needs`, and
the end of the header is the first line that is not a comment, so a
program's code can never declare a key by accident.

## The architecture

```
   shortcuts.lua (Lua: what a person does)
     the window, the sidebar of groups and applications, search, Print
         |
         |  fs.send("/Running/wm", {type="shortcuts"})   once, at open
         |  ui.TEXT_KEYS                                  a table in the kit
         |  appkeys.of(path)  for each application        once, at open
         |  g:keycaps(x, y, chord)                        each row drawn
         v
   UI kit (Lua)    ui.sidebar, ui.field, ui.list, keycaps, menus' keys
   keys.lua        keys.chord, keys.spell - one spelling of a chord
   appkeys.lua     a program's header: its `kosmos: key` lines
   writedoc / pageset / pdfwrite   the cheat sheet, as a PDF
         |
         v
   window manager   the desktop's keys, from the arrays it dispatches
   binfs, diskfs    /Kosmos/Apps and /Home/Apps: headers, icons, names
         |
         v
   gfx (C)  text, round fills; the window's region, composed
```

**What is C and what is Lua.** All Lua, and nothing in it is a loop over
bytes. The rows are a few hundred at most, and the window draws only the
dozen or two on the screen. Keycaps are a round fill and a line of text
each, drawn by gfx in C. Reading a header stops at the first line of code.

**What crosses where.**
- The window manager's list is one message, asked at open. Its keys are
  compiled into it, so asking again would only get the same answer.
- Headers are files read through `binfs` and `diskfs`: one-shot, as any
  file is.
- The PDF is written to the disk by the PDF Kit, in the window's own
  process.
- Nothing recurs, so nothing needs a region.

**The modal window** (Super+º) is the same program with `--modal`, as now:
a popup over everything, the same drawing, closed by Escape or a press
outside it.

## The busiest path: a key typed into the search

Each key typed into the search:
1. **Filter.** The query is split into words, and each word into its
   spellings (`keys.chord` knows "ctrl" is Control). Every row's words and
   keys are matched: a few hundred `string.find`s, well under a
   millisecond.
2. **Draw.** The rows that fit, grouped under their application: about
   twenty, each a keycap or three and a line of text, with the matched
   words lit. A few dozen gfx calls.
3. **Commit.** One commit, and the window manager composes.

**Opening** is the other one to watch:
- one message to the window manager;
- one header read per application in `/Kosmos/Apps` and `/Home/Apps`
  (about sixty, each a few hundred bytes of header);
- the icons, decoded once.

The aim is under 200 ms from Super+/ to the window drawn on the M700,
measured with the window's own log line. If reading headers is where the
time goes, `binfs` can answer a program's `key` lines as it answers its
`needs`. That is the change that would come next, written down so it is not
mistaken for the design.

## Steps

- **K1** - `keys.chord` and `keys.spell`: a chord's spelling to a key
  number and back, every alias, a sequence as two. Held on the Mac by
  `tools/test_keys.lua`.
- **K2** - the UI kit:
  - `g:keycaps` and `ui.keycaps_width`;
  - a menu item's `keys`, drawn in the menu and answered by the kit;
  - `ui.TEXT_KEYS`, from which the text field dispatches.

  The window manager's `EDIT_BINDINGS` goes. Held by a host test of the
  menu's dispatch and by the display harness.
- **K3** - `kosmos: key` and `appkeys.lua`, held on the Mac to a header's
  lines. `tools/check_appkeys.py` joins the host suite, with a control: an
  undeclared `keys =` in a test application, refused.
- **K4** - the applications declare. Each key they answer today is read
  out of its handler, declared in the header, and moved onto its menu item
  where it has one. Tracker, Mail, Web, the IDE, Music, Write, Maps,
  Cafesa3D and the Terminal come first.
- **K5** - the window, as drawn:
  - the sidebar, the groups, keycaps and search;
  - in the dock by default, named Shortcuts and listed among the
    applications rather than in Preferences;
  - Super+/ and Super+º as now.

  `tools/run_shortcuts.py`: the window opens with every group; Super+Tab's
  row reads as two keycaps; Mail's keys listed from its header; "ctrl w"
  finds the Control-W rows; Escape clears; Print writes a PDF of the right
  number of rows.
- **K6** - Print as a cheat sheet: the lists as a Write document, through
  `pageset` and `pdfwrite`, into `/Home/Documents/Shortcuts.pdf`, opened
  once written. `docs/cheatsheet.html` is retired in favour of it.

Each step leaves a permanent test. Only x86-64 runs them for now
(`CLAUDE.md`).

## For Diego to decide

1. **The header line**: `-- kosmos: key Ctrl+R Reply`, as above? The other
   form would be a table in the program that Shortcuts runs to read, and
   that starts every application to draw a list. Recommended: the header.
2. **Keys the kit answers from menus come before the application's own
   `on_key`.** So an application cannot shadow its own menu's key by
   accident. Recommended: yes.
3. **The order of applications in the sidebar**: A to Z, or the dock's order
   first and then A to Z? Recommended: A to Z, since it is a list to look
   things up in.
