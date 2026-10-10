# Shortcuts

An application, in the dock and the Kosmos menu: **an index of every key
combination that works everywhere in Kosmos**. That covers the desktop's,
the windows', the screen's and text editing's keys. They are grouped,
searchable, and drawn as keys rather than written as words.

Diego, 10 October 2026: "a better shortcut app to show all available
shortcuts in a nice way", "right now is very raw", "a real app that can be
launched from the dock, called Shortcuts", "can be also extended as a help,
cheatsheet window in the future", "make it look nice".

Drawn the same day in `docs/shortcuts.html` and agreed ("All mockups are
perfect"): the dark window, and Print as a cheat sheet now.

**System-wide keys only.** A first version of this design had every
application declare its own keys in its header, for Shortcuts to list.
Diego, the same evening: "The shortcuts app it's just an index of all system
wide key combinations that do something like taking a screenshot, maximize
windows etc", and "Why would i add an entry in every app for shortcuts?".
So applications' own keys are not in it, and no application changes for it.

**And it is where shortcuts are made and changed.** Diego, then: "we could
define and create new ones on that app or remap existing ones", "Like
Super+t opens a new terminal", "And changes should persist", "So it's a user
setting". The editing views were drawn into `shortcuts.html` and agreed
the same evening ("Mockup looks great"), with its two questions answered as
recommended: new shortcuts are Super chords only, and a shortcut opens an
application or program or does one of the system keys' actions.

This document is the feature set and the architecture. `shortcuts.html` is
the window, and `shortcuts-architecture.png` is the picture of what it
stands on.

## Global and local

Diego, the same evening: "Shortcuts are global keys mapping", "Then every
app has its own local app key shortcuts", and then "We can use ctrl as the
super key for local key mappings?", "Ctrl+n in mail app for new mail?".

So there are two layers, and each has a key of its own:

- **Global keys are Super's.** They are the keys in Shortcuts, made and
  changed there. They do the same thing whichever window is in front, and
  the window manager answers them before any application sees them.
- **Local keys are Control's.** An application's own keys, such as Mail's
  Control N for a new message. They work only while that application has
  the keyboard, the application answers them, and Shortcuts does not list
  them. Applications already work this way: the IDE runs a program on
  Control Enter and completes a word on Control Space.

**The two cannot collide**, because they are on different keys. That is why
it is better than Super for both, which was written here first:
- a global key made in Shortcuts could have silently taken a key from an
  application;
- Shortcuts could not have warned about it, since applications do not
  declare their keys.

**What Control is not free for**:
- **Control W** is the desktop's prefix (Control-W Q ends the desktop,
  Control-W and an arrow moves a window). An application never sees it;
  Control-W Control-W sends one through.
- **Control C, X, V and A** are text editing's: copy, cut, paste, select
  everything. An application uses them for exactly that and nothing else.
- **In the Terminal**, Control belongs to the program running inside it:
  Control C stops it and Control D ends input. The Terminal's own keys are
  Control Shift (Control Shift T for a new tab), as a terminal's are
  elsewhere.

**And Super stays the system's**: a Super chord that nobody made global
still reaches the window in front (`super_command` and `flush_super` in
`wm/keys.lua`), but applications do not use it for their own keys.

Applications' local keys are their own work, one application at a time:
Mail's Control N first (`roadmap.md`).

## What a person can do

- **Open it** from the dock, the Kosmos menu, or Super+/. Super+º opens the
  same window over everything, closed by Escape, as it does today.
- **See every system-wide key, grouped.** The sidebar holds:
  - All;
  - The desktop: the Kosmos menu, the launcher pad, the app switcher,
    closing and hiding;
  - Windows: Alt+Tab, Control-W's commands, moving a window, and Magnet's
    keys when Magnet is built;
  - The screen: captures;
  - Text: what every field and editor answers - copy, paste, select, words
    and lines.
- **Read a key as a key.** Each shortcut is drawn as keycaps (Super, then
  Tab) with what it does in words beside it. A sequence reads as one:
  Control-W, *then* Q.
- **Search everything at once.**
  - Typing searches what a key does and the keys themselves. "ctrl w"
    finds the Control-W rows; "screen" finds the captures.
  - The matches are shown under their group, the matching words lit.
  - Escape clears the search.
- **Make a shortcut.** *Add a shortcut* opens a sheet:
  - press the keys, such as Super T;
  - choose what they do: open an application or program (Terminal), or one
    of the things the system's keys do (close, hide, the switcher, a
    capture, and Magnet's once it is built);
  - the sheet says at once whether the keys are free.

  The shortcuts made here are listed under **Yours**.
- **Change one.** Every system-wide key has *Change*. Pressing it makes the
  row listen for new keys.
  - If the new keys belong to another shortcut, the row says so in words
    and offers the choice: take them and leave the other without keys, or
    press others.
  - A changed row says what it was, and *Default* puts it back.
- **Kept.** What is made or changed is a user setting, in
  `/Home/Preferences/shortcuts`. It works at once and on every start after.
- **Print as a cheat sheet.** The whole list becomes a PDF at
  `/Home/Documents/Shortcuts.pdf`, to keep beside the keyboard, and opens in
  the PDF viewer once written.
- **Always the keys in force.** The window keeps no list of its own:
  - the desktop's and windows' keys come from the window manager, which is
    what takes them;
  - text's keys come from the UI kit, which is what answers them.

  A key added to either shows up here without anybody touching Shortcuts.

**Not in it**:
- applications' own keys;
- changing text editing's keys (Control-C and the rest) or Control-W's:
  they are shown here, and changing them would change every application;
- chords with Control or Alt as new shortcuts. Only Super chords can be
  made: Control is the applications' (*Global and local* above);
- Help, the "help, cheatsheet window in the future". The sidebar has room
  for it to become another group.

## Where each piece comes from

| Piece | Supplied by | New? |
|---|---|---|
| The desktop's, windows' and screen's keys, with their words and group | **window manager**: `handlers.shortcuts` in `wm/keys.lua`, from the arrays it dispatches from | exists; each binding gains its group |
| Text editing's keys | **UI kit**: `ui.TEXT_KEYS`, declared beside the text field's key handling, which dispatches from it | new, in the kit; replaces the window manager's `EDIT_BINDINGS`, which was a copy |
| A chord's spellings for search: "ctrl" is Control, "cmd" and "win" are Super | **keys.lua**: `keys.spellings` | new, in `keys.lua`; Preferences' Keyboard pane will want it too |
| Keycaps: a chord drawn as raised keys, a sequence with "then" | **UI kit**: `g:keycaps(x, y, chord)` and `ui.keycaps_width` | new, in the kit; the switcher's hint, Preferences and the IDE's lessons want it too |
| The sidebar, the search field, the list of keys | **UI kit**: `ui.sidebar`, `ui.field`, `ui.list` | exists |
| The cheat sheet as a document | **writedoc.lua**, the Write document | exists |
| Its pages and its PDF | **pageset.lua** and **pdfwrite.lua**, the PDF Kit writing | exists |
| Opening the PDF | the file type's application, through **filetypes.lua** | exists |

| What each system key does, by name - `close`, `hide`, `switcher`, `capture`, `open` - so a shortcut can point at it | **window manager**: each binding in `wm/keys.lua` gains an `action` name; the dispatch table is built from the defaults and then the user's setting | new, in the window manager |
| Listening for keys the window manager would otherwise act on | **window manager**: `{type="keys", listen=true}` - the next Super chord is posted to the asking window instead of acted on, until it stops listening or the window loses the keys | new, in the window manager |
| The user's shortcuts, kept | **settings kit** (`prefs.lua`), as `shortcuts`; the window manager reads it when it starts and is told when it changes, as it is told about the Super key's "Pressed alone" | exists; a new setting |
| The sheet over the window | **UI kit**: `ui.sheet`, moved out of Mail, whose Add Account draws one of its own today | new in the kit; Shortcuts is its second user, so it is a kit piece and Mail moves onto it |
| The sheet's choices and buttons | **UI kit**: `ui.dropdown`, `ui.button` | exists |

**Two copies this removes.** The current window's "Editing" group comes from
a list the window manager keeps of keys the UI kit answers
(`EDIT_BINDINGS`); the kit's own table replaces it. And
`docs/cheatsheet.html` is replaced by what Print writes, from the same
lists, so it can never again be missing the Super keys as it was on 3
October.

## The architecture

```
   shortcuts.lua (Lua: what a person does)
     the window, the sidebar of groups, search, Print
         |
         |  fs.send("/Running/wm", {type="shortcuts"})   once, at open
         |  ui.TEXT_KEYS                                  a table in the kit
         |  g:keycaps(x, y, chord)                        each row drawn
         v
   UI kit (Lua)    ui.sidebar, ui.field, ui.list, keycaps, TEXT_KEYS
   keys.lua        keys.spellings - ctrl and Control as one
   writedoc / pageset / pdfwrite   the cheat sheet, as a PDF
         |
         v
   window manager   the system's keys, from the arrays it dispatches
         |
         v
   gfx (C)  text, round fills; the window's region, composed
```

**How a shortcut is kept.** The setting holds only what was made or
changed here, one entry each, each saying in full what it does:

```lua
{ keys = "Super+T", action = "open", program = "/Kosmos/Apps/terminal.lua" }
{ keys = "Super+M", action = "hide", was = "Super+H" }
```

Holding only the differences means a system key added in a later version
still arrives. Spelling each entry out in full means a person reading the
file sees what it does without knowing the defaults.

When the window manager starts, it builds its dispatch table from its
defaults and then applies the entries. Shortcuts saves through the settings
kit and then posts `{type="keys", shortcuts=true}`, which makes the window
manager read the setting again. That message is posted, never sent: the
window manager's key path must not wait on anything (`nothing blocks in the
key path`).

**Listening.** Super chords never reach an application, Shortcuts included,
so the window manager has to hand them over while the sheet listens.
- Shortcuts asks it to listen, and the window manager posts the next chord
  to that window as an event, without acting on it.
- Listening ends when Shortcuts stops it, when the window closes, or when
  the window loses the keyboard.

  A window that forgot to stop listening therefore cannot leave the
  desktop's keys dead.

**What is C and what is Lua.** All of it is Lua, and none of it is a loop
over bytes. There are about thirty rows, and the keycaps are a round fill
and a line of text each, drawn by gfx in C.

**What crosses where.**
- The window manager's list is one message, sent once at open. Its keys
  are compiled into it, so asking again would get the same answer.
- The PDF is written by the PDF Kit in the window's own process.
- Nothing recurs, so nothing needs a region.

**The modal window** (Super+º) is the same program with `--modal`, as now:
a popup over everything, closed by Escape or a press outside it.

## The busiest path

There isn't one to worry about. A key typed into the search matches about
thirty rows and draws the ones that fit, a few dozen gfx calls and one
commit. Opening the window is one message to the window manager and one read of the setting. The window
logs how long it took to open, and the aim is under 100 ms from Super+/ on
the M700.

## Steps

- **K1** - the pieces in the kit:
  - `g:keycaps` and `ui.keycaps_width`;
  - `ui.TEXT_KEYS`, which the text field dispatches from;
  - `keys.spellings`.

  The window manager's `EDIT_BINDINGS` goes. Held on the Mac by
  `tools/test_keys.lua` and by the display harness.
- **K2** - the window manager:
  - each binding gains its group (desktop, windows or screen) and its
    `action` name;
  - the dispatch table is built from the defaults and the `shortcuts`
    setting;
  - `listen`, and reading the setting again when told.

  When Magnet arrives its keys go in the same arrays and appear here with
  nothing else done. Its test boots with a `shortcuts` setting holding Super
  T and checks that the chord opens a Terminal, then moves Super H to Super
  M and checks that the old chord does nothing and the new one hides.
- **K3** - the window, as drawn:
  - the sidebar, keycaps and search;
  - `ui.sheet`, out of Mail, with Mail's Add Account moved onto it and its
    suite run;
  - Add a shortcut and Change;
  - in the dock by default, named Shortcuts and listed among the
    applications rather than in Preferences;
  - Super+/ and Super+º as now.

  `tools/run_shortcuts.py` checks that:
  - Add a shortcut, with Super and T pressed while the sheet listens, makes
    a Terminal open on the next Super T, and again after a restart;
  - Change, with keys another shortcut has, says so and takes them only on
    the choice;
  - Default puts a key back;
  - every group is there;
  - Super+Tab's row reads as two keycaps;
  - "ctrl w" finds the Control-W rows;
  - Escape clears the search.
- **K4** - Print as a cheat sheet: the lists as a Write document, through
  `pageset` and `pdfwrite`, into `/Home/Documents/Shortcuts.pdf`, opened
  once written; its test counts the PDF's rows. `docs/cheatsheet.html` is
  retired in favour of it.

Each step leaves a permanent test. Only x86-64 runs them for now
(`CLAUDE.md`).
