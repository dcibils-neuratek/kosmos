-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_MediaPlayer
-- kosmos: section applications
--
-- Video: a window that plays a film.
--
--   wm video:/home/magicword-mjpeg.mp4
--   video /home/magicword-mjpeg.mp4          (from a Terminal)
--
-- Drawn before it was written (`docs/video.html`, `roadmap.md` 4e) and
-- built to that drawing: the picture at its own size with **the controls
-- under it rather than over it**, so the film is never covered and nothing
-- has to fade; the window manager's menu bar above it, as the Super
-- Nintendo has; and the window's tab saying the film's name.
--
-- **Nothing in this file knows what an MP4 is.** It opens a film through
-- `media.open` and asks it for the frame at a moment; which decoder is
-- behind that is the kit's business and not this program's (`CLAUDE.md`,
-- on kits). When H.264 arrives, this file does not change.
--
-- **The sound plays** (`roadmap.md` 4e): AAC and MP3, and the picture
-- follows what is heard - the kit keeps the time. The volume items in the
-- Play menu work; where a film cannot be heard they stay, greyed, saying
-- why, the way the Drives app shows `Format...`: an item that is missing
-- teaches nothing, and one that refuses says what it would do.
--
local ui = use("/lib/ui.lua")
local media = use("/lib/media.lua")
local panel = use("/lib/panel.lua")
local wmproto = use("/lib/wmproto.lua")
local theme = ui.theme

--------------------------------------------------------------------------
-- What this was asked for.
--
-- `--size` and `--at` exist because a window that draws its own pixels
-- cannot be resized - its buffers are a region this process allocated, and
-- the compositor cannot make them bigger (`wm.lua`). So View starts the
-- program again at the new size, which is what the Super Nintendo does for
-- Double Size, and `--at` carries the moment across so the film picks up
-- where it was rather than starting over.
--------------------------------------------------------------------------

local path, want_size, start_at, debugging = nil, "1", 0, false
local words = {}

for word in (args or ""):gmatch("%S+") do words[#words + 1] = word end

local i = 1

while i <= #words do
  local word = words[i]

  if word == "--size" then
    i = i + 1
    want_size = words[i] or "1"
  elseif word == "--at" then
    i = i + 1
    start_at = tonumber(words[i]) or 0
  elseif word == "--debug" or word == "-d" then
    debugging = true
  elseif not path then
    path = word
  end

  i = i + 1
end

local FILMS = { mp4 = true, m4v = true, mov = true }

local function is_film(name)
  local kind = tostring(name):lower():match("%.(%w+)$")

  return kind ~= nil and FILMS[kind] == true
end

--------------------------------------------------------------------------
-- A window with something to say, for the two cases that are not a film.
--
-- `docs/video.html`: "Never a black rectangle with nothing to say." One is
-- opening the program with no film, and the other is a film this system
-- cannot decode - and the second must say *what it is* and what would
-- play, because "cannot play" on its own is a dead end for somebody who
-- has a perfectly ordinary file.
--------------------------------------------------------------------------

local function say_instead(lines, title)
  local W, H = 540, 280
  local L = ui.layout
  local win = ui.window{ title = title or "Video", w = W, h = H,
                         x = 220, y = 160 }

  if not win then return end

  --
  -- **As `docs/apps.html` draws it** (`roadmap.md` 5zp): the header with
  -- Open as the one verb, and the sentence centred in what is left - the
  -- first line as the thing that happened, the rest dim under it. It was a
  -- column of labels at 18 with Open at the bottom left and a Quit at the
  -- bottom right, beside a window's own close box.
  --
  local function open_one()
    local chooser = panel.open{
      start = "/home", title = "Open a film", filter = is_film,
      on_choose = function(chosen)
        fs.send("/app/wm", { type = "launch", program = "video",
                             args = chosen })
        win:close()
      end,
    }

    if chooser then chooser:run() end
  end

  win:add(ui.header{
    x = 0, y = 0, w = W, title = "Video",
    -- The film's name, when there is one: the tab already says "Video".
    sub = title and title:gsub("^Video:%s*", "") or "nothing open",
    right = { ui.button{ text = "Open a film...", go = true,
                         on_click = open_one } },
  })

  -- Blank lines were spacing in the old column; the block centres itself.
  local shown = {}

  for _, line in ipairs(lines) do
    if line ~= "" then shown[#shown + 1] = line:match("^%s*(.-)%s*$") end
  end

  local step = gfx.height("text") + 6
  local y = L.head + (H - L.head - #shown * step) // 2

  for i, line in ipairs(shown) do
    local face = (i == 1) and "label" or "text"
    local w = gfx.measure(line, face)

    win:add(ui.label{ x = (W - w) // 2, y = y, w = w + 2, text = line,
                      role = face, color = (i == 1) and "text" or "text_dim" })
    y = y + step
  end

  win:run()
end

if not path then
  say_instead({
    "Nothing open",
    "Open a film, or start this with one:",
    "video /home/magicword-clip.mp4",
  })
  return
end

local film, why = media.open(path, { debuginfo = debugging })

if not film then
  --
  -- The kit's own sentence, which names the codec when that is the trouble
  -- - "this film is H.264, and this system has no H.264 decoder yet" - and
  -- what this system does play beside it.
  --
  say_instead({
    tostring(why) .. ".",
    "",
    "This machine plays Motion JPEG pictures and MP3 sound.",
    "H.264 and AAC are being ported (roadmap 4e).",
  }, "Video: " .. (path:match("([^/]+)$") or path))
  return
end

--------------------------------------------------------------------------
-- The window: a picture, and the controls under it.
--------------------------------------------------------------------------

local name = path:match("([^/]+)$") or path
local screen_w, screen_h = gfx.screen():size()

--
-- Three sizes, as drawn: its own, twice it, and as large as the screen
-- will take with its shape kept. `Fit` is the kit's `fit`, so the
-- arithmetic that keeps a face from being squashed lives in one place and
-- every application that scales a film gets it.
--
--
-- **And full screen, which Diego asked for on 20 September** - "the video
-- player needs a switch to fullscreen mode, as most video players do".
--
-- The window *is* the screen: no tab, no menu bar, no controls, the film
-- centred on black with its shape kept. The flag is the window manager's
-- and every application that draws its own pixels can ask for it, which is
-- why it is not a thing this file invented for itself.
--
-- Escape comes back, as it does in every player - and "comes back" is
-- another start of this program, because the buffers of a window that
-- draws its own pixels are the size they were made.
--
local full = (want_size == "full")
local scale_w, scale_h = film.width, film.height

if want_size == "2" then
  scale_w, scale_h = film.width * 2, film.height * 2
elseif want_size == "fit" then
  local _, _, fw, fh = film:fit(screen_w - 40, screen_h - 160)

  scale_w, scale_h = fw, fh
elseif full then
  local _, _, fw, fh = film:fit(screen_w, screen_h)

  scale_w, scale_h = fw, fh
end

-- The controls sit under the picture, and full screen has none: there is
-- nowhere under the picture to put them that is not over it.
local BAR_H = full and 0 or (gfx.height() * 2 + 26)
local W, H = scale_w, scale_h + BAR_H

if full then W, H = screen_w, screen_h end

--
-- **The film keeps the time**, and it is the sound's: frames heard, or the
-- counter when there is no sound to hear (`/lib/video.lua`). This file
-- kept its own clock once, from the counter, and it had a fault the kit's
-- does not: pausing showed the frame of the last seek rather than the one
-- on screen, because the moment it paused at was never written down.
--
film:play(start_at)

local function now_at()
  local when = film:position()

  -- At the end it goes round again, as it always has.
  if when >= film.duration then
    film:seek(0)
    return 0
  end

  return when
end

local function seek_to(when)
  film:seek(when)
end

local function play_or_pause()
  if film:playing() then film:pause() else film:play() end
end

local function clock(seconds)
  local whole = math.floor(seconds)

  return ("%d:%02d"):format(whole // 60, whole % 60)
end

--
-- Starting again at another size, with the moment carried over. The window
-- goes when the new one has been asked for, not before: if the launch is
-- refused there is still a film on screen and a line saying why.
--
local win

local function again(size)
  local at = now_at()
  local reply, sent = fs.send("/app/wm", {
    type = "launch", program = "video",
    args = ("--size %s --at %.2f %s%s"):format(size, at,
            debugging and "--debug " or "", path),
  })

  if reply and reply.ok then
    win:close()
    return
  end

  print("video: could not start again at " .. size .. ": "
        .. tostring(reply and reply.error or sent))
end

local function open_another()
  local chooser = panel.open{
    start = "/home", title = "Open a film", filter = is_film,
    on_choose = function(chosen)
      local reply = fs.send("/app/wm", { type = "launch", program = "video",
                                         args = chosen })

      if reply and reply.ok then win:close() end
    end,
  }

  if chooser then chooser:run() end
end

--
-- **The volume**, a tenth at a time, and mute - which remembers the level
-- it came from. Refused, and saying why, when the film cannot be heard:
-- it has no sound, the machine has no device, or its sound is a codec
-- this system does not play.
--
local audible, silent = film:audible()
local level, muted = 1.0, false

local function set_volume(to)
  level = math.max(0, math.min(1, to))
  muted = false
  film:volume(level)
end

local function toggle_mute()
  muted = not muted
  film:volume(muted and 0 or level)
end

local quiet_why = silent and (" (" .. silent .. ")") or ""

--
-- **Not `full and nil or {...}`.** In Lua that is always the table: `and`
-- gives `nil`, and `nil or x` is `x`. `wm.lua` has a comment about this
-- exact shape and I wrote it anyway - the menu bar was built in full
-- screen, `strips.accept` added its 26 pixels to the window, and the
-- window came back taller than the screen it was supposed to be.
--
local bar = nil

if not full then
  bar = {
    { title = "File", items = {
      { text = "Open...", on_choose = function() open_another() end },
      { separator = true },
      { text = "Quit", on_choose = function() win:close() end },
    } },
    { title = "View", items = {
      { text = "Actual Size", on_choose = function() again("1") end },
      { text = "Double Size", on_choose = function() again("2") end },
      { text = "Fit to the Screen", on_choose = function() again("fit") end },
      { separator = true },
      { text = "Full Screen", on_choose = function() again("full") end },
    } },
    { title = "Play", items = {
      { text = "Play or Pause", on_choose = function() play_or_pause() end },
      { text = "Back 10 s", on_choose = function() seek_to(now_at() - 10) end },
      { text = "On 10 s",   on_choose = function() seek_to(now_at() + 10) end },
      { separator = true },
      { text = "Louder" .. (audible and "" or quiet_why),
        disabled = not audible or nil,
        on_choose = function() set_volume(level + 0.1) end },
      { text = "Quieter" .. (audible and "" or quiet_why),
        disabled = not audible or nil,
        on_choose = function() set_volume(level - 0.1) end },
      { text = "Mute" .. (audible and "" or quiet_why),
        disabled = not audible or nil,
        on_choose = function() toggle_mute() end },
    } },
  }
end

win = ui.window{
  title = name, w = W, h = H, x = 140, y = 90, direct = true,
  fullscreen = full or nil,
  menubar = bar,
}

if not win or not win:surface() then
  print("video: no window")
  return
end

print(("video: %s, %s, %dx%d at %dx%d, %.1f a second, sound %s%s")
      :format(name, film.codec, film.width, film.height, scale_w, scale_h,
              film.fps, film.sound and film.sound.codec or "none",
              audible and ", heard" or (film.sound and (", not heard: "
                                                         .. tostring(silent))
                                                    or "")))

--------------------------------------------------------------------------
-- The controls, under the picture.
--------------------------------------------------------------------------

local MARK = 34                         -- the play/pause button's width

--
-- **A window that draws its own pixels has a surface, not a context.**
--
-- `win:surface()` is a `gfx` surface - `fill`, `text`, `blit`, `stretch` -
-- and not the `gc` the widget kit hands a view, so there is no `frame` and
-- no `raised` on it: those are the kit's, and they are drawn from fills.
-- Four fills is what a frame is anyway, and the alternative is the kit's
-- whole view tree for a strip with three things in it.
--
local function frame(s, x, y, w, h, colour)
  s:fill(x, y, w, 1, colour)
  s:fill(x, y + h - 1, w, 1, colour)
  s:fill(x, y, 1, h, colour)
  s:fill(x + w - 1, y, 1, h, colour)
end

local function bar_rect()
  return MARK + 12, scale_h + 10, W - MARK - 24, gfx.height() + 4
end

local function draw_controls(s)
  local y = scale_h

  s:fill(0, y, W, BAR_H, theme.window)
  s:fill(0, y, W, 1, theme.edge_dark)

  -- Play or pause, drawn rather than lettered: a triangle or two bars.
  local bx, by = 8, y + 8
  local bh = gfx.height() + 8

  s:fill(bx, by, MARK - 4, bh, theme.raised)
  frame(s, bx, by, MARK - 4, bh, theme.line)

  if film:playing() then
    s:fill(bx + 10, by + 4, 4, bh - 8, theme.text)
    s:fill(bx + 18, by + 4, 4, bh - 8, theme.text)
  else
    for i = 0, bh - 9 do
      local half = (bh - 8) // 2
      local wide = half - math.abs(i - half + 1)

      s:fill(bx + 11, by + 4 + i, math.max(1, wide), 1, theme.text)
    end
  end

  -- How far through, as a bar that can be clicked.
  local rx, ry, rw, rh = bar_rect()
  local at = now_at()
  local through = (film.duration > 0) and (at / film.duration) or 0

  s:fill(rx, ry, rw, rh, theme.sunken)
  frame(s, rx, ry, rw, rh, theme.line)
  s:fill(rx + 1, ry + 1, math.floor((rw - 2) * through), rh - 2, theme.accent)

  -- The two numbers the drawing asks for: where it is, and which frame.
  local said = clock(at) .. " of " .. clock(film.duration)
  local frame = ("frame %d of %d"):format(film.shown or 1, film.frames)
  local ty = ry + rh + 5

  s:text(rx, ty, said, theme.text)
  s:text(W - 12 - gfx.measure(frame), ty, frame, theme.text_dim)
end

--------------------------------------------------------------------------
-- Playing.
--------------------------------------------------------------------------

local picture_x, picture_y = 0, 0

if full then
  picture_x = (screen_w - scale_w) // 2
  picture_y = (screen_h - scale_h) // 2
end

--
-- **The black around the picture, on whichever buffer is in hand.**
--
-- Filling it once was wrong and would have shown as a flicker on alternate
-- frames: a window that draws its own pixels has *two* buffers and `commit`
-- swaps them, so anything drawn once is on one of them. The bars are the
-- four strips the film does not cover - usually nothing at all, a 16:9 film
-- on a 16:9 screen scaling exactly - so this costs what it needs to.
--
local function letterbox(s)
  if not full then return end

  if picture_y > 0 then
    s:fill(0, 0, W, picture_y, 0xff000000)
    s:fill(0, picture_y + scale_h, W, H - picture_y - scale_h, 0xff000000)
  end

  if picture_x > 0 then
    s:fill(0, picture_y, picture_x, scale_h, 0xff000000)
    s:fill(picture_x + scale_w, picture_y,
           W - picture_x - scale_w, scale_h, 0xff000000)
  end
end

local showing, controls_at = nil, -1

while win.running do
  -- The sound first: it has a ring to keep full, and the picture waits
  -- for it rather than the other way round.
  film:tick()

  local when = now_at()
  local frame = film:index_at(when)
  local drew = false

  --
  -- The picture when the frame changes, and the controls when the *second*
  -- changes: redrawing a progress bar sixty times a second to move it a
  -- pixel every thirty is work nobody sees.
  --
  if frame ~= showing then
    showing = frame

    -- Centred on black, full screen: a film's shape is almost never the
    -- screen's, and stretching it to fit would be the one thing a player
    -- must not do.
    letterbox(win:surface())
    drew = film:draw(win:surface(), when, picture_x, picture_y,
                     scale_w, scale_h)
  end

  if not full and (drew or math.floor(when) ~= controls_at) then
    controls_at = math.floor(when)
    draw_controls(win:surface())
    drew = true
  end

  if drew and not win:commit{ x = 0, y = 0, w = W, h = H } then break end

  --
  -- A tick's wait when nothing was drawn, and none when something was: the
  -- sound's ring holds 186 ms, so a 4 ms wait costs it nothing, and a loop
  -- that never waits is a process at a hundred per cent between frames.
  --
  local reply = wmproto.poll(win.handle,
                             (film:playing() and not drew) and 1
                             or (film:playing() and 0 or 4))

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      -- A menu answered it.
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "mouse" and not ev.menu and ev.action == "press" then
      local rx, ry, rw, rh = bar_rect()

      if ev.y >= ry - 4 and ev.y <= ry + rh + 4 and ev.x >= rx
         and ev.x <= rx + rw then
        seek_to(film.duration * (ev.x - rx) / rw)
        showing = nil
      elseif not full and ev.y >= scale_h and ev.x < MARK + 8 then
        play_or_pause()
      elseif full or ev.y < scale_h then
        -- A click on the picture pauses, as the drawing says.
        if not film:pointer(ev.x, ev.y, picture_x, picture_y,
                            scale_w, scale_h) then
          play_or_pause()
        end
      end

      controls_at = -1
    elseif ev.type == "rawkey" and ev.down then
      local c = ev.code

      if c == 57 then                             -- space
        play_or_pause()
      elseif c == 105 then seek_to(now_at() - 10) -- left
      elseif c == 106 then seek_to(now_at() + 10) -- right
      elseif c == 2 then again("1")               -- 1
      elseif c == 3 then again("2")               -- 2
      elseif c == 33 then again("fit")            -- f
      elseif c == 87 or c == 88 then again("full")  -- F11, F12
      elseif c == 1 then                            -- escape
        if full then again("1") else win:close() end
      end

      showing, controls_at = nil, -1
    end
  end

end

film:close()
