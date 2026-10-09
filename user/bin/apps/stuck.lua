-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon Misc_Bug
-- kosmos: name Stuck Window
-- kosmos: section development
-- An application that hangs, on purpose.
--
-- It opens a window, draws it once, and then stops answering for ever.
-- This is the other half of this milestone's definition of done: with this
-- running, its window must still be there, must still show what it drew,
-- and must still move when you drag it.
--
-- If dragging ever stops working while this is on screen, something has
-- started waiting for an application, and no amount of speed anywhere else
-- will fix that.

local W, H = 300, 140

-- Its own pixels, as every window has them since Astra's D2: two pictures in
-- a region handed over with the window, drawn into and committed once.
local regions = use("/Kosmos/Libraries/regions.lua")
local bytes = gfx.bytes(W, H)
local region = regions.make(bytes * 2)

if not region then
  print("stuck: no memory for its pictures")
  return
end

local win, err = fs.send("/Running/wm", {
  type = "open", title = "hung", w = W, h = H, x = 470, y = 300,
}, region.cap)

if not win then
  print("stuck: " .. tostring(err))
  return
end

local s = gfx.wrap{ at = region.at + bytes, w = W, h = H }

s:fill(0, 0, W, H, 0xff3d1418)
s:fill(0, 0, W, 28, 0xffda3633)
s:text(10, 7, "not answering", 0xffffffff, 0xffda3633)
s:text(10, 50, "This process never replies again.", 0xffffc9c9, 0xff3d1418)
s:text(10, 70, "Drag this window anyway.", 0xffffc9c9, 0xff3d1418)
fs.send("/Running/wm", { type = "commit", window = win.window })

-- Not a sleep and not a yield: a loop that gives nothing back, which is the
-- worst an application can do to the machine while remaining an application.
while true do end
