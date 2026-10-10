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
     as well). **Popups, tips and banners too** (0.11.110): the window
     manager has nothing of its own for them but their place. **And the
     rest** (0.11.111): a strip - the Deskbar - the backdrop - the desktop
     - and a full screen, which the window manager sizes to the screen and
     tells their sizes as it tells any (`swap_surface`). Every window the
     kit makes draws itself; what still sends drawing commands is a
     program written without the kit (`hello-win`, `stuck`).
   - **D2e, done 9 October** (`testing.md` 18.507) - the drawing commands
     retired: the window manager's `draw` handler, its table of commands
     and `scale.op` gone, and the kit's sending with them. `hello-win` and
     `stuck`, the two programs written without the kit, draw into regions
     of their own. Measured again on the M700 is D6's.
4. **D3** - the window server: the screen, surfaces, stacking, damage and
   composing move to C; the window manager tells it through the manager
   door. `wm.lua` loses `compose.lua` and the pixel work around it.
   **Planned 10 October**, in two steps, the desktop working after each:
   - **D3a - composing in C, still in the window manager's process.** A
     *scene* in the Graphics Kit (`gfx.scene`): every window's place, size,
     stacking, kind (backdrop, strip, menu, tip, full screen), whether it
     blends, its rounding and shadow, and its picture - a window's
     committed buffer, or a surface the window manager draws, the menu
     bars' and the lights' among them. The window manager keeps the scene
     up to date as it decides - a window opened, moved, raised, hidden,
     committed - and asks it to compose the damage; the scene does what
     `compose.lua` and `drawwindow.lua` do now, per damaged rectangle, in
     C: which windows show, the desktop under them, each window's picture
     copied, blended or stretched, its corners and shadow, then what is
     over everything - the menus, the resize outline, the volume display,
     the cursor - and the screen, flushed, and the watcher's copy (VNC).
     What the decorations look like stays the window manager's: it draws
     the three lights, a menu bar and the volume display into pictures of
     its own and hands the scene those; the scene composes, and decides
     nothing a person would choose. Measured with `frames` before and
     after: the window manager's Lua per busy pass, and what it allocates.
     **Done 10 October** (0.11.114, `testing.md` 18.509): `gfx.scene` in
     `user/kits/gfx/scene.c`, compiled inside `gfx.c` with the kit's own
     primitives - each now a C function on surfaces the Lua method calls.
     Under QEMU a composing pass of four stacked animating windows went from
     3.53 ms to 1.59, of one from 0.80 to 0.47, and the window manager's busy
     time fell by a quarter to a third. **The Lua composing stays until D3b**,
     as the reference: `wm compose-check` composes every pass both ways, and
     `x86-scene` holds the two to the same pixels over 1500 passes. It goes
     when the scene leaves this process, where the Lua could not follow it.
   - **D3b - the scene in a process of its own**, `windowserver`
     (`user/servers/windowserver.c`, `/Running/windowserver`): it takes
     the screen; the scene's calls become the **manager door**, a declared
     shape (`user/include/windowproto.h`) only the window manager holds;
     an application's region is handed on to it with the window; the
     window manager's own pictures are regions it shares with it. Commit
     and poll still come to the window manager until D4.
     **In two parts** (planned 10 October):
     - **D3b-1, done 10 October** (0.11.115, `testing.md` 18.510) - the
       drawing primitives and the scene's composing in C that needs no Lua
       (`user/kits/gfx/draw.c`, `scene_core.c`, with `surface.h` and
       `draw_primitives.h`), so the Graphics Kit and a server share one
       copy; the kit's Lua methods and `gfx.scene` are thin bindings over
       them, and `test_draw` holds them on this Mac.
     - **D3b-2** - `windowserver`, a role of the init image like every C
       server: it holds the screen and the back buffer; the window manager
       writes its scene into a region they share - it changes every pass,
       so it is data, not messages - and sends one `compose` with the
       damage; a window's region, and the window manager's own pictures,
       are handed over once each, by slot. **Decided by Diego, 10
       October: init starts it at boot** ("Let's go with option a as you
       recommended"), as it starts every C server. It outlives a window
       manager that ends or crashes, keeping the screen; with none
       connected the console has the screen as it does today, and init
       hands the manager door only to the window manager.
       **In four steps**, the desktop working after each:
       1. **The door** (0.11.116, `testing.md` 18.511): init starts
          `windowserver` with the screen, lends its door to the window
          manager alone, and the window manager greets it.
       2. **It composes** (0.11.119, `testing.md` 18.514). The window
          manager hands it the back buffer and a region for the scene, and
          every picture the scene names by a slot - a window's two
          buffers, and the window manager's own wallpaper, stamp, menu
          bars, level bar, arrow and a drag's label, each now a surface
          made `shared` (`gfx.surface{ shared = true }`, `s:region()`).
          Each pass the scene is described by slot, written into the
          shared region (`scene:wire`) with the damage, and one
          `WS_OP_COMPOSE` composes it into the back buffer and puts it on
          the screen. A picture whose surface is freed or collected is
          forgotten on the next pass. The server holds every count, slot
          and source rectangle to what it was given. Without a window
          server the window manager composes in its own process as before;
          `wm compose-check` composes each pass there and again in Lua,
          and `x86-scene` holds the two to the same pixels.
       3. **The console's screen handed over** (0.11.120, `testing.md`
          18.515): the window server, not the window manager, takes the
          screen from the console when handed the back buffer, and gives
          it back when the endpoint the window manager greeted with names
          nothing - it ended or crashed.
       4. **The window manager's own composing gone**: the screen, the
          Lua composing and its check leave `wm.lua`; the watcher's copy
          (VNC) and the screenshot are taken from the window server.
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
