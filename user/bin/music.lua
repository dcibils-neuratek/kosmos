-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Music: pick a file, press play.
-- kosmos: application
-- kosmos: icon App_MediaPlayer
--
--   wm music
--
-- The plain one. A list of what is in `/home`, a play button, a bar that
-- says where you are, and a meter that says it is really coming out. The
-- VOX-shaped one drawn in `docs/music.html` comes after this, and the playing
-- itself is `/lib/media.lua`'s now, so it borrows all of it; what this exists
-- to settle is that an *application* can feed the audio server
-- without going deaf or going still, which is a different problem from
-- what `play` solves at a prompt.
--
-- **The difference is that this may never block.** `play` has nothing else
-- to do, so it hands over a period and waits for the next slot. A window
-- has to answer the pointer while it does that, so it uses the other half
-- of the same interface: `stream:play` reports "full" and returns, and the
-- feeding happens a little at a time on the window's own tick. That is why
-- `audio.lua` has both `play` and `write` - not two ways to do one thing,
-- but the two shapes a caller can be.

local ui    = use("/lib/ui.lua")
local audio = use("/lib/audio.lua")
local media = use("/lib/media.lua")

local theme = ui.theme

--
-- Where to look, and what to start on.
--
--   wm music                      everything in /home
--   wm music:/home/Music          everything in there
--   wm music:/home/groove.mp3     that folder, with that track picked
--
-- A file rather than a folder is what Tracker sends when somebody opens an
-- MP3, and without this it arrived, was ignored, and the window opened on
-- `/home` showing no sign that anything had been asked for.
--
local FOLDER = "/home"
local START                     -- the track named on the command line

do
  local given = tostring(args or ""):match("^%s*(%S+)")

  if given and given:sub(1, 1) == "/" then
    local attrs = fs.getattr(given)

    if attrs and attrs.kind == "directory" then
      FOLDER = given
    else
      FOLDER = given:match("^(.*)/") or "/home"
      START = given:match("([^/]+)$")
    end
  end
end

local W, H   = 420, 300

local win, err = ui.window{ title = "Music", w = W, h = H, x = 200, y = 130 }

if not win then
  print("music: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- What is playing. The playing itself is `/lib/media.lua`'s - reading,
-- decoding, feeding, the clock and seeking - and this window chooses what,
-- shows where, and asks.
--------------------------------------------------------------------------

local player                    -- the media player, or nil
local status = "nothing loaded"

local function unload()
  if player then player:close() end

  player = nil
end

local function load(file)
  unload()

  local p, why = media.open(FOLDER .. "/" .. file)

  if not p then status = tostring(why) return end

  local info = p.info

  status = ("%d Hz %s %d-bit"):format(info.rate,
            info.channels == 2 and "stereo" or "mono", info.bits)

  if info.format == "MP3" then
    status = status .. (" MP3 %d kbps%s"):format(info.bitrate or 0,
                                               info.vbr and " VBR" or "")
  end

  player = p
  player:play()
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local files = {}
local listed, list_why = fs.list(FOLDER)

for _, f in ipairs(listed or {}) do
  local low = tostring(f):lower()

  if low:match("%.wav$") or low:match("%.mp3$") then
    files[#files + 1] = f
  end
end

--
-- **Why there is nothing, said rather than hidden.** On the ThinkPad, opened
-- from Tracker beside a Tracker window listing the MP3 in `/home`, this window
-- said "(nothing to play in /home)" and nothing else - and under QEMU the same
-- stick, the same launch and the same listing found the song every time. A
-- folder that could not be listed and one with no music in it looked the
-- same, so the window says which, and the log keeps it for `diagnose`.
--
if not listed then
  files = { ("(could not list %s: %s)"):format(FOLDER, tostring(list_why)) }
  print(("music: could not list %s: %s"):format(FOLDER, tostring(list_why)))
elseif #files == 0 then
  files = { "(nothing to play in " .. FOLDER .. ")" }
  print(("music: nothing to play among %d names in %s"):format(#listed, FOLDER))
end

local list = ui.list{ x = 10, y = 10, w = W - 20, h = 150, items = files }

-- On the track that was asked for, if it is here. Selected rather than
-- played: opening a file should show it ready, not start a noise in a
-- window that has not appeared yet.
if START then
  for i, f in ipairs(files) do
    if f == START then list.selected = i break end
  end
end

local transport = ui.view{ x = 10, y = 200, w = W - 20, h = 60 }

local function clock(secs)
  local whole = math.floor(secs)

  return ("%d:%02d"):format(whole // 60, whole % 60)
end

function transport:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  --
  -- Where we are, from what came out of the speaker (`media.lua` says why a
  -- count of what was handed over would run ahead), as a fraction of the
  -- file's length.
  --
  local secs = player and player:position() or 0
  local total = player and player.info.seconds or 0
  local frac = (total > 0) and math.min(1.0, secs / total) or 0

  g:sunken(0, 0, self.w, 12, "sunken")

  if frac > 0 then
    g:fill(2, 2, math.floor((self.w - 4) * frac), 8, theme.accent)
  end

  g:text(0, 12 + gfx.font.h + 4,
         ("%s / %s"):format(clock(secs), clock(total)), theme.text)

  --
  -- Far enough right that the clock cannot run into it. "0:00 / 0:00" is
  -- eleven characters and the first version put this at ninety pixels,
  -- which is where the eleventh character ends - so it read
  -- "0:00 / 0:00press Play" and looked like one broken string.
  --
  g:text(150, 12 + gfx.font.h + 4, status,
         player and theme.text or theme.dim)

  --
  -- The meter, straight off the server's own peak - the same number the
  -- Mixer draws, and the honest answer to "is this actually coming out".
  -- A progress bar moves whether or not there is a sound device.
  --
  local mw = self.w - 4
  local lit = math.min(mw, math.floor((player and player:peak() or 0) * mw))

  g:fill(2, self.h - 8, mw, 5, theme.sunken)

  if lit > 0 then
    g:fill(2, self.h - 8, lit, 5,
           lit > (mw * 4) // 5 and theme.bad or theme.good)
  end
end

--
-- A click on the bar is a place to go: that far across it, of the file's
-- length. Only the bar - the top twelve pixels; the clock and the meter below
-- it are for looking at. The coordinates are the view's own.
--
function transport:on_click(x, y)
  if not player or y >= 12 then return end

  local frac = math.max(0, math.min(1, (x - 2) / (self.w - 4)))

  player:seek(frac * player.info.seconds)
  win:paint()
end

local play_btn = ui.button{ x = 10, y = 168, text = "Play" }
local stop_btn = ui.button{ x = 90, y = 168, text = "Stop" }

--
-- Awake often while it is playing, and lazy the rest of the time.
--
-- **Two numbers, and they are not the same number.** `poll_wait_ticks` is how
-- long the window is willing to *wait* for an event; `tick_every` is how
-- often `tick` is allowed to *fire*. Setting only the first was the whole
-- of a bug worth writing down: the loop woke every four milliseconds and
-- called `tick` once a second, because `tick_every` defaults to a second
-- and nothing had said otherwise. The feed loop hands over at most twelve
-- periods a turn, so the machine played exactly twelve periods a second -
-- seven-tenths of a second of music in every twelve, and audibly so.
--
-- `poll_wait_ticks` is in scheduler ticks (4 ms each now); `tick_every` is in
-- counter ticks, which is the other clock and six hundred thousand times
-- finer. Two clocks and two units, which is why they were confused.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

local function pace()
  if player then
    win.poll_wait_ticks = 1                     -- 4 ms, one scheduler tick
    win.tick_every = counter_hz // 250    -- 4 ms, in the counter's units
  else
    win.poll_wait_ticks = nil                   -- back to the lazy default
    win.tick_every = counter_hz
  end
end

function play_btn:on_click()
  local pick = list.items[list.selected]

  if not pick or pick:sub(1, 1) == "(" then return end

  if player and player.name == pick then return end   -- already on it

  load(pick)
  pace()
  win:paint()
end

function stop_btn:on_click()
  unload()
  status = "stopped"
  pace()
  win:paint()
end

function list:on_select(item)
  if item and item:sub(1, 1) ~= "(" then
    status = "press Play"
    win:paint()
  end
end

local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

--
-- Feeding and painting run at different rates, and conflating them cost
-- half the playback speed.
--
-- The tick has to be fast because the audio server's backlog is four
-- periods - 23 ms - and a feed that arrives later than that is a gap. The
-- *picture* has no such deadline: a progress bar and a meter are for a
-- person to look at, and twenty-five times a second is more than a person
-- can see. Painting on every tick meant a whole window redrawn 250 times a
-- second, plus an `audio.streams` round trip inside the draw to read the
-- peak - and the process spent so long doing it that it fed the server at
-- half the rate the device drained it.
--
local PAINT_EVERY = 10          -- ticks; 4 ms each, so about 25 Hz
local since_paint = 0

function ticker:tick()
  if not player then return end

  player:tick()

  if player.error then
    status = player.error
    unload()
    pace()
    win:paint()
    return
  end

  since_paint = since_paint + 1

  if player:finished() then
    unload()
    status = "finished"
    pace()
    win:paint()
    return
  end

  if since_paint >= PAINT_EVERY then
    since_paint = 0

    win:paint()
  end
end

win:add(list)
win:add(play_btn)
win:add(stop_btn)
win:add(transport)
win:add(ticker)

if audio.format().period == 0 then status = "this machine has no sound device" end

win:run()
