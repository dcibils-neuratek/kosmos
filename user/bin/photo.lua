-- kosmos: application
-- A picture, in a window.
--
--   wm photo                     the test pattern
--   wm photo:image00.png         something else in assets/images/
--
-- The picture is named, not carried. This program never holds a pixel of
-- it: it tells the window manager which asset to draw and where, and the
-- window manager - which owns every pixel on the screen already - decodes
-- it once and blits it. That is the same arrangement that lets a hung
-- application keep a window.
--
-- Drag it, or use the arrows, if it is bigger than its frame. There is no
-- scaler: one written in Lua would be the per-pixel loop `gfx.md` 19.2
-- forbids, and when one exists it will be a `gfx` primitive.

local ui = use("/lib/ui.lua")
-- The *kit's* palette, not a copy of it.
--
-- `use` runs the chunk again and hands back a different table, and only the
-- one `ui.lua` holds is the one it mutates when the desktop changes theme.
-- An application that loaded its own kept the colours it started with while
-- every widget around it changed - which is exactly what Monitor, Processes,
-- Photo and the Terminal did.
local theme = ui.theme

local name = tostring(args or ""):match("^%s*(%S+)") or "test-pattern.png"

if not name:match("%.png$") then name = name .. ".png" end

local W, H = 560, 420

local win, err = ui.window{ title = "Photo", w = W, h = H, x = 110, y = 70 }

if not win then
  print("photo: " .. tostring(err))
  return
end

local picture = ui.image{ x = 10, y = 30, w = W - 20, h = H - 66,
                          asset = name }

win:add(ui.label{ x = 10, y = 10, text = name, color = "text" })
win:add(picture)

local size = (picture.image_w > 0)
             and ("%d x %d, drag to pan"):format(picture.image_w,
                                                 picture.image_h)
             or "not carried in the image"

win:add(ui.label{ x = 10, y = H - 26, text = size, color = "text_dim" })

--
-- What else there is, so the list is discoverable from inside the system
-- rather than only from the source tree.
--
local others = {}

for _, a in ipairs(sys.asset()) do
  if a ~= name then others[#others + 1] = a end
end

if #others > 0 then
  win:add(ui.label{ x = 240, y = H - 26,
                    text = "also: " .. table.concat(others, "  "),
                    color = "line" })
end

win:run()
