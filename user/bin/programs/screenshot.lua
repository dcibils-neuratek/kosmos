-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs desktop
-- screenshot: the whole screen, as a PNG in /Home/Captures.
--
--   Print Screen, Control Alt 1, or Super Shift 3 - or `screenshot` at the
--   prompt of a machine whose desktop is running
--
-- Diego, 22 September 2026: "like ctrl+alt+1 full screen screenshots", and
-- on 3 October "what is the shortcut in kosmos to grab a screenshot and save
-- it in captures dir?", "so i can send you screenshots without taking photos
-- from my mobile". Named by the moment it was taken, where you are -
-- `screenshot-2026-10-03-224512.png` - with no spaces, so a prompt can name
-- it, beside the camera's and the recorder's files in Tracker's Captures.
--
-- **A program of its own, not the window manager's work.** Encoding a
-- screen and writing a few megabytes to a stick is time, and nothing on the
-- desktop may wait on a server (`CLAUDE.md`): so the key starts this, and
-- the window manager goes on drawing. The screen is borrowed the way `vncd`
-- borrows it - `/Running/wm/remote`, which only a program in the image that
-- `needs desktop` is lent - into a region of this process, which the window
-- manager fills with the frame as it answers. It lets the copy go by itself
-- five seconds after nobody asks.

local REMOTE = "/Running/wm/remote"
local clock = use("/Kosmos/Libraries/clock.lua")

local size = fs.send(REMOTE, { type = "watch" })

if type(size) ~= "table" or not size.w then
  print("screenshot: the desktop did not lend its screen - the desktop has to be running")
  return
end

local cap = sys.memory((size.bytes + 4095) // 4096)
local at = cap and sys.memory_map(cap)

if not at then
  print("screenshot: no memory for a picture of the screen")
  return
end

local lent = fs.send(REMOTE, { type = "watch" }, cap)

if type(lent) ~= "table" or not lent.ok then
  print("screenshot: the desktop would not share its screen: "
        .. tostring(type(lent) == "table" and lent.error or lent))
  return
end

local picture = gfx.wrap{ at = at, w = size.w, h = size.h }
local png = gfx.encode_png(picture)

local t = clock.now()
local stamp = t and ("%04d-%02d-%02d-%02d%02d%02d"):format(t.year, t.month, t.day,
                                                           t.hour, t.min, t.sec)
              or tostring(sys.ticks())
local folder = "/Home/Captures"

if not fs.getattr(folder) then fs.send(folder, { type = "mkdir" }) end

local path = folder .. "/screenshot-" .. stamp .. ".png"
local ok, why = fs.write(path, png)

if not ok then
  print("screenshot: " .. path .. " was not written: " .. tostring(why))
  return
end

print(("screenshot: %s, %dx%d, %d bytes"):format(path, size.w, size.h, #png))
