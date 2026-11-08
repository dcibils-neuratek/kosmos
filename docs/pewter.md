<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Pewter, and desktops as a thing Kosmos has several of

Written on 7 November 2026, before any of it is built. The desktop as it
should look is `docs/pewter.html`; the chooser shown at boot is
`docs/desktops.html`; the picture of what it stands on is
`docs/pewter-architecture.png`, drawn from `docs/pewter-architecture.html`.

Diego, that day: "lets add this new desktop ui as an option for kosmos. so
now we have the astra desktop and i want to have this new added called
Pewter", "upon launching i can select one of the two desktops to work in",
and "in the future i would also like to add a 3rd one to try new ideas for
desktop UI". Asked whether the chooser comes up on every boot and whether
Pewter brings applications of its own: "every boot, real apps in pewter
chrome, go ahead".

So this document is about two things, and the first one matters more:
**a desktop becomes something Kosmos has several of**, and Pewter is the
second. The third should cost a file and the programs it brings, and
nothing else.

---

## What a person can do

### At every boot

- **Choose a desktop.** A chooser fills the screen once the machine is up
  and before any desktop starts: one card for each desktop, with a small
  picture of it, its name and a line saying what it is. The one used last
  is already chosen. A click, or the arrow keys and Return, starts it.
- **See every desktop the system has**, the ones that ship and any added
  later, without the chooser being changed to know about them.
- **Watch it start.** Once a desktop is chosen, the chooser becomes a
  loader (Diego, 7 November: "a loader that shows what component and app
  is being loaded when selecting the desktop"). It lists the pieces of
  that desktop - for Pewter the banners, the desktop, the menu bar and the
  Control Strip - and then the applications the Startup page opens with
  it, each with what it is, its state - waiting, starting, ready, did not
  start - and how long it took. A bar under the list fills as they come
  up, and when they have, the loader goes and the desktop is there.
- **Not be held by one that fails or is slow.** One that does not start
  says why, in red, and the desktop opens without it; one still starting
  after a second and a half is left starting, says so, and opens when it
  is ready. The loader never waits for longer than that on anything.
- **Leave without choosing**: Restart and Shut Down at the foot of the
  chooser, as on any desktop.

### Once inside, on either desktop

- **Switch Desktop…**, in Astra's Deskbar menu and Pewter's Kosmos menu.
  It asks every application to quit, as Restart does - one that has a
  document unsaved asks its own question - and then shows the chooser
  again. The machine does not restart.

### On Pewter

As drawn in `docs/pewter.html`, which Diego has used and agreed as the
2026 design ("Keep all the 2026 changes, and keep the name Pewter"):

- **One menu bar across the top of the screen.** The Kosmos menu at its
  left, with search; then the menus of whichever application is in front;
  then the indicators and modes, the clock, and the Application menu, which
  lists what is running and hides or shows it.
- **Kosmos's own applications, in Pewter's chrome.** Tracker, Write,
  Terminal, Music, the IDE and the rest, the same programs Astra runs: a
  title bar the whole width, the close box at its left and zoom and
  collapse at its right, and no menu bar inside the window, because the
  menus are in the bar at the top.
- **Collapse a window to its title bar** - a double click on the bar, or
  its box - and back.
- **A Tracker desktop in 9.2's arrangement**: the disks at the top right,
  the Trash at the bottom right, and folders that open spatially - a
  folder is one window, always the same one, opening where it was left, at
  the size and in the view it was left in.
- **The Control Strip** at the bottom left, for what a person changes:
  volume, the screen, the network, the modes.
- **Notifications**: banners at the top right, the day's list under the
  clock, held while Focus or Rest is on.
- **Every window at once** on F3, and a click on one brings it forward.
- **The menus searched**, from the Help menu: any item of the application
  in front, by what it says.
- **Quick Look** on the space bar, Undo and labels in Tracker, a
  screenshot key - each of these exists or is planned for Astra already,
  and Pewter uses the same one.
- **An application asks before it reaches the microphone**, and the menu
  bar says so while it does.

What the mockup has that Pewter does not: its own Write, Player, Calculator
and Terminal. They were drawn to show the chrome on something; the real
desktop runs the real applications.

---

## Architecture

### There is one window manager, not one per desktop

The question Diego asked first was whether this means two `wm.lua` files.
It does not, and the reason is in what `wm.lua` does today. It is 7,374
lines, and nearly all of them are mechanism: composing, damage, input and
focus, the pointer and its cursor, the clipboard, drag and drop, launching
and reaping, strips, game mode, the power button. None of that depends on
what a desktop looks like.

**And it no longer draws any window's chrome.** Since one chrome's step 3
(8 October, `ui.md` 16.25) every window is headed: its title bar is a
`ui.header` the UI kit draws inside the window's own pixels, and the
window manager draws no tab and no border round anything. The look of a
window is the kit's already.

**What makes a desktop is the programs `wm` starts**. With nothing asked
for, `wm.lua` starts `notifications,desktop,deskbar` (line 6824): the
banners, Tracker as the backdrop, and the Deskbar. That list is Astra.

A copy of `wm.lua` for Pewter would therefore be seven thousand lines kept
in step to change a few hundred, which is the second copy the premise
calls a defect. Haiku made the same choice for the same reason: one
app_server, with the window decorator a piece that can be exchanged.

### A desktop is a file

`/Kosmos/Desktops/<Name>.desktop`, `user/desktops/` in the tree, carried in
the image beside the looks. Text, in the same `key = value` form a
`.theme` file uses, its lines read by the same reader (`theme.lines`), so
the format is one format. `desktops.lua` is the kit both its readers use -
the chooser lists them, the window manager starts one - so the two cannot
disagree about what a desktop is.

**As built in P1** a file says four things, and nothing it does not yet
act on: `name`, `says` (the chooser's line under it), `shell`, and
`background` - those of the shell's programs that open no window when they
start, and are ready as soon as they run. A key it does not know is a
complaint in the log rather than silence. The rest arrive with the step
that reads them - `chrome` with P2, `menus` with P3, `looks` and `picture`
when Pewter has its own - and Pewter's file when it is drawn will read:

```
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
# Pewter: Mac OS 9.2's manner, reimagined for 2026 (docs/pewter.md).
name        = Pewter
says        = One menu bar for everything, and Platinum.
shell       = notifications, pewterdesk, menubar, controlstrip
chrome      = platinum
menus       = screen
looks       = platinum, platinum-dark
picture     = Pewter.png
```

- **`shell`** is what `wm` starts, in order, and restarts if one dies,
  exactly as it treats the Deskbar now.
- **`chrome`** is which title bar the kit draws: `astra` (the headers of
  today, the three circles at the right) or `platinum`.
- **`menus`** is where an application's menu bar goes: `window`, drawn by
  the kit inside the window as now, or `screen`, handed to the window
  manager and shown by whoever draws the bar at the top.
- **`looks`** is which looks Appearance offers on this desktop. Astra's
  are the eleven it has; Pewter's are its own two, light and dark, because
  Platinum's bevels are part of the desktop rather than a colour on it.
- **`picture`** is what the chooser shows.

`Astra.desktop` says what runs today and nothing new:
`shell = notifications, desktop, deskbar`, `background = notifications`.
Building P1 had to leave Astra exactly as it is, and the gate is how that
is known.

**A third desktop** is a third file and the shell programs it names. If it
needs a new kind of title bar, that is one more `chrome` in the kit; if it
puts menus somewhere new, one more `menus` value. Neither is a fork of
anything.

### Who supplies each piece

| Piece | What supplies it | New or existing |
|---|---|---|
| Composing, input, focus, clipboard, drag, strips | `wm.lua` | existing, unchanged |
| Reading a `.desktop` file | `desktops.lua`, over `theme.lines` | new kit, the format's reader shared |
| Starting its shell, the loader's rows | `wm.lua`, `handlers.desktop`, on the `starting` list | new, small |
| The Startup page's items, `maild`, `autod` | `wm.lua`, moved out of `deskbar.lua` | moved, not copied |
| The chooser | `chooser.lua`, a program `wm` starts when no desktop is named | new |
| Which desktop was used last | `prefs.lua`, the `desktop` setting | existing kit, new key |
| Platinum's title bar | UI kit, `ui.header` with `chrome = "platinum"` | new style in the kit |
| Collapse to the title bar | `wm.lua`, `handlers.shade`; the kit's double click | new, small |
| Menus handed to the screen | UI kit (`ui.window`, `ui.menubar`); `wm.lua` carries them | 16.8h made general |
| The menu bar | `menubar.lua`, a top strip | new shell program |
| Indicators, modes, the clock | the code the Deskbar uses for them, moved to a library both read | moved, not copied |
| The Kosmos menu and its search | `deskbarmenu.lua` (the folder that is the menu) and Tracker's query | existing |
| Application menu | `wm.lua`'s `windows` list, as the Deskbar reads it | existing |
| Pewter's desktop and spatial folders | Tracker, `tracker pewterdesk`; folders' rects as attributes | Tracker, new mode |
| Control Strip | `controlstrip.lua`, a bottom strip; its modules the same library as the indicators | new shell program |
| Banners and the day's list | `notifications.lua` and the notification server, drawn in the desktop's chrome | existing |
| Every window at once (F3) | `wm.lua`, composing each window scaled | new in the compositor |
| Searching the menus | the kit answers the window manager with the focused window's items | new, one request |
| Quick Look, screenshots, Undo, labels | Tracker, `wm.lua`'s screenshot key | existing or planned for Astra |
| Asking for the microphone | the consent step in `roadmap.md` (*Per-launcher permissions*) | planned, shared |

**What moves rather than copies.** The Deskbar draws the battery, sound,
network and the clock today, inside `deskbar.lua`. Pewter's menu bar and
its Control Strip want the same indicators, so the code that reads them and
draws them leaves the Deskbar for a library - `indicators.lua` - that the
Deskbar, the menu bar and the Control Strip all use. Three applications,
one copy.

### Menus at the top of the screen

This is the one real change to how applications work, and it is a general
form of something already built.

**As built (P3)** a window's menus are the menu bar it was given, or,
with none - which is nearly every application, Astra's menu being the
dots in a header - the buttons in it that open a menu, in the order they
are drawn: the dots go by the application's name (its title after the
last ` - `), any other by its words. So every application has its menus at
the top on Pewter without a line of it changing. The bar is a strip that
`keeps_front`: pressed, it does not take the keys or the boxes from the
application whose menus it shows.

**16.8h, as it is.** A window that draws its own pixels can carry a menu
bar: `ui.window{ direct = true, menubar = ... }` sends the titles - only
the titles - to the window manager, which draws the strip above the
window. A press on a title is a `menubar` event to the window, and the kit
opens its own menu there. The items never leave the application, so their
`on_choose` runs where it was written.

**On Pewter every window does that.** When the desktop says
`menus = screen`:

1. `ui.window` sends its menu bar's titles to the window manager on open,
   for every window rather than only a direct one, and again when they
   change. The kit draws no menu bar inside the window.
2. The window manager keeps them with the window, and when the focus moves
   it tells its watchers - the `watch` events the Deskbar already hears for
   the focus (`tell_watchers`) - with the titles of the window in front.
3. `menubar.lua` draws them between the Kosmos menu and the indicators.
4. A press on a title is a `menu_open` posted to the window manager: which
   window, which title, where. The window manager posts the `menubar` event
   to that window, as it does now for a direct one.
5. The kit opens its menu under the title, at the top of the screen. It is
   the application's own menu, as on Astra.

Everything in that is a message, once, because somebody pressed or the
focus moved. Nothing recurs on a clock, so nothing needs a region.

**Nothing waits.** The menu bar never asks the window manager anything on a
press or in a repaint: it draws the titles it was last told, and posts.
That is *Nothing on the desktop waits on a server*. An application that has
stopped answering still shows its titles, a press opens nothing, and after
five seconds the Application menu offers Force Quit, which it does already
on Astra.

**An application written once.** Its menus are the same table on both
desktops. Astra draws them in the window, Pewter at the top, and nothing in
the application knows which.

### The chooser

`wm` starts `chooser` when it is given nothing to start, where today it
would start Astra's list. Every harness and scripted run names what it
wants, so what they start is what they started before - the same argument
as when the desktop became the default (`wm.lua`, *With nothing asked for,
a desktop*). A test that wants a desktop names its file: `wm Astra.desktop`
- which the harnesses that typed a bare `wm`, or booted with `boot=wm`,
now do.

1. The chooser lists `/Kosmos/Desktops` and reads each file, and the
   `desktop` setting for the one used last.
2. It draws a card for each, full screen, as `docs/desktops.html` has it.
3. On a choice it sends `desktop{ name = "Pewter", watch = <its window> }`
   to the window manager, which answers at once and keeps the choice in the
   `desktop` setting.
4. The window manager reads the file - and, from P2, applies its chrome,
   menus and look before the first window opens, so nothing is drawn twice
   - and starts its shell through `launch`, as everything is started. Once
   the shell has settled it opens the Startup page's items (`startup.lua`)
   and `maild` and `autod` when they have something to do. **The Deskbar
   opened those, and does not any more**: a desktop with no Deskbar would
   have opened none, and the loader could list only what the window
   manager starts. A list given to `wm` with the Deskbar in it opens them
   when the Deskbar's window is up, as before. Every item is tried, a name
   that is no program included, and fails as a launch fails - a stale tick
   is shown where a person sees it rather than skipped where nobody does.
5. **The chooser stays, as the loader**, and the window manager tells it
   how each launch goes. It already kept that for the Deskbar - `starting`,
   each launch's program, its process and why it failed - so each entry
   there carries a row, and the moments it already noticed post a
   `loading` event: `{ index, program, part, state, ms, error }`, `part`
   the desktop's own or what opens with it, `state` one of `starting`,
   `ready` (its first window), `ended` (it ran and went without one, as
   `desktop` does once it has started Tracker), `failed`, or `slow` (still
   starting when the Deskbar would give up on it). A piece that opens no
   window - the file's `background`, and any program rather than an
   application - is ready once it runs. Posted, never sent, and the next
   step is taken once a pass: a row settles inside `step_launches`, and a
   launch begun from in there would be lost.
6. When every row has settled the window manager posts `done`; and when
   nothing has moved for a second and a half the loader stops waiting for
   a slow one. It lingers for 0.7 s and closes, and the desktop is under
   it. While it is up it is raised over each window the desktop opens, and
   the `windows` list calls it chrome, so the Deskbar lists no button for
   it. It draws what it was posted and asks nothing.

**Switch Desktop…**, in the Deskbar's menu above Restart, sends
`desktop{ name = false }`. The window manager asks every window to close,
as a close box does - and ends it a second later if it has not - ends what
the desktop started that has no window, and when the screen is empty
starts the chooser again.

The chrome is told to each window in the reply to its `open`, beside
`headed` (`ui.lua` line 7713), so a window learns how to draw itself before
its first frame. A window cannot outlive a switch, which is why changing
desktop closes them: telling every open window to redraw in another
chrome, with its menu bar leaving or arriving, is a feature nobody asked for
and a second path through every layout.

### C or Lua

| What | Which | Why |
|---|---|---|
| The chooser, the menu bar, the Control Strip | Lua | applications: what a person does, a few dozen rectangles |
| Reading a `.desktop` file | Lua, `theme.lua`'s parser | ten lines of text, once a boot |
| Platinum's title bar | Lua decides, `gfx` draws | as every header is now |
| F3, windows drawn scaled | C, in the compositor | a loop over pixels: a scaled blit in `gfx`, vectorised |

### The busiest path

**A press on a menu title, to the menu drawn**, which is the thing a
person on Pewter does more than anything else.

1. The press reaches the menu bar: the window manager's ordinary routing.
2. The menu bar posts `menu_open` - one message, no reply waited for.
3. The window manager posts `menubar` to the window in front.
4. The application's kit opens its menu: a new window, drawn, committed.
5. The window manager composes it.

Against Astra's menu inside the window, that is two posts more (steps 2
and 3), each a message of a few dozen bytes and a context switch. The
budget is the same as Astra's: the menu drawn inside the frame after the
press, 16 ms. Step 4 is most of it on both desktops.

**As built, measured** (P3): the kit says, for every menu it opens, how
long after the press it was drawn - from the press the bar saw, under
Pewter, so steps 2 and 3 are in it. Under QEMU, two machines side by
side: about 30 ms on Pewter, 11 on Astra. Drawing the menu is about 14
on both. **The rest is not the two posts but a wait**: the window manager
hands the bar its press at the end of a pass and then composes, and the
bar's `menu_open` is read only once that is done - 10 to 17 ms of QEMU's
composing. Astra's window draws its menu while the window manager
composes. QEMU's composing is many times the M700's, so it is measured
there first; if it shows, the bar tells the window manager where its
titles are, and a press on one is posted to the window in the pass that
read it, as a direct window's strip is (`roadmap.md`).

The other path worth naming is F3: every window scaled, every frame while
it animates. That is the compositor's pixel loop, in C, and it is measured
on the M700 with the GPU's composing and without.

---

## The steps

Each is built, held by a test that stays, and lived with before the next.

- **P1 - Desktops as files, and the chooser. Built, 8 November 2026**;
  `tools/run_desktops.py`, `x86-desktops`, 13 checks in about twenty
  seconds, its control a failed launch reported as ready, which fails it.
  `Astra.desktop` describes
  what runs today; `chooser.lua`; `handlers.desktop`; the `desktop`
  setting; Switch Desktop… in the Deskbar. The loader, and the window
  manager's `launch` events. Pewter's file exists and starts Astra's
  shell, so the choice can be made before Pewter is drawn. Test: a suite
  boots to the chooser, picks each desktop and checks its shell is
  running, the loader having listed each piece as ready; a startup item
  that names no program shown as failed, and the desktop opened anyway; the gate's suites, which name what they start, unchanged.
- **P2 - Platinum's title bar. Built, 8 November 2026**;
  `tools/run_platinum.py`, `x86-platinum`, 9 checks in two machines side
  by side, about fifteen seconds. `chrome = platinum` in `Pewter.desktop`
  (`chrome = astra` in Astra's); the window manager keeps it and tells it
  to each window in its `open` reply; `ui.header` draws Platinum's bar -
  the look's ground, pinstripes, the title centred on a patch of plain
  ground - and leaves room for close at its left and the pair at its
  right. **The window manager still draws and presses the boxes**, as it
  does Astra's three, so a window that has stopped answering can still be
  closed: `SC_PLATINUM` in the scene draws close alone at `bx` and zoom
  and collapse at `bx2` (the wire's spare word), grey boxes with their
  marks always shown and none at all on a window behind. Collapse is
  `handlers.shade`, from the right-hand box or a double click on the bar:
  the window is shown as tall as its header, nothing asked of the
  application. A window that knows only Astra's run - Groove's own bar - is
  given the pair at the end of that room and close at the left. Test: the
  gallery's 25 applications opened under Pewter, each that places its
  boxes placing them Platinum's way and none Astra's; Calculator's close
  box drawn in Platinum's grey, collapsed by its box, opened out by a
  double click, closed by its close box. Its control is a press between
  the boxes: measured as Astra's one run, the whole bar was boxes and the
  bar's double click never reached the window, which fails it.
  **Left for later:** the pinstripes stay on a window behind, because the
  kit is not told when its window loses the front; and a window whose
  title bar is its right-hand pane (Tracker, Preferences) has close at that
  pane's left rather than the window's.
- **P3 - Menus at the top. Built, 8 November 2026**; `tools/run_menus.py`,
  `x86-menus`, 11 checks in two machines side by side, about seventeen
  seconds. `menus = screen` in `Pewter.desktop` (`window` in Astra's);
  the kit hands every window's titles over (`handlers.menus`) - a menu
  bar it was given, or the buttons that open a menu, the dots by the
  application's name - and draws none itself; the `windows` reply's
  `front` carries the application in front and its titles, and `watch`
  says when they change; `menubar.lua`, in the Deskbar's place, with the
  Kosmos and Application menus, the clock and the indicators;
  `indicators.lua` and `kosmosmenu.lua` out of the Deskbar for both bars.
  A press on the bar does not take the front (`keeps_front`). Test: the
  PDF viewer (a direct window) and the Text Editor (a kit window, its
  menu the dots) under Pewter - titles handed over and none in the
  window, the bar following the front, View opened from the bar with the
  PDF viewer still in front, New window chosen from the Text Editor's
  menu there, the Kosmos menu - and the PDF viewer under Astra, its titles
  in its header and nothing handed over. Write has no menus of its own
  to test - its ribbon is its own - so the PDF viewer took its place.
  **`wmlatency` measures a poll's round trip, not this path**, so the
  kit says instead how long each menu took from the press to drawn: under
  QEMU about 30 ms from Pewter's bar, 11 from Astra's window, the
  difference the bar's request waiting for the window manager's pass to
  finish composing - see *The busiest path*, and `roadmap.md` for what to
  do if the M700 shows it too. **Left for later:** the dots stay in the
  header as well as in the bar; a title in the bar is not lit while its
  menu is open; the Kosmos menu has no search yet (P6).
- **P4 - Pewter's desktop. Built, 0.11.318, 8 November.** Not a program
  of its own but Tracker's desktop with a word: `Pewter.desktop`'s shell
  starts `desktop:pewter`, which is `tracker desktop pewter`.
  - **The arrangement**: `iconlayout.place` with `from = "right"` - the
    columns from the right edge inward, so the Drive is at the top right -
    and an item with `corner = "bottom_right"`, the Trash, placed there
    before anything else. Each icon's place is kept in `pewter_x` and
    `pewter_y`, beside Astra's `desktop_x` and `desktop_y`: one folder,
    two arrangements, and neither desktop disturbs the other's.
  - **Spatial folders**: a folder opened from Pewter's desktop, and the
    Drive, open as `tracker <path> spatial` - a window named for the
    folder, with no sidebar, where a folder inside opens a window of its
    own rather than this one wandering. Before it opens it asks the
    window manager whether a Tracker window already is that folder - the
    window's `doc`, a path, since a title is a name and two folders can
    share one - and if one is, brings it forward and ends.
  - **What it keeps**, on the folder as attributes: `spatial_x`,
    `spatial_y`, `spatial_w`, `spatial_h` and `spatial_view`, written when
    the window is moved (the kit's new `on_moved`), resized, made icons or
    a list, and when it closes - only when one of them differs.
  - Test: `x86-spatial` (`tools/run_spatial.py`), 9 checks, two machines
    at once. Under Pewter, the Drive at the top right and the Trash at the
    bottom right; the Trash opened spatially and with no sidebar; opened
    again, the same window brought forward; moved, resized and made a
    list, each kept; closed by its box and opened again in the same
    place, at the same size, in the same view. Under Astra its own
    arrangement and an ordinary window. Its controls - `keep` writing
    nothing, and the window manager forgetting `doc` - each fail it.
  - **Left for later**: writing to a folder's attributes changes the
    folder it is in, so the desktop lists itself again on every move of a
    window of one of its folders; a `doc` longer than 127 bytes is not
    kept (six windows go in a `windows` page of 2048), so a folder that
    deep can open twice; a spatial window has no Back or Forward to use;
    and Astra's desktop starts under the Deskbar, so its Trash is under
    the bar - seen by this suite, older than it.
- **P5 - The Control Strip**, and notifications drawn in Pewter's chrome.
- **P6 - The 2026 pieces that are Pewter's**: F3, the menus searched, the
  modes in the menu bar. The ones that are not Pewter's - the screenshot
  key, Undo and labels in Tracker, asking for the microphone - are built
  for both desktops, where `roadmap.md` already has them.

---

## What this does not do

- **Change desktop with windows open.** A switch closes them; see the
  chooser above.
- **Mix desktops**: Pewter's menu bar with Astra's Deskbar. A desktop is
  chosen whole, as a look is (`ui.md` 16.9).
- **Recreate Mac OS 9.** Pewter takes 9.2's manner and is drawn fresh: its
  own art, its own name, Kosmos's applications.
