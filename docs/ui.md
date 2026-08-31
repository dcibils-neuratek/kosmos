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

The exception is shared memory for surfaces (an image canvas, video, a game). It is requested explicitly, justified by the use case, and is not the default path. That path is designed separately in [gfx.md](gfx.md).

---

## 16.7 The app server

Responsibilities, all in Lua except the blit:

**Decoration.** The BeOS yellow tab, which takes only the width of the title instead of the whole bar. It is the system's most recognizable visual decision and it is functional: it lets you see the titles of several stacked windows at once.

**Stacking and focus.** Click to focus. Plus the two features Haiku added that are worth having from the start: **stack** (several windows sharing one frame, with tabs side by side) and **tile** (attached windows that resize together). They come almost free out of the tab model.

**Workspaces.** BeOS had 32, with independent resolution per workspace. It is the best implementation of virtual desktops anyone has done. In Kosmos they are one more field in the window struct and a filter in the compositor. Twenty lines.

**Input.** Highest-priority thread, always. Non-negotiable. If an app hangs while drawing, dragging its window still works.

**Compositing.** Damage tracking over the list of dirty rectangles, a backbuffer in cached RAM, one blit to the framebuffer synchronized with vblank. If an app did not respond in time, compose with its previous command list and move on. Never block.

---

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

---

## 16.8b The look: BeOS's structure, not BeOS's skin

Decided deliberately, because "BeOS-style" can mean two very different
things and only one of them is worth having.

**Take the structure.** The widget vocabulary, the tab that is only as wide
as its title, follow modes instead of a constraint solver, click to focus,
stack and tile, the replicant. These are decisions that were right in 1998
and are still right, and reinventing them would be work spent arriving back
where BeOS already was.

**Do not take the skin.** The specific 1998 surface - the grey bevels, the
two-pixel light-and-dark chamfer on every button, the exact yellow - is a
period costume. Copying it makes the system look like a museum piece and,
worse, like a clone rather than a descendant.

So: the same bones, a different finish. Concretely, what is already chosen
here and what it means:

| BeOS did | Kosmos does | Why |
|---|---|---|
| Tab as wide as its title | The same | Functional, not decorative: the titles of several stacked windows stay readable at once. It is the one visual decision worth copying exactly |
| `#FFCC00` tab yellow, hard-edged | A warmer amber on a dark desktop | Keeps the "the focused one is the yellow one" reading, drops the 1998 palette |
| Light grey chrome, bevelled | Dark, flat, one-pixel separators | A bevel says "this is a raised physical control", which stopped being a useful lie once everyone knew what a button was |
| Every control chamfered | Weight and spacing do the work | Fewer pixels spent saying what a thing is, more spent on what it contains |

The rule for anything not in that table: **if the BeOS decision is about how
something behaves, copy it. If it is about how something is shaded, decide
it fresh.**

---

## 16.9 What we do not copy from BeOS

**The C++ class hierarchy.** `BApplication`, `BLooper`, `BHandler`, `BWindow`, `BView`, `BArchivable`, `BInvoker`. It existed because 1990s C++ had no better way to express composition. In Lua it is table composition with closures, no inheritance.

**`BLooper` as a system thread.** Coroutines, per 16.5.

**The entire locking API.** The condition that required it does not exist.

**The four-byte `what` codes.** They were an optimization to compare integers instead of strings. In Lua strings are interned and comparison is a pointer. A readable `type` field instead.

**The Translation Kit.** BeOS's abstraction over image and sound formats. Good idea, out of scope.

---

## 16.10 Build order

| What | Milestone |
|---|---|
| A single window, no decoration, one view, draw and key | 6 |
| View tree with clipping and nested coordinates | 6 |
| Multiple windows, tabs, stacking, focus, drag | 6 |
| Follow modes and row/column containers | 6 |
| Basic widgets: button, list, text, scroll | 6 |
| Workspaces | 7 |
| Stack and tile | 7 |
| Replicants | 7 |
| Shared-memory surfaces (see `gfx.md` §19.9) | 7 |
