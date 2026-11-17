<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos IDE, the next one - where wonderful apps are made

Written on 17 November 2026, before any of it is built. Diego, the same
day: "review the kosmos IDE - it is outdated and it needs new features to
create wonderful apps".

The IDE is where a person writes a Kosmos application. Kosmos's bet is that
"an audio editor or a planet simulation is a weekend rather than a port"
(`CLAUDE.md`), and the kits are being built for that. The IDE is the tool
that turns those kits into a weekend's work, and today it stops short of
that. This document says what it does now, what the next one does, what
that is made of, and the order to build it in.

The windows are drawn in `docs/ide.html`. What it stands on is drawn in
`docs/ide-architecture.png`, rendered from `docs/ide-architecture.html`.
The IDE as it was agreed in September is `docs/kosmos-ide.html` and
`docs/ide-layout.html`, and those stay as the record.

Nothing here is built and nothing here is agreed until Diego says so. The
questions that are his are at the end.

---

## What the IDE is today

Read from `user/bin/apps/ide.lua` (3,121 lines, one file), its libraries,
`tools/run_ide.py` and `roadmap.md` 6n. It was built between 26 September
and 7 October 2026: steps 0 to 5 and 7 of 6n, TinyCC's C5 and C6, and the
tutorial's part one. Since then it has been touched three times, each a
change made to every application (a click as a press and a release, a
field's selection, the old browser removed).

**What it does**:

- **A project** is a folder. The sidebar has three tabs: **Project**, the
  folder as a tree; **Outline**, the file's functions; **System**,
  Kosmos's own Apps, Programs, Libraries, Kits, Templates and Examples,
  read only. It opens on the last project.
- **The editor** is the kit's `ui.editor` in its code look, one per tab:
  Lua coloured by `lualex.lua`, C by `clex.lua`, line numbers, the current
  line, marked lines, a scrollbar, undo, and the text larger and smaller.
  Its text and undo are `textbuf.lua`, which Text Editor's `docview.lua`
  also uses. Cafesa3D's Script panel uses the same `ui.editor`.
- **Finding**: in the file, in the project (Search tab), replace, go to
  line, and Find a file (Ctrl P) over the project and `/Kosmos`.
- **Run and Stop**: the file runs as its own process, as it is on the
  screen. The IDE serves the console protocol, so a program's `print`
  arrives in Output and Console, and a program that reads a line waits for
  one typed under Console. How the run ended is said, and an error's line
  is marked.
- **Checking**: Lua's own parser a moment after typing stops; luacheck,
  vendored and told Kosmos's names, on open, save and F7; and "ui has no
  slidr - did you mean slider?", asked of the library. Problems lists them.
- **Suggestions**: after a name bound by `use`, the names that library
  defines, each with the comment above it in its source (`libdoc.lua`);
  after `win:` a window's methods; `sys`, `fs`, `gfx` and the kits from
  the running tables; for C, what the project's headers declare
  (`cdoc.lua`).
- **C**: Build (F6) compiles the project's `.c` files with TinyCC into the
  project's image (`tccbuild.lua`), with TinyCC's problems as Problems.
  Run builds first when the C is newer than the image.
- **New Project**: a Lua app (one template, a window with a counting
  button), a Lua and C app (two), a C app (two), and ten Examples, copied
  into `/Home/Projects/<name>`.
- **The tutorial**: F1 opens its pages in JetSki, and Lesson's Project
  opens a lesson's finished project. Six lessons are written (1 to 5 and
  15) of the fifteen drawn in `docs/ide-tutorial.html`.

**Why it is outdated.** Each of these was checked in the code, not
remembered.

1. **The loop is slow.** Edit, Run, find the app's window somewhere on the
   desktop, close it, Run again. There is no preview beside the code and
   nothing restarts on save. Every tool a developer uses today (SwiftUI's
   previews, Godot's editor, LÖVE's quick loop, Swift Playgrounds) is
   built around that loop being instant.
2. **There is no debugger.** The `debug` library is absent from every
   process, rightly (`design.md` §5.3), and `lint.lua` says so: "and no
   debugger yet". An error ends the program with a line number; there is
   no stack, no value, no way to stop before it.
3. **There is no profiler within reach.** The system has the parts:
   kernel sampling (`profile`, `SPAWN_PROFILE`), the window manager's
   frame counters (`frames`), the collector's pause (`sys.info`). None of
   them is reachable from the IDE, so the first goal on `CLAUDE.md`'s list,
   *fast*, has no tool for the person writing an app.
4. **It knows little about the code.** Suggestions work after a library's
   name and `win:`, and nowhere else: not a project's own modules, not
   `self.`, not a function's parameters. There is no go to definition, no
   references, no rename, no hover. *Kits and their calls* and *Keyboard
   Shortcuts* in the More menu are disabled. And what the IDE does know
   (`bindings`, `members`, `library_problems`, lines 1198-1370) is inside
   `ide.lua`, so Cafesa3D's Script panel, which shares the editor, has none
   of it.
5. **It cannot start the apps Kosmos is for.** One Lua template, a
   counter. Nothing for a game, a 3D scene, a sound tool or a document
   app, although the kits have arrived since: the Playback Kit, the Media
   Kit, the synth, the Game Kit, the 3D and compute kits, the WebKit Kit,
   the LLM Kit, Automations' published controls.
6. **A program in `/Home` is given nothing it declares.** The launcher
   grants `audio`, `midi`, `camera` and the rest from the `needs` that
   `getattr` reports (`user/init/init.lua`, `launch`), and only `binfs`
   reads a header's `needs` (`user/servers/binfs.c`). `diskfs` does not. So
   an audio tool written in the IDE gets no MIDI and no audio band,
   whatever its header says. Found on 28 September with Groove's MIDI step
   (`roadmap.md`) and still true.
7. **Making an app real stops at the editor.** The name, icon, section,
   file types and powers are header lines typed by hand. An app of one's
   own cannot have its own icon (an icon is the name of one built into the
   image). Nothing installs it into `/Home/Apps` or makes a package, though
   Packages exists.
8. **Agreed and not built**: the panes resized by dragging (7 October),
   Format Code (wanted 7 October), `use("fractal.c")`, and a C app as its
   image.
9. **No tests for a person's app, and no history.** A file saved over is
   gone.
10. **luacheck runs in the IDE's own process**, on open and on save, so a
    long file holds the window while it is checked. How long has never been
    measured; step I1 measures it.

---

## What the next IDE does

In order of what most helps someone make a wonderful app. **New** is new;
**grows** is something there today made larger.

### The loop

1. **A live preview, restarted on save** - **new**. The app's own window
   shown inside the IDE beside the code, running for real: its own
   process, its own window, the real kit, nothing simulated. Saving
   restarts it in place, at the same size, with the same look. The
   preview has a size menu (the window's own size, or one chosen), a Light
   and Dark switch for the look, and a restart button. It is a hot
   *restart*, not a hot reload (see *Yours to decide*, 2).
2. **The preview inspected** - **new**. Hovering a control in the preview
   outlines it and names it (kind, label, size); a click goes to the line
   that made it. The names are Automations' (`/Running/<app>/controls`,
   built in 0.11.218), so nothing new is published to have them.
3. **New Project by what you are making** - **grows**. Kinds, each with
   templates that run as they are and say which kits they stand on: an
   **App** (a window with controls), a **Game** (a loop at the screen's
   rate, the Game Kit's pads, sprites), a **3D scene** (a camera, lights
   and a mesh), a **Sound** tool (a synth on the Playback Kit, MIDI in, a
   level meter), a **Document** app (Open, Save, Recent and its file
   type), a **Program** (prints and reads), a **Library** (Lua for other
   projects), a **Kit in C** (`KOSMOS_KIT` and its Lua), and the
   **Examples** as today.
4. **A person's programs given what they declare** - **new**, and a
   condition of 3: the Sound template needs MIDI and the audio band. Run
   from the IDE, a program's `needs` are read from its file and granted
   after the IDE asks once (*Yours to decide*, 5).

### Knowing the code

5. **Go to definition, references, rename** - **new**. F12 on a name goes
   to where it is made, in the project or in Kosmos (read only); Shift F12
   lists every use; F2 renames a local or a project function everywhere
   it is used.
6. **Hover and signatures** - **grows**. Resting the pointer on a name
   shows what it is and the comment above it; typing `(` after a function
   shows its parameters, the one being typed in bold. Completion knows the
   project's own modules, `self.` in a method, and a table's fields where
   the file makes them.
7. **The Reference** - **new**, and the disabled *Kits and their calls*.
   A sidebar tab of every library and kit, each name with its comment, its
   example where the source has one, and which bundled apps use it, so
   "how does Music play a file" is two clicks.
8. **Problems in the line** - **grows**. The problem's words at the end of
   its line, as well as the mark; *did you mean* taken with Tab; luacheck
   in a process of its own so typing never waits for it.
9. **Format Code** - **new**, wanted on 7 October: Lua and C set out in
   the system's style as one undo step.

### Seeing it run

10. **Stop at an error** - **new**, and the debugger's first part. An
    error does not end the program: it stops where it happened, with the
    call stack and every local, upvalue and `self`, and Continue ends it as
    today.
11. **The debugger** - **new**. Breakpoints in the gutter, Pause,
    Continue, Step Over, Step In, Step Out, the stack, variables opened a
    level at a time, watches, a breakpoint with a condition. Lua only;
    a fault in a project's C is named by the Debug Kit, function and
    offset.
12. **A console attached** - **grows**. The Console's line evaluates Lua
    in the running app: in the frame it stopped in when paused, in its
    main chunk between events when running. `celsius.text` answers
    `"20"`.
13. **The profiler** - **new**. While the app runs: its frame time, each
    frame a bar against the screen's 16.7 ms; CPU by Lua function (self
    and total); where the time is by layer (the Lua VM, the collector, the
    C kits, the window manager and the other servers, Nebula); bytes
    allocated a second by function; the worst collector pause. The
    busiest lines get their share in the gutter.

### Making it an app

14. **The app's page** - **new**. Name, icon, section, the file types it
    opens and the powers it needs, as a form; what is set is written into
    the header, which stays the one source.
15. **An icon of its own** - **new**. Chosen from the system's, or drawn
    in Paint at 16, 32 and 64 and kept in the project:
    `-- kosmos: icon icon.png`.
16. **Install and package** - **new**. *Install* puts the app in
    `/Home/Apps` so it is in the Deskbar and the launcher like any other.
    *Make Package* writes the zip and `package.txt` that Packages
    installs.

### Building windows

17. **The layout builder** - **new**. A window's controls placed by hand
    on a canvas that is the real window, from a palette of the kit's
    controls; each with its name, its words, its size and which edges it
    follows. It is saved as a form file that the code loads and names, so
    the code says what the controls do and the form says where they are.
    The form is text, so it can be read and edited by hand too.

### Keeping it right

18. **Tests** - **new**. A project's `tests/` folder: checks on its Lua,
    and checks on its window by its controls' names (`automate.lua`:
    press "Reset", read the field). Run from the IDE, results in a Tests
    tab.
19. **History** - **new**. Every save of every file kept; a file's
    versions listed, a version shown beside today's as a diff, and put
    back. Git when the Git client is designed (`roadmap.md`), with the IDE
    as its user.

### Learning and help

20. **The tutorial in the IDE** - **grows**. A lesson's page in a pane
    beside its code, rather than in another window; parts two to four
    written.
21. **Xavier** - **new**, after Xavier's X11. Ask about the selection or
    the file; a change Xavier proposes shown as a diff, taken or refused a
    file at a time; a run it asks for shown first. `xavier.md` already
    says what it does; the IDE gives it a pane and the diff.
22. **Keyboard Shortcuts** - **grows**: the disabled item opens
    Shortcuts on the IDE's keys (`docs/shortcuts.md`).

### What it does not do

- **Step through C.** A debugger for C needs a process to stop another
  and read its registers and memory, which is a power Nebula does not have
  and should not grow for this. A fault in a project's C is named by the
  Debug Kit, and C is debugged with what it prints. Asked again when a C
  application needs it.
- **Build the system's kits.** A kit in `/Kosmos/Kits` is linked into the
  image on the Mac. A person's own C is built into their project's image,
  as today, and a *Kit in C* template is one that Lua uses with
  `use("name.c")` (agreed on 6 October).
- **Edit a running app's code in place.** See *Yours to decide*, 2.

---

## What it is made of

**The application is the thinnest layer there is** (`CLAUDE.md`). Every
piece below was asked two questions: *does another application want
this?*, and if so it is a kit with one door; and *does something already
supply this?*, and if so it is used.

| Piece | Supplied by | New, grows, or as it is | Who else uses it |
|---|---|---|---|
| The editor, its text and undo, colouring | `ui.editor`, `textbuf.lua`, `lualex.lua`, `clex.lua` | grows: an inline note at a line's end, a breakpoint gutter, a cost gutter, a diff view | Cafesa3D's Script panel; Text Editor (`textbuf`) |
| Panes dragged | `ui.splitter`, which nothing uses yet | as it is | Tracker, Preferences, Mail (agreed 7 October) |
| **What a name is, where it is made and used** | **`luaindex.lua`**: `libdoc.lua` grown, plus what is in `ide.lua` today (`bindings`, `members`, `library_problems`) | **new**, from what exists | Cafesa3D's Script panel; Xavier, for the context it hands the model |
| Lua's parser and luacheck | `lint.lua`, run by a small program, `lint --report`, in a process of its own | grows | the build's own check; Cafesa3D |
| **Code set out** | **`luafmt.lua`** over `lualex.lua`, and C's over `clex.lua` | **new** | Cafesa3D's Script panel; Xavier's proposed edits |
| Running the app, its console | `run`, the Console Kit | as it is | Terminal |
| **The preview: an app's window inside another's** | **the window manager**: a *hosted window* | **grows**, beside the inset of 0.11.457 | the layout builder; the tutorial's pane |
| The preview's controls by name | Automations A1, `/Running/<app>/controls` | as it is | Automations, Xavier |
| **Stopping a program, its stack and values, samples of where it is** | **the Inspect Kit** (`user/kits/inspect/`): an agent in every Lua process's runtime, dormant until armed, and the door the IDE uses | **new**, C | the App Inspector (`roadmap.md`); Automations' scripts (A6) |
| A fault in C named | the Debug Kit | as it is | JetSki, Log View |
| Where the time is by layer | Nebula's sampling (`kernel/profile.c`, `SPAWN_PROFILE`) and its report's layers | grows: one process's samples, read live | the App Inspector, `profile` |
| A window's frame time | the window manager's counters (`frames`) | grows: per window | `frames`, the App Inspector |
| **A window's controls as a file** | **`form.lua`**: a form read and written; `ui.form(win, path)` | **new** | applications that use the builder; Xavier, which can write a form; Preferences' panes later |
| A person's program given its `needs` | the launcher in `init.lua`, and one header reader | grows | Tracker's double click; the agreed C app as its image (`KOSMOS_APP`) |
| The app's page, installing | the header; `/Home/Apps` (`elf.md` step 5) | as it is | |
| **A package made** | **`packages.lua`** grows `make`; `zip.lua`; the Crypto Kit's SHA-256 | grows | `pkg make` at the prompt; `tools/packages.py`'s work, done in Kosmos |
| An icon drawn | Paint | as it is | |
| Tests by a control's name | `automate.lua` | as it is | Automations |
| **Two texts compared** | **`diff.lua`** | **new** | Xavier's proposed changes; the Git client |
| **A file's earlier versions** | **`versions.lua`** | **new** | Text Editor |
| C built | `tccbuild.lua`, the C Kit | as it is | `tcc` at the prompt |
| A lesson's page in a pane | the WebKit Kit, a view | as it is | JetSki, Mail |
| Xavier | `llm.lua`, Xavier's tools (X11) | as it is | Write, Mail |

### Decisions, and why

**The preview is the real app, in its own process.** Not a picture of the
code, not a copy of the kit inside the IDE. A preview that is the app
cannot disagree with the app, and a crash in it is a crash in its process,
which the IDE reports as a Run does today. SwiftUI's previews run the code
for the same reason.

**And its window is hosted by the window manager, not drawn by the IDE.**
A kit window does not own pixels: it sends drawing, and the window manager
keeps the picture (`ui.md`). So the cheapest way to put the app's window
inside the IDE's is for the window manager to compose the picture it
already holds into a rectangle of the IDE's, and to send the pointer and
keys that land there to the app. No pixel travels between the two
processes and nothing is copied. The inset of 0.11.457 is half of this
already: a picture placed in a window's picture, composed by
`scene_core.c`. A hosted window is an inset whose picture is another
window. *Control by message, data by shared memory* holds: the IDE sends
one message to host the window, and the pixels never leave the window
manager.

**A restart, measured, rather than a reload.** On save the IDE ends the
process and starts it again, and the window manager puts the new window
where the old one was. LÖVE's loop is this. A reload that kept the running
state would have to decide what to keep of a program the IDE did not
write, which is the problem hot reload was removed from the servers for
(`CLAUDE.md`, *Hot reload is gone*). Save to first frame is measured in I5
and held to.

**The debugger is outside the program, and the program never has
`debug`.** `design.md` §5.3 keeps Lua's `debug` library out of every
process because it reaches through any abstraction. That stays. The
**Inspect Kit's agent** is C in the runtime that every Lua process already
links. It is armed only when the process was started with an endpoint at
`/Devices/inspect` in its namespace, which only the process that started
it can hand it - the IDE, the way it hands `/Devices/console` today. Armed,
the agent sets Lua's hooks from C (as `sys.budget` already does,
`user/init/sys_user.c`), and answers the IDE's questions from C. The
program's own Lua cannot reach any of it. So debugging is a capability:
the IDE can stop a program because it was given the door when it started
it, and nothing else can.

**Stopping is the agent waiting.** When a breakpoint is hit, the hook
sends one message to the IDE and waits for the next. The program's thread
is blocked on its own endpoint, in its own process, so Nebula has nothing
new to do. A program with threads of its own stops the thread that hit
the breakpoint; the others go on (said in the Debug pane).

**A breakpoint costs nothing until one is set.** The line hook is set
only while there is a breakpoint, and the agent checks a line against a
set of `(chunk, line)` pairs in C. Step and Pause set it for one step.
The cost with breakpoints set is measured in I8.

**Debugger control is messages; profile samples are a ring.** A request
for the stack or a table's fields is asked once and answered once, so it
is a message in a declared shape (`inspectproto.h`): a value is its kind,
its first 96 characters, and a handle to open it a level further, so a
table of a million entries is never sent whole. Samples recur on a clock
- the count hook every so many instructions - so they go in a
single-producer, single-consumer ring in a region (`inspectring.h`), and
the IDE drains it on its own clock. That is the rule from the audio
server, applied before it is needed rather than after.

**Layers come from Nebula, functions from Lua.** Nebula's sampling knows
which instruction a process was at, which says *the collector*, *the gfx
kit*, *waiting on the window manager*; it cannot say *which Lua
function*. The agent's samples say the Lua function and line. The
profiler shows both, and the App Inspector, still to be drawn, is the
same engine pointed at any application.

**The index is Lua, the agent is C.** The index is structure: names,
tables, files, done when a person types or saves. The agent runs on the
program's busiest path, inside every line hook, and must not allocate or
collect there. That is the *Language split* as written.

**Layouts as form files, not as code the builder rewrites.** Interface
Builder kept layouts out of the code; SwiftUI keeps them in it. A builder
that rewrites a person's Lua has to understand all of Lua to leave
everything else alone, and gets it wrong eventually. A form file is data
only: a Lua table of controls (kind, name, words, place, edges), read by
`form.lua` with no code allowed in it, and the program says
`local f = ui.form(win, "main.form")` and then `f.celsius.on_change =
...`. The form is text, so a change is readable in History and by hand.
And a control has a name from the start, which is what Automations wants.

**Installed apps stay a folder.** `/Home/Apps/<Name>` holds its Lua, its
image when it has C, and now its icon. A package is that folder zipped
with its manifest, made by the same `packages.lua` that installs it, so
the two cannot disagree.

### What travels how

| What | How | Why |
|---|---|---|
| A preview's pixels | **nowhere**: the window manager composes the picture it holds | it already has them |
| Host this window here, end it, start it again | messages to the window manager and the launcher | one-shot |
| Breakpoints, Continue, Step, the stack, a value | messages, `inspectproto.h` | asked once, answered once |
| Where the program is, every so many instructions | a ring in a region, `inspectring.h` | recurs on a clock |
| What a program prints, a line it reads | the console protocol, as today | as today |
| A file checked | the file written, `lint --report` started, its report read | once a save |
| A control's name and line | `/Running/<app>/controls`, and the line the agent noted | one-shot |

### The busiest paths

**A key in a long file** - `ui.lua`'s 10,708 lines is the file to measure
on:

1. `textbuf` changes one line.
2. `lualex` colours that line, and the lines after it only while the
   state at a line's end changed (a long comment or string opened).
3. The editor draws the rows that are on the screen, as drawing sent to
   the window manager.
4. Completion, if open, looks the prefix up in a sorted list.
5. After typing stops, Lua's own parser, `load`, in C.
6. On save, luacheck in its own process; the index updates that file.

Nothing on steps 1 to 4 is allowed to grow with the file. I1 measures a
key to its picture on `ui.lua` on the M700, by stage and by what each
allocates (*when a Lua process is on a deadline, ask what it allocates*),
and every step after is held to it.

**A preview redrawn while a person types.** The app draws through the
window manager as any window does; the window manager composes the hosted
rectangle into the IDE's place on the screen. The IDE itself does nothing
per frame, and its editor is not drawn again because the app drew. Two
processes, two cores where there are two.

**Save to first frame.** Save, end the process, start it, its first
frame hosted. Measured in I5.

**A program under the debugger.** With no breakpoint set, nothing is
hooked. With samples on, the count hook runs every so many instructions
and writes 16 bytes to a ring. Both measured.

---

## Steps

Each is used and tested before the next, and each leaves a test that is
never deleted. Only x86-64 runs the tests for now (`CLAUDE.md`). The
first five are the loop, which is what most helps; the debugger and the
profiler follow.

1. **I1 - the ground.** The panes dragged (`ui.splitter`, agreed on 7
   October), sizes remembered. luacheck moved to `lint --report` in a
   process of its own. A key to its picture measured on `ui.lua` on the
   M700, stage by stage, with what each allocates. **Test**: `run_ide.py`
   drags both edges and reads the sizes back after a restart; a long file
   checked while keys are typed, every key answered.
2. **I2 - Format Code.** `luafmt.lua` and C's over `clex.lua`, a tool and
   Ctrl Shift I. **Test**: on the Mac, every Lua file in `user/` formatted
   twice is formatted once (the same bytes), and its tokens are the
   tokens it had; in the guest, Format then Undo is the file.
3. **I3 - what a person's program declares.** One header reader for a
   file on any disk, used by the launcher; a program in `/Home` run from
   the IDE asks once for what it declares, as drawn. **Test**: a program
   declaring `midi` is refused it without the yes and given it with;
   `midi play` reaches it.
4. **I4 - New Project by kind.** The kinds and templates of *What it
   does* 3, each running as it is. **Test**: every template created and
   run, its window or its words seen; the Sound template heard by
   `x86-sound`'s reading of the device.
5. **I5 - the preview.** The window manager's hosted window; Preview in
   the IDE; restart on save; size and look; the inspected controls.
   **Test**: a template previewed, a line changed and saved, the new
   words seen in the preview's place on the screen, save to first frame
   printed; a click on a control in the preview goes to its line; a
   control: the app's own window gone from the Deskbar while hosted.
6. **I6 - the index.** `luaindex.lua` out of `libdoc.lua` and `ide.lua`;
   go to definition, references, rename, hover, signatures, the
   Reference tab, and Cafesa3D's Script panel given it. **Test**: on the
   Mac, the index of `user/` answers where `ui.slider` is made and every
   use of it; in the guest, F12, Shift F12 and F2 on a project.
7. **I7 - the Inspect Kit, part one: stopping at an error.** The agent,
   `inspectproto.h`, `/Devices/inspect`, an error that stops with its
   stack and locals. **Test**: a program that fails three calls deep
   stops there, its stack and a local read back exactly; a program not
   started by the IDE has no `/Devices/inspect` and cannot be stopped.
8. **I8 - the debugger.** Breakpoints, Pause, the steps, watches,
   conditions, the console attached. **Test**: breakpoints hit in order,
   Step In, Over and Out land on the lines expected, a watch changes, an
   expression typed in Console answers; the cost of a breakpoint set
   measured and held.
9. **I9 - the profiler.** `inspectring.h`, the frame times per window,
   Nebula's samples for one process read live, allocations, the cost
   gutter. **Test**: a template with a deliberately slow function finds
   it first; samples counted against what was run.
10. **I10 - making it an app.** The app's page, an icon of its own,
    Install, Make Package (`packages.lua` `make`). **Test**: a project
    installed appears in the Deskbar with its icon; its package installed
    by `pkg` on a fresh disk runs.
11. **I11 - the layout builder.** `form.lua`, `ui.form`, the canvas on the
    hosted window, the palette and properties. **Test**: on the Mac, a
    form written and read is the same table and code in a form is
    refused; in the guest, a button dragged in and saved is pressed by
    name.
12. **I12 - tests.** `tests/`, a small checks library, UI checks by name.
    **Test**: a template's tests pass, one broken by hand fails and names
    its line.
13. **I13 - history.** `versions.lua`, `diff.lua`, the History pane.
    **Test**: on the Mac, `diff.lua` held to known pairs; in the guest,
    three saves, the first put back.
14. **I14 - the tutorial in the IDE**, a pane, and parts two to four.
    **Test**: the lessons' suite as part one has it.
15. **I15 - Xavier in the IDE**, after X11. **Test**: Xavier's proposed
    change shown as a diff, refused, the file unchanged; taken, the file
    changed.

---

## Yours to decide

Each with a recommendation first.

1. **The preview inside the IDE's window, hosted by the window manager.**
   *Recommended.* The alternative is the app's real window placed beside
   the IDE and kept there, which needs nothing new in the window manager
   and loses the one-window feel, and the layout builder would want the
   hosted window anyway.
2. **A restart on save, not a reload that keeps the app's state.**
   *Recommended.* It is always right and it is LÖVE's loop. If restarting
   loses too much (a game's level, a document's scroll), a later step can
   add `ui.keep(name, value)`, which an app opts into and the IDE carries
   across one restart.
3. **The debugger reaches only programs the IDE started.** *Recommended.*
   That is the capability: the door is handed at start. Attaching to an
   app already running would need a consent step, and can wait for
   per-launcher permissions.
4. **Layouts as form files, not rewritten code.** *Recommended*, for the
   reasons above.
5. **A program in `/Home` asks for its powers the first time the IDE runs
   it**, and the answer is kept for that file until its header changes.
   *Recommended.* The other answers are to grant every `/Home` program
   what it declares (simple, and against *no ambient authority*), or to
   wait for per-launcher permissions (and the Sound template waits too).
6. **History now, Git later.** *Recommended.* A save kept is enough to
   undo a bad afternoon. Git is a client of its own, designed first, and
   the IDE will be its user.
7. **The tutorial in a pane of the IDE**, on the WebKit Kit's views.
   *Recommended.* The other answer is to leave it in JetSki, which costs
   nothing and keeps two windows.
8. **The order: the loop first (I1-I5), then knowing the code (I6), then
   the debugger and profiler (I7-I9).** *Recommended.* The preview changes
   every hour spent in the IDE; the debugger changes the bad hours.
9. **The kit's name: the Inspect Kit.** *Recommended*, since it is what
   the App Inspector stands on too. "Debug Kit" is taken by the one that
   names C addresses.
