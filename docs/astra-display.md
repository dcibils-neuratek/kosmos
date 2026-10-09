# Astra's window server - a design, decided

Written 7 October 2026, after Astra was measured (`testing.md` 18.438) and
composing's garbage taken away (18.439). Nothing here is built.

Diego: "does it makes sense to think about a migration of the window manager
from lua to c? and leave the lua for apps? because my premise is: lua is
great for fast development of apps but not necessary efficient for system
components", then "lets measure the display server split and make a choice
of the display server in c and lua window manager".

## Why

**The layer rule says so.** `CLAUDE.md`: "if something else's correctness or
timing depends on you, you do not get a garbage collector". Every frame and
every key on the desktop passes through Astra's window manager, and it is
Lua.

**The measurement says it is not urgent, and says where.** On the M700 under
load (18.438): 98.9% of passes under 1 ms, none over 4; the slowest passes
the collector's, 1.6-3.1 ms, about one and a half a second; composing - C -
80% of the busy time; a key to the screen 3.8 ms on average. Composing made
more than half the garbage, and making none (18.439) cut collections four
times. What is left is mostly the window manager's Lua answering applications -
the whole process makes 0.7 KB a pass now, under QEMU - and the Pi 5, the
target, is several times slower than the M700.

**So the split is the architecture Kosmos wants, chosen on principle and
built without hurry**: C for what every frame waits on, Lua for what a person
decides - the same line as the rest of the system, drawn through Astra.

## What there is today

One process, `wm` (`user/bin/programs/wm.lua` and `user/lib/wm/`, about
nine thousand lines of Lua), which:

- **owns the screen** (`sys.screen_take`), the back buffer and the cursor;
- **keeps every window**: its place, size, stacking, its pixels - a surface
  it draws into, or for a direct window the application's shared region;
- **draws ordinary windows for their applications**: the UI kit (`ui.lua`)
  sends each frame as drawing commands - tables of fills, text, icons - and
  the window manager carries them out into the window's surface;
- **composes** the damaged rectangles, in C (`gfx`);
- **reads input** from the console (`wait_input`) and routes it: shortcuts,
  the window under the pointer, the focused one, drags of title bars;
- **answers applications** on `/Running/wm`: tables for opening, moving,
  menus, the clipboard, polls; structs for a direct window's commit and poll
  (`wmproto.h`, the Window Kit's W2);
- **decides**: where a window opens, focus, stacking, title bars and tabs in
  the look, menus, drag and drop, the Deskbar's and the dock's places.

## The split

```
   applications ─┬─ frames (a region each, two buffers) ──┐
                 ├─ commit, poll, events (structs) ───────┤
                 └─ open, menus, clipboard (tables) ──┐    │
                                                      ▼    ▼
   Astra's shell (Lua)  ◄── manager door (structs) ── window server (C)
   placement, focus, title bars,                       screen, surfaces,
   menus, drag and drop, the look,                     stacking, damage,
   shortcuts, the Deskbar's place                      composing, cursor,
                                                       input routing,
                                                       event queues
                                                            ▲
                                    console: keys, pointer ─┘
```

**The window server, in C** (`user/servers/windowserver.c`): owns the screen,
the back buffer and the hardware cursor; holds every window's surfaces,
place, stacking and flags; keeps the damage and composes it; reads the
console and routes each key and press to the window it belongs to, holding
it in that window's queue; and answers a window's commit and poll itself -
the frame path, no Lua on it. It decides nothing a person would choose.

**Astra's shell, in Lua** (what `wm.lua` becomes): every decision - where a
window opens, which has the focus, what stacks over what, how a title bar
looks and what pressing it does, menus, drag and drop, Super's shortcuts,
the look. It tells the window server through a door of its own, the
**manager door**, in structs: make a window here, move it, raise it, focus
it, give me presses on decorations and these keys. The tables applications
send for policy - open, menus, the clipboard - still reach it on
`/Running/wm`.

## The one decision under the split: who draws an ordinary window

The window server would have to carry out every kit window's drawing
commands, which are Lua tables today. Two ways:

**(a) The server carries them out** - in C, the commands turned into a
declared shape: a stream of structs in a region the application writes and
the server reads, as `docs/gfx.md` already plans for a big frame. The server
grows a drawing interpreter: text, fills, icons, clipping, scaling.

**(b) Every window draws itself** (recommended). The UI kit draws its widgets
into the window's own surface, in the application's process, with `gfx` -
already C - and commits it, exactly as Plasma and cube3d do now. The server
only composes surfaces and routes input; the drawing commands stop crossing
processes at all. It is how Wayland and today's macOS work, and BeOS's
app_server is the other way. What it costs: two buffers a window in the
application's memory (a 1000x700 window is 5.6 MB), each process drawing
its own text - from the same fonts, which the image shares read-only - and a
window that hangs keeps its last picture rather than being redrawn by the
desktop.

## Steps, the desktop working after each

1. **D0, done** - composing makes no garbage (0.11.43, 18.439).
2. **D1, decided** - this design, with its diagram (`astra-display.png`).
3. **D2** - the UI kit draws into its window's own surface: every
   window a direct one; the drawing commands retired. Measured. In steps,
   since every application stands on it:
   - **D2a, done 9 October** - a command scaled to the screen's pixels is
     `paint.scale`, in `paint.lua` beside the commands themselves, so a
     window that draws itself scales them as the window manager does.
   - **D2b, done 9 October** (`testing.md` 18.501) - a kit window asks to
     `draws_itself`: its frame's commands run through `paint.lua` in its own
     process, into a region of two buffers, and are committed. The same
     window drawn that way and sent is the same in every pixel below its
     header. Calculator is the first. At a scale other than 100 the window
     manager declines it, because a region in points is shown stretched,
     and the window sends its drawing as before. Measured under QEMU, 60
     frames: drawing itself, the window manager's processor time fell from
     25 ticks to 14 and the program spent 11; a frame 1.74 ms against 2.24.
   - **D2c, done 9 October** (`testing.md` 18.502) - a window that draws
     itself at a scale: it asks the scale, makes its region in the
     screen's pixels, scales its commands by `paint.scale` and draws their
     text in faces loaded at the scale, laying out in points as before. A
     change of scale tells it its new size, and it makes a region at the
     new scale's pixels. At 150 per cent it is the same as the window
     manager's drawing in every pixel, opened there or rescaled.
   - **D2d, every ordinary window done 9 October** (`testing.md` 18.503) -
     one with the kit's header or one of its own; `draws_itself = false`
     keeps one sending. What it costs is arithmetic rather than a
     measurement - the machine says its free memory in whole megabytes: a
     window holds two buffers of its size where the window manager held one
     surface, so one more a window, 0.5 MB for a 420x306 one at 100 per
     cent and 1.2 MB for the Open window at 150. **Menus too, the same
     day** (18.504): a region at the scale, painted and committed as the
     pointer moves over it, given back when it closes - by the window or
     by the window manager (`menus_gone`, which a direct window now hears
     as well). **Still sent**: banners, tips, popups and strips.
   - **D2e** - the drawing commands retired from the window manager.
4. **D3** - the window server: the screen, surfaces, stacking, damage and
   composing move to C; the window manager tells it through the manager
   door. `wm.lua` loses `compose.lua` and the pixel work around it.
5. **D4** - the frame path: commit and poll answered by the window server,
   held polls and event queues with it.
6. **D5** - input routing: keys and presses from the console to windows in
   C; the shell given the keys and presses it asked for.
7. **D6** - measured again: the M700, and the Pi 5 when it is here.

## Decided

Diego answered D1 on the Kosmos Board, 9 October 2026:

1. **The split, as drawn** - a window server in C, Astra's shell in Lua
   (agreed with the order of the work, 7 and 8 October).
2. **Who draws an ordinary window: (b), every window draws itself** - "Every
   window draws itself (recommended)". The drawing commands stop crossing
   processes; the server composes surfaces and routes input.
3. **The name: the window server** - "window server". `windowserver` at
   `/Running/windowserver`, its source `user/servers/windowserver.c`.
4. **When**: after Mail, as the roadmap orders it (7 and 8 October).
