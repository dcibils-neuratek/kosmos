# Kosmos — UI kit and window manager

Section 16 of the [design](design.md).

BeOS lineage, with the corrections a microkernel and a Lua userland make possible.

---

## 16.1 The split

Two pieces, in separate processes. Same as BeOS.

**The kit** (`lib/ui.lua`) is a library running **inside the app's process**. It handles the view tree, layout, event routing and the generation of drawing commands. If it has a bug, the app dies and nothing else.

**The app server** is a separate process. It handles windows, decoration, stacking, focus, workspaces, compositing and input routing. It knows nothing about views: to it a window is a rectangle with an endpoint on the other side.

That boundary is the one BeOS had, and it is correct. UI complexity lives in the app, where it can crash without consequence.

---

## 16.2 The view tree

A view is a rectangle with coordinates relative to its parent that knows how to draw itself and receive events. Same as `BView`.

```lua
local ui = require("ui")

local panel = ui.view{
  frame  = { 0, 0, 200, 400 },
  follow = { "left", "top", "bottom" },
}

panel:add(ui.button{
  frame = { 12, 12, 120, 28 },
  label = "Save",
  on_click = function() save() end,
})

panel:add(ui.list{
  frame  = { 12, 52, 176, 300 },
  follow = { "left", "right", "top", "bottom" },
  items  = notes,
  on_pick = function(i) open(notes[i].name) end,
})
```

Properties that matter:

- **Nested coordinates.** A view draws in its own system, starting at (0,0). The kit accumulates the translation as it walks down the tree.
- **Automatic clipping.** A child does not draw outside its parent. The kit intersects the rectangles and passes the clip to the server.
- **Bottom-up hit testing.** A click goes to the deepest view containing it, and bubbles up if that view does not consume it.
- **Invalidation by region.** `view:invalidate()` marks only that view's rectangle. Damage tracking falls out of the tree, not out of a heuristic.

---

## 16.3 Messages

A BeOS `BMessage` was a typed dictionary with a four-byte `what`. In Kosmos it is a Lua table with a `type` field, which is the same concept without the marshalling.

```lua
{ type = "mouse_down", x = 120, y = 44, button = 1, mods = { shift = true } }
{ type = "key",        char = "a", code = 0x61, mods = {} }
{ type = "resize",     w = 640, h = 480 }
{ type = "draw",       clip = { 0, 0, 200, 400 } }
```

The same kind of table that travels over IPC between servers. An input event, a message from the app to the app server, and a `read` to the filesystem server are the same class of thing.

Handlers register by type, and a view that does not handle a type lets it pass to its parent:

```lua
view:on("mouse_down", function(ev) ... end)
view:on("key", function(ev) ... end)
```

---

## 16.4 Layout: follow modes, no constraint solver

Original BeOS used resizing modes: each view declares which edges of the parent it stays attached to, and the kit recomputes on resize. Haiku later added a layout kit with a constraint solver.

**Kosmos uses follow modes.** The solver stays out.

```lua
follow = { "left", "right", "top" }    -- stretches horizontally, pinned to the top
follow = { "left", "top" }             -- fixed size, top-left corner
follow = { "right", "bottom" }         -- pinned to the bottom-right corner
```

Either form works, and the kit normalises the first into the second:

```lua
follow = { left = true, right = true, top = true }   -- the same thing
```

That is not a convenience. This section documented the list form and
`ui.lua` read the set form, so a list gave a table whose `.left` was nil and
every edge came out unpinned - the widget silently stayed where it was.
Nothing caught it for a long time because **no application had ever used
`follow`**: nothing could be resized until M13, so the entire layout path
was dead code that happened to compile. The first window to be dragged
bigger is what found it.

The reason is the complexity budget. A constraint solver is thousands of lines, hard to debug, and has non-obvious behavior when the system is overdetermined. Follow modes are fifty lines and cover 90% of what you need.

On top of that, two containers cover nearly everything else:

```lua
ui.row{ spacing = 8, children = { a, b, c } }
ui.column{ spacing = 8, children = { x, y } }
```

Proportional distribution with weights, without solving a system of equations.

---

## 16.5 What disappears from BeOS: the locks

This is the biggest improvement over the original.

In BeOS every `BWindow` was a `BLooper`, meaning a system thread with its own message queue. To touch a window from another thread you had to call `window->Lock()` and `Unlock()`. Forgetting was a crash or a deadlock, and it was the number one source of bugs in BeOS apps. The entire locking API existed because threads shared memory.

In Kosmos each window is a **coroutine**, not a thread. No preemption inside the process, no shared memory between processes.

**There are no locks. There is no `Lock()`. The class of bug does not exist.**

And you keep what made the architecture valuable: a busy window does not freeze the others, because each one yields control at its `receive`. If you want real parallelism inside an app (a heavy filter over an image), you spawn a kernel thread that sends the window a message when it finishes. The window receives it like any other event.

---

## 16.6 Draw: commands, not a shared buffer

BeOS gave the app direct access to the app_server's buffer, with a lock. Fast and fragile.

Kosmos uses model B: `Draw()` produces a list of commands sent over IPC.

```lua
win:on("draw", function(gc)
  gc:fill(0xf8f8f8)
  gc:text(12, 24, "Hello", 0x111111)
  gc:rect(0, 40, 200, 2, 0xcccccc)
end)
```

Underneath, `gc` accumulates tables and sends them in a single message at the end of the handler.

Commands being data gives you things BeOS could not do:

- **Cache the list** of a view that did not change and resend it without re-running the handler.
- **Log and replay** a window's drawing.
- **Inspect from the REPL** what an app is drawing right now.
- **Redirect to another display** without the app knowing.

The exception is shared memory for surfaces. It is requested explicitly with `direct = true`, justified by the use case, and is not the default path. That path is designed separately in [gfx.md](gfx.md).

**And there is a second reason to ask for one, which is not speed.** The first is the obvious one: an image canvas, video, a game - the whole surface changes every frame and describing it costs more than copying it. The second was *typography*, and **it stopped being true on 15 September**: this said that an application sending commands "cannot have a heading at 28 pixels and a paragraph at 16 on the same screen", because a text command carried a role - one of the four faces the desktop chose - and nothing else. A command now carries a size beside the role, the compositor resolves it against its own pool of faces, and a heading at 28 over a paragraph at 16 is what Music's window is made of (`testing.md` §18.79). What survives is the case that asks for *many* faces at once: the pool is eight beyond the four roles, and a document laying out arbitrary runs of type will exhaust it, which is why `pdfview` and `browser` still draw their own pixels while being nobody's idea of a game.

**The trade is that a direct window has no widgets**, and it is absolute rather than awkward: the compositor owns the pixels of an ordinary window and the application owns the pixels of this one, so a button drawn by the widget kit would be drawing into the copy that is not on screen. The chrome of such a window is rectangles the application knows the position of, and a click is a comparison against them. That is a real cost and it is the reason the mode is opt-in - it buys the page and charges for the toolbar.

---

## 16.7 The app server

Responsibilities, all in Lua except the blit:

**Decoration.** The BeOS yellow tab, which takes only the width of the title instead of the whole bar. It is the system's most recognizable visual decision and it is functional: it lets you see the titles of several stacked windows at once.

**Stacking and focus.** Click to focus. Plus the two features Haiku added that are worth having from the start: **stack** (several windows sharing one frame, with tabs side by side) and **tile** (attached windows that resize together). They come almost free out of the tab model.

**Workspaces.** BeOS had 32, with independent resolution per workspace. It is the best implementation of virtual desktops anyone has done. In Kosmos they are one more field in the window struct and a filter in the compositor. Twenty lines.

**Input.** Highest-priority thread, always. Non-negotiable. If an app hangs while drawing, dragging its window still works.

**And the window manager reserves exactly one key.** The first version took
Tab for "next window" and the arrows for "move the window", which was wrong
and took one screenshot of the widget gallery to see: Tab is how every user
interface moves between controls and the arrows are how every list is used,
so a manager holding them has decided no application may have a second
control.

Control-arrow is a terminal escape sequence this system does not speak, so
it takes the approach `screen` and `tmux` took for the same reason: one key
is reserved and it *introduces* a command rather than being one.

    Control-W then an arrow    move the focused window
    Control-W then Tab         focus the next window
    Control-W then Q           end the desktop
    Control-W then C           a literal Control-C to the application
    Control-W then Control-W   a literal Control-W to the application

**This paragraph used to open "there are no modifiers to escape into - no
Alt, no Super", and that is no longer true.** `hal/keys.c` reads the Windows
key on a PC keyboard and the Command key on an Apple one - the same HID usage
- and carries it up as an escape sequence, so there is a modifier now and
Super owns the desktop commands. What is left behind the prefix is the small
set that has to work on a keyboard with no Super key at all.

The pointer divides the same way. A press on a title bar is the window
manager's - raise and drag. A press anywhere else is the application's, and
is forwarded in that window's own coordinates, on the same queue as a key
and collected the same way. The compositor never calls an application, and a
click is not an exception to that.

**A press grabs.** Everything until the release goes to the window the press
landed in, and inside that window to the view the press landed in, even after
the pointer has left. Without it, releasing outside would deliver the release
to whatever happened to be underneath and leave two controls half-operated.

**Hover is not sent.** Every movement would be a message, an application
would poll a queue full of them, and the whole path would run at the rate the
pointer moves rather than at the rate anything changes. Movement *is* sent
while a button is held, because that is what a control needs to un-press when
you slide off it.

One key out of the application's vocabulary instead of five, and the one
taken is the one applications want least.

**Compositing.** Damage tracking over the list of dirty rectangles, a backbuffer in cached RAM, one blit to the framebuffer synchronized with vblank. If an app did not respond in time, compose with its previous command list and move on. Never block.

Two things that turned out to be the same rule, both found as flicker on a
real machine rather than in any test here:

**The cursor is part of the composite.** Drawing it onto the screen after
the blit is cheaper and wrong - every repaint blits the finished region over
it and puts it back on the next line, and the display is scanned out on its
own schedule, so it can be sampled in between. What that looks like is the
cursor blinking on every click, once on the press and once on the release,
because each of those repaints the window under it. Moving it costs two
rectangles of damage instead of one: where it was and where it is going.

**A window's damage waits until its drawing is finished.** A window's
commands do not fit in one message, so they arrive in several, and damaging
after each one composited the window half-drawn - the first message clears
the background and the widgets arrive over the following two. The surface is
written either way; only the damage waits. That is what a backbuffer is for,
applied one level up: an application composes off-screen and says when it is
done.

---

## 16.7b Where a new window goes

A window asks for a position and usually gets it. It is moved only when more
than a third of it would be hidden by windows already on screen - and then it
takes the first free quarter of the workspace, falling back to a cascade when
all four are spoken for.

**"Hidden" is measured as area, and that is the whole of the fix.** The rule
used to compare *origins*: two windows collided only when their top-left
corners were within a title bar of each other. That is the right question for
a cascade and the wrong one for a screen - two 850-pixel windows whose
origins are 110 apart pass it comfortably and bury each other by seven
hundred pixels. Four applications opening at login came up as a pile with an
inch of each showing, on a 1920x1080 panel that had room for all of them.

Three details, each of which was got wrong first:

- **The backdrop and the menu strip do not count.** Tracker's desktop window
  is the whole screen and lives under everything by construction, so counting
  it made every position on the machine occupied and the search a no-op that
  silently fell through to the cascade.
- **The trigger asks only whether the *new* window is buried.** Asking it
  both ways everywhere means a large window will not overlap a small one -
  and the Deskbar is a 210x266 panel, so every window on the machine fled it.
  A window is entitled to overlap a panel.
- **The quarter search asks it both ways.** There the opposite case is the
  one that bites: dropping an 850x482 window on a 380x112 one covers the
  small window completely while leaving nine tenths of the large one showing,
  which the trigger's rule calls fine.

## 16.8 Replicants

The most BeOS feature of all, and the one that turns out better in Kosmos than in the original.

In BeOS you could drag a view out of an app and drop it on the desktop or inside another app. It stayed alive and working. A clock, a CPU monitor, a mini player. It was implemented with `BArchivable` and by loading a binary add-on into the destination process, which was fragile and an enormous attack surface.

In Kosmos a view is **Lua source plus a state table**. Both are serializable with no special mechanism.

A replicant is a message:

```lua
{
  type   = "replicant",
  source = "...the view's code...",
  state  = { zone = "Montevideo", format = "24h" },
  needs  = { "/dev/clock" },
}
```

The destination process receives that, does `load()` on the source, instantiates it with the state, and mounts in its namespace exactly what `needs` declares. The replicant runs in the destination process with the capabilities it asked for, and none beyond them.

A replicant that asks for `/dev/clock` cannot read your files. In BeOS a replicant was native binary code with full access to the process hosting it.

This is the intersection of the system's three ideas: the Lisp Machine live image makes the code transportable, seL4 capabilities make it safe, and the BeOS idea gives it its purpose.

**The honest limit, now that it is built.** A replicant runs *inside* its
host's process, so the restriction is one the language enforces and not one
the kernel does: the address space is the host's, and Lua is what stands
between them. That is strictly more than BeOS offered - which was nothing,
since a replicant there was native code with the full run of its host - and
strictly less than a separate process would be. Something needing the
stronger guarantee should be an application in a window, which is a
different thing wanting a different mechanism.

And the evidence has to come from inside. The first version had the *host*
build the same restricted namespace and probe that, which measures the
function rather than the environment: opening the sandbox in `ui.replicant`
left the probe reporting a refusal exactly as before. The replicant tries
both paths itself now and leaves the answers where the host can read them.

---

## 16.8b The look: dimensional on purpose

**Reversed, September 2026.** This section used to say "take the structure,
not the skin", and it called bevels "a period costume" that would make the
system look like a museum piece. That was wrong, and it was wrong in a
specific way worth recording rather than quietly editing out.

**What it got wrong is that it conflated pastiche with a style.** Cloning a
particular 1998 desktop pixel for pixel is pastiche, and that part still
stands. But *committing to dimensionality* - raised, sunken, grooved, with a
palette chosen now - is not a costume. It is a design language, and it
carries information that flat design threw away and then spent a decade
reinventing badly with drop shadows and outlines:

- **raised** means you can press this
- **sunken** means content lives in here
- **a groove** means these two things are separate

The old text said a bevel "stopped being a useful lie once everyone knew
what a button was". That mistook the bevel for decoration. It is not
decoration; it is a two-pixel-wide sentence about what a thing does, read
without looking directly at it.

**And the argument that actually settles it is about personality.** Every
desktop now converges on the same flat rectangle with the same grey sans
face, for the same reason every car now has the same silhouette: convergent
optimisation - wind tunnels and regulation there, design systems and A/B
tests here. Kosmos owes compatibility to nothing and has no market to test
against. Spending that freedom on the house style everyone else already has
would be the one genuinely wasteful thing it could do.

So: **Kosmos is dimensional, and it means it.** The palette in
`/system/ui/theme` is ours and is chosen fresh; the geometry is deliberate.

**And a second look is being tried, in one application first** (15 September).
Diego, seeing `docs/music.html`: "i love the music app design. can we do it
for real in kosmos?", then "i might redo a lot of the current apps with this
style and design aesthetic". That design is flat, dark with an orange accent
and a large title - not what this section says the system is. So Music is
built as the **pilot**: what it needs goes into the kit (a text size, pictures
drawn at any size) rather than into Music, and when Diego has used it he
decides whether the other applications follow. Until he does, this section
stands and the flat look is one application's.

| | Kosmos does | Why |
|---|---|---|
| Buttons | Raised, with a light top-left and a dark bottom-right edge | It says "press me" before you have read the label |
| Content wells - lists, fields, text views | Sunken | The boundary between chrome and content is structural, so it should be visible |
| Panels and groups | A one-pixel groove, light under dark | Separates without drawing a heavy line |
| Window frame | The bar spans the window and the border takes its colour: amber when focused, grey when not | Which window is listening, seen from the corner of the eye without reading. See the row below |
| The focused window | Amber, warmer than 1998's `#FFCC00` | Keeps "the focused one is the yellow one" and drops the period palette |

**What is still true from the old section**, and is the rule that survives:

> If the decision is about how something *behaves*, copy what already got it
> right. If it is about how something is *shaded*, decide it fresh.

That rule was always right. What changed is the answer it gives about
shading: the answer is dimensional, not flat.

**The title bar is across the whole window, and not a setting** (22
September, `roadmap.md` 5y). It has gone both ways. It was a full-width bar,
recorded here as a departure from BeOS; then on 18 September Diego: "i love
the tabs in the windows like BEOS instead of the full windoe tab like we
have today", so a tab as wide as its title became the default and the bar a
choice in Appearance, with the pointer reaching what was behind beside the
tab as the eye saw it. The choice left with the looks, and on 22 September
Diego again: "i want to switch back the tabs from be os style to full
width". So every title bar is the window's width, and a shape an older
`/home/.appearance` saved is read by nothing (`tabs` in `wm.lua`, the
display harness's `tabs` phase). 0.10.113 has the tab, if it is wanted
back.

**A control that cannot be used is greyed, not removed.** The maximise box
of a window that cannot be maximised - one that draws its own pixels into
a surface of a fixed size - was left off its title bar, so the controls
changed from window to window. Diego, 22 September: "when a window cant be
maximixed we shouldnt remove the button we should just gray it out and
disable it". It is drawn flat, since raised is how this look says a thing
can be pressed, with its glyph in `text_dim`, and a press on it does
nothing.

**A scrollbar's thumb is the tab's colour, with a grip** (22 September,
`roadmap.md` 5y). Mac OS 9's Platinum filled the thumb with the accent a
person chose and ridged its middle; Diego, showing its Appearance control
panel: "i want the scrollbar handle to be colored after the tab bar color
as an accent color like how macos 9 had it". So the thumb is `tab` - the
part of a list you drag coloured like the part of a window you drag it
by - with four raised ridges, lit and shaded from the same colour
(`theme.toward`); the trough and the arrows stay the widget grey, so the
colour marks exactly the thing that moves. Mac OS 9 also emptied the thumb
in a window behind the front one, and that is not done: a kit window does
not know whether it is in front.

**The measurement that keeps this honest.** A bevel is more pixels per
widget than a flat rectangle, and `make frames` exists now. Composing is
already 83% of a busy pass, so the dimensional style is a thing to *watch*
in the profile rather than a thing to assume is free.

---

**Where an application's own look is switched, decided 16 September.**
Music is the pilot for the flat look and carries both palettes, and it began
with the `V` key - which is to say with nothing a person could find. Diego's
answer is **a control in the window**, in the foot rather than the transport
row: the transport is a seven-column grid of drawn controls and an eighth
would re-space every one of them, while the foot already holds quiet
secondary text. The key stays as a shortcut for whoever learns it.

**This is the pilot's answer, not yet the system's.** If the flat look
spreads, a per-application switch in every window is the wrong shape and it
becomes a desktop setting in Appearance - which is the third option Diego was
offered and deliberately did not take yet, because Music has to be lived with
first (§16.8b).

## 16.8c Clicking a row again opens it

**The kit had no notion of a double click, and that was a deliberate
refusal.** `tracker.lua` wrote it down where its file list wanted one: "A
double click would be the BeOS answer and this kit has no notion of one;
adding it to serve a single caller would be a widget change made for an
application, which is the wrong way round." That is a good rule and it held
for as long as one application wanted the gesture.

**Reversed for trees, 16 September 2026**, at Diego's asking: "I want double
click to open the folders like home and desktop, not only clicking on the
little arrow on the left." The disclosure marker is ten pixels wide and it
was the only way to expand a node - a target you have to aim at, in a sidebar
whose whole job is being glanced at and hit.

What changed is the *scope* rather than the principle. A tree is not one
caller: `ui.tree` is the kit's widget, Tracker's sidebar is one user of it,
and the Drives app and the Open and Save window will be others. A gesture
every one of them needs belongs in the widget, which is the same test the
original refusal applied and got the other answer to.

**The first press still selects.** A single click means what it always did;
the second adds opening rather than replacing it, so nothing that worked
before works differently. A row with no children ignores the second press,
since there is nothing to expand.

**How long "again" is, is read from the machine.** `sys.ticks` is CNTFRQ_EL0
- 62.5 MHz under QEMU's TCG and 24 MHz when the same machine runs on this
Mac's own cores under `hvf` - so a constant would be one interval in one case
and a quite different one in the other. The counter's own frequency is a
second wherever it runs.

**A second rather than half of one, and the difference was measured rather
than guessed.** The counter is the generic timer and QEMU advances it against
the *host's* clock while the guest's execution lags behind under TCG. Two
presses 0.12 seconds apart on this Mac arrived 46,187,937 ticks apart, which
at 62.5 MHz is three quarters of a second as the machine counts it - so
against a half-second threshold the gesture could not be performed at all,
and the widget looked broken when only the number was wrong. A whole second
is what a person manages on an emulated desktop and is still nowhere near two
clicks meant as two.

## 16.8d A path is a row of targets, and the punctuation belongs to a name

**Tracker's path line was a label and is now a trail**: each name in
`/home/Desktop` is drawn separately and clicking one goes there. The last
segment is deliberately not a target - it is where you already are, and a
control that does nothing when pressed is worse than no control.

**The rule worth keeping is about the gaps.** The segments are separated by
` > `, and the first version gave each name a hit area exactly as wide as the
name. That leaves three characters of punctuation between every pair of
targets which respond to nothing, and the failure is invisible: a press that
lands there behaves exactly like a handler that was never reached. It cost an
afternoon of looking for a bug in dispatch that was not there, because the
test clicked at x=30 and `home` began at x=32.

So **a segment's target runs to the start of the next one**, separator
included, and the trail has no dead pixels in it. This is the same answer
§16.8c gave the tree's ten-pixel disclosure marker, arrived at from the other
direction: there, a target too small to hit; here, a gap between targets that
should not have existed. Both say the pointer should not have to be accurate
about something the eye reads as one strip.

**Widths come from `gfx.measure`, never from `#text * gfx.font.w`.** The
faces here are proportional, so counting characters puts every span after the
first in the wrong place - and wrongly by an amount that grows along the line,
which looks like the last segment being broken rather than all of them being
shifted.

## 16.8e Places you make: a shortcut, found by what its drive is

**Tracker's Places holds Home, Desktop, and whatever a person adds**, which is
`drives.html`'s MyPhotos: drag a drive or a folder onto the sidebar, and the
box Rename uses asks what to call it - offering the folder's own name, there
to be typed over. A right-click takes a place back out, into the Trash like
every other delete in Tracker, so the wrong one is one drag away from
returning. It is the shortcut that goes, never what it points at; Home and
Desktop are built in and say so.

**Anywhere on the sidebar is the target**, not only the Places heading. The
trail taught that the pixels between targets should not be dead (16.8d), and
there is nothing else a drop there could mean.

**A place is a file in `/home/Places`**, named what the person called it, and
its attributes say what it points at in words - `path = "/home/Music"`, or
for one on a drive `volume = "fat:1A2B-3C4D"` and `within = "/Italy"`. A
stored value has to read as itself, so nothing about it depends on knowing
`drivesproto.h`.

**A place on a drive is found by what the volume *is*, never by its name.**
Names depend on the order drives arrived, and a unit number is handed out
afresh on every replug (`usb.md` 6c). The case that separates the two is
another stick that happens to be called PHOTOS while yours is away: found
by name, MyPhotos would silently open somebody else's drive. Found by
identity it says *unplugged* - dimmed, still in the list, as `drives.html`
draws it - and opens again the moment the real one is back, under whatever
name it now has. A volume with nothing to know it by, a filesystem with no
serial on a drive with no GPT, is refused as a place rather than remembered
by its name.

**The rule is `/lib/places.lua`, the rows are `/lib/sidebar.lua`, and what
is done with a row is the caller's.** The rule for `deskbarmenu.lua`'s reason:
the decisions are worth testing where a test costs no boot. The rows since
USB step 6d, when the Open and Save window became the sidebar's second user
and `drives.html` asked for "the same sidebar as Tracker" - so it is the same
code, moved out of `tracker.lua` as it was, one `sidebar.new()` per window.
Making and removing places stayed Tracker's; an Open window only gets
around. `tools/test_places.lua` is 17 checks, and the control that
matters keys `resolve` on the name: the replugged stick and the other stick
called PHOTOS both then open `/drives/PHOTOS/Italy`, the wrong drive, and
both checks fail.

**Two changes to the kit came with it, both small.** The tree has
`node_at(y)`, which its own clicks now use too: a drop and a right-click
need the row under the pointer, and a second copy of that arithmetic in an
application would be a row height it does not own. And `/drives` is asked
once per refresh, shared by the Drives group and any place on a drive -
only a place on a drive asks at all, so a sidebar without one costs the
first frame nothing.

## 16.8f The Open and Save window, and the three things the kit took from it

**Every application opens and saves through one window, `/lib/panel.lua`**,
and since USB step 6d it is the one `drives.html` draws: the same sidebar as
Tracker - Places, System, Drives - the path as a trail, and the folder as
Name, Size and Kind. It runs inside the application that opened it, so it
sees exactly what that application can and nothing more, and what it hands
back is the file at its real place: an application never learns that
MyPhotos exists. Its interface did not change, so Editor, Photo, Reader and
the PDF viewer have the new window without a line changed.

**One click selects; a second click, Enter or Open opens.** The old panel
chose a file the moment it was clicked, which is the one place in the
system a single click did something irreversible - Tracker and its sidebar
select on one and open on the second (16.8c). A folder is entered, a file
is chosen, and in the Save window a file's name is offered in the name box.

**A caller may pass `filter`**, a function of a name, and files it answers
false for are not shown. Folders always are, because a folder is how you
reach the files.

**Cancel calls `on_cancel`.** The old panel's header promised it and nothing
ever called it.

**Three things moved into the kit, each because it had a second user.**

- **`ui.trail`**, Tracker's path line, with its two rules from 16.8d: a
  segment's target runs to the next, and widths are measured, never counted.
- **A list that draws its rows through `draw_item`** when they have fields.
  Which row, the scrolling and the selection stay the list's; a caller paints
  the row. A list without one draws its items as text, as before.
- **A list that opens a row with `on_open`**, on a second click on the same
  row or on Enter - timed by `ui_again()`, the tree's one second of the
  counter, now shared by both so a second click means the same thing
  everywhere. A list with no `on_open` behaves exactly as it did.

## 16.8g The level bar, drawn by the window manager

**A key that changes a level shows the level**, over everything, for two
seconds after the last press, and then fades in a fifth of a second. It is
`docs/levels.html` as Diego approved it on 18 September - after macOS's
Display and Sound panels, top right under the bar, and smooth rather than
notched: a dark rounded panel titled by what it controls, a small and a large
icon either side of a track with a knob. Muted keeps the level and greys the
fill, with a cross by the small speaker. The Sound half is built; Display
waits for the ThinkPad's brightness.

**The window manager draws it, because the window manager took the key.**
The volume keys are the system's and never reach a window (the ThinkPad keys,
`roadmap.md`), so the panel is drawn at the moment of the press from the
level the audio server has just answered, with no process between the key and
the picture. It is drawn once, into a surface of its own, when the level
changes; each frame only blends that surface over the windows with one alpha
for the fade, before the pointer is drawn.

**Shapes built so nothing is drawn twice.** `fill` replaces pixels and
`disc` blends only its anti-aliased edge, so a rounded rectangle is four
corner discs and then three rectangles over them: the rectangles replace what
the discs left inside, and the corners keep their smooth edge. Everything in
the panel is pre-mixed at the panel's own opacity, because a less opaque
colour written in by replacement would be a hole.

**Drawing it may not cost a key.** The first version called
`gfx.surface(w, h)` - written from memory; the kit takes a table,
`gfx.surface{ w =, h = }` - and the error it raised inside the key handler
took the window manager's keys down with it: the bar never drew, and of three
presses only the first was heard. The level had already changed by then. So
`osd.show` runs under `pcall`, a failure to draw is said in the log, and the
keys go on working; `run_media.py` counts every press (`testing.md` 18.94).

**One table, `osd`, because `wm.lua`'s main chunk is at Lua's limit of two
hundred locals.** The bar first went in as twenty of them and the file stopped
loading.

## 16.8h A menu bar above a window that draws its own pixels

**A direct window can carry a menu bar**, and the window manager draws it.
`ui.window{ direct = true, menubar = { { title = "File", items = {...} } } }`
sends the titles; the window manager puts a strip of them above the
application's buffer, drawn as `ui.menubar` draws - `theme.chrome` over
`theme.raised`, a groove under it, the same title spans - and the window
is the buffer and a strip tall. A press on a title is a `menubar` event
saying where the menu goes; `window:direct_event(ev)` opens the items as
an ordinary kit menu, and takes the menu's own events after, so an
application with its own loop hands it every event first. Everything the
application is told about the pointer, and every commit's damage, is in the
buffer's coordinates.

**Why the window manager and not the application.** A direct window's
pixels are the application's memory, so the kit cannot paint a bar into
them, and each game painting an imitation of one would be a menu bar in
three styles by the third game. Diego chose this on 18 September for the
Super Nintendo's File menu, and Doom and Quake can have one the same way.
The menus themselves stay the kit's, because a menu is already a window a
window owns.

## 16.9 Themes, and colours that are named rather than captured

**Since 22 September there are four looks and nothing else to choose**
(`roadmap.md` 5y): Plex, Plex Night, Classic and Studio, in `themes.lua`,
each a whole designed in `docs/looks.html`, all four naming the same faces.
The Appearance panel offers a look and a wallpaper, and
`/home/.appearance` holds those two. What follows is how a theme came
to carry its faces at all, and the per-role choices the panel no longer has.

**A theme is its colours and its faces.** There were two palettes - `dark`,
which is what Kosmos looked like first, and `light`, the 1998 one on
purpose - then the four in `themes.lua`, Photon, BeOS, Platinum and IRIX,
each a system that solved the dimensional look differently (16.8b). They
were colours and nothing else, and the five font roles were chosen one by
one. On 22 September 2026 a theme became complete (`roadmap.md` 5s): Diego
asked for one called **Plex** that looks "exactly as the mockup, same fonts
same sizes same spacing, same colors", and then "like a theme is a complete
color scheme + font selection?".

So a theme file names a face and a size for each role beside its colours:

```
name         = plex
desktop      = #3d63b8
font.ui      = ibmplexsans 14
font.heading = ibmplexsans-semibold 15
```

Every theme that ships names all five, so what it looks like is written in
it; the four that were palettes name the faces they have always had and did
not change. A file somebody writes may name only what it changes, and the
rest comes from the theme it is based on and the faces the system ships
with (`theme.default_fonts`) - the rule colours already followed.
`appearance` lists every theme, and **choosing one sets its colours and its
five faces**; a face changed afterwards is the person's own, and the panel
says so beside it (", yours"). `Back to this theme` puts both back. The
choice is written to `/home/.appearance` - the theme's name, the desktop
colour, and all five faces spelled out - and read back at startup, where
the window manager finds the theme by name in `themes.lua` or
`/system/themes` and says `wm: theme <name>`. It knew only `dark` and
`light` until 22 September, so every other theme came back as `dark`.
`wm appearance:--theme plex` does from a command line what a click on the
row does.

**Applying a palette never touches the faces.** `theme.apply` copied every
field of what it was given, which was the same thing while a palette held
only colours; with a theme's faces in it, the copy would have replaced
`theme.fonts` - the faces a process has in force - in every window sent the
palette, without one face being loaded to match. It copies the colour
tokens and nothing else now, and the faces are applied on purpose by
whoever chose the theme. `tools/test_theme.lua` holds both, with a control.

**The layout is fixed, and no theme changes it** (`roadmap.md` 5x,
`theme.metrics`): a row is 24 pixels, a button 28, a field 26, a window's
tab 26 and the Deskbar 32, in every look and at every face, and the kit
centres the words of the face in force inside those boxes. A look's faces
are chosen to fit them. For a morning on 22 September a theme could pad
rows, buttons and fields; a larger face then moved every widget below it,
and Diego asked for the layout to be fixed instead. The tab's number said
20 here while the window manager drew 26; it is 26, and `wm.lua` reads it
from `theme.metrics` rather than keeping a copy.

**The Deskbar is the look's tab colour, and 32 pixels tall.** For an
afternoon its colour and its height were each a choice in Appearance -
eight colours, and 36, 44 or 52 pixels - kept in `/home/.appearance` over
the theme. The looks took the colour back (`roadmap.md` 5u), and a look
named its Deskbar's colours, `bar` and `bar_text`, apart from its tab's -
Plex's stone, as the mockups drew it - until Diego: "the deskbar tab color
should be yellow or at least the same color of the acccent color of the
theme". So the tab's colour is the look's accent, on a focused window's
title bar, across the Deskbar and on a scrollbar's thumb, and `bar` is not
a token any more. He fixed the height too: "Taskbar size should not be
changeable let's make it fixed at 32" (5v). A height saved before is
ignored. With them went `win.on_theme`, the hook the Deskbar
resized itself from, which nothing else used once the layout was fixed.

**How large all of it is will be one number** (`roadmap.md` 5z, drawn in
`docs/looks.html` and approved, not built): a scale, chosen on a stepped slider in
Appearance, applied by the window manager to every length and every face
as it draws - so the layout above keeps its proportions at every step,
where a face made larger on its own would not.

**The palette table is mutated in place, never replaced.** Every widget
reads `theme.text` at the moment it draws, so changing the fields of the one
table changes what the next repaint looks like across all of them, with
nothing subscribing to anything. Swapping in a new table would leave every
existing reference pointing at the old one and the theme would change only
for windows opened afterwards.

**A colour may be a number or the name of one in the palette**, and a name
is resolved on every draw:

```lua
ui.label{ text = "Widgets", color = "text_dim" }   -- follows the theme
ui.label{ text = "Widgets", color = 0xffc9d1d9 }   -- exactly that colour
```

The distinction is not academic and cost a debugging round. `color =
theme.text_dim` reads the palette *once*, at construction, and freezes that
number - so a window followed a theme change while the labels inside it did
not, and the light theme had near-invisible headings still holding the dark
palette's near-white. A number still means exactly that number, which is
what an application wants when it is drawing something that is not part of
the theme at all.

The window manager reads the palette through accessors for the same reason,
and one of them found a second instance of the same bug: `window.background`
was resolved at creation, so a window's *body* kept its old colour while
every widget in it changed.

---

## 16.11 The clipboard, and a machine with no modifier keys

**One buffer, held by the window manager.** A clipboard is state shared
between programs that are not allowed to reach each other, which is exactly
the shape of the screen and of the console, and it gets the same answer: the
one process both of them already talk to holds it, and everybody asks. There
is no global name and no shared page. An application that was never handed
`/app/wm` has no clipboard, which is the correct answer rather than a
missing feature.

`wmproto.copy(text)` and `wmproto.paste()` are the whole client side, and
they are in `wmproto` for the reason `poll` is: it is the one module that
knows the shape of a message to the window manager, and a second place that
built one by hand is what cost a factor of a quarter of a million in 0.9.1.

### What the keys had to work around

**There are no modifier keys on this machine.** A virtio keyboard gives
Control plus a letter and nothing else - no Alt, no Super - and keys reach
an application as a byte stream, so shift-plus-arrow arrives as the same
four bytes as an arrow. Two consequences, and both of them shaped the
design:

**Control-C was not available, and now it is.** This section argued that it
is the key that stops a program, that eighteen checks in `run_screenshot.py`
use it to get the screen back, and that the CUA triple therefore could not be
had - so the four edits went behind the prefix, `Control-W c` to copy.

Both halves of that have since stopped being true, and the correction is
worth keeping because neither was refuted by argument. **A modifier
arrived**: Super is read by the board and carried up as an escape sequence,
so the window manager has its own place for a command and does not need to
borrow one. And **Control-C ending the desktop was never a decision** - it
was the first line of `key`, before anything could look at the byte, dating
from a window manager that had no applications in it. What it meant in
practice was that pressing copy closed the desktop.

So the four are the four:

```
Control-A    select everything in the focused control
Control-C    copy
Control-X    cut
Control-V    paste
```

and ending the desktop moved to `Control-W Q`, behind the prefix, because it
is the most destructive thing this keyboard can do and two deliberate presses
is the right price for it. `Control-W C` sends a literal Control-C through,
which nothing needs yet - the Terminal has never interrupted a child - and
which exists so that the day it does, the key is reachable.

**What a person has to learn is the system commands, not the editing ones.**
That is the whole of the change: §16.6's argument for one reserved key rather
than five is intact, and what sits behind that key is now only the things
that could not sit anywhere else.

**A selection is made with the pointer, not with shift.** `ui.editor` holds
an *anchor* and a *cursor* and nothing else: the press sets the anchor, the
drag moves the cursor, and any key that moves the cursor on its own drops
the anchor. That is why a click deselects without anything having to say
so, and it is the whole model - there is no separate selecting flag and no
state where a selection exists with the caret somewhere else.

`ui.field` gets select-all and not a dragged range, and that is a decision
rather than an unfinished job. A single line of text has one selection
anybody actually makes - all of it, to replace it - and the machinery a
range needs would be spent to let somebody drag over half a URL.

### What crosses, and what does not

**The window manager is told the intent, not the text.** The prefix posts
`{type = "copy"}` to the focused window; the window hands it to the focused
widget; the widget decides what its selection is and sends the bytes back
as a `clip_put`. So the window manager holds a string it never looked
inside, which is the same division it already keeps with pixels, and a
widget never has to know which keys a board happens to have.

### The cap, and why it is visible

A message is `MSG_BYTES`, which is 2048, and the text travels inside a
serialised table - so a copy larger than about nineteen hundred bytes
cannot cross in one piece. §7.4's rule in `design.md` says a *stream*
belongs in shared memory and a one-shot payload is fine as a message; a
copy is one-shot, so a message is the right shape and the cap is the honest
edge of it.

**The cap is enforced in `wmproto`, on the way out, and that is not a
detail.** A table too big to serialise makes `fs.send` *raise* in the
caller: capping in the server would be too late by one process, and an
application that copied a long report would die where it stood. The window
manager keeps a bound of its own anyway, because a server does not get to
assume its callers are the library.

**And the truncation is shown rather than mentioned.** When more was
selected than fits, `ui.editor` moves the highlight back to exactly the run
that left. A selection is already a picture of a range of text, so
shrinking it makes the limit something you can see, in the one place you
were already looking - where a dialog saying "1900 of 4212 bytes" would be
the same fact, later, and in the way.

## 16.12 The desktop: icons where they are put, launchers, and pictures

The desktop is Tracker in backdrop mode, showing `/home/Desktop` and
nothing else. Several things arrived together in September 2026, and they
belong together because each one is a decision about what that folder is.

**It is the screen less the strip.** The backdrop asks for the screen,
because the screen is all it knows. The window manager sizes it from
`reserved_top` - what the strip across the top has claimed - and when the
strip starts after the desktop, or closes, `fit_backdrop` moves and resizes
it and posts a `resize`. A desktop that draws its icons from its own
top-left corner is then below the bar without knowing there is one.

**An icon is where it was dragged.** The place is two attributes of the
file, `desktop_x` and `desktop_y`. BeOS kept it the same way and for the
same reason: the place belongs to the file, so it moves with it and goes
with it, and no positions file beside the folder can fall out of step. A
drag from the desktop let go on the desktop moves the icons by how far the
pointer went; let go on a folder's icon it moves the files, as a drop does
anywhere else. An icon never placed takes the next free cell down the first
column. `user/lib/iconlayout.lua` holds that arithmetic and a name's two
lines, so both are checked on the build machine rather than by looking.

**A launcher is an empty file that says what to start**: `kind=launcher`,
`program`, `args`, and an `icon` if it wants one. Opening it sends the
window manager the `launch` the Deskbar sends, so a launcher starts nothing
the Deskbar could not, and the window manager's check on the program's name
stays the only check. `launcher` makes one at the prompt. Drive - Tracker
at `/` - is one, and it, the cheat sheet and the Trash are on every
desktop, put back whenever they are missing. A launcher is not a link: a
link is another name for a file, and this starts a program with arguments.

**The Trash is a folder.** Delete moves things into it, under a free name
if it holds that one already, and only inside it does Delete destroy
anything. Emptying it is in Tracker's File menu, by name, because it is the
one delete that cannot be taken back. A folder rather than a flag on each
file, because a folder is already something every window can open, list,
drop onto and take things back out of - and dragging onto it is how a thing
is thrown away from the desktop, which has no menu bar and never gets keys:
the backdrop is never raised, and the window manager gives keys to the top
window.

**And it hides nothing.** The desktop is cleared to transparent between its
icons rather than filled, `compose_rect` leaves it out of the occlusion it
culls with, and the compositor blends it over what is underneath instead of
copying it. That layer - the wallpaper, or the flat colour, and the version
stamp - is painted only where no window reaches, so while the desktop
counted as an opaque window covering the screen a chosen wallpaper was not
hidden behind it: it was never painted at all. Icon labels lost their filled
background with the same change and carry a one-pixel shadow instead, which
reads on a dark picture and on a light one, where a box of the desktop's
colour reads as a mistake.

**Pictures come from the programs.** `-- kosmos: icon App_Tracker` in a
header is reported by `/bin` next to `section`, and the Deskbar draws it
beside the name. A menu with any picture in it gives every row the
picture's height, because nothing here scales one and a column of names
that did not line up would be worse than a taller menu. The icons are
Haiku's, and `assets/icons/README.md` says which, and from where.

---

## 16.13 One bar: the Deskbar is the strip

There were two pieces of chrome that were always on the screen: the Deskbar,
a panel in the top-right corner with a menu and a list of what was running,
and `topbar`, a strip across the top with five shortcuts and a clock. The
shortcuts were a hard-coded list in a source file - a menu that cannot be
edited, sitting beside a menu that can.

It is one strip now, 32 pixels tall (36 until 22 September), and
`topbar.lua` is deleted:

- **The Kosmos menu at the left**, which is `/home/Deskbar` read off the
  disk. Right-clicking it offers **Reload Menus** and **Open Deskbar
  Folder**, the second because nothing on the screen said the folder
  existed.
- **A button per running window across the middle**, each drawing that
  application's own picture. The window manager reports the *program* that
  opened each window - a path - and `/bin` reports what its header declares,
  because a title cannot give you a picture and changes whenever the
  application likes.
- **What the machine is doing at the right**: processor and memory meters,
  the network, the volume, the battery, the date and the clock. Each one
  opens the application that owns it.

**24-pixel icons in a 32-pixel bar**, since 22 September (`roadmap.md`
5v). They were 32 in a 36-pixel bar, because 32 was the only size the image
carried and nothing scaled a picture - any other number would have been a
crop. The image carries Haiku's 16s and 64s now, and `gc:icon` draws any
size other than those three by averaging the 64 down (`stretch`'s
`smooth`), so a bar Diego fixed at 32 holds its icons with four pixels
above and below.

**Drawn as one view rather than a row of widgets.** A `ui.button` is a
bevel, a label and a focus ring, and none of those belong on a bar; what
this wants is something you can click, which is a fill and a picture. That
is `topbar.lua`'s decision and it outlived the file.

**The shades are a ladder, not three numbers.** The strip is `theme.tab`
under a gradient, a button is a touch lighter, and a pressed button is a
shade darker *than the button* - measured from the button rather than from
the strip, or a pressed one ends up darker than the bar and lighter than its
neighbours, which reads as a hole. Corners are rounded by a two-entry table
of insets, which is Mac OS X's menu-bar highlight: a hard rectangle reads as
a panel bolted on.

**A second click on the focused window's button minimises it.** The only
gesture on the bar that has to be learned, and the alternative - a click
always raises - leaves the button under the window you are in doing nothing
at all.

**Instant feedback.** A control paints its new state at the moment it is
pressed, out of what the program already knows, and never waits for a
periodic tick to learn what it just did. Diego's words, on the ThinkPad:
the click has to feel like the interface responding in real time, not
catching up. The server's answer still wins - the early picture is the same
answer arriving sooner, and when the request is refused the program asks
again at once rather than keeping its guess. A press on a window's button
paints the button before the window manager's list says anything, and a
press on the Kosmos end is lit by the repaint the press itself causes.

**And the bar is told when the list changes, rather than asking on a
clock.** It learned the focus by asking the window manager for its list of
windows on its tick, which is once a second, so a focus that moved anywhere
but on the bar - a click on a window, Control-W Tab, a window opening -
reached the bar up to a second late: 290 to 1029 ms under QEMU, about 600
on average, and "like half a second" on the ThinkPad. Its own clicks were
quick all along, which is why the fault read as the bar being sometimes
slow. Now a `windows` request may carry `watch` with the caller's handle,
and the window manager posts that window a `windows` event whenever the
answer would differ:

- **The event carries nothing.** A list of titles does not fit in an event,
  and the reply is the one place the list is written, so the bar asks again
  and there is no second copy to fall out of step.
- **Posted, never sent.** It is raised inside the compositor's loop, and a
  synchronous call from there to a process that is not answering stops the
  desktop.
- **Compared once a pass, not announced by whatever moved a window.** The
  list changes in `raise`, `open`, `close`, `minimise`, the reaper and
  whatever sets a title; `tell_watchers` looks at the list itself, before
  polls are answered, so a change is delivered in the pass that made it and
  nobody has to remember to announce the next kind.

**Pressed in means the window you are in, and a minimised window is not
one.** The window manager reports as focused whatever is on top of its
stack, and a window put away by its own minimise box stays on top - so the
bar drew a minimised window as the selected one. The bar's click already
treated a window as selected only while it was showing; the drawing now asks
the same question, through one predicate, so the picture and the gesture
cannot disagree.

**Every window keeps a button, however many there are.** They share the room
between the menu and the indicators, capped so that two windows do not each
get half the screen, and shrink past that: icon and title, then icon alone,
then slivers. A sliver is ugly and it is *reachable*, which is the property
that matters - this first stopped shrinking at icon width and dropped any
window that did not fit, which is a taskbar hiding the thing you are looking
for.

**The indicators are drawn only when the machine can answer.** `topbar.lua`
refused to draw them for subsystems that did not exist, on the grounds that
a picture which lies about what the system knows is worse than a gap. What
decides it now is the kernel rather than a comment: `needs audio` grants
nothing on a board with no sound card, so `/dev/audio` is absent from the
Deskbar's namespace and the speaker is not drawn. The battery is the
exception and says so - it is drawn with a question mark beside it, because
this machine cannot read one and a battery drawn at 72% would be
indistinguishable from a battery that works.

**The Deskbar holds `network` and `audio` now, and that is a change of
position.** It declared `needs screen` and nothing else, on the argument
that launching goes through the window manager so that reaching the Deskbar
is not reaching everything. That still holds for *power* - `processes` stays
with the window manager, and Restart is a request this sends. What changed
is that the bar is where a person manages the machine from, and a bar that
cannot see the volume cannot show it. **Reading state is not holding
power**, and the two are kept apart deliberately.

## 16.14 Log View: console output, drawn the way the console draws it

Diego, on the ThinkPad: the log viewer should look like the Terminal, on
black, and scroll as lines arrive; it had an overlapping title and grey text
nobody could read. All three were reproduced in QEMU at 1920x1080 before
anything was changed.

**It was a document widget showing console output.** Log View was a
`ui.text` - built for paragraphs of help - with a heading label above it and a
status line below, on the window's own colour. `ui.text` draws body text in
`text_dim`, which in the BeOS palette is #808080 on the panel grey #d8d8d8.
And `ui.text` and `ui.label` lay text out on the cell `ui.lua` copied from
`gfx.font` when it loaded - the bitmap's 8x16 - while the compositor draws a
string with no role in the interface face. With a 20-pixel TrueType face
chosen, rows sat 16 pixels apart, the heading's line ran into the first row's,
and the heading and the status line were both cut short. At the default font
the two cells agree, which is why no screen the harness photographed had ever
shown it.

**A log is console output, so it is drawn the way the Terminal draws console
output**: `theme.console` and `console_text`, black in every theme; the `mono`
face; the row height and column width asked of `gfx.height("mono")` and
`gfx.measure("0", "mono")` on every draw, so a face changed while the window
is open is followed. Lines are wrapped by character rather than clipped: a log line is
whatever somebody wrote, and the end of a boot line is usually the part worth
reading.

**Colour by shape, in the console's numbers.** Faults in 0xda3633 and boot
stages in 0x3fb950 - `CONSOLE_COLOURS` in `init.lua`, which are the dark
palette's values - rather than `theme.bad` and `theme.good`, which are chosen
for the window's colour and in BeOS are dark red and dark green: dark on
black. The stage pattern had matched nothing since the kernel began starting
every line in the ring with a stamp, `[12.345] `, so it is matched after it.

**It follows the log, and holds still while you read.** `back` is how many
rows the view sits above the newest. At 0 whatever arrives is drawn at the
bottom. Scrolled up by any amount - the arrows, backspace and space, the
kit's scroll bar, or dragging the text - the text on screen is *held*: it
stays what it was however much is written, and `new lines below` appears in
the corner where the Terminal says what it is running. Back at the bottom it
takes the newest text there is at that moment and follows again. Each of
those is painted in the same pass as the input that caused it.

**And the header says which of the two it is in** (16.20): `following . 127
lines`, or `held, 40 back . 127 lines`. This window had no heading for a
long time, on the grounds that the title bar already says what it is - which
was right about the *name* and missed the state. The note in the corner is
not the same fact: it appears when something has arrived and says nothing
when nothing has, so a held window on a quiet machine looked exactly like a
following one.

**Held rather than adjusted**, and this is the decision. The alternative goes
on following underneath and adds the arriving rows to `back`, so the same
lines stay in view while the scroll range grows. That needs to know how many
rows arrived, and the kernel does not say: `sys.log` is the last 64 KB of a
quarter-megabyte ring, so once the ring holds more than that, each read loses
lines at the top as well as gaining them at the bottom, and telling the two
apart means matching strings against each other and hoping no line repeats.
Holding is correct by construction. If the kernel ever says how much it has
written - which the refresh's own comment already names as the fix for its
cost - adjusting becomes exact and this is worth revisiting.

**It used to follow by asking `ui.text` for a scroll far past the end** and
letting the widget clamp it. The clamp measures the height of the *previous*
draw, which on the first was nothing, so the window opened at the top of the
log and stayed there until the log changed - and then sat a draw behind it.

**It repaints when the log changes, not on a clock.** The refresh was a view
with a `tick`, which to the kit means "changes on its own", so an idle Log
View re-sent every row it shows twice a second for as long as it was open -
what the Terminal found about itself (`testing.md` §18.23). It reads the ring
in `on_frame` now, at most twice a second, and asks for a paint only when
something changed.

**Still open, and wider than this window.** `ui.lua` copies `gfx.font.w` and
`gfx.font.h` into `GW` and `GH` once, as it loads. `gfx.use_font` refreshes
that table in place when the interface face changes, but `ui.window` applies
the desktop's faces after `ui.lua` has loaded - so every widget in the kit
still lays out on the bitmap's 8x16 cell, whatever face it is drawn in. Log
View no longer asks the kit for a cell; the kit's own widgets do.

`testing.md` §18.31 has the check and its negative controls.

## 16.15 A program, run by its file

Diego, on 14 September 2026, having written a console program called
`diego.lua` in his home folder: "we need to have an easy way to run programs
from the command line and from the tracker". It could be run - `run
/home/diego.lua` - and none of the things he tried first did it.

**A file is named from where you are.** At the prompt a first word that ends
in `.lua` is a file, and in a Terminal and after `run` so is a word with a
`/` in it: `./diego.lua`, `diego.lua`, `../diego.lua`, `/home/diego.lua`. The
prompt keeps a leading `/` without `.lua` for commands, as it always has.
`.` and `..` are taken out of the path before anything is asked, because no
server has a directory called `..`.

**A bare name is still a program in `/bin`, and only that.** The current
directory is not searched for a word. If it were, a file called `ls.lua` left
in a folder would be what `ls` ran there - the reason `.` is kept off a Unix
`PATH` - and `ls` and `./ls.lua` would stop being two different requests: one
for a program, one for a file.

**At the prompt, what was Lua stays Lua.** `hello.lua` could be the field of
a table called `hello`, so a name whose stem already means something in Lua
is still Lua, and so is a line that carries on the way Lua would - `m.lua(3)`,
`t.lua = 1`. What changed is only what used to fail.

**Opening a Lua file in Tracker runs it.** Tracker asks
`filetypes.how_to_open`, as it asked `opener` before: a file whose opening
comment says `kosmos: application` starts as itself, and draws its own
window; anything else is a console program and gets a Terminal of its own,
started with the file's path as its argument, which runs that in place of the
banner, from the file's folder. The window stays when the program ends, with
what it printed and a prompt. The comment is read by the rule `/bin`'s server
reads - the lines from the top that are empty or begin with `--`, and nothing
below them - because two rules would let a program be an application in the
Deskbar and a console program in Tracker.

**Edit is beside Open** in Tracker's File menu, since opening a Lua file no
longer edits it. It asks `opener`, which is still the editor for a `.lua`.

`testing.md` §18.55 has the checks and their controls.

## 16.17 Five roles, and the panel that sets them

**A role is a *decision somebody makes*, and there are five of them.** A
title bar is a label on chrome and can carry a face with character; a widget
font has to work at every size in every list; a heading inside a window is
bigger than the text under it; a paragraph wants something to read; a
terminal wants a fixed width or its columns stop lining up.

`heading` was the fourth to arrive and for a while it was not really a role
at all: the style guide named it, applications asked for it, and `gfx`'s
`role_of` did not know the word - so every heading was drawn in the widget
font. Nothing failed, which is the difficulty: a face that is merely the
*wrong* face has no error to report. It is now `ROLE_HEADING`, and the five
names live in `theme.roles` rather than being written out in the three
loops that apply them.

**The Appearance panel is where they are chosen**, and it was redrawn in
September (`docs/appearance.html`, `testing.md` 18.124) because it had grown
a group at a time into seven of them stacked in a 380-pixel column with
lists three rows deep. Two things about the new one are worth keeping in
mind when any other panel is built:

- **A row that labels is worth less than a row that reports.** The old role
  list said *Widgets*; the new one says *Widgets &mdash; Plex Sans 14*, so
  the panel answers the question you opened it with before you touch it.
- **A panel that sets the font cannot have a constant for its own size.**
  Every measurement in it is taken at the faces in force, and the window is
  resized to what the two columns came to. The file's oldest comment already
  said this - "a layout of constants is a layout that is correct at exactly
  one font size" - about a bug in its own vertical spacing.

And the preview is a miniature desktop rather than a line of sample text,
which is Diego's choice and the more expensive one: a palette and a face are
chosen *together*, and what somebody wants to see is not what 15-pixel Plex
Sans Condensed looks like but what their desktop looks like now.

---

## 16.16 A face is not a cell, and both halves of the desktop have to agree

Two rules, learned on the same morning, from one screenshot.

**The window manager draws the text of every ordinary window.** An
application sends drawing commands and the compositor owns the pixels
(`gfx.md` 19.4), so a string is *measured* in the application - to place it,
centre it, size a button around it - and *drawn* in the window manager. That
only works while both hold the same face, which makes the font table the
window manager sends its clients a promise about its own state rather than a
preference it passes on.

It was not one. `load_appearance` applied the fonts in `/home/.appearance`
and no others, so on a machine with nothing saved the window manager loaded
no face at all, while still sending applications the defaults - which they
loaded. The desktop then laid itself out in IBM Plex and painted in the 8 by
16 bitmap: a menu bar whose titles were spaced for 18 pixels of "File" drew
it 32 wide, so the next title began inside it.

It hid for two years' worth of commits because **a face that is not loaded
is the bitmap**, and the default was the bitmap. Nothing distinguishes "we
never loaded anything" from "we loaded spleen" until the default changes.
The rule that comes out of it: *what is advertised is what is held*. A role
the window manager fails to load is named to clients as whatever is actually
in force, so the two halves cannot disagree silently.

**And a width is measured, never counted.** `gc:text` clipped a string to
its view by dividing the room by a cell width - one cell per character -
which is exactly right for a bitmap font and wrong for every other. `New
folder` in a 96-pixel button became `New folde` with twenty-five pixels to
spare, `Delete` became `Delet`, and the widget gallery's labels lost their
tails.

This is `gfx.md` 19.3 one level up. That rule says nothing in Lua computes a
pixel offset, because the pitch is almost never `width * 4`; this one says
nothing in Lua computes a character count from a width, because the advance
is almost never the same for two glyphs. `gc:text` measures now: one
`gfx.measure` when the whole string fits, which is the common case and costs
a single C call, and a binary search over characters - about four
measurements - when something really does have to be cut. Cutting is still
by the character, because half a glyph is worse than a missing one.

---

## 16.18 Size: one scale for everything

**Asked for on 22 September, drawn in `docs/looks.html` and approved** -
"the proposed size slider is great as it is", "with the %" (`roadmap.md`
5z). Diego, on the ThinkPad, where a 14-inch 1920 by 1080 panel makes
everything read small: "add a setting like Windows does", "a factor
multiplier of all the things in the UI", "instead of choosing independent
font sizes", "Something like that slider of iOS". Seven steps - 100, 110,
120, 135, 150, 175 and 200 per cent - on a slider in Appearance, with the
percentage beside it, kept in `/home/.appearance` as `scale`.

**Applications do not change.** Every size and position an application
gives - a window's width, a widget's place, the fixed layout's 24-pixel
row, a 16-pixel face - stays in the units it gives today, which are the
screen's pixels at 100 per cent and *points* at any other step. The
scale is applied where pixels are made, which is the window manager, and
**at its edge with each application** rather than all through it: inside,
the window manager goes on working in the screen's own pixels, exactly as
now - its windows' rectangles, damage, hit tests and composing are
untouched. What crosses the edge is converted:

- **A window opening**: its asked-for size and place multiplied by the
  scale, and the size it was given divided back in the reply.
- **Drawing commands**, the kit's four - `fill`, `triangle`, `text` and
  `image`: every coordinate multiplied, a rectangle by its two edges so
  neighbours still meet at a fractional step; a text command in its role's
  face loaded at the size times the scale (`sized`), so words are drawn at
  that size rather than magnified; an icon from the largest of Haiku's
  exports and averaged down (`stretch`'s `smooth`).
- **Events**, all through `post`: a pointer's position divided back, a
  new size divided back.
- **A window that draws its own pixels** keeps a surface of the size it
  asked for, and the window manager composes it stretched to its place on
  the screen - so a game, a film and the cube are larger with no change to
  them. The factor is per window, the surface's size against the window's:
  a full-screen window asked for the screen in the screen's own pixels and
  its factor is one.
- **The window manager's own chrome** - the title bar, its boxes and its
  words, the borders, the grip - scales with everything else, since those
  sizes are its, not an application's.

**The screen gets smaller, to a window.** At 150 per cent the ThinkPad's
1920 by 1080 is 1280 by 720 to an application, and an application that
sizes itself from the framebuffer - the Deskbar does, by `gfx.screen()` -
has to take the size it was given instead. A window asked for larger than
the room is fitted to it, as today.

**What the scale costs.** A text command loads a face per size, and there
are seven sizes; `sized` keeps each. A stretched composite is an add per
pixel where a blit is a copy; for a window that redraws every frame it is
the one new cost on the frame path, and `make frames` is where it is
watched.

**Built in stages, each a test at 100 and at 150 per cent.** The first
two are done (0.10.115): the window manager at a scale read at startup -
opening, commands, commits, events, chrome, own-pixel windows - and
Appearance's slider, which changes the scale with windows open: every
window is rebuilt at its new size in pixels, keeping its size and place
in points, and told to draw again, and the log names each one's new size.
The third is what reads the screen's size before it has a window - Lite
XL sizes its buffers from `/dev/screen` - and the window manager's
drawings that are not windows: the pointer, the level bar, a drag's
label, the launcher pad.

**Three things the building found.** `scale.pt` rounds to the nearest
point rather than down, so a size taken to pixels and back is the size it
was and a window changed to 150 and back to 100 comes home at exactly
the size it left. The window manager's main chunk is at Lua's limit of
200 locals, counting the block locals of its main loop - so `scale` took
the minimum window size into itself, two names for the one it cost. And
a display phase that no part of the gate lists never runs and says
nothing: the scale's did exactly that, "0 on everything at 150 per cent"
in a gate that passed, so `gate.py` now refuses to start while
`run_screenshot.py` has a phase no part names.

## 16.19 Icon sizes, and a menu that says which one

**Asked for on 22 September** (`roadmap.md` 5za). Diego, once the 16s and
64s were vendored beside the 32s: "with the new icon sizes we should also
be able to select icon size on desktop, tracker icon view and else", and,
of the sizes, "16,32,64 are the correct ones".

**Three sizes, and no fourth.** They are the three Haiku exports the image
carries (`assets/icons/README.md`), so each is drawn pixel for pixel with
nothing averaged. `gc:icon` will happily draw any size by shrinking the 64
- that is what the Deskbar's 24 is, and what every icon at a scale is -
and that is right for a size the *system* worked out and wrong for one a
person chose. A size somebody picks off a menu should be the best picture
there is of it.

**In points, like everything else since 16.18.** A 32 at 150 per cent is
48 pixels, and the window manager does the shrinking: `scale.op` swaps an
icon's asset for the 64 when it is about to be stretched. Nothing in
`iconsize.lua` or in Tracker knows there is a scale at all.

**Kept per place**, in `/home/.tracker`, under a key for each -
`desktop_icon_px`, `window_icon_px`. The desktop and a Tracker window are
the same program with the frame taken off, and they are not the same
place: a desktop of large pictures over a photograph and a window of small
ones you can see two hundred of is the pair of things people want. The
default, 32, is kept as *nothing*, so a place that never chose follows a
default that changes rather than freezing the one in force the day
somebody opened the menu - the same rule as a window's own text size
(`textsize.lua`).

**The grid follows the pictures**, and this is where the size actually
does something. Tracker's cell was `84, 56 + GH` compiled in; it is now
`iconsize.cell(px, gh)`, the sum it always was:

```
w = max(84, px + 52)     -- the icon, and 26 pixels either side
h = px + 8 + 2 * gh      -- 2, the icon, 4, and two lines for the name
```

It lives beside the sizes rather than in Tracker because it is the
arithmetic of *a size* rather than of a file manager - and because there it
is a function over two numbers that a host test can hold exactly, which a
screen cannot: see `testing.md` 18.148 for the check that measured an
icon's artwork and thought the cell had moved 14 pixels.

At 32 both are the 84 by 72 they were, and 52 is not a number picked for
this: it is what the old 84 gave a 32, so the air either side is the same
at every size.

**The first version made the width the label's** - `max(84, px + 20)`,
which is 84 at all three sizes, since a name is wider than any of the
three pictures. Diego, looking at a 64 with ten pixels either side and a
name cut to `cheats~.html`: "yes widen the cell at 64". He is right, and
the reason is that a cell is not a picture with a caption underneath - it
is one thing, and at 64 an 84-wide cell reads as a large icon squeezed
into a small one. At 116 the same name fits whole.

**The floor of 84 is what keeps 16 and 32 where they were.** Below it a
name has nowhere to go: a grid of tiny icons under two characters each is
denser and unreadable, which is not what Small icons is for. So only Large
widens.

### A menu item can be marked

A menu of three sizes that does not say which one is in force is a menu
you have to guess at, and `ui.lua`'s menus had no way to say. So an item
may carry `mark`, and a menu with any markable item gives every row a
column for it on the left - `mark = false` is not the same as no mark,
which is what keeps the names lined up and stops every row shifting when
the mark moves.

A diamond rather than a tick: every one of these is a choice among several
rather than something switched on, and it is five rectangles where a tick
would be a new verb in the file. It is built the way the submenu arrow
beside it is.

**And a menu's `items` may be a function**, worked out when the menu
opens. A list is built once with the window, so anything it says about the
state of the program would be however things were when the program
started. Tracker's View menu is a function now, and marks three separate
things: the layout, the column the listing is sorted on, and the icon
size. The sizes are only in it in icon view, because a list has no icons
and a choice that changes nothing is worse than no choice.

It said *a menu bar's* `items` when it was written, because a menu bar was
where a menu came from. Since 16.20 they come from a `...` in a header, and
the property is the same one and matters more: a menu built at the press is
the only kind that can say what is true at the press.

### Where the choice lives, and what that exposed

A Tracker window has a header with a `...` in it. **The desktop has
none** - it is Tracker with the frame taken off - so the only place to put
this is a right press on the background, which is what a right press on a
desktop's background has meant since there were two buttons.

That was the first menu the desktop had ever opened, and it came up 32
pixels too high, over the Deskbar. A menu is a *window*, placed on the
screen, so whoever opens one adds their own origin to a local point - and
the desktop's origin was 0 while the desktop was at 32. `fit_backdrop` in
`wm.lua` moves the backdrop below the strip when the strip opens and told
it with a `resize` and nothing else; `swap_surface` posts a `resize` and
never a `moved`. Nothing had ever noticed, because the only thing that
needs a window's origin is a menu.

It is a good example of what a feature is for. Nothing here is about
window origins, and the bug had been in the tree since the Deskbar could
start after the desktop.

## 16.20 One header, and three controls in it

`docs/desktop.html`, `docs/apps.html`, `roadmap.md` 5zj and 5zs. Every window
with controls opens with the same row across its top: **what it is doing on
the left, and on the right the one or two things anybody does to it, plus a
&#8942; for the rest.**

It is `ui.header` (0.10.149), in `ui.layout`'s numbers - the drawings':

```
head      = 46    the row, its rule the last pixel
head_in   = 18    the subject, in from the left
head_edge = 10    controls, in from either edge
head_gap  = 4     between two controls
```

`title` in the title face, `sub` beside it in the dim `ui` face and cut with
an ellipsis before the controls rather than run under them; `left` controls
before the subject (Tracker's back, forward and place), `right` against the
far edge, each centred in the band and a hidden one taking no room. It places
them from its own width every time it is drawn, so a resized window keeps
them in its corner without a `follow` of their own.

**Three is the rule.** Tracker has search, a new folder, the view and the
dots; Preferences draws its own header from its drawing and has no controls
in it; Paint has its tools in a strip rather than a bar, because a tool is
chosen far more often than a menu is opened. **A window that wants a fourth
button wants the dots instead** - which is what the Editor's Save, Open,
Save as and Run became. A button that *starts* something is `go = true`,
filled with the accent, and a window has one at most.

**This used to say "five numbers repeated in six files and not a widget"**,
on the argument that a widget arranging a list of buttons would be
`ui.menubar` renamed. It was wrong in the way the rest of 0.10.149 showed:
the numbers were copied into sixteen windows, each copy a little different,
and "no margin or spacing" was what Diego saw. What each header *holds* is
still each window's own; where it holds it is the kit's.

**The title bar above it is not the header.** The name, the three coloured
controls and the frame are the window manager's and every window has them
alike; the header is inside the window, built by the application with the
kit. Diego asked which of the two the new style was; it is both, and the
answer is where to look when one window does not match.

### What the left-hand side is for

It is the one fact the window would otherwise not say. Two of the five
found one nobody had noticed was missing:

- **Log View** follows the log until you scroll back, and then holds. The
  only sign of it was a note saying *new lines below* - which appears when
  something has arrived and says nothing when nothing has, so a held window
  on a quiet machine looked exactly like a following one. The header says
  `following . 127 lines` or `held, 40 back . 127 lines`.
- **Processes** said its summary under the menu bar; it says it in the
  header, and the `View` menu that used to sit beside it turned out to have
  two items that did nothing at all.

### The trap it sets

A header carries state, and state in two places drifts. The Terminal's
working directory and the Editor's path each changed in two places, and each
now changes in one function that also updates the header. **Both were one
call site a version ago**: the third one written the obvious way sets the
variable, leaves the label saying where the window used to be, and is a bug
nothing catches, because the label is right most of the time.

## 16.21 A list that is navigation, and one that is a list of things

`ui.list` fires `on_select` when you press Enter or click a row. Arrowing
through it moves a highlight and nothing else, and that is right for almost
every list in the system: a list of files is one you arrow through to *reach*
the row you want, and opening each on the way past would be unbearable.

It is exactly wrong for a list that **is** the navigation of its window.
Preferences' sidebar is nine categories, and a page that waits for Enter is a
window whose categories cannot be browsed at all - you cannot look down them,
you have to commit to each one.

So `arrows_choose = true`, opt-in, set by the sidebar and by nothing else.
Every other caller behaves exactly as it did.

**Two things about that sidebar were wrong together and are worth keeping.**
It could not be reached from the keyboard at all: `win:add(page)` came before
`win:add(side)`, and `root:focusables()` walks the tree in the order things
were added - so the first control to receive a key was the Theme dropdown
and the window's own navigation was last. The order of `win:add` calls is a
*keyboard* decision as much as a drawing one, and for two views that do not
overlap it is only a keyboard decision.

And the name: `arrows_choose` was `follow` for one build, which is already a
view's list of edges to keep its distance to when its parent resizes. `#self.follow`
over a boolean ended the window on its first arrow key. A name the kit
already uses for something else is not a name.

`testing.md` 18.161.

**Tracker's places are a sidebar too** (0.10.150, `docs/tracker2.html`):
`ui.sidebar` with a `pitch` of 33 and gaps that carry a `rule`, a hairline
between the built-in places, the ones a person has, and the drives. A row
that leads nowhere - a place whose drive is away - is `quiet`: dim, and a
press on it chooses nothing.

## 16.22 A frame on every side, and a scrollbar in one colour

`roadmap.md` 5zr. Two photographs of corners, the same afternoon.

**The frame.** A window's sides and bottom were a 2-pixel line of the title
bar's colour, and the rounding was done by putting the desktop back over the
four corners - so at a bottom corner the arc cut through the page while the
line ran square past it, which on a dark console is a sliver of console
outside the frame. Diego: *"We need to add some extra chrome to the other
borders of the apps as now it looks weird and make better rounded
borders"*. The frame is 4 - it was 6 for an afternoon, until *"the chrome
arround the window is too thick, we should take a couple of pixels
out"* - and the page is rounded *inside* it by the corner
less the frame (`OUT.round_inside` in `wm.lua`): the frame's colour painted
over the page's pixels outside an arc of 6 - the corner of 10 less the
frame - with the coverage that rounds
the frame itself - so the frame is one width all the way round. The page's
top corners meet the title bar and stay square.

**A rounded window does not cover its corners, and the compositor has to
know it.** Its culling pass cuts every opaque window out of what it paints
behind, and it cut the frame's whole rectangle - so under a corner neither
the desktop nor the window behind was painted, and the corners were put
back from whatever the backbuffer last held. Photo showed its own page
there after `tile` moved it onto its old place. The four corner squares are
handed back to be painted behind (`OUT.uncover`); a few hundred pixels a
window, against a picture that was wrong.

**The thumb.** From 22 September a scrollbar's thumb wore the look's tab
colour, Mac OS 9's Platinum, at Diego's asking. On 24 September, beside a
list's blue selection: *"the scroll bars look bad now with the colors"*,
*"We should go back to scrollbars and handle with the same color"*. The
thumb is the controls' face with a grip in the look's edge colours; in a
flat look it is a pill 6 across in a grey between the list's ground and its
dim words, with no trough and no arrows, as `docs/apps.html` draws a list.
The column it sits in is still 16 wide, so the hit test did not change.

## 16.10 What we do not copy from BeOS

**The C++ class hierarchy.** `BApplication`, `BLooper`, `BHandler`, `BWindow`, `BView`, `BArchivable`, `BInvoker`. It existed because 1990s C++ had no better way to express composition. In Lua it is table composition with closures, no inheritance.

**`BLooper` as a system thread.** Coroutines, per 16.5.

**The entire locking API.** The condition that required it does not exist.

**The four-byte `what` codes.** They were an optimization to compare integers instead of strings. In Lua strings are interned and comparison is a pointer. A readable `type` field instead.

**The Translation Kit.** BeOS's abstraction over image and sound formats. Good idea, out of scope.

---

## 16.10 Build order

| What | Milestone | State |
|---|---|---|
| A single window, no decoration, one view, draw and key | 6 | **done** |
| View tree with clipping and nested coordinates | 6 | **done** - `lib/ui.lua`, clipped in the graphics context so a view cannot draw outside itself |
| Multiple windows, tabs, stacking, focus, drag | 6 | **done**, except that dragging is with the keyboard: there is no pointer device yet |
| Follow modes and row/column containers | 6 | follow modes **done**; containers not started |
| Basic widgets: button, list, text, scroll | 6 | label, button, checkbox, field and list **done**, keyboard and pointer both; scroll not started |
| Workspaces | 7 |
| Stack and tile | 7 |
| Replicants | 7 |
| Shared-memory surfaces (see `gfx.md` §19.9) | 7 |
