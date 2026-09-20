-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- `play <film>` - a window with a film in it.
--
-- **This program is the media kit's measure**, and it is meant to be read
-- rather than used: what is below the line is this application's own
-- business - where the window goes, what the keys do, when to stop. Not one
-- line of it is about MP4 boxes, sample tables, JPEG, or what a frame
-- costs. That is the whole argument for a kit (`CLAUDE.md`), and Diego's
-- for this one on 20 September: the kits are what "will make making new
-- apps super enjoyable", "so the user that creates app don't reinvent the
-- wheel every time".
--
-- The Video app (`docs/video.html`) is this with chrome around it, and so
-- is anybody else's.
--
local ui = use("/lib/ui.lua")
local media = use("/lib/media.lua")
local wmproto = use("/lib/wmproto.lua")

--
-- The argument a program is started with is a string in `args`, which is
-- what `run` hands it (`init.lua`), and not `...`.
--
-- **`--debug` is what turns the overlay on**, and Diego asked for it that
-- way round on 20 September: "make it optional that is to be shown only if
-- when loaded and played there is a parameter passed to the player that
-- show the debug info overlay button". So a film played to be watched has
-- nothing drawn over it and says nothing to the console; a film played to
-- be *measured* has both, and the badge is the only sign of it.
--
local path, debugging = nil, false

for word in (args or ""):gmatch("%S+") do
  if word == "--debug" or word == "-d" then
    debugging = true
  elseif not path then
    path = word
  end
end

if not path then
  print("play [--debug] <film>")
  print("  for instance: play --debug /home/magicword-mjpeg.mp4")
  return
end

--
-- **`debuginfo` is the whole of it.** The badge, the overlay, the frames a
-- second it is really managing and what each one costs are the kit's, not
-- this program's - so an application asks for them with one word and this
-- file has none of it in it.
--
local film, why = media.open(path, { debuginfo = debugging })

if not film then
  print("play: " .. tostring(why))
  return
end

print(("play: %s, %dx%d, %d frames, %.2f s, %.1f a second")
      :format(film.codec, film.width, film.height, film.frames,
              film.duration, film.fps))

--------------------------------------------------------------------------

local win = ui.window { title = "play", w = film.width, h = film.height,
                        direct = true }

if not win or not win:surface() then print("play: no window") return end

-- The counter, and its rate read three lines from the sum that uses it,
-- which is the rule this system has about clocks (`CLAUDE.md`).
local hz = (fs.read("/dev/cpu") or {}).counter_hz or 1
local began = sys.ticks()
local showing = nil

while win.running do
  local when = (sys.ticks() - began) / hz

  if when >= film.duration then began, when = sys.ticks(), 0 end

  local frame = film:index_at(when)
  local drew = false

  -- Only when the picture would change: decoding the frame that is already
  -- on screen is the whole cost of a film for none of the benefit.
  if frame ~= showing then
    showing = frame
    drew = film:draw(win:surface(), when)

    if drew and not win:commit { x = 0, y = 0,
                                 w = film.width, h = film.height } then
      break
    end
  end

  --
  -- What the window manager has to say. A tick of waiting when there was
  -- nothing to draw, none at all when there was: a loop that never waits is
  -- a process at a hundred per cent between frames, and this one has 33
  -- milliseconds to spare out of every 33.
  --
  local reply = wmproto.poll(win.handle, drew and 0 or 1)

  if not reply then break end

  for _, ev in ipairs(reply.events or {}) do
    if win:direct_event(ev) then
      -- The window manager's own chrome answered it.
    elseif ev.type == "close" then
      win:close()
    elseif ev.type == "mouse" and not ev.menu and ev.action == "press" then
      -- The kit's badge, if the click was on it; otherwise this film has
      -- nothing to say about where it was pressed.
      film:pointer(ev.x, ev.y)
    end
  end

end

film:close()
