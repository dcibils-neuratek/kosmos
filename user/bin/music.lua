-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Music: pick a file, press play.
-- kosmos: application
--
--   wm music
--
-- The plain one. A list of what is in `/home`, a play button, a bar that
-- says where you are, and a meter that says it is really coming out. The
-- Winamp-shaped one comes after this and can borrow all of it; what this
-- exists to settle is that an *application* can feed the audio server
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
local wav   = use("/lib/wav.lua")

local theme = ui.theme

local FOLDER = "/home"
local W, H   = 420, 300

local fmt = audio.format()

local win, err = ui.window{ title = "Music", w = W, h = H, x = 200, y = 130 }

if not win then
  print("music: " .. tostring(err))
  return
end

--------------------------------------------------------------------------
-- What is playing, and how far in.
--------------------------------------------------------------------------

local stream                    -- the open audio stream, or nil
local info                      -- what `wav.scan` said about the file
local name                      -- what is loaded, for the label
local window_page               -- a page to read the file through
local at, last                  -- where we are in the file, and where it ends
local carry, pending, phase     -- bytes read but not converted, and converted
local played                    -- bytes of source handed over, for the bar
local status = "nothing loaded"


--
-- How much of the file to fetch at a time.
--
-- This was one page, on the reasoning that a window has a tick to keep and
-- a big read inside one is a stutter. That reasoning was wrong in an
-- interesting way, and the instrument said so: a page is a tenth of a
-- second of audio, so it meant forty-odd round trips to the disk every
-- second, and each tick that had to do one took long enough that the tick
-- rate collapsed from 250 a second to ten. Twelve periods a tick times ten
-- ticks is 120 periods a second against the 172 the device drains, and the
-- music played at two thirds speed.
--
-- Bigger and rarer wins: 32 KB is about 190 ms of audio and is fetched
-- roughly five times a second, so most ticks touch no disk at all.
--
local READ = 32768

local function unload()
  if stream then stream:close() end

  stream, info, window_page = nil, nil, nil
  carry, pending, phase = "", "", 0.0
  at, last, played = 0, 0, 0
end

unload()

local function load(file)
  unload()

  local path = FOLDER .. "/" .. file
  -- Sized to the read, which is not a detail: `fs.read_into` writes what it
  -- is asked for, and asking for 32 KB into one page is a buffer overrun
  -- with the length written three lines away from the allocation.
  local page = sys.memory(READ // 4096)

  if not page then status = "no memory for a read buffer" return end

  local got, why = wav.scan(function(off, n)
    local n2 = fs.read_into(path, page, off, n)

    if not n2 or n2 == 0 then return nil end

    return sys.region_read(page, 0, n2)
  end)

  if not got then status = tostring(why) return end

  local s
  s, why = audio.open(file)

  if not s then status = tostring(why) return end

  info, stream, window_page, name = got, s, page, file
  at, last, played = got.offset, got.offset + got.bytes, 0
  status = ("%d Hz %s %d-bit"):format(got.rate,
            got.channels == 2 and "stereo" or "mono", got.bits)
end

local function finished()
  return info ~= nil and at >= last and #pending == 0 and #carry < info.frame * 2
end

--
-- Hand over as much as the server will take, and not one period more.
--
-- The loop has a ceiling rather than running until "full" alone, because
-- the server answering "full" is what *should* stop it and a bug that made
-- it always answer "taken" would otherwise be an application that reads a
-- whole file inside one tick and stops painting. A ceiling turns that into
-- audio that runs ahead, which is visible and recoverable.
--
--
-- How many periods one turn may hand over.
--
-- A ceiling rather than "until full", so that a server which wrongly always
-- answered "taken" would make the audio run ahead - visible and
-- recoverable - instead of reading the whole file inside one tick and
-- taking the window still with it.
--
-- Sixteen, against the 172 periods a second the device drains and the
-- seventeen turns a second this loop actually gets: 272 of headroom over
-- 172 needed, which is margin rather than a fit.
--
local FEED_MAX = 16

local function feed()
  if not stream or not info then return 0 end

  local fed = 0

  for _ = 1, FEED_MAX do
    if #pending == 0 then
      if at < last and #carry < READ then
        local n = fs.read_into(FOLDER .. "/" .. name, window_page, at,
                               math.min(READ, last - at))

        if not n or n == 0 then at = last break end

        carry = carry .. sys.region_read(window_page, 0, n)
        at = at + n
      end

      if #carry < info.frame * 2 then break end

      local pcm, used
      pcm, used, phase = sys.pcm(carry, info.rate, info.channels, info.bits,
                                 phase, fmt.period * 4)

      if used == 0 or #pcm == 0 then break end

      played = played + used
      carry = carry:sub(used + 1)
      pending = pcm
    end

    local chunk = pending:sub(1, fmt.period)
    local took, why = stream:play(chunk)

    if not took then
      if why ~= "full" then status = tostring(why) unload() end

      return fed                -- the server is ahead; come back next tick
    end

    pending = pending:sub(fmt.period + 1)
    fed = fed + 1
  end

  return fed
end

--------------------------------------------------------------------------
-- The window.
--------------------------------------------------------------------------

local files = {}

for _, f in ipairs(fs.list(FOLDER) or {}) do
  if f:lower():match("%.wav$") then files[#files + 1] = f end
end

if #files == 0 then files = { "(no .wav files in " .. FOLDER .. ")" } end

local list = ui.list{ x = 10, y = 10, w = W - 20, h = 150, items = files }

local transport = ui.view{ x = 10, y = 200, w = W - 20, h = 60 }

function transport:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  --
  -- Where we are, as a fraction of the samples rather than of the file:
  -- a file with four kilobytes of padding in front of it would otherwise
  -- start the bar a little way along, which looks like a bug and is one.
  --
  local frac = 0

  if info and info.bytes > 0 then
    frac = math.min(1.0, played / info.bytes)
  end

  g:sunken(0, 0, self.w, 12, "sunken")

  if frac > 0 then
    g:fill(2, 2, math.floor((self.w - 4) * frac), 8, theme.accent)
  end

  local secs = info and (info.seconds * frac) or 0
  local total = info and info.seconds or 0

  g:text(0, 12 + gfx.font.h + 4,
         ("%d:%02d / %d:%02d"):format(secs // 60, math.floor(secs) % 60,
                                      total // 60, math.floor(total) % 60),
         theme.text)

  --
  -- Far enough right that the clock cannot run into it. "0:00 / 0:00" is
  -- eleven characters and the first version put this at ninety pixels,
  -- which is where the eleventh character ends - so it read
  -- "0:00 / 0:00press Play" and looked like one broken string.
  --
  g:text(150, 12 + gfx.font.h + 4, status,
         stream and theme.text or theme.dim)

  --
  -- The meter, straight off the server's own peak - the same number the
  -- Mixer draws, and the honest answer to "is this actually coming out".
  -- A progress bar moves whether or not there is a sound device.
  --
  local peak = 0

  for _, one in ipairs(audio.streams() or {}) do
    if stream and one.stream == stream.id then peak = one.peak or 0 end
  end

  local mw = self.w - 4
  local lit = math.min(mw, (peak * mw) // 32767)

  g:fill(2, self.h - 8, mw, 5, theme.sunken)

  if lit > 0 then
    g:fill(2, self.h - 8, lit, 5,
           lit > (mw * 4) // 5 and theme.bad or theme.good)
  end
end

local play_btn = ui.button{ x = 10, y = 168, text = "Play" }
local stop_btn = ui.button{ x = 90, y = 168, text = "Stop" }

--
-- Awake often while it is playing, and lazy the rest of the time.
--
-- **Two numbers, and they are not the same number.** `poll_wait` is how
-- long the window is willing to *wait* for an event; `tick_every` is how
-- often `tick` is allowed to *fire*. Setting only the first was the whole
-- of a bug worth writing down: the loop woke every four milliseconds and
-- called `tick` once a second, because `tick_every` defaults to a second
-- and nothing had said otherwise. The feed loop hands over at most twelve
-- periods a turn, so the machine played exactly twelve periods a second -
-- seven-tenths of a second of music in every twelve, and audibly so.
--
-- `poll_wait` is in scheduler ticks (4 ms each now); `tick_every` is in
-- counter ticks, which is the other clock and six hundred thousand times
-- finer. Two clocks and two units, which is why they were confused.
--
local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

local function pace()
  if stream then
    win.poll_wait = 1                     -- 4 ms, one scheduler tick
    win.tick_every = counter_hz // 250    -- 4 ms, in the counter's units
  else
    win.poll_wait = nil                   -- back to the lazy default
    win.tick_every = counter_hz
  end
end

function play_btn:on_click()
  local pick = list.items[list.selected]

  if not pick or pick:sub(1, 1) == "(" then return end

  if stream and name == pick then return end        -- already on it

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
  if not stream then return end

  feed()


  since_paint = since_paint + 1

  if finished() then
    --
    -- The samples are all handed over, which is not the same as played:
    -- the server still holds up to four periods. Letting go here would cut
    -- the last twenty milliseconds off every file.
    --
    if stream:queued() == 0 then
      unload()
      status = "finished"
      pace()
      win:paint()
      return
    end
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

if fmt.period == 0 then status = "this machine has no sound device" end

win:run()
