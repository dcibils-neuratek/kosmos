-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_CodyCam
-- kosmos: section applications
-- kosmos: needs camera
-- A USB camera's live picture (`roadmap.md` 6d), as `docs/camera.html`
-- draws it: the header with the camera and its size, the size as a
-- dropdown and the rest behind the three dots; the picture at one pixel to
-- one, black around it; and under it how fast frames really arrive.
--
-- Diego, 24 September 2026: "i want to have a simple app that shows the
-- live feed of the camera", and on the drawing - "yes but put an option to
-- mirror or not", "size dropdown", "yes is perfect".
--
-- **A window that draws its own pixels**, as the video player is: a frame
-- is a surface's worth of pixels thirty times a second, which a widget -
-- a list of commands the window manager draws - cannot carry. So the header
-- and its controls come from `pixelkit`, at the kit's numbers, and the
-- picture comes from `surface:camera`, in C, straight out of the region the
-- driver writes (`/lib/camera.lua`). No pixel passes through this file.
--
-- **Record** is drawn and not yet pressable: recording to H.264 is step 8f.
--
--   camera                 the first camera, at 640x480
--   camera 320x240         at that size, or the largest inside it

local ui = use("/lib/ui.lua")
local theme = ui.theme
local pk = use("/lib/pixelkit.lua").new(ui)
local camera = use("/lib/camera.lua")
local wmproto = use("/lib/wmproto.lua")
local L = ui.layout

local want_w, want_h = 640, 480

do
  local w, h = tostring(args or ""):match("(%d+)x(%d+)")

  if w then want_w, want_h = tonumber(w), tonumber(h) end
end

-- The drawing's window: 680 by 596 for a 640 by 480 picture - the header,
-- 520 of picture with 20 of black round it, and a 30-pixel foot.
local W, FOOT = 680, 30
local PICTURE_H = 520
local H = L.head + PICTURE_H + FOOT
local PICTURE_Y = L.head

local win = ui.window{ title = "Camera", w = W, h = H, x = 120, y = 90,
                       direct = true }

if not win or not win:surface() then
  print("camera: no window")
  return
end

--------------------------------------------------------------------------
-- What is showing, and what could be.
--------------------------------------------------------------------------

local cameras, why_none = camera.all()
local chosen = 1                        -- in `cameras`

-- Where a camera's frames come from (`CAMERA_SOURCE_*`): in the log line,
-- and in the foot after the rate - "over USB" only when they are.
local FROM = { usb = "over USB", pattern = "drawn by the driver" }
local FOOT_FROM = { usb = " over USB", pattern = ", drawn by the driver" }
local stream, size, picture = nil, nil, nil
local mirror = true                     -- as a Mac's own preview is
local trouble = nil                     -- a sentence when there is no picture
local fps, counted, counted_at = 0, 0, sys.ticks()
local counter_hz = (sys.info() or {}).counter_hz or 62500000
local retry_at = 0

local function sub_line()
  local cam = cameras[chosen]

  if not cam then return "no camera" end

  if not size then return cam.name end

  return ("%s \u{b7} %d \u{d7} %d"):format(cam.name, size.width, size.height)
end

local function close_stream()
  if stream then stream:close() end
  stream, picture = nil, nil
end

local function open_at(new_size)
  local cam = cameras[chosen]

  close_stream()
  trouble = nil

  if not cam or not new_size then
    trouble = { "No camera", "Plug in a USB camera. Most webcams are",
                "USB Video Class, which is what Kosmos drives." }
    return false
  end

  local s, why = camera.open(cam.index, new_size)

  if not s then
    if why == camera.IN_USE then
      trouble = { "In use", "Another Camera window has it." }
    else
      trouble = { "The camera would not start", tostring(why) }
    end

    return false
  end

  stream, size = s, new_size
  picture = gfx.surface{ w = new_size.width, h = new_size.height }
  picture:fill(0, 0, new_size.width, new_size.height, 0xff000000)
  counted, counted_at, fps = 0, sys.ticks(), 0

  print(("camera: %s at %dx%d, %s, picture at %d,%d, %s"):format(
    cam.name, size.width, size.height, FROM[cam.source] or "from somewhere",
    0, PICTURE_Y,
    mirror and "mirrored" or "as the camera sees it"))
  return true
end

--------------------------------------------------------------------------
-- The header's three controls, from the right: the dots, the size, Record.
--------------------------------------------------------------------------

local more = { icon = "more" }
local size_box = { h = 31 }
local record = { text = "Record", disabled = true }

local function place_controls()
  local right = W - L.head_edge

  more.x, more.y = right - 26, (L.head - 1 - 26) // 2
  right = more.x - L.head_gap

  local label = size and ("%d \u{d7} %d"):format(size.width, size.height)
                or "Size"

  size_box.label = label
  size_box.w = gfx.measure(label) + 12 + 8 + 7 + 11
  size_box.x, size_box.y = right - size_box.w, (L.head - 1 - 31) // 2
  right = size_box.x - L.head_gap

  record.w = pk.button_width(record.text) + 16
  record.x, record.y = right - record.w, (L.head - 1 - 31) // 2
end

-- The drawings' chevron: a "v" seven across and three down.
local function chevron(s, cx, cy)
  s:fill(cx - 3, cy,     2, 1, theme.text_dim)
  s:fill(cx + 2, cy,     2, 1, theme.text_dim)
  s:fill(cx - 2, cy + 1, 2, 1, theme.text_dim)
  s:fill(cx + 1, cy + 1, 2, 1, theme.text_dim)
  s:fill(cx - 1, cy + 2, 3, 1, theme.text_dim)
end

local function draw_header(s)
  place_controls()
  pk.header(s, 0, 0, W, "Camera", sub_line(), record.x - 8)

  -- Record: a red dot and the word, greyed until recording lands (8f).
  pk.button(s, { x = record.x, y = record.y, w = record.w, text = "",
                 disabled = true })
  s:fill_round(record.x + 12, record.y + (31 - 9) // 2, 9, 9,
               theme.mix(theme.sunken, 0xffe5484d, 450), 4)
  s:text(record.x + 12 + 9 + 7, record.y + (31 - gfx.height()) // 2,
         record.text, theme.mix(theme.sunken, theme.text_dim, 500), nil, "ui")

  -- The size, as a dropdown.
  s:fill_round(size_box.x, size_box.y, size_box.w, 31, theme.sunken, 7)
  s:frame_round(size_box.x, size_box.y, size_box.w, 31, theme.line_soft, 7)
  s:text(size_box.x + 12, size_box.y + (31 - gfx.height()) // 2,
         size_box.label, theme.text, nil, "ui")
  chevron(s, size_box.x + size_box.w - 11 - 3, size_box.y + 14)

  pk.iconbutton(s, more)
end

local function draw_foot(s)
  local y = PICTURE_Y + PICTURE_H

  s:fill(0, y, W, FOOT, theme.window)
  s:fill(0, y, W, 1, theme.line_soft)

  local ty = y + (FOOT - gfx.height()) // 2
  local x = 18

  if stream and size then
    local a = ("%d frames a second"):format(fps)
    local rate = size.width * size.height * 2 * math.max(fps, 0)
    local from = cameras[chosen] and FOOT_FROM[cameras[chosen].source]
    local b = ("YUY2, %.1f MB a second%s"):format(rate / 1e6, from or "")

    s:text(x, ty, a, theme.text_dim, nil, "ui")
    x = x + gfx.measure(a) + 18
    s:text(x, ty, b, theme.text_dim, nil, "ui")
  end

  if mirror then
    local m = "Mirrored"

    s:text(W - 18 - gfx.measure(m), ty, m, theme.text_dim, nil, "ui")
  end
end

-- The picture, centred in its area: at one to one when it fits, and scaled
-- down - never up, never stretched - when it does not.
local function picture_rect()
  local pw, ph = size.width, size.height
  local room_w, room_h = W - 40, PICTURE_H - 40

  if pw > room_w or ph > room_h then
    local k = math.min(room_w / pw, room_h / ph)

    pw, ph = math.floor(pw * k), math.floor(ph * k)
  end

  return (W - pw) // 2, PICTURE_Y + (PICTURE_H - ph) // 2, pw, ph
end

local function draw_picture(s)
  s:fill(0, PICTURE_Y, W, PICTURE_H, 0xff000000)

  if trouble then
    s:fill(0, PICTURE_Y, W, PICTURE_H, theme.window)
    pk.empty(s, 0, PICTURE_Y, W, PICTURE_H, trouble)
    return
  end

  if not picture then return end

  local x, y, pw, ph = picture_rect()

  if pw == size.width and ph == size.height then
    s:blit(picture, 0, 0, pw, ph, x, y)
  else
    s:stretch(picture, 0, 0, size.width, size.height, x, y, pw, ph, nil, true)
  end
end

local function draw_all()
  local s = win:surface()

  draw_header(s)
  draw_picture(s)
  draw_foot(s)
  return win:commit{ x = 0, y = 0, w = W, h = H }
end

--------------------------------------------------------------------------
-- Menus: the size, and the dots - Mirror, and which camera.
--------------------------------------------------------------------------

local function size_menu()
  local cam = cameras[chosen]

  if not cam then return end

  local items = {}

  for _, s in ipairs(cam.sizes) do
    if s.pixels == "yuy2" then
      local here = size and s.index == size.index

      items[#items + 1] = {
        text = ("%d \u{d7} %d"):format(s.width, s.height),
        mark = here and true or false,
        on_choose = function()
          if not here then open_at(s) draw_all() end
        end,
      }
    end
  end

  win:open_menu(win.origin_x + size_box.x, win.origin_y + L.head, items)
end

local function more_menu()
  local items = {
    { text = "Mirror", mark = mirror,
      on_choose = function() mirror = not mirror draw_all() end },
  }

  if #cameras > 0 then items[#items + 1] = { separator = true } end

  for i, cam in ipairs(cameras) do
    items[#items + 1] = {
      text = cam.name, mark = (i == chosen),
      on_choose = function()
        if i ~= chosen then
          chosen = i
          open_at(camera.pick(cameras[chosen], want_w, want_h))
          draw_all()
        end
      end,
    }
  end

  win:open_menu(win.origin_x + more.x + 26 - 190, win.origin_y + L.head,
                items)
end

--------------------------------------------------------------------------
-- The loop: a frame when there is one, the foot once a second, events.
--------------------------------------------------------------------------

if #cameras > 0 then
  open_at(camera.pick(cameras[chosen], want_w, want_h))
else
  trouble = { "No camera", "Plug in a USB camera. Most webcams are",
              "USB Video Class, which is what Kosmos drives." }
  print("camera: no camera - " .. tostring(why_none))
end

if not draw_all() then return end

while win.running do
  local now = sys.ticks()
  local drew = false

  if stream then
    local got, why = stream:draw(picture, mirror)

    if got then
      --
      -- **The whole window, every frame.** A window that draws its own
      -- pixels has two surfaces and a commit flips them, so drawing only
      -- the picture left the header in one of the two and black in the
      -- other - which is what the first screenshot showed. `win:surface()`
      -- is asked again each time for the same reason.
      --
      counted = counted + 1
      drew = draw_all()
    elseif why == "stopped" then
      -- Ended by the driver: the camera pulled out, or a pause long enough
      -- to lose the lease. Asked for again, once a second.
      stream:close()
      stream = nil

      if now >= retry_at then
        retry_at = now + counter_hz
        cameras = camera.all()
        chosen = math.min(chosen, math.max(#cameras, 1))

        if cameras[chosen] then
          open_at(camera.pick(cameras[chosen], want_w, want_h))
        else
          trouble = { "No camera", "Plug in a USB camera. Most webcams are",
                      "USB Video Class, which is what Kosmos drives." }
        end
      end

      drew = draw_all()
    end
  end

  if now - counted_at >= counter_hz then
    fps = counted * counter_hz // (now - counted_at)
    counted, counted_at = 0, now
    if stream then
      print(("camera: %d frames a second, %d in all"):format(fps,
                                                           stream.frames))
    end

    if not stream then draw_all() end
  end

  local reply = wmproto.poll(win.handle, 1)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      -- a menu's own
    elseif ev.type == "close" then
      close_stream()
      win:close()
    elseif ev.type == "mouse" and not ev.menu and ev.action == "press" then
      if pk.inside(size_box, ev.x, ev.y) then
        size_menu()
      elseif pk.inside(more, ev.x, ev.y) then
        more_menu()
      end
    elseif ev.type == "rawkey" and ev.down and ev.code == 50 then
      -- M: the mirror, without the menu.
      mirror = not mirror
      print("camera: " .. (mirror and "mirrored" or "as the camera sees it"))
      draw_all()
    end
  end
end

close_stream()
