-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_Generic
-- kosmos: section demos
-- An application with a window.
--
-- Started by `wm`, which hands it the window manager under /Running/wm and
-- nothing else it did not already have. It draws once, then redraws when a
-- key arrives, and it never touches a pixel: everything it wants on screen
-- leaves here as a list of commands.
--
-- That is `ui.md` 16.6, and the reason for it is visible from the other
-- side: because the pixels live in the window manager, this program can
-- stop answering and its window carries on existing.

local wmproto = use("/Kosmos/Libraries/wmproto.lua")

local W, H = 360, 200

local win, err = fs.send("/Running/wm", {
  type = "open", title = "hello", w = W, h = H, x = 80, y = 120,
})

if not win then
  print("hello-win: " .. tostring(err))
  return
end

local handle = win.window
local presses = 0

local function draw()
  fs.send("/Running/wm", { type = "draw", window = handle, ops = {
    { op = "fill", x = 0, y = 0, w = W, h = H, color = 0xff101820 },
    { op = "fill", x = 0, y = 0, w = W, h = 28,  color = 0xff1f6feb },
    { op = "text", x = 10, y = 7, s = "A window of my own",
      color = 0xffffffff, bg = 0xff1f6feb },
    { op = "text", x = 10, y = 48,
      s = "The pixels are not here. They are in the",
      color = 0xffc9d1d9, bg = 0xff101820 },
    { op = "text", x = 10, y = 64,
      s = "window manager, which is why this window",
      color = 0xffc9d1d9, bg = 0xff101820 },
    { op = "text", x = 10, y = 80,
      s = "outlives whatever happens in here.",
      color = 0xffc9d1d9, bg = 0xff101820 },
    { op = "text", x = 10, y = 120,
      s = ("keys received: %d"):format(presses),
      color = 0xff7ee787, bg = 0xff101820 },
    { op = "text", x = 10, y = 150,
      s = "Tab switches windows, arrows move one.",
      color = 0xff8b949e, bg = 0xff101820 },
  } })
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
