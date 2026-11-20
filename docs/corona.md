<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Corona: CDE and IRIX, for a media workstation

Written on 20 November 2026, before any of it is built. The desktop as it
should look, playable, is `docs/corona.html`; the picture of what it stands
on is `docs/corona-architecture.png`, drawn from
`docs/corona-architecture.html`.

Corona was first drawn on 7 November (Diego: "a desktop that i always
loved is the old solaris desktop from SUN", then "keep Corona, CDE, and
give it a 2026 pass too"), as CDE alone, and left as a mockup. On 20
November Diego brought it back and widened it: "i want to keep
investigating new desktop UI ideas beyond astra and pewter and starlight.
we did one similar to CDE and IRIX which i want to bring back to have that
old unix feel but modernized to fit kosmos ultra speed and multimedia
oriented features".

So Corona is now **the two Unix workstation desktops of the late nineties
together** - CDE's Front Panel, workspaces and Motif frames, and IRIX's
Toolchest, Desks Overview, sculpted icons and Indigo Magic blue-grey - and
in its 2026 form **a media workstation's desktop**, which is what an SGI
Indigo was for and what Kosmos says it is for ("a multimedia monster",
`CLAUDE.md`). It is the third desktop file `docs/pewter.md` planned for:
one window manager, a file naming its shell programs, and what is new
built so that the other desktops can use it too.

---

## What a person can do

Marked **1999** where it is CDE or IRIX as they were, **2026** where it is
Corona's own. In the mockup the switch at the Front Panel's right end flips
between them; what Kosmos builds is question 9 below.

### The Front Panel (CDE)

- **The panel along the bottom**, one row of controls: the clock, the
  date, Home, the Text Editor, Mail, Find; the padlock, the busy light,
  the workspace buttons and Exit in the middle; Record, the load meter,
  Status, Style, Applications and the Trash at the right. **1999**, with
  the Printer and Help in it; **2026** without them (Kosmos does not print,
  and Help is in the Toolchest).
- **Subpanels**: the arrow above a control opens its list - Personal
  Applications above the Text Editor, Files above Home. **Install Icon**
  at the top of one takes a launcher dropped on it.
- **The busy light** blinks while something is starting.
- **The padlock** locks the screen; **Exit** ends the session - Switch
  Desktop, Restart, Shut Down.
- **Minimise the panel** to an icon, and back.

### The Toolchest (IRIX)

- **A small titled stack at the top left** - Desktop, Selected, Find,
  Media, System, Help - each a menu to its right: a Terminal, the home
  folder, the desks, Customize, Log Out; what the selected icon offers;
  finding an icon or a file, the applications, the media library; Music,
  the Movie Player, the Audio Panel, recording, Captures; the Performance
  Meter, This Machine, restarting the window manager; help and About.
- It is **Kosmos's own menu in IRIX's shape**: the same sections and
  launchers the Deskbar's menu and Pewter's Kosmos menu show.

### Workspaces, which IRIX called desks

- **Four workspaces**, each with its own windows and its own backdrop,
  chosen by their buttons in the Front Panel. A name changed by a double
  click on its button.
- **The window menu's Occupy Workspace, Occupy All Workspaces**: a window
  in one, or in every one.
- **The Desks Overview** (IRIX), a window listing every desk, drawn small
  with its windows; a click goes there.
- **2026**: the buttons are **live miniatures** of their desks - windows
  where they are, a film moving in its window, a note on the desk that is
  making the sound; **a window's title dropped on a button sends it
  there**; **F3 shows every desk at once**; **Alt and a number or an
  arrow slides between them**, the change itself animated.

### Windows (Motif, as dtwm and 4Dwm drew them)

- **A bevelled frame to take hold of** at any edge or corner, the title
  bar to move by, the **window-menu button at the left** (Restore, Move,
  Size, Minimise, Maximise, Lower, Occupy, Close; a double click closes),
  minimise and maximise at the right.
- **A minimised window is an icon on the backdrop**, down the left, under
  the Toolchest; a double click brings it back.
- **The active window is the one clicked, or the one pointed at** - the
  Style Manager's Window page.
- **2026**: the window moves itself rather than an outline, and snaps to a
  half of the screen at the left or right edge, or fills it at the top.

### The backdrop (IRIX)

- **Sculpted icons down the right**: the home folder, Music, Movies,
  Captures, the drive and the Trash, drawn as lit slabs. A click lays a
  coloured plate under one; a double click opens it, an outline growing
  from the icon to the window.
- **2026**: an icon carries what is true of it - Captures how many new
  captures there are, Music a play mark while a song plays.
- **The Workspace Menu** on the right button: programs, shuffle, the panel,
  restarting the window manager, Log Out.

### Media (2026; the Media Tools in 1999)

- **Now Playing, in the Front Panel**: the song, its cover, previous, play
  or pause, next, where it is, and **its two levels, moving**. A click on
  its bar seeks; the wheel over it is the volume. Its subpanel lists what
  comes next and **every program making a sound**, with the volume.
- **Drag to play**: a song dropped on Now Playing plays; a film opens in
  the Movie Player. A file dropped on the Trash goes there; on a workspace
  button, it opens in that workspace.
- **Record**, the red button: the screen into Captures as an MP4,
  pressed again to stop, with a red frame round the screen and the time in
  the panel while it records. Its subpanel chooses the whole screen, the
  window in front or the camera, and takes a picture of the screen.
- **The load meter, CPU GPU ♪**: the four processors, the graphics
  processor, and **how much of the sound's deadline the mix is using** -
  the three things a media machine runs out of. A click opens the
  Performance Meter, which has all three over time.
- **The Audio Panel** (IRIX's `apanel`): a fader, a mute and a meter for
  every stream and the master. A muted stream's meter still moves, because
  it is measured before its fader.

### Looks

- **Palettes chosen whole** in the Style Manager: Indigo Magic (SGI's
  blue-grey, 2026's first), Solaris (CDE's, 1999's), Sand, Ocean, Orchid,
  Lichen, Charcoal, and the night ones, Indigo Night and Night.
- **A backdrop for each workspace**; **2026**: a palette for one
  workspace - Ocean for work, Night for music.
- **2026**: Find (applications and files as you type), Today above the
  date (the day's notifications), Status (network, volume, brightness,
  battery, holding notifications), banners rising above the panel.

### What the mockup has that Corona does not

The Mailer, Calendar, Text Editor, File Manager, Style Manager, Help
Viewer, Terminal, Music, Movie Player and Audio Panel in the mockup are
stand-ins, drawn to show Motif's frame on something. On Kosmos they are
Kosmos's own applications - Mail, Text Editor, Tracker, Preferences,
Terminal, Music, Video, Mixer - unchanged. **Kosmos has no calendar**: the
date's Today subpanel is the notifications' history until a Calendar is
designed as an application of its own (question 11).

---

## Architecture

### It is a desktop file, and one window manager

```
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
# Corona: CDE and IRIX, for a media workstation (docs/corona.md).
name       = Corona
says       = CDE and IRIX: the Front Panel, the Toolchest, desks
shell      = notifications, desktop:corona, toolchest, frontpanel
background = notifications
chrome     = motif
menus      = window
look       = indigo
desks      = 4
cut        = no
star       = no
plate      = no
flat       = no
title_bars = yes
shadow     = 14
```

Every key but `desks` exists and is read by `user/lib/desktops.lua`
(`KEYS`); `motif` is one more `chrome`, as `platinum` was for Pewter. The
window manager (`user/bin/programs/wm.lua`) is the same one Astra, Pewter
and Starlight run, and the applications are the same programs.
`docs/pewter.md` says why there is one window manager; nothing here asks
for a second.

### Who supplies each piece

Checked against the tree on 20 November 2026.

| Piece | What supplies it | New or existing |
|---|---|---|
| Composing, input, focus, clipboard, drag and drop, strips, launching | `wm.lua`, the window server (`windowserver.c`) | existing |
| Reading `Corona.desktop` | `desktops.lua` over `theme.lines` | existing, one key more (`desks`) |
| Motif's frame and its window menu | the UI kit, `ui.header` with `chrome = "motif"`; the boxes drawn and pressed by the window manager as Platinum's are (`SC_PLATINUM`) | new chrome in the kit, new scene code |
| Resizing from any edge | `pointer.lua`'s resize, which today takes only the bottom-right grip (`OUT.GRIP`) | **small, new**: the frame's eight zones, in the window manager for every desktop |
| Snapping to halves | Magnet, `handlers.magnet` | existing |
| Focus follows the pointer | the window manager, a setting | existing |
| Workspaces | `wm.lua`: a desk on every window | **new**, in the window manager, for every desktop |
| The Front Panel | `frontpanel.lua`, a bottom strip (`ui.window{ strip = "bottom" }`, as the dock is) | **new shell program** |
| Subpanels and Install Icon | folders of launchers, read by `deskbarmenu.lua` as the Deskbar's menu is | existing kit, new folders |
| Clock, battery, network, volume | `indicators.lua` | existing, moved there in Pewter's P3 |
| The busy light | the window manager's `launch` events, the loader's (`pewter.md` P1) | existing |
| Exit's choices | `kosmosmenu.lua`: Switch Desktop, Restart, Shut Down | existing |
| The Toolchest | `toolchest.lua`, a small window that `keeps_front`, its menus `kosmosmenu.items` | **new shell program**, existing menu |
| Sculpted icons down the right | Tracker's desktop, `desktop:corona`: `iconlayout.place{ from = "right" }` as Pewter's P4 does, places kept as `corona_x`, `corona_y` | Tracker, a third arrangement |
| Minimised windows as icons | Tracker's desktop, told by the `watch` events the Deskbar already hears | small, new |
| Desk miniatures, the Desks Overview, F3 | the window server composing each desk's windows scaled (`picture:stretch`, as `switcher.lua` draws its window row) into a picture lent to the panel | **new**, shared with Astra's Overview (`astra.md` §5 #6) |
| Now Playing: what is playing | a stream's description, carried by the audio server beside its name and peak | **new fields** in `audioproto.h` |
| Now Playing: the transport | `media.lua`, the one door under Music and Video, answering `transport` posts for any player | **new**, in an existing library |
| Levels at frame rate | the audio server's **meter page**, a region its mix writes and anyone may map | **new**, in the audio server |
| The load meter | `sys.cpuload()` (as `sysmon.lua`), `gpu.stats()` and `gpu.since` (as `gpumon.lua`), `audio.stats()`'s `late` and `starved` (as `mixer.lua`) | existing sources |
| Drag to play | `handlers.drag` and `handlers.dropped` (kind `files`); `filetypes.lua` says what opens it | existing |
| Record | `screenrec`, a program like `screenshot.lua`, borrowing `/Running/wm/remote` as `vncd` does; the Record Kit encoding | **new program**; the Record Kit **grows two entries** |
| A picture of the screen | `screenshot.lua` | existing |
| The Audio Panel | Mixer (`mixer.lua`) | existing |
| Music, Movie Player | Music, Video, both on `media.lua` and the Playback Kit | existing |
| Performance Meter | System Monitor (`sysmon.lua`) and GPU Monitor (`gpumon.lua`) | existing |
| Style Manager | Preferences' Appearance and its backdrops | existing |
| Banners above the panel | `notifications.lua`, its corner said by the desktop | small, new key |
| Looks | `user/themes/Indigo.theme`, `IndigoNight.theme`, `Solaris.theme`, with `edge_light` and `edge_dark` doing the bevels | **new looks**, existing format |
| The padlock | the lock screen in `roadmap.md` (*Login, lock screen and encryption at rest*) | planned, shared |

### What is new, and who else wants it

**The premise** (`CLAUDE.md`, *Kits, servers and drivers supply*) asks of
every new piece whether another application wants it. Every new piece
here is answered yes, which is why each lives below the desktop:

1. **Workspaces, in the window manager.** Each window carries a desk; the
   window manager composes only the current desk's windows and the strips;
   `handlers.desk` goes to one, sends a window to one, or puts it on all of
   them; the `watch` events say which desk is current and how many there
   are. `desks = N` in a desktop file turns them on, and Astra's file says
   nothing and has none. Astra lists workspaces as missing
   (`astra.md` §5 #7); built here, they are a line in its file when Diego
   wants them there. Switching is the window manager's alone - no
   application is asked to draw, since every window keeps its surface - so
   a switch costs one composed frame.
2. **Desk pictures, composed by the window server.** The Front Panel
   cannot draw the miniatures itself: it holds no other window's pixels,
   and holding them would be authority it was not handed. So it lends the
   window manager a picture - an inset, as `handlers.inset` lends one for a
   browser's page - and the window manager composes each desk's windows
   into it, scaled with `picture:stretch`. The same composing is the F3
   view, the Desks Overview, Astra's Overview and the switcher's window
   row, which already does it for one application's windows.
3. **A stream that says what it is.** The audio server knows every stream:
   its name, its gain, its peak, what it has queued. Now Playing needs a
   title, an artist, a length and where it is - so `media.lua`, which every
   player opens its sound through, describes the stream when it opens a
   file (a fixed `struct audio_describe` in `audioproto.h`: title and
   artist of 64 bytes, length in frames, the player's name in `/Running`),
   and position is the stream's own (`media.lua`'s *time is the sound's*).
   Pewter's menu bar, the Deskbar's sound indicator, the lock screen, the
   keyboard's media keys and Xavier asked "what is playing?" all read the
   same thing.
4. **A transport any player answers.** Play, pause, next, seek: posted to
   the player's name in `/Running` as `{ type = "transport", ... }`, and
   answered by `media.lua` for whichever application is playing, so Music
   and Video answer without either changing more than a line. Posted,
   never sent: the panel paints the new state at the press, from what it
   already knows, and the player catches up.
5. **The meter page.** The mix already touches every sample and keeps each
   stream's peak before its gain (`mixer.lua`'s header). Asking for it 60
   times a second would be a stream of messages on a clock - which
   `CLAUDE.md`'s *Control by message, data by shared memory* says is a
   region's: so the audio server writes, once a period, into one page it
   shares read-only - each stream's peak, the master's left and right
   after gain, the worst gap and the periods mixed, under a sequence count
   so a reader sees a whole set - and the Front Panel, Mixer and Groove map
   it and read it each frame with no message at all.
6. **Screen recording.** `screenrec` borrows the screen as `screenshot.lua`
   does and hands frames to the Record Kit, which takes YUY2 from a camera
   today and writes the whole file into memory before one write. A screen
   wants two entries more: **BGRA in**, converted to YUY2 in C with the
   vector types (`gfx`'s `pack.c` and `yuv.c` are the pattern), and **the
   file out in pieces** as it grows, so a recording is not limited by a
   region. Remote Screen, a recorded tutorial and the IDE's lessons
   (`roadmap.md`, *Lua app tutorial*) want both.

Everything else - the Front Panel, the Toolchest, the arrangement of the
icons - is the desktop itself, and is Lua shell programs on existing kits,
as Pewter's menu bar is.

### Region or message

| What | How | Why |
|---|---|---|
| Levels, every frame | the meter page, a region | recurs because the device's clock says so |
| A desk's picture | an inset, a region the window manager composes into | pixels |
| Recorded frames | the borrowed screen and the Record Kit's regions | pixels, every frame |
| What is playing, the transport | messages, once each | because somebody pressed, or a song changed |
| Load: processors, GPU, sound | a request a second, on the panel's own clock | a reading, small and asked for |
| Desk changes, a window sent | posts to the window manager | presses |
| A file dropped | the window manager's `drag`, a string | once |

### C or Lua

| What | Which | Why |
|---|---|---|
| The Front Panel, the Toolchest, Tracker's desktop | Lua | applications: what a person does, a few dozen rectangles |
| Desks, the window manager's part | Lua, in `wm.lua` | which windows to compose: a list, not a loop over pixels |
| Desk pictures, F3 | C, the window server's `stretch` | a loop over pixels |
| Motif's frame | Lua decides, `gfx` draws | as every header is |
| The meter page | C, in the audio server | it is the mix's own loop |
| BGRA to YUY2, H.264 | C, the Record Kit | a loop over pixels, then an encoder |

### The busiest paths

**Levels moving while music plays**, the thing on the screen that never
stops. The mix writes a few dozen bytes into the meter page each period -
inside a loop that already visits every sample, so about nothing. The
panel, once a frame, reads them and draws two meters 18 by 52 points: a few
thousand pixels into its own surface, and that rectangle's damage composed.
Against the alternative, a request a frame: about 1.4 KB of reply and a
Lua table each, sixty times a second, in a shell program on the frame path
- exactly the garbage `CLAUDE.md` found in the window manager's `wait_input`
and took from 4.2 KB a pass to 0.6. The page is the answer before the
question arises.

**A desk's miniature staying live.** Four miniatures of about 120 by 68 are
33 thousand pixels - under 2% of a 1920 by 1080 frame, if every one were
redrawn every frame (an estimate, not measured). They need not be: a
miniature is composed again when a window on its desk commits, at most ten
times a second, which is what makes a playing film move in it.

**A desk switch.** One frame: the window manager composes the other desk's
windows, whose surfaces it already holds, and asks nobody. The 2026 slide
is about thirteen frames of that composing offset, the compositor's work,
which the M700's GPU composing (`rendering.md`) takes when it is in force.

**A recording**, the one that is not known to fit. `minih264e` encodes in
software; whether it keeps 1920 by 1080 at 30 frames a second on the M700
has not been measured, and it is measured before C8 is designed further
(question 8).

---

## The steps

Each is built, held by a test that stays, and lived with before the next,
as Pewter's were.

- **C1 - Corona's file, Motif's frame and the looks.** `Corona.desktop`
  starting Astra's shell for now; `chrome = motif` in `ui.header` and a
  `SC_MOTIF` in the scene - the window-menu box at the left, minimise and
  maximise at the right, the frame bevelled all round; the window menu;
  `Indigo.theme`, `IndigoNight.theme` and `Solaris.theme`. Test: the
  gallery's applications under Corona, each with Motif's boxes and none of
  Astra's or Platinum's; the window menu's Minimise, Maximise and Close;
  Astra's unchanged. And a window resized from any of its edges and
  corners, not only the grip.
- **C2 - Desks, in the window manager.** `desks = N`; a desk on every
  window; `handlers.desk`; Occupy and Occupy All in the window menu;
  Alt and a number or an arrow; the `watch` events saying which desk.
  Test: a window opened on desk one is not composed on desk two; sent, it
  is; occupying all, it is on each; the switch composes one frame and asks
  no application anything; Astra has no desks.
- **C3 - The Front Panel.** `frontpanel.lua`, a bottom strip: the clock and
  date, Home, the Text Editor, Mail, Find, the desk buttons by name, the
  busy light from the `launch` events, the padlock, Exit, Style,
  Applications, the Trash, Status from `indicators.lua`; subpanels as
  folders, Install Icon. Test: each control opens what it names; a
  launcher dropped on a subpanel's Install Icon is in it after a restart;
  the busy light lit while a program starts.
- **C4 - The Toolchest and the sculpted desktop.** `toolchest.lua`, its
  menus `kosmosmenu.items`; `desktop:corona`, the icons from the right,
  minimised windows as icons down the left. Test: the Toolchest's menus
  hold what the Deskbar's does; the home folder at the top right;
  minimised, a window is an icon, and a double click brings it back.
- **C5 - Desks drawn small.** The window server composing a desk's windows
  scaled into a lent picture: the panel's live buttons, the Desks
  Overview, F3; a window's title dropped on a button. Test: a window moved
  on desk two moves in its miniature; F3's choice goes there; the
  miniatures cost, measured on the M700, under its budget.
- **C6 - Now Playing.** `struct audio_describe` in `audioproto.h`;
  `media.lua` describing its stream and answering `transport`; the panel's
  control and its subpanel; drag to play. Test: Music playing shows its
  song in the panel; the panel's pause pauses it; a song dropped on the
  control plays; a film opens in Video.
- **C7 - Levels and load.** The meter page in the audio server; the
  panel's levels each frame; Mixer reading the page rather than asking;
  the CPU GPU ♪ meter. Test: a stream's tone moves its meter and the
  master's; a muted one still moves its own; the panel allocates nothing
  per frame for its meters (`frames` says so).
- **C8 - Recording the screen.** First, `minih264e` measured on the M700
  at the screen's size; then `screenrec`, the Record Kit's BGRA entry and
  its output in pieces; the Record control, the frame round the screen.
  Test: ten seconds recorded play back in Video at their length; the
  desktop's frame rate while recording, measured.
- **C9 - The rest.** The sound in recordings (question 8), a palette for
  one desk (question 4), banners above the panel, the sculpted icons'
  badges.

---

## Open questions, for Diego

Each with what I would choose.

1. **The Front Panel: a program of its own, or the Deskbar in a third
   layout** (as the dock is the Deskbar laid out as Googlebook's)?
   *Recommendation*: its own program. The Deskbar's heart is the list of
   what is running, and CDE's panel has none; what they share - the
   indicators, the menu's folders, the launch events - is already a
   library each reads.
2. **Desks for every desktop, or Corona's alone?** *Recommendation*: in the
   window manager, turned on by `desks` in a desktop's file, so Astra can
   have them by adding a line, and Corona is not a fork of anything.
3. **Four desks, as CDE had, or as many as IRIX allowed?**
   *Recommendation*: `desks = 4`, any number from one to nine in the file;
   adding one while it runs later, if it is missed.
4. **A palette for one desk.** A look changed redraws every window, which
   is not instant on a desk switch. *Recommendation*: a backdrop per desk
   from C2, and a palette per desk only after a look change is measured on
   the M700 to fit in a frame.
5. **A minimised window: an icon on the backdrop (CDE, IRIX) or in the
   Deskbar as now?** *Recommendation*: on the backdrop, as the mockup
   does - Corona has no list of windows elsewhere.
6. **Sculpted icons: a Corona set, or Haiku's?** Kosmos draws Haiku's
   vector icons (`assets/icons/`), already shaded. *Recommendation*:
   Haiku's for C1 to C8; a Corona set drawn later if the backdrop wants
   IRIX's slabs, as the mockup's are.
7. **Now Playing's door: the audio stream described, or a server of its
   own?** *Recommendation*: the stream described and `media.lua`'s
   transport. The audio server already knows every stream and where it
   is; a second server would be a second list of the same things.
8. **Recording: its size, its rate and its sound.** Nothing captures the
   mix today and Kosmos has no audio encoder (FFmpeg's are decoders).
   *Recommendation*: silent recordings first, at the screen's size and 30
   frames a second if the M700 keeps up, half its size if not; the sound -
   a tap of the mix in the audio server, and AAC - as a step of its own.
9. **The 1999 / 2026 switch.** *Recommendation*: as with Pewter, build
   2026; 1999's feel kept as the Solaris look with Corona's bevels, rather
   than a second behaviour to keep working.
10. **The 2026 slide between desks**, about 220 ms. *Recommendation*: on,
    with the reduced-motion setting turning it off, and off by default
    until the GPU composes on the M700.
11. **The date control, with no Calendar in Kosmos.** *Recommendation*:
    Today, the notifications' history, until a Calendar is designed as an
    application with its four documents.
12. **The name.** *Recommendation*: Corona, kept; it has been the name
    since 7 November.

---

## What this does not do

- **Recreate CDE or IRIX.** Their manner, drawn fresh: no Sun or SGI art,
  no X, no Motif library, Kosmos's own applications.
- **Change desktop with windows open**, or mix desktops: a desktop is
  chosen whole (`pewter.md`).
- **Bring POSIX's desktop with it.** CDE's `dtterm` ran a Unix shell;
  Corona's Terminal runs Kosmos's.
