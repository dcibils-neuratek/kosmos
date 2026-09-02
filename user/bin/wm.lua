-- kosmos: server
-- kosmos: needs processes screen
-- The window manager: windows, decoration, stacking, focus, and the
-- compositor underneath them.
--
--   wm                    the Deskbar, and nothing else yet
--   wm hello-win          start /bin/hello-win.lua in a window
--   wm hello-win,stuck    two applications, one of which hangs
--   wm gallery,setprop:/app/gallery/title=hello
--                         and one that changes the other's title
--
-- Control-C gives the screen back to the shell.
--
--------------------------------------------------------------------------
-- Why this is a separate process from the applications it draws.
--
-- The definition of done for this milestone is BeOS's test: drag a window
-- with a hung application inside it and have the window keep moving. That
-- is not a performance question, it is a question about who owns the
-- pixels. Here the application owns none of them. It sends a list of
-- drawing commands - `ui.md` 16.6, commands and not a shared buffer - and
-- this process renders them into a surface it owns and keeps.
--
-- So an application that stops answering changes nothing. Its window still
-- has its last contents, because those contents were never in its address
-- space, and this process never waits for it: every request is taken with a
-- non-blocking receive, and one that is not there is simply not there.
--
-- The BeOS app_server did the opposite - it handed the application a
-- pointer into its own buffer with a lock around it - and that is exactly
-- the arrangement that makes a hung application freeze a desktop.
--------------------------------------------------------------------------

local KEY_TAB    = 9
local KEY_ESC    = 27
local KEY_CTRL_C = 3
local KEY_PREFIX = 23      -- Control-W

local theme = use("/lib/theme.lua")

local TAB_H      = 20
local BORDER     = 2
local CLOSE_W    = 12             -- the close box at the left of a tab

--
-- The sizing grip, bottom right, and how far into the window it reaches.
--
-- The border is two pixels, which is a fine thing to look at and an
-- impossible thing to hit: a target that thin is a target you miss. So the
-- grip claims a square of the window's own bottom-right corner, which is
-- what every desktop that has ever had one does, and for the same reason.
--
-- The cost is real and worth saying out loud: a click in that corner is the
-- window manager's and never reaches the application. Sixteen pixels square
-- in the one corner least likely to hold anything you meant to press.
--
local GRIP       = 16
local BOX_W      = 12             -- minimise and maximise, at the right

-- Nothing may be resized smaller than this. Below it a window is all
-- decoration and no window.
local MIN_W      = 120
local MIN_H      = 60

-- The decoration reads the palette at the moment it draws rather than
-- copying it into constants here, which is what lets the theme change
-- without restarting anything. See `theme.lua`: the palette table is
-- mutated in place and never replaced, so these functions see the change.
local function desktop_colour()  return theme.desktop end
local function focused_colour()  return theme.tab end
local function idle_colour()     return theme.tab_idle end
local function title_colour()    return theme.tab_text end
local function stamp_colour()    return theme.stamp end

--------------------------------------------------------------------------
-- What was chosen last time.
--
-- Read once at startup from the disk, if there is one. A machine with no
-- filesystem gets the default palette and says nothing about it - the
-- appearance of the desktop is not a reason to fail to start one.
--------------------------------------------------------------------------
local SETTINGS = "/home/.appearance"

-- Three faces, not one: a titlebar, a paragraph and a terminal want
-- different things, and the terminal's has to be fixed-width whatever the
-- other two are. `theme.fonts` is what was asked for, which is not always
-- what is loaded - a face that will not parse leaves the previous one in
-- place, and saying so is the caller's job.
local function apply_fonts(fonts)
  if type(fonts) ~= "table" then return end

  local why

  for _, role in ipairs { "ui", "text", "mono" } do
    local want = fonts[role]

    if type(want) == "table" and want.font then
      local px = tonumber(want.px) or 16
      local ok, err = gfx.use_font(want.font, px, role)

      if ok then
        theme.fonts[role] = { font = want.font, px = px }
      else
        why = tostring(err)
      end
    end
  end

  return why
end

local function load_appearance()
  local saved = fs.read(SETTINGS)

  if type(saved) ~= "table" then return end

  if saved.palette then theme.apply(saved.palette) end
  if saved.desktop then theme.override { desktop = saved.desktop } end

  apply_fonts(saved.fonts)
end

local screen = gfx.screen()

if not screen then
  print("wm: this process was not given the screen")
  return
end

local W, H = screen:size()

-- The backbuffer. Everything is composed here and only rectangles that
-- changed are copied out - `ui.md` 16.7. Under QEMU the framebuffer is
-- ordinary cached RAM and a full copy is not expensive, so this buys
-- nothing in speed here and is not pretending to: what it buys is that a
-- frame is never seen half-composed, and it is the shape the hardware
-- needs. On a real board the scanout buffer is often uncached, where
-- drawing into it directly is the mistake that costs 10-50x.
local back = gfx.surface{ w = W, h = H }

--
-- The screen is this process's now.
--
-- Until this call the kernel console is also drawing here, and the two
-- cannot share: printing one line scrolls the whole display, which drags
-- every window up sixteen pixels and leaves a copy of its tab behind. It
-- reads as a compositor bug and is not - the console's scroll moves every
-- pixel, because as far as it knows every pixel is text.
--
-- Nothing stops being reported. The console keeps writing to the serial
-- line, which is where a system with a window manager on it is debugged
-- from, and a panic takes the screen back regardless.
--
sys.screen_take(true)

-- Whatever appearance was chosen last time, before the first pixel is
-- drawn. After `screen_take` because a failure to read it must not stop the
-- desktop starting, and before compositing because otherwise the first
-- frame is the default palette and the second is the chosen one.
load_appearance()

local ep = sys.endpoint()

if not ep then
  print("wm: no endpoint")
  return
end

-- Which build this is, in the corner. `sys.build()` is compiled in by the
-- Makefile from the commit, so it identifies the source that produced the
-- image rather than the moment it was linked.
local b = sys.build()
-- ASCII only. The font is the 8x16 one from `assets/`, which has glyphs for
-- 0x20 to 0x7e and a box for everything else - so a middot separator came
-- out as a row of boxes, which is the font saying exactly what it should.
local stamp = ("%s %s  |  %s  |  %s  |  %s")
              :format(b.name, b.version, b.build, b.date, b.platform)

--------------------------------------------------------------------------
-- Windows, back to front. The last one is on top and has the focus.
--------------------------------------------------------------------------

local windows = {}
local by_handle = {}
local pending_pid = nil

-- Decoded pictures, by asset name. `false` means it was tried and would not
-- decode, which is remembered so a broken image is not re-decoded every
-- frame for the life of the desktop.
local image_cache = {}

--------------------------------------------------------------------------
-- Applications that are waiting for something to happen.
--
-- `poll` used to answer immediately, always, with an empty list when there
-- was nothing - so every application span: ask, get nothing, yield, ask
-- again, for ever. A desktop with four windows open had five threads that
-- were permanently runnable and a processor meter that read ninety-six per
-- cent with nothing happening. The meter was right.
--
-- Now the reply is parked. An application asks and is not answered until
-- there is an event for it or its own deadline arrives, so it is blocked
-- rather than running - not scheduled, not costing anything. It is the same
-- mechanism the filesystem uses for a live query, and for the same reason:
-- waiting is not the same as asking repeatedly.
--
-- This process still spins, because it polls a keyboard and a tablet that
-- have no interrupt wired up yet. One spinner instead of one per window is
-- the whole of what this buys, and it is most of it.
--------------------------------------------------------------------------

local DEFER = { "deferred" }
local waiting = {}

-- How long a poll waits when the caller does not say. Long enough to be a
-- block rather than a poll, short enough that a bug here shows up as a
-- sluggish window rather than a dead one.
local POLL_DEFAULT = 0
local next_handle = 1
local damage = {}
local running = true

local function add_damage(x, y, w, h)
  if w <= 0 or h <= 0 then return end
  damage[#damage + 1] = { x = x, y = y, w = w, h = h }
end

-- Everything a window occupies on screen, decoration included. Used for
-- damage, so it has to be the outside of the outermost thing drawn.
--
-- Menus, above everything, in a list of their own.
--
-- `roadmap.md` M13 item 2 and the design that won: **a menu is a window.**
-- Same record, same `by_handle`, same `draw`, same `close`, same
-- `collect_closing` - so a menu inherits every death path the desktop
-- already survives rather than inventing one. That is the whole argument
-- for it: the alternative designs each grew a modal input grab held by the
-- window manager on behalf of an application, which is the one new way to
-- wedge a desktop whose entire premise is that it cannot be wedged.
--
-- What is *not* shared is the stacking. `windows` is walked in a dozen
-- places, two of which read `i == #windows` as "focused", and threading a
-- kind flag through all of them to keep menus out of the focus order would
-- be twelve chances to get it wrong. A second list, composited after the
-- first and hit-tested before it, leaves every one of those walks exactly
-- as correct as it was.
--
local menus = {}

--
-- A menu has no decoration, so its frame is its rectangle. Everything that
-- damages, hit-tests or composites goes through here, which is why this is
-- the only place that has to know.
--
local function frame_of(win)
  if win.kind == "menu" then
    return win.x, win.y, win.w, win.h
  end

  return win.x - BORDER,
         win.y - TAB_H,
         win.w + BORDER * 2,
         win.h + TAB_H + BORDER
end

local function damage_window(win)
  add_damage(frame_of(win))
end

--
-- Where the minimise box starts. The maximise box is `BOX_W` further right.
--
-- One function rather than the same arithmetic in the compositor and in the
-- pointer, because those two agreeing by coincidence is how a control ends
-- up drawn in one place and clickable in another.
--
local function boxes_x(win)
  local fx, _, fw = frame_of(win)

  return fx + fw - BOX_W * 2 - 8
end

--------------------------------------------------------------------------
-- The drawing commands an application may send.
--
-- A small set on purpose. Each one is a primitive that already exists in
-- C, so this table is a name-to-primitive map and not an interpreter -
-- there is nothing here that loops over pixels in Lua, which is the rule
-- `gfx.md` 19.2 exists to keep.
--
-- Anything unrecognised is skipped rather than refused. An application
-- built against a later version of this list should lose a rectangle, not
-- its window.
--------------------------------------------------------------------------

local ops = {
  fill = function(s, o)
    s:fill(o.x or 0, o.y or 0, o.w or 0, o.h or 0, o.color or 0xff000000)
  end,

  text = function(s, o)
    s:text(o.x or 0, o.y or 0, tostring(o.s or ""),
           o.color or 0xffffffff, o.bg)
  end,

  --
  -- Removed, because it could only ever have failed.
  --
  -- This called `s:blend(x, y, w, h, colour)` as though it were an alpha
  -- fill. `l_blend` in `gfx.c` is not one: it is a surface-to-surface alpha
  -- *blit* and wants a surface at argument two, so this passed a number
  -- where a userdata was checked for and raised every time.
  --
  -- It never raised, because nothing could reach it: `ui.lua`'s graphics
  -- context has no `blend` verb, so no application has ever sent the op.
  -- Dead and wrong at the same time, which is the pair that survives
  -- longest - neither the compiler nor the tests have anything to say about
  -- code nobody calls.
  --
  -- An alpha fill is genuinely wanted for a dimmed control or a wash behind
  -- a menu, and when it arrives it is new C in `gfx.c` plus a verb in the
  -- kit, not this line.
  --
  blend = function(s, o)
    local _ = s, o
  end,

  --
  -- A picture, named rather than carried.
  --
  -- Every other command carries what it draws. This one cannot: a decoded
  -- image is megabytes and a message is two kilobytes, so a surface can no
  -- more travel in one than a window's contents can.
  --
  -- So the application sends a *name* and this process loads it. That is
  -- not a workaround, it is the same rule as everywhere else here - the
  -- compositor owns the pixels, which is why a hung application still has a
  -- window - applied to pictures. An application says what it wants drawn;
  -- it never holds what is drawn.
  --
  -- Decoded once, on first use, and kept. A photograph is four megabytes
  -- and several hundred milliseconds of inflate; doing that per frame would
  -- be a slideshow.
  --
  image = function(s, o)
    local picture = image_cache[o.asset]

    if picture == nil then
      --
      -- `local ok, decoded = bytes and pcall(...)` is what this was, and it
      -- is wrong in a way Lua does not warn about: `and` truncates a
      -- multi-value expression to one value, so `ok` got pcall's first
      -- return and `decoded` got nothing. The picture decoded perfectly and
      -- was thrown away, and the window said there was no such picture.
      --
      picture = false

      local bytes = sys.asset(tostring(o.asset))

      if bytes then
        local ok, decoded = pcall(gfx.png, bytes)

        if ok then picture = decoded end
      end

      image_cache[o.asset] = picture
    end

    if not picture then return end

    s:blit(picture, o.sx or 0, o.sy or 0, o.w or 0, o.h or 0,
           o.x or 0, o.y or 0)
  end,
}

--------------------------------------------------------------------------
-- The pointer.
--
-- The tablet reports absolute position in a range of its own - 0 to 32767
-- on this machine, whatever the display is - so the scaling happens here,
-- where the size of the screen is known. The kernel passes the range out
-- undecoded for exactly this reason.
--
-- Absolute rather than relative is the right kind of device for a virtual
-- machine: there is no acceleration curve to agree on with the host, so the
-- guest cursor cannot drift away from the real one.
--
-- The cursor is composited like everything else: drawn into the backbuffer
-- after the windows, so the blit that reaches the screen already has it.
--
-- It was drawn straight onto the screen after the composite, which is
-- cheaper and wrong. Every repaint blits the finished region over the
-- cursor and then puts it back on the next line - and QEMU scans out on its
-- own schedule, so it can sample between those two. What that looks like is
-- the cursor flickering on every click, once on the press and once on the
-- release, because each of those repaints the window it is sitting on.
--
-- Moving it costs two rectangles of damage instead of one: where it was, so
-- it is erased, and where it is, so it is drawn. That is the price of the
-- frame being whole.
--------------------------------------------------------------------------

local CURSOR_W, CURSOR_H = 10, 16

-- An arrow. Two colours so it stays visible on a light window and on the
-- dark desktop: white body, dark outline.
local CURSOR = {
  "X.........",
  "XX........",
  "XoX.......",
  "XooX......",
  "XoooX.....",
  "XooooX....",
  "XoooooX...",
  "XooooooX..",
  "XoooooooX.",
  "XooooooooX",
  "XoooooXXXX",
  "XooXooX...",
  "XoX.XooX..",
  "XX..XooX..",
  "X....XooX.",
  "......XX..",
}

local pointer_x, pointer_y = 0, 0
local buttons = 0
local dragging = nil          -- { win, dx, dy } while a title bar is held
local resizing = nil          -- { win, ox, oy, ow, oh } while a grip is held

-- Declared here and defined below, because the compositor and the pointer
-- both ask about resizing and both run above the code that answers. Locals
-- rather than globals: `luaglobals` is what caught this, which is what it
-- is for.
local resizable, resize_window
local grabbed = nil           -- the window a press landed in, until release

--------------------------------------------------------------------------
-- The frame profile.
--
-- Where a pass of the loop below actually goes, measured rather than
-- argued. The system is aiming at a fast desktop on a Pi 5 and every
-- discussion about moving this process into C has had to guess at whether
-- its Lua half is five per cent of a frame or fifty. This is the thing
-- that answers that.
--
-- **Off by default, and costing nothing while it is off.** The timer reads
-- are behind `profiling`, so a desktop nobody asked to measure itself does
-- not make eight extra syscalls a hundred times a second.
--
-- **Waiting is not working, and the two are counted apart.** Most of a
-- pass on an idle desktop is `wait_input` sleeping, which is the loop
-- doing its job rather than costing anything. `busy` is every stage except
-- that one, and `busy.max` - the worst pass ever seen - is the number that
-- matters: responsiveness is a promise about the worst case.
--
-- **Pixels, so compose time can be divided by something.** Knowing that
-- compose took 3 ms means nothing without knowing whether it drew a
-- cursor or the whole screen. With both, the quotient can be compared
-- against what the C primitives do on their own, and the difference is
-- what the Lua around them costs.
--
-- **And the collector, because that is the whole argument.** C is proposed
-- here for jitter rather than throughput: a GC pause is about 1.25 ms and
-- arrives when it chooses, against a 16 ms frame. So a pass during which
-- the heap shrank is a pass a collection finished in, and the worst of
-- those is recorded separately. If the worst pass overall is also a
-- collection pass, the argument is made. If it is not, it is not.
--------------------------------------------------------------------------

local STAGES = { "wait", "keys", "messages", "pointer",
                 "waiting", "collect", "compose" }

local profiling = false

--
-- Whether *this* pass is being measured, decided once at the top of it.
--
-- Not the same question as `profiling`, and conflating them was a crash on
-- the first run: `frames` turns the profile on by sending a message, which
-- is handled in the middle of a pass, so a pass that began unmeasured
-- reached the next stage boundary with no reading to subtract from. A pass
-- measures throughout or not at all - which is also the only way its
-- stages can add up to its total.
--
local measuring = false

local prof
local pass_busy = 0

--
-- A client blocked in `fs.send` until its measurement is over.
--
-- `frames` cannot sleep for itself: `sys.wait_input` is refused to anything
-- that does not own the console, and this process owns it. Spinning instead
-- would be worse than useless - a program burning processor beside the loop
-- it is measuring lands in whichever stage the loop was preempted in, and
-- the measurement would be of the measuring.
--
-- So the wait happens here, and the client spends it blocked in IPC, which
-- costs a descheduled thread and nothing else. Same shape as `poll`.
--
local profile_waiting = nil

local function prof_reset()
  prof = { passes = 0, frames = 0, rects = 0, px = 0,
           busy = { total = 0, max = 0 },
           gc = { collections = 0, worst = 0 } }

  for _, name in ipairs(STAGES) do
    prof[name] = { total = 0, max = 0 }
  end
end

-- Charge the time since `t0` to a stage, and hand back the reading so the
-- next stage starts where this one ended - one clock read per boundary
-- rather than two.
local function charge(stage, t0, idle)
  local now = sys.ticks()
  local took = now - t0
  local s = prof[stage]

  s.total = s.total + took
  if took > s.max then s.max = took end
  if not idle then pass_busy = pass_busy + took end

  return now
end

-- Everything measured, flattened: the serialiser crosses this as a table of
-- scalars, and a stage is two numbers rather than a structure worth naming
-- twice. Times are counter ticks - what a tick is worth is `/dev/cpu`'s
-- business and the reporting program's, not this one's.
local function profile_report()
  local out = { ok = true, profiling = profiling,
                passes = prof.passes, frames = prof.frames,
                rects = prof.rects, px = prof.px,
                busy_total = prof.busy.total, busy_max = prof.busy.max,
                collections = prof.gc.collections, gc_worst = prof.gc.worst,
                heap = collectgarbage("count") }

  for _, name in ipairs(STAGES) do
    out[name .. "_total"] = prof[name].total
    out[name .. "_max"]   = prof[name].max
  end

  return out
end

--
-- Answer a client whose measurement is over.
--
-- Called at the very top of a pass, before that pass decides whether it is
-- being measured, and stopping first so that it is not. Building a report
-- takes a few microseconds and charging them to a stage would land them in
-- `max` - which is the one number this whole thing exists to report, and
-- the last place to put an artefact of reporting it.
--
local function profile_due()
  if not profile_waiting then return end
  if sys.ticks() < profile_waiting.deadline then return end

  local who = profile_waiting.who

  profiling = false
  profile_waiting = nil

  pcall(sys.reply, who, profile_report())
end

-- Into the backbuffer, clipped by the surface primitives like anything
-- else. A run of identical pixels at a time rather than one fill per pixel:
-- the arrow is ten columns wide and mostly runs.
local function draw_cursor()
  for row = 0, CURSOR_H - 1 do
    local line = CURSOR[row + 1]
    local col = 0

    while col < CURSOR_W do
      local ch = line:sub(col + 1, col + 1)

      if ch == "." then
        col = col + 1
      else
        local run = 1

        while line:sub(col + run + 1, col + run + 1) == ch do
          run = run + 1
        end

        back:fill(pointer_x + col, pointer_y + row, run, 1,
                  (ch == "X") and 0xff000000 or 0xffffffff)
        col = col + run
      end
    end
  end
end

--------------------------------------------------------------------------
-- Compositing.
--
-- One rectangle at a time: desktop, then every window that overlaps it,
-- back to front, then the copy out. Windows are opaque, so there is no
-- blending between them and the order is the whole of the occlusion.
--------------------------------------------------------------------------

local function compose_rect(r)
  back:fill(r.x, r.y, r.w, r.h, desktop_colour())

  -- What is running, bottom right, on the desktop and under everything
  -- else. Drawn as part of the composite rather than once at startup, so a
  -- window dragged over it and away again leaves it intact.
  --
  -- And only when this rectangle actually reaches that corner. The
  -- primitives clip to the backbuffer, so a rectangle in the top left was
  -- still paying for every glyph of it.
  local sx, sy = W - #stamp * gfx.font.w - 10, H - gfx.font.h - 8

  if r.x < W and r.x + r.w > sx and r.y < H and r.y + r.h > sy then
    back:text(sx, sy, stamp, stamp_colour(), desktop_colour())
  end

  for i = 1, #windows do
    local win = windows[i]
    local focused = (i == #windows)
    local fx, fy, fw, fh = frame_of(win)

    --
    -- Windows that this rectangle does not touch are skipped, and the ones
    -- it does touch are drawn only where it touches them.
    --
    -- This is what makes dragging cost the same with eight windows open as
    -- with one. Without it every damage rectangle redrew every window in
    -- full - the primitives clip to the *backbuffer*, not to the rectangle
    -- being composed - so moving one window re-blitted the entire desktop
    -- twice per step, once for where it was and once for where it now is.
    -- What that feels like is a drag that gets heavier as you open things,
    -- which is exactly what it was.
    --
    if not win.hidden
       and fx < r.x + r.w and fx + fw > r.x
       and fy < r.y + r.h and fy + fh > r.y then
      local tab = focused and focused_colour() or idle_colour()

      -- The whole decoration in one colour: the bar across the top and the
      -- border all the way round, yellow when this window has the focus and
      -- grey when it does not.
      --
      -- **This is where Kosmos stops copying BeOS**, and the departure is
      -- deliberate. A BeOS tab is as wide as its title so that several
      -- stacked windows show their titles at once - the most recognisable
      -- decision in that whole look, and a functional one. Kosmos does not
      -- stack windows, so the narrow tab bought nothing here and cost the
      -- one thing a border can do for free: say which window is listening,
      -- from the corner of your eye, without reading anything.
      --
      -- It also makes the picture agree with the behaviour. Dragging has
      -- always been the full width of the frame - the hit test below is
      -- `ny < fy + TAB_H` and knows nothing about the title's length - so
      -- the narrow tab was drawing a handle smaller than the one you could
      -- actually grab.
      --
      -- Clipped to the damage rectangle, which the contents below have
      -- always been and this had never been.
      --
      -- The comment above explains that the primitives clip to the
      -- backbuffer rather than to the rectangle being composed, and uses
      -- that to skip windows the rectangle does not touch. It did not
      -- finish the thought: a window the rectangle touches *at all* was
      -- having its whole frame filled. Ten pixels of damage on a 360x264
      -- window cost 95,040 of them.
      --
      -- Which is what the profile found. A dragged window composed half
      -- the pixels of an animating one and took four fifths of the time,
      -- and a cost that does not fall when the damage does is a cost that
      -- is not being charged to the damage.
      --
      local dx0 = (fx > r.x) and fx or r.x
      local dy0 = (fy > r.y) and fy or r.y
      local dx1 = math.min(fx + fw, r.x + r.w)
      local dy1 = math.min(fy + fh, r.y + r.h)

      back:fill(dx0, dy0, dx1 - dx0, dy1 - dy0, tab)

      -- The close box, at the left of the tab where BeOS put it. A square
      -- outline rather than a cross: at this size a cross is four grey
      -- pixels and a smudge.
      --
      -- Both it and the title only when the rectangle reaches the tab at
      -- all. A window whose *contents* changed damages the area below the
      -- bar, and redrawing a title nobody disturbed is a string of glyphs
      -- per frame for nothing.
      if r.y < fy + TAB_H and r.y + r.h > fy then
        local bx, by = fx + 4, fy + (TAB_H - 8) // 2

        back:fill(bx, by, 8, 8, title_colour())
        back:fill(bx + 1, by + 1, 6, 6, tab)

        back:text(fx + 4 + CLOSE_W, fy + (TAB_H - gfx.font.h) // 2, win.title,
                  title_colour(), tab)

        --
        -- Minimise and maximise, at the *right*, with close staying at the
        -- left where BeOS put it and `ui.md` 16.8b records it.
        --
        -- Not all three together at one end, which is what Photon does and
        -- what every system since has copied. Splitting them is the older
        -- arrangement and it is the better one for a reason that outlived
        -- the fashion: close is the irreversible one, and putting it a
        -- window's width away from the two harmless ones means a slip
        -- hides a window instead of ending it.
        --
        local mx = boxes_x(win)
        local my = fy + (TAB_H - 8) // 2

        -- Minimise: a bar along the bottom of its box.
        back:fill(mx, my, 8, 8, title_colour())
        back:fill(mx + 1, my + 1, 6, 5, tab)

        if resizable(win) then
          -- Maximise: an outline, filled when the window already is.
          local zx = mx + BOX_W

          back:fill(zx, my, 8, 8, title_colour())

          if not win.restore then
            back:fill(zx + 1, my + 2, 6, 5, tab)
          end
        end
      end

      -- And the contents, clipped to the intersection. The window's own
      -- surface is the source, so the source rectangle moves with the clip:
      -- reading from 0,0 and drawing at the clipped position would slide
      -- the picture inside its own frame.
      local x0 = (win.x > r.x) and win.x or r.x
      local y0 = (win.y > r.y) and win.y or r.y
      local x1 = math.min(win.x + win.w, r.x + r.w)
      local y1 = math.min(win.y + win.h, r.y + r.h)

      if x1 > x0 and y1 > y0 then
        -- Whichever buffer the application is not drawing into, or the
        -- surface this process owns for an ordinary window.
        local from = win.surface

        if win.shared then
          from = win.shared[win.shared.live]
        end

        back:blit(from, x0 - win.x, y0 - win.y,
                  x1 - x0, y1 - y0, x0, y0)
      end

      --
      -- The sizing grip, over the window's own bottom-right corner.
      --
      -- Three diagonal steps rather than a solid block, which is the
      -- shape every desktop uses for this and is readable at a glance
      -- without a label. Drawn after the contents so it sits on top of
      -- them, and only on windows that can actually be resized - see
      -- `resizable`.
      --
      if resizable(win) then
        local gx = win.x + win.w - GRIP
        local gy = win.y + win.h - GRIP

        for step = 0, 2 do
          local o = step * 5
          local n = GRIP - 3 - o

          if n > 0 then
            -- Light above, dark below: the same two edges every raised
            -- thing here is made of, at a diagonal.
            back:fill(gx + o + 2, gy + GRIP - 3 - o, n, 1, theme.edge_light)
            back:fill(gx + o + 2, gy + GRIP - 2 - o, n, 1, theme.edge_dark)
          end
        end
      end
    end
  end

  --
  -- Menus, above every window and below the cursor.
  --
  -- No decoration and no tab: a menu is its rectangle. One groove around
  -- it so it reads as sitting on top of what is behind it rather than
  -- being part of it - which is the whole job of the border on a thing
  -- that floats.
  --
  for i = 1, #menus do
    local m = menus[i]

    if m.x < r.x + r.w and m.x + m.w > r.x
       and m.y < r.y + r.h and m.y + m.h > r.y then
      local x0 = (m.x > r.x) and m.x or r.x
      local y0 = (m.y > r.y) and m.y or r.y
      local x1 = math.min(m.x + m.w, r.x + r.w)
      local y1 = math.min(m.y + m.h, r.y + r.h)

      if x1 > x0 and y1 > y0 then
        back:blit(m.surface, x0 - m.x, y0 - m.y,
                  x1 - x0, y1 - y0, x0, y0)
      end
    end
  end

  -- Last, so it is on top of everything, and before the blit, so what
  -- reaches the screen is a frame with a cursor in it.
  draw_cursor()

  screen:blit(back, r.x, r.y, r.w, r.h, r.x, r.y)
end

local function compose()
  if #damage == 0 then return end

  if measuring then
    prof.frames = prof.frames + 1
    prof.rects  = prof.rects + #damage
  end

  for _, r in ipairs(damage) do
    if measuring then prof.px = prof.px + r.w * r.h end
    compose_rect(r)
  end

  damage = {}
end

--------------------------------------------------------------------------
-- The protocol.
--------------------------------------------------------------------------

local function focused_window()
  return windows[#windows]
end

--
-- Full screen, and back again.
--
-- `win.restore` holds where it was and is the flag as well as the record:
-- a window with one is maximised, and restoring clears it. Keeping the two
-- separate would let them disagree, which is how a maximised window ends up
-- restoring to its own maximised size and can never be got back.
--
local function maximise(win)
  if not resizable(win) then return false end

  if win.restore then
    local r = win.restore

    win.restore = nil
    damage_window(win)
    win.x, win.y = r.x, r.y
    resize_window(win, r.w, r.h)

    return true
  end

  win.restore = { x = win.x, y = win.y, w = win.w, h = win.h }

  damage_window(win)
  win.x, win.y = BORDER, TAB_H
  resize_window(win, W - BORDER * 2, H - TAB_H - BORDER)

  return true
end

--
-- Out of sight, and the Deskbar is how it comes back.
--
-- Nothing else is needed: the Deskbar already lists every window and
-- already raises one by handle, so `raise` clearing this is the whole of
-- restore. A minimised window keeps its surface and its contents and is
-- simply not composed - an application that carries on drawing into it is
-- not interrupted, which is the same arrangement as a window buried under
-- another one.
--
local function minimise(win)
  if win.hidden then return false end

  win.hidden = true
  damage_window(win)

  return true
end

local function raise(win)
  -- Raising a minimised window is how it comes back, and the Deskbar is
  -- what does the raising. See `minimise`.
  if win.hidden then
    win.hidden = false
    damage_window(win)
  end

  if windows[#windows] == win then return end

  -- The window *losing* the focus is damaged as well as the one gaining it.
  --
  -- Its decoration is about to be drawn in a different colour, and this
  -- compositor only repaints what is damaged - so without this the old
  -- window keeps the focused colour until something else happens to cover
  -- it. Two windows both looking focused, which is the one thing the colour
  -- is there to tell you.
  --
  -- It was always wrong and was easy to miss while the tab was as narrow as
  -- its title. Painting the whole border made it obvious.
  local losing = windows[#windows]

  for i, w_ in ipairs(windows) do
    if w_ == win then
      table.remove(windows, i)
      break
    end
  end

  windows[#windows + 1] = win
  damage_window(win)

  if losing then
    damage_window(losing)
  end
end

local handlers = {}

--------------------------------------------------------------------------
-- A window whose pixels the application draws itself.
--
-- `gfx.md` 19.4. Everything else here sends drawing *commands* and this
-- process owns every pixel, which is what lets a hung application keep a
-- window. That is right for a window of widgets and wrong for a video
-- frame or a rendered scene, where the pixels change wholesale thirty
-- times a second and describing them costs more than copying them.
--
-- So an application may hand over a region of memory instead. It draws into
-- that region with the same C primitives everything else uses, and says
-- when it is finished.
--
-- **Two buffers and an index, because there are no locks.** If the
-- application wrote the buffer while this process read it there would be
-- tearing and nothing to prevent it - `design.md` 6 has no locks and is not
-- getting any. So the region holds two, the application draws into the one
-- that is not being shown, and `commit` swaps which is which. Neither side
-- ever touches the buffer the other is using.
--
-- **Damage is part of the commit, not a separate call.** Without it this
-- process would have to blit the whole surface every frame, which is the
-- cost the whole arrangement exists to avoid. Making it a field of the
-- message rather than another message makes it hard to forget.
--
-- **An application that never commits is composed from its last frame.**
-- The hung-window property is unchanged: nothing here waits.
--------------------------------------------------------------------------

handlers.open = function(req, who, cap)
  local w_ = math.min(math.max(tonumber(req.w) or 320, 32), W - 8)
  local h_ = math.min(math.max(tonumber(req.h) or 200, 32), H - TAB_H - 8)

  local win = {
    handle  = next_handle,
    title   = tostring(req.title or "window"),
    x       = math.min(math.max(tonumber(req.x) or 40, BORDER), W - w_ - BORDER),
    y       = math.min(math.max(tonumber(req.y) or 40, TAB_H), H - h_ - BORDER),
    w       = w_,
    h       = h_,
    surface = gfx.surface{ w = w_, h = h_ },
    events  = {},
  }

  win.surface:fill(0, 0, w_, h_, 0xff202020)

  --
  -- A shared region came with the request, so this window's contents are
  -- the application's own memory rather than a surface this process draws
  -- into. Two buffers in one region, and `live` says which one is being
  -- shown - `gfx.md` 19.4.
  --
  if cap and cap >= 0 then
    local at, why = sys.memory_map(cap)

    if at then
      local bytes = gfx.bytes(w_, h_)

      win.shared = {
        cap = cap,
        [1] = gfx.wrap{ at = at, w = w_, h = h_ },
        [2] = gfx.wrap{ at = at + bytes, w = w_, h = h_ },
        live = 1,
      }
    else
      -- Said rather than silently ignored. A window that quietly refuses a
      -- shared surface is a window that opens, stays blank, and gives the
      -- application no idea why - which is how this arrived the first time.
      print("wm: could not map a shared surface: " .. tostring(why))
    end
  end

  --
  -- A menu rather than a window, and the difference is only in where it is
  -- kept: no decoration, above everything, placed exactly where it was
  -- asked for rather than clamped into the workspace, and it never takes
  -- the focus.
  --
  -- `owner` is the window it belongs to. Its events go to that window's
  -- queue, so an application polls one handle and gets everything - which
  -- is what keeps `ui.window`'s loop a loop rather than two.
  --
  if req.kind == "menu" then
    win.kind = "menu"
    win.owner = tonumber(req.owner)

    -- Placed where it was asked, only pulled back far enough to be on the
    -- screen. A menu under the pointer is the whole point of a menu.
    win.x = math.min(math.max(tonumber(req.x) or 0, 0), W - w_)
    win.y = math.min(math.max(tonumber(req.y) or 0, 0), H - h_)
  end

  -- Whoever was launched most recently, if this is their first window.
  win.pid = pending_pid
  pending_pid = nil

  next_handle = next_handle + 1

  by_handle[win.handle] = win

  if win.kind == "menu" then
    --
    -- Into the menu list, and it takes no focus: the window that owns it
    -- keeps the focus while it is up, which is what makes a menu feel like
    -- part of the window it came from rather than a window of its own.
    --
    menus[#menus + 1] = win
    damage_window(win)
  else
    -- The window that had the focus loses it to this one, and has to be
    -- repainted to say so. Same reason as `raise`: only damage is redrawn.
    local losing = windows[#windows]

    windows[#windows + 1] = win
    damage_window(win)

    if losing then
      damage_window(losing)
    end
  end

  -- The appearance in force, in the reply that creates the window.
  --
  -- An application loads `theme.lua`, which defaults to dark, and had no
  -- way to learn that the desktop is currently light - so every new window
  -- opened dark and only became light when somebody changed the theme
  -- *again*. Telling it here rather than posting an event means it knows
  -- before its first paint, so there is no flash of the wrong colours.
  -- `x` and `y` go back as well as `w` and `h`. A menu asks to appear at a
  -- particular place on the screen and may have been pulled back to fit, and
  -- a caller that does not know where its menu ended up cannot hit-test it.
  return { ok = true, window = win.handle, w = w_, h = h_,
           x = win.x, y = win.y,
           palette = theme.current(), desktop = theme.desktop,
           fonts = theme.fonts }
end

handlers.draw = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  for _, o in ipairs(req.ops or {}) do
    local fn = ops[o.op]
    if fn then fn(win.surface, o) end
  end

  --
  -- `more` means the application has not finished this frame.
  --
  -- A window's drawing does not fit in one message, so it arrives in
  -- several - and damaging after each one composited the window
  -- half-redrawn. What that looks like is a flicker on every click: the
  -- first message clears the background and the widgets arrive over the
  -- following two, and the screen is scanned out somewhere in the middle.
  --
  -- The surface is written either way. Only the *damage* waits, so what
  -- reaches the screen is one complete frame rather than three partial
  -- ones. That is what a backbuffer is for, applied one level up: an
  -- application composes off-screen and says when it is done.
  --
  if not req.more then
    damage_window(win)
  end

  return { ok = true }
end

--
-- Start a program, in a window, from inside the desktop.
--
-- The window manager does this rather than the application asking for it,
-- and the reason is capabilities: launching means handing the new process
-- an endpoint to this one, and the only process that holds that endpoint is
-- this one. A launcher that could do it itself would have to be given the
-- desktop's own door, and then every application that could reach the
-- launcher could reach that too.
--
-- So the Deskbar asks. It can name a program and nothing else.
--
handlers.launch = function(req)
  local name = tostring(req.program or "")

  if name == "" or name:match("[^%w%-_./]") then
    return { ok = false, error = "not a program name: " .. name }
  end

  local path = name:sub(1, 1) == "/" and name or ("/bin/" .. name .. ".lua")
  local ok, err, id = run(path, req.args or "", true, { ["/dev/wm"] = ep })

  if not ok then
    return { ok = false, error = tostring(err) }
  end

  -- Remembered so that the *next* window to open can be tied to it. There
  -- is nothing better available: a window arrives in a message and a
  -- message does not say which process sent it - the sender is a thread
  -- pointer, and the thread that opens a window is the one this started.
  -- Good enough to end what was just launched, and honestly not more.
  pending_pid = id
  return { ok = true }
end

--
-- Every window on the desktop, back to front.
--
-- The Deskbar asks this rather than asking /app, and the difference matters:
-- /app holds applications that registered, which means the ones that used
-- `ui.window`. A program that opens a window by talking to this process
-- directly - as the first two demonstrations here do, because they were
-- written before there was a kit - has a window on screen and no
-- registration anywhere.
--
-- What belongs in a list of what is running is what is on the screen, and
-- this process is the only one that knows that.
--
handlers.windows = function(req)
  local out = {}

  for i, win in ipairs(windows) do
    out[i] = { handle = win.handle, title = win.title,
               focused = (i == #windows) or nil }
  end

  return { ok = true, windows = out }
end

--
-- Bring one to the front. What clicking a name in the Deskbar does.
--
handlers.raise = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  raise(win)
  return { ok = true }
end

--
-- How big a picture is, so an application can lay out around it.
--
-- Asked of this process because this process is the one that has it. An
-- application that could decode it itself would have the pixels, which is
-- the thing the whole design is arranged to prevent.
--
handlers.image_size = function(req)
  local name = tostring(req.asset or "")
  local picture = image_cache[name]

  if picture == nil then
    picture = false

    local bytes = sys.asset(name)

    if bytes then
      local ok, decoded = pcall(gfx.png, bytes)

      if ok then picture = decoded end
    end

    image_cache[name] = picture
  end

  if not picture then
    return { ok = false, error = "no such picture, or it would not decode" }
  end

  local w_, h_ = picture:size()
  return { ok = true, w = w_, h = h_ }
end

--
-- The application has finished a frame.
--
-- Swaps which buffer is shown and damages what it said changed. Nothing is
-- copied: the compositor simply reads the other one from now on.
--
handlers.commit = function(req)
  local win = by_handle[req.window]

  if not win or not win.shared then
    return { ok = false, error = "that window does not have a shared surface" }
  end

  win.shared.live = (win.shared.live == 1) and 2 or 1

  local x = math.max(0, math.floor(tonumber(req.x) or 0))
  local y = math.max(0, math.floor(tonumber(req.y) or 0))
  local w_ = math.min(win.w - x, math.floor(tonumber(req.w) or win.w))
  local h_ = math.min(win.h - y, math.floor(tonumber(req.h) or win.h))

  if w_ > 0 and h_ > 0 then
    add_damage(win.x + x, win.y + y, w_, h_)
  end

  -- The buffer the application should draw into next: the one this process
  -- has just stopped showing.
  return { ok = true, draw_into = win.shared.live == 1 and 2 or 1 }
end

--
-- A window asking for a size, rather than a person dragging one.
--
-- Same clamps and the same event as the grip, because an application that
-- resizes itself has to lay out again exactly as one that was resized by
-- hand - and having two paths that agree only by accident is how they stop
-- agreeing.
--
handlers.resize = function(req)
  local win = by_handle[req.window]

  if not win then return { ok = false, error = "no such window" } end

  if not resizable(win) then
    return { ok = false,
             error = "a window that draws its own pixels cannot be resized yet" }
  end

  resize_window(win, tonumber(req.w) or win.w, tonumber(req.h) or win.h)

  return { ok = true, w = win.w, h = win.h }
end

handlers.retitle = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  -- Both the old tab and the new one: a shorter title leaves the tail of
  -- the longer one behind, and the tab is as wide as its text.
  damage_window(win)
  win.title = tostring(req.title or win.title)
  damage_window(win)

  return { ok = true }
end

handlers.move = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  -- The old place has to be repainted as well as the new one, or the window
  -- leaves a copy of itself behind.
  damage_window(win)
  -- A window may hang off the edge, as long as enough of it stays to grab.
  --
  -- Clamping it entirely inside the screen is the obvious thing and it is
  -- wrong: every desktop lets you push a window aside to see what is under
  -- it, and a window that stops dead at the edge cannot be pushed anywhere.
  --
  -- What must not happen is losing it. So the tab may not go above the top -
  -- it is the only handle - and KEEP pixels of the window stay on screen in
  -- every other direction, which is always enough of the tab to catch.
  local KEEP = 48

  win.x = math.min(math.max(tonumber(req.x) or win.x, KEEP - win.w),
                   W - KEEP)
  win.y = math.min(math.max(tonumber(req.y) or win.y, TAB_H),
                   H - KEEP)
  damage_window(win)

  return { ok = true }
end

local function events_for(win)
  local out = win.events
  win.events = {}
  return { ok = true, events = out }
end

handlers.poll = function(req, who)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  if #win.events > 0 then
    return events_for(win)
  end

  -- Nothing yet. Hold the answer rather than sending an empty one.
  local wait = tonumber(req.wait) or POLL_DEFAULT

  waiting[#waiting + 1] = {
    who = who, win = win, deadline = sys.ticks() + wait,
  }

  return DEFER
end

--
-- Answer everyone whose window has something, or whose wait is over.
--
local function answer_waiting()
  local now = sys.ticks()
  local still = {}

  for _, w in ipairs(waiting) do
    if by_handle[w.win.handle] == nil then
      -- Its window closed underneath it. An empty answer, so the
      -- application's loop notices and leaves rather than hanging on a
      -- reply nobody is going to send.
      pcall(sys.reply, w.who, { ok = true, events = {} })
    elseif #w.win.events > 0 or now >= w.deadline then
      pcall(sys.reply, w.who, events_for(w.win))
    else
      still[#still + 1] = w
    end
  end

  waiting = still
end

handlers.close = function(req)
  local win = by_handle[req.window]
  if not win then return { ok = false, error = "no such window" } end

  damage_window(win)
  by_handle[req.window] = nil

  -- Whichever list it is in. A menu is a window in every way except where
  -- it is stacked, and this is the one place that difference has to be
  -- spelled out on the way out.
  local list = (win.kind == "menu") and menus or windows

  for i, w_ in ipairs(list) do
    if w_ == win then
      table.remove(list, i)
      break
    end
  end

  --
  -- And a window takes its menus with it. Without this a menu outlives the
  -- window it belongs to and floats above a desktop with nothing behind
  -- it, still taking clicks - which is the leak the whole design was
  -- chosen to avoid, arriving by the back door.
  --
  if win.kind ~= "menu" then
    for i = #menus, 1, -1 do
      if menus[i].owner == req.window then
        local orphan = menus[i]

        damage_window(orphan)
        by_handle[orphan.handle] = nil
        table.remove(menus, i)
        orphan.surface:free()
      end
    end
  end

  win.surface:free()
  return { ok = true }
end


--
-- Everything the window manager did not claim goes to the window with the
-- focus, as an event it can collect whenever it gets round to asking.
-- Queued and not delivered: delivering would mean calling the application,
-- and calling it is what this process must never do.
--
local function post(win, event)
  if not win then return end

  win.events[#win.events + 1] = event

  if #win.events > 64 then
    -- An application that has stopped collecting its events is not going to
    -- start. Dropping the oldest is better than growing without limit in a
    -- process that everything else on the screen depends on.
    table.remove(win.events, 1)
  end
end

--------------------------------------------------------------------------
-- The appearance of everything.
--
-- One request changes the palette and the desktop colour, repaints the
-- whole screen, and tells every window so its widgets are redrawn in the
-- new colours too. `ui.lua` mutates its own palette table in place when it
-- gets that event, so a running application changes appearance without
-- restarting and without knowing this happened.
--
-- The window manager is where this lives because it is the one process
-- that already talks to every window. A settings *server* would be the
-- other answer and is more machinery than one palette needs.
--------------------------------------------------------------------------
handlers.theme = function(req)
  if req.palette then
    local ok, err = theme.apply(req.palette)

    if not ok then
      return { ok = false, error = tostring(err) }
    end
  end

  -- The desktop colour is chosen separately from the palette it sits with,
  -- so a light theme over a dark desktop is a thing somebody can have.
  if req.desktop then
    theme.override { desktop = req.desktop }
  end

  -- The font travels with the palette, because they are the same decision
  -- from the user's side and arrive from the same window. Applied here and
  -- forwarded, so a window drawing its own pixels changes too.
  local font_why = apply_fonts(req.fonts)

  --
  -- The whole palette, not its name.
  --
  -- A name only works while every window already holds the same palettes,
  -- which was true when two were compiled in and stopped being true the
  -- moment a theme could be a file. `theme.apply` takes either, so this is
  -- the one line that had to change.
  --
  local now = theme.current()

  for _, win in ipairs(windows) do
    post(win, { type = "theme", palette = now, desktop = theme.desktop,
                fonts = theme.fonts })
  end

  -- Menus wear the theme too, and they are not in `windows`.
  for _, m in ipairs(menus) do
    post(by_handle[m.owner], { type = "theme", palette = now,
                               desktop = theme.desktop, fonts = theme.fonts })
  end

  add_damage(0, 0, W, H)

  return { ok = true, palette = theme.name, desktop = theme.desktop,
           fonts = theme.fonts, font_why = font_why }
end

--
-- Whether a window can be resized at all.
--
-- One that draws its own pixels cannot, yet. Its buffers are a region the
-- *application* allocated and handed over, sized for exactly these
-- dimensions and holding two of them - so this process cannot make them
-- bigger, and pretending otherwise would mean compositing from memory that
-- was never mapped. Giving it back a new region needs a message in the
-- protocol and a round trip through an application that may be hung, which
-- is a second piece of work and not this one.
--
-- Until then the grip is not drawn on those windows, which is the honest
-- way to say no: a control that is not there cannot be pressed and be
-- ignored.
--
function resizable(win)
  return win.shared == nil
end

--
-- A new size, and a new surface to go with it.
--
-- The old contents are not carried over. It would be a blit and it would be
-- wrong: the application lays out for the size it is given, so what is on
-- screen a moment after a resize should be what it drew for the new size,
-- not the old picture stretched or cropped underneath it. The window is
-- filled with its background and the application is told; the next frame it
-- sends is the right one.
--
function resize_window(win, w, h)
  w = math.floor(w)
  h = math.floor(h)

  -- Not smaller than a window, and not bigger than the screen it has to fit
  -- inside along with its own decoration.
  if w < MIN_W then w = MIN_W end
  if h < MIN_H then h = MIN_H end
  if w > W - BORDER * 2 then w = W - BORDER * 2 end
  if h > H - TAB_H - BORDER then h = H - TAB_H - BORDER end

  if w == win.w and h == win.h then return false end

  -- Where it was, so the part it no longer covers is repainted.
  damage_window(win)

  local fresh = gfx.surface{ w = w, h = h }

  if not fresh then
    -- Out of memory for a surface. The window keeps the one it has, which
    -- is a window that did not resize rather than a window that is gone.
    return false
  end

  fresh:fill(0, 0, w, h, 0xff202020)

  win.surface:free()
  win.surface = fresh
  win.w, win.h = w, h

  damage_window(win)

  -- And the application, so it can lay out again. Queued like every other
  -- event: this process does not call applications.
  post(win, { type = "resize", w = w, h = h })

  return true
end

local function to_focused(c)
  post(focused_window(), { type = "key", code = c })
end

--------------------------------------------------------------------------
-- Input.
--
-- `ui.md` 16.7: input is never behind anything else. Here that is not a
-- thread priority but the shape of the loop - keys are read every pass,
-- before any application is served, and nothing in this loop can block.
--
-- Until there is a pointer device the keyboard does the dragging:
--
--   the pointer               click to raise, drag a title bar to move
--   Control-W then an arrow    move the focused window
--   Control-W then Tab         focus the next window
--   Control-W then Control-W   a literal Control-W to the application
--   Control-C                  give the screen back
--
-- Everything else belongs to the application. See `prefixed` below for why
-- there is a prefix key at all rather than a handful of reserved ones.
--
-- Arrows arrive as three bytes - escape, [, then A to D - which is the one
-- piece of terminal grammar this has to know, and it knows it here rather
-- than in the console because the console has no idea what a window is.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- Closing, and ending what will not close.
--
-- Two steps, because they are two different things. Asking a window to
-- close is a message: an application that is listening saves what it was
-- doing and goes, which is the only version that can ever be correct.
-- Ending the process is what happens when nobody is listening, and it
-- cannot be polite because there is nobody to be polite to.
--
-- The grace period is what separates them. A second is far longer than a
-- healthy application needs to notice a message - a window polls its
-- events every pass - and short enough that a wedged one does not sit
-- there being clicked at.
--
-- The kernel does the ending, and only for a child: `sys.kill` is the
-- authority a parent already has by being able to wait for one. This
-- process started these programs, so this process may end them, and
-- nothing else may.
--------------------------------------------------------------------------

local close_grace = 0

do
  local cpu = fs.read("/dev/cpu")
  close_grace = (cpu and cpu.counter_hz or 62500000)
end

--------------------------------------------------------------------------
-- Ending an application, on somebody else's behalf.
--
-- `SYS_KILL` lets a parent end a child and nothing else, which is right and
-- is why `procs` could not do it: it did not start anything. The process
-- that *did* start every application is this one, so this is where the
-- authority lives - and asking whoever holds it is the whole shape of the
-- system rather than a workaround for it.
--
-- Only a process with a window here. A server was started by init and this
-- has no business ending it; init would be the one to ask, and nothing has
-- a reason to yet.
--------------------------------------------------------------------------
--
-- Start, stop, or read the profile. `/bin/frames.lua` is the client.
--
-- Reading does not stop the measurement and does not reset it, so a long
-- run can be sampled while it happens; `on = true` is what clears the
-- counters. Times are in counter ticks, because this process has no
-- business deciding what a tick is worth - `/dev/cpu` says, and the
-- program that prints the report is where that division belongs.
--
handlers.profile = function(req, who)
  if req.on == true then
    prof_reset()
    pass_busy = 0
    profiling = true

    --
    -- `run_for` counter ticks and then the answer, which is how `frames`
    -- waits without spinning. Deferred exactly as `poll` is: the reply is
    -- this process's to send later, and until it does the client is a
    -- blocked thread costing nothing.
    --
    if req.run_for then
      profile_waiting = { who = who,
                          deadline = sys.ticks() + req.run_for }
      return DEFER
    end

    return { ok = true, profiling = true }
  elseif req.on == false then
    profiling = false
    profile_waiting = nil
    return { ok = true, profiling = false }
  end

  if not prof then
    return { ok = false, error = "nothing measured yet: start it with on=true" }
  end

  return profile_report()
end

handlers.end_process = function(req)
  local pid = tonumber(req.pid)

  if not pid then
    return { ok = false, error = "no such process" }
  end

  for _, win in ipairs(windows) do
    if win.pid == pid then
      -- Asked first, as the close box does: a window that is listening
      -- tidies up and goes, and one that is not is taken by force on a
      -- later pass.
      win.closing = sys.ticks() + close_grace
      post(win, { type = "close" })

      if sys.kill(pid) then
        return { ok = true, ended = true }
      end

      return { ok = true, ended = false }
    end
  end

  return { ok = false,
           error = "that is not an application this desktop started" }
end

local function collect_closing()
  local now = sys.ticks()

  for i = #windows, 1, -1 do
    local win = windows[i]

    if win.closing and now > win.closing then
      -- It had its chance. Take the window off the screen first, so the
      -- desktop is usable the instant this is decided, and then end the
      -- process behind it.
      handlers.close{ window = win.handle }

      if win.pid then
        sys.kill(win.pid)
      end
    end
  end
end

-- Which window is under a point, front to back, decoration included.
--
-- The menu under a point, if any. Front to back, like windows.
--
local function menu_at(x, y)
  for i = #menus, 1, -1 do
    local m = menus[i]

    if x >= m.x and x < m.x + m.w and y >= m.y and y < m.y + m.h then
      return m
    end
  end

  return nil
end

--
-- Every menu, gone.
--
-- Called when a press lands outside them, when the owner is raised away,
-- and when the owner dies. It goes through `handlers.close` rather than
-- deleting the record, so a menu leaves by the same path a window leaves
-- by - which is the reason for building a menu as a window in the first
-- place and would be wasted by tearing one down by hand here.
--
local function dismiss_menus(owner_handle)
  local n = #menus

  for i = n, 1, -1 do
    local m = menus[i]

    if owner_handle == nil or m.owner == owner_handle then
      -- pcall because this runs from `pointer_pass`, which the main loop
      -- calls bare rather than inside the pcall that wraps handlers. An
      -- error here would take the desktop with it.
      pcall(handlers.close, { window = m.handle })
    end
  end

  return #menus < n
end

local function window_at(x, y)
  for i = #windows, 1, -1 do
    local win = windows[i]
    local fx, fy, fw, fh = frame_of(win)

    if x >= fx and x < fx + fw and y >= fy and y < fy + fh then
      return win, fx, fy
    end
  end

  return nil
end

local function pointer_pass(p)
  if not p then return end

  local range_x = (p.max_x - p.min_x)
  local range_y = (p.max_y - p.min_y)

  if range_x <= 0 or range_y <= 0 then return end

  local nx = (p.x - p.min_x) * (W - 1) // range_x
  local ny = (p.y - p.min_y) * (H - 1) // range_y

  local was_down = (buttons & 1) ~= 0
  local is_down = (p.buttons & 1) ~= 0

  local moved_this_pass = (nx ~= pointer_x or ny ~= pointer_y)

  if moved_this_pass then
    -- Both rectangles: where it was, so it is erased, and where it is going,
    -- so it is drawn. Both are composited, so neither is ever half-done.
    add_damage(pointer_x, pointer_y, CURSOR_W, CURSOR_H)
    pointer_x, pointer_y = nx, ny
    add_damage(pointer_x, pointer_y, CURSOR_W, CURSOR_H)
  end

  --------------------------------------------------------------------------
  -- What a press means depends on where it lands.
  --
  --   the title bar    the window manager's: raise and drag
  --   the contents     the application's: forwarded, in that window's own
  --                    coordinates
  --
  -- Forwarded and not delivered, like a key: it goes on the window's queue
  -- and the application collects it whenever it gets round to asking. This
  -- process never calls an application, which is the whole reason a hung one
  -- cannot freeze the desktop - and a click is not an exception to that.
  --
  -- A press grabs. Everything until the release goes to the window the press
  -- landed in, even after the pointer has left it, because that is what lets
  -- a button un-press when you slide off it and a drag keep working past the
  -- edge. Without a grab, releasing outside would deliver the release to
  -- whatever happened to be underneath.
  --------------------------------------------------------------------------
  if is_down and not was_down then
    --
    -- Menus first, and they take the press whatever is under them.
    --
    -- A press inside one goes to the *owner's* queue tagged with the menu's
    -- handle, so an application polls one window and gets everything - which
    -- is the reason a menu belongs to a window rather than standing alone.
    --
    -- A press outside every menu dismisses them and stops there. It does not
    -- also reach whatever is underneath, which is what every desktop does
    -- and is the right answer: the first click after opening a menu is how
    -- you change your mind, not how you press the thing behind it.
    --
    if #menus > 0 then
      local m = menu_at(nx, ny)

      if m then
        post(by_handle[m.owner],
             { type = "mouse", menu = m.handle, action = "press",
               x = nx - m.x, y = ny - m.y })
        grabbed = m
      else
        dismiss_menus()
      end

      buttons = p.buttons
      return
    end

    local win, fx, fy = window_at(nx, ny)

    if win then
      raise(win)

      if ny < fy + TAB_H then
        local mx = boxes_x(win)

        if nx >= mx and nx < mx + BOX_W then
          minimise(win)
        elseif nx >= mx + BOX_W and nx < mx + BOX_W * 2 and resizable(win) then
          maximise(win)
        elseif nx < fx + 4 + CLOSE_W then
          --
          -- The close box. Asked first, taken by force second.
          --
          -- The window is told, and a window that is listening tidies up
          -- and goes. One that is not listening - the whole point of this
          -- desktop being able to survive one - never answers, so the
          -- request is remembered and collected on a later pass.
          --
          win.closing = sys.ticks() + close_grace
          post(win, { type = "close" })
        else
          dragging = { win = win, dx = nx - win.x, dy = ny - win.y }
        end
      elseif resizable(win)
             and nx >= win.x + win.w - GRIP and nx < win.x + win.w
             and ny >= win.y + win.h - GRIP and ny < win.y + win.h then
        --
        -- The grip, and it is tested before the contents on purpose: this
        -- square belongs to the window manager, and an application that
        -- happens to have drawn something there does not get the press.
        --
        resizing = { win = win, ox = nx, oy = ny, ow = win.w, oh = win.h }
      else
        grabbed = win
        post(win, { type = "mouse", action = "press",
                    x = nx - win.x, y = ny - win.y })
      end
    end
  elseif not is_down and was_down then
    if grabbed then
      if grabbed.kind == "menu" then
        post(by_handle[grabbed.owner],
             { type = "mouse", menu = grabbed.handle, action = "release",
               x = nx - grabbed.x, y = ny - grabbed.y })
      else
        post(grabbed, { type = "mouse", action = "release",
                        x = nx - grabbed.x, y = ny - grabbed.y })
      end

      grabbed = nil
    end

    dragging = nil
    resizing = nil
  end

  if resizing and is_down then
    resize_window(resizing.win,
                  resizing.ow + (nx - resizing.ox),
                  resizing.oh + (ny - resizing.oy))
  end

  if dragging and is_down then
    handlers.move{ window = dragging.win.handle,
                   x = nx - dragging.dx, y = ny - dragging.dy }
  end

  --
  -- Movement, only while something is held.
  --
  -- Hover is not sent, and that is a decision rather than an omission. Every
  -- movement would be a message, an application would poll a queue full of
  -- them, and the whole path from here to a widget would run at the rate the
  -- pointer moves rather than at the rate anything changes. What a button
  -- needs to un-press when you slide off it is drag, and this is drag.
  --
  if grabbed and is_down and moved_this_pass and grabbed.kind == "menu" then
    post(by_handle[grabbed.owner],
         { type = "mouse", menu = grabbed.handle, action = "move",
           x = nx - grabbed.x, y = ny - grabbed.y })
  elseif grabbed and is_down and moved_this_pass then
    post(grabbed, { type = "mouse", action = "move",
                    x = nx - grabbed.x, y = ny - grabbed.y })
  end

  buttons = p.buttons
end

local STEP = 16
local prefix = false
local pending_escape = {}

local function move_focused(dx, dy)
  local win = focused_window()
  if not win then return end
  handlers.move{ window = win.handle, x = win.x + dx, y = win.y + dy }
end

--
-- A prefix key, rather than a set of reserved ones.
--
-- The first version took Tab for "next window" and the arrows for "move the
-- window". Both were wrong, and the gallery showed it in one screenshot:
-- Tab is how every user interface ever built moves between controls, and
-- the arrows are how every list is used. A window manager that keeps them
-- has decided that no application may have a second control.
--
-- There are no modifiers to escape into. A virtio keyboard gives Control
-- plus a letter and nothing else - no Alt, no Super, and Control plus an
-- arrow is a terminal escape sequence this system does not speak. So this
-- takes the approach screen and tmux took for exactly the same reason:
-- **one** key is reserved, and it introduces a command rather than being
-- one.
--
--   Control-W then an arrow    move the focused window
--   Control-W then Tab         focus the next window
--   Control-W then Control-W   send a literal Control-W to the application
--
-- One key out of the application's vocabulary instead of five, and the one
-- taken is the one applications want least.
--
local function prefixed(c)
  prefix = false

  if c == KEY_TAB then
    if #windows > 1 then raise(windows[1]) end
    return
  end

  if c == KEY_PREFIX then
    to_focused(KEY_PREFIX)
    return
  end

  if c == KEY_ESC then
    -- An arrow after the prefix. The escape belongs to the sequence, not to
    -- the application, so the prefix stays on until the sequence finishes.
    prefix = true
    pending_escape = { KEY_ESC }
    return
  end
end

local function key(c)
  if c == KEY_CTRL_C then
    running = false
    return
  end

  -- Halfway through an escape sequence that began after the prefix.
  if #pending_escape > 0 then
    if #pending_escape == 1 then
      if c == 91 then                                   -- '['
        pending_escape[2] = c
        return
      end

      pending_escape = {}
      prefix = false
      to_focused(c)
      return
    end

    pending_escape = {}
    prefix = false

    if c == 65 then move_focused(0, -STEP) return end   -- A, up
    if c == 66 then move_focused(0,  STEP) return end   -- B, down
    if c == 67 then move_focused( STEP, 0) return end   -- C, right
    if c == 68 then move_focused(-STEP, 0) return end   -- D, left
    return
  end

  if prefix then
    prefixed(c)
    return
  end

  if c == KEY_PREFIX then
    prefix = true
    return
  end

  to_focused(c)
end

--------------------------------------------------------------------------
-- Start whatever was asked for, each in a process of its own, each handed
-- this endpoint under a name. They cannot reach it any other way and they
-- cannot pass it anywhere this process did not.
--------------------------------------------------------------------------

-- `name` or `name:arguments`, comma separated. The colon is so a program
-- started here can be given something to work on - `wm gallery,setprop:...`
-- - without this having to understand a shell's quoting rules, which it is
-- not and should not become.
--
-- With nothing asked for, the Deskbar. A desktop with no way to start
-- anything is a desktop you can only look at, and every other system starts
-- something for the same reason.
--
local wanted = tostring(args or ""):match("^%s*(.-)%s*$")

if wanted == "" then wanted = "deskbar" end

for entry in wanted:gmatch("[^,]+") do
  entry = entry:match("^%s*(.-)%s*$")

  if entry ~= "" then
    local name, argument = entry:match("^([^:]+):(.*)$")
    name = name or entry

    local path = name:sub(1, 1) == "/" and name or ("/bin/" .. name .. ".lua")
    local ok, err, id = run(path, argument or "", true,
                            { ["/dev/wm"] = ep })

    if not ok then
      print(("wm: could not start %s: %s"):format(path, tostring(err)))
    else
      pending_pid = id
    end
  end
end

--------------------------------------------------------------------------
-- The loop.
--------------------------------------------------------------------------

add_damage(0, 0, W, H)

--
-- How long a pass is allowed to sleep for, in *scheduler* ticks.
--
-- One of them, so this loop wakes a hundred times a second when nothing is
-- happening. An input interrupt cuts the sleep short, so this is not the
-- latency of a keystroke - it is only how often an idle desktop wakes up to
-- find there is still nothing to do.
--
-- Scheduler ticks and not the counter `sys.ticks()` returns. They differ by
-- a factor of six hundred thousand here, and passing one for the other asks
-- for a sleep of several hours - which looks exactly like the desktop
-- having hung, because it has.
--
local PASS = 1

while running do
  -- 1. Input, always first - and this is where the pass sleeps if there is
  -- none. One call for keys and the pointer together, because the console
  -- has to be asked anyway and two round trips to learn nothing is one
  -- more than necessary.
  local t, heap

  profile_due()

  measuring = profiling

  if measuring then
    prof.passes = prof.passes + 1
    heap = collectgarbage("count")
    t = sys.ticks()
  end

  local input = fs.wait_input("/dev/console", PASS) or {}

  -- Idle, and charged as such: this is the loop asleep with nothing to do,
  -- and counting it as work would make an empty desktop look busy.
  if measuring then t = charge("wait", t, true) end

  for _, c in ipairs(input.keys or {}) do
    key(c)
  end

  if measuring then t = charge("keys", t) end

  -- 2. Whatever the applications have asked for, and not one message more
  -- than has already arrived.
  while true do
    --
    -- The third value is a capability that came *with* the request, at
    -- whatever index the kernel put it in this process's table. Only `open`
    -- uses it, to receive the shared region a direct window draws into -
    -- and forgetting to take it here is exactly how that arrived as a
    -- window that opened and stayed blank.
    --
    local req, who, cap = sys.receive(ep, true)
    if not req then break end

    local handler = handlers[req.type]
    local reply

    if not handler then
      reply = { ok = false, error = "no such operation: " .. tostring(req.type) }
    else
      local ok, result = pcall(handler, req, who, cap)
      reply = ok and result or { ok = false, error = tostring(result) }
    end

    -- A handler that took responsibility for its own answer, which `poll`
    -- does when there is nothing to report yet.
    if reply ~= DEFER then
      pcall(sys.reply, who, reply)
    end
  end

  if measuring then t = charge("messages", t) end

  -- 3. The pointer, before the picture: a click can raise a window and a
  -- drag can move one, and both are damage that this pass should draw.
  pointer_pass(input.pointer)

  if measuring then t = charge("pointer", t) end

  -- 4. Anybody who has been waiting long enough, or now has something.
  answer_waiting()

  if measuring then t = charge("waiting", t) end

  -- 5. Anything that was asked to close and did not, and the slots of
  -- anything that has already gone.
  collect_closing()

  --
  -- A process that has exited keeps its slot until somebody collects its
  -- exit code, which is what makes an exit code readable at all. Nobody was
  -- collecting these: closing two applications left two processes in the
  -- table for ever, and a desktop is exactly the thing that starts and ends
  -- programs all day.
  --
  -- Non-blocking, so a desktop with nothing to collect does not stop.
  --
  while sys.wait(true) do end

  if measuring then t = charge("collect", t) end

  -- 6. The picture, cursor included.
  compose()

  if measuring then
    charge("compose", t)

    local b = prof.busy
    b.total = b.total + pass_busy
    if pass_busy > b.max then b.max = pass_busy end

    --
    -- A pass the heap ended smaller than it started is a pass a collection
    -- finished in. That is not every collection - an incremental step that
    -- only marked will not show - but it is every one that freed anything,
    -- which is the kind that takes the time.
    --
    if collectgarbage("count") < heap then
      local gc = prof.gc
      gc.collections = gc.collections + 1
      if pass_busy > gc.worst then gc.worst = pass_busy end
    end

    pass_busy = 0
  end
end

-- Given back, which repaints: the console has no scrollback, so it starts
-- again from the top rather than restoring something nobody kept.
sys.screen_take(false)

back:free()

for _, win in ipairs(windows) do
  win.surface:free()
end

sys.destroy(ep)
