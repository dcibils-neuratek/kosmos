-- kosmos: application
-- kosmos: section demos
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

local win, err = fs.send("/dev/wm", {
  type = "open", title = "hung", w = W, h = H, x = 470, y = 300,
})

if not win then
  print("stuck: " .. tostring(err))
  return
end

fs.send("/dev/wm", { type = "draw", window = win.window, ops = {
  { op = "fill", x = 0, y = 0, w = W, h = H, color = 0xff3d1418 },
  { op = "fill", x = 0, y = 0, w = W, h = 28, color = 0xffda3633 },
  { op = "text", x = 10, y = 7, s = "not answering",
    color = 0xffffffff, bg = 0xffda3633 },
  { op = "text", x = 10, y = 50, s = "This process never replies again.",
    color = 0xffffc9c9, bg = 0xff3d1418 },
  { op = "text", x = 10, y = 70, s = "Drag this window anyway.",
    color = 0xffffc9c9, bg = 0xff3d1418 },
} })

-- Not a sleep and not a yield: a loop that gives nothing back, which is the
-- worst an application can do to the machine while remaining an application.
while true do end
