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

local clock = use("/Kosmos/Libraries/clock.lua")
local files = use("/Kosmos/Libraries/files.lua")
local wmproto = use("/Kosmos/Libraries/wmproto.lua")

-- The screen asked for as `vncd` asks for it (`wmproto.lua`): its size,
-- a region that size, and the region handed over and filled.
local screen, why, lent = wmproto.screen()

if not screen then
  print("screenshot: " .. why .. (lent and "" or " - the desktop has to be running"))
  return
end

local lent

lent, why = wmproto.watch(screen)

if not lent then
  print("screenshot: " .. why)
  return
end

local png = gfx.encode_png(screen.surface)

local t = clock.now()
local stamp = t and clock.stamp(t) or tostring(sys.ticks())
local folder = "/Home/Captures"

files.make_folder(folder)

local path = folder .. "/screenshot-" .. stamp .. ".png"
local ok

ok, why = fs.write(path, png)

if not ok then
  print("screenshot: " .. path .. " was not written: " .. tostring(why))
  return
end

print(("screenshot: %s, %dx%d, %d bytes"):format(path, screen.w, screen.h, #png))
