-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: name Hello Window
-- kosmos: section development
-- An application with a window.
--
-- Started by `wm`, which hands it the window manager under /Running/wm and
-- nothing else it did not already have. It draws once, then redraws when a
-- key arrives - **into pixels of its own**: a region of two pictures this
-- process owns and hands the window manager when it opens the window, one
-- shown while the other is drawn, and `commit` says which. Since Astra's D2
-- (`docs/astra-display.md`, 9 October 2026) every window draws itself so;
-- the window manager only puts the pictures on the screen.
--

local wmproto = use("/Kosmos/Libraries/wmproto.lua")
local regions = use("/Kosmos/Libraries/regions.lua")

local W, H = 360, 200

-- Two pictures of the window's size, in one region.
local bytes = gfx.bytes(W, H)
local region = regions.make(bytes * 2)

if not region then
  print("hello-win: no memory for its pictures")
  return
end

local pictures = { gfx.wrap{ at = region.at, w = W, h = H },
                   gfx.wrap{ at = region.at + bytes, w = W, h = H } }

-- The window manager shows the first until the first commit; this draws
-- into the second.
local draw_into = 2

local win, err = fs.send("/Running/wm", {
  type = "open", title = "hello", w = W, h = H, x = 80, y = 120,
}, region.cap)

if not win then
  print("hello-win: " .. tostring(err))
  return
end

local handle = win.window
local presses = 0

local function draw()
  local s = pictures[draw_into]

  s:fill(0, 0, W, H, 0xff101820)
  s:fill(0, 0, W, 28, 0xff1f6feb)
  s:text(10, 7, "A window of my own", 0xffffffff, 0xff1f6feb)
  s:text(10, 48, "The pixels are here, in this process:", 0xffc9d1d9, 0xff101820)
  s:text(10, 64, "it draws them and hands them over,", 0xffc9d1d9, 0xff101820)
  s:text(10, 80, "and the window manager shows them.", 0xffc9d1d9, 0xff101820)
  s:text(10, 120, ("keys received: %d"):format(presses), 0xff7ee787, 0xff101820)
  s:text(10, 150, "Tab switches windows, arrows move one.", 0xff8b949e, 0xff101820)

  local r = fs.send("/Running/wm", { type = "commit", window = handle })

  draw_into = (r and r.draw_into) or (draw_into == 1 and 2 or 1)
end

draw()

-- **Asking, and waiting for the answer.** The window manager holds a poll
-- until an event comes or the wait runs out, so between keys this process
-- is blocked rather than running. This asked with no wait and yielded, for
-- ever - the spin `ui.lua`'s own loop was cured of long before - and on a
-- machine with one processor the window manager spent its time answering
-- it: a window opened after it never drew its first frame (`testing.md`
-- 18.291). The wait is in scheduler ticks, a second of them.
local second = (sys.info() or {}).tick_hz or 250

while true do
  local reply = wmproto.poll(handle, second)

  if not reply then return end            -- the manager went away

  if #reply.events > 0 then
    presses = presses + #reply.events
    draw()
  end
end
