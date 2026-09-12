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

**And there is a second reason to ask for one, which is not speed.** The first is the obvious one: an image canvas, video, a game - the whole surface changes every frame and describing it costs more than copying it. The second is *typography*. The compositor rasterises the commands it is sent, in the four faces the desktop chose, so an application that sends commands cannot have a heading at 28 pixels and a paragraph at 16 on the same screen. A document viewer needs exactly that, which is why `pdfview` and `browser` both draw their own pixels while being nobody's idea of a game.

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

**One departure from BeOS that stays**, because it was about behaviour and
not shading: BeOS made a tab only as wide as its title so several stacked
windows keep their titles readable. Kosmos does not stack windows, so it
bought nothing and cost what a full-width border gives for free. It also
made the picture disagree with the behaviour, since dragging was always the
full width of the frame.

**The measurement that keeps this honest.** A bevel is more pixels per
widget than a flat rectangle, and `make frames` exists now. Composing is
already 83% of a busy pass, so the dimensional style is a thing to *watch*
in the profile rather than a thing to assume is free.

---

## 16.9 Themes, and colours that are named rather than captured

There are two palettes - `dark`, which is what Kosmos looked like first, and
`light`, which is the 1998 one on purpose. `appearance` switches between
them and picks the desktop colour, and the choice is written to
`/home/.appearance` and read back at startup.

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

It is one strip now, 36 pixels tall, and `topbar.lua` is deleted:

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

**32-pixel icons in a 36-pixel bar**, because 32 is the size every icon in
`assets/icons/` actually is and nothing here scales one - any other number
would be a crop rather than a smaller picture.

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
