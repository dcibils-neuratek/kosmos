-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Mixer: a fader for every program making a noise.
-- kosmos: application
-- kosmos: icon Misc_Speaker
-- kosmos: name Sound Mixer
-- kosmos: section preferences
--
--   wm mixer
--
-- One row per open stream, plus a master at the top. Drag a fader; the
-- switch at a stream's end is on while it is heard.
--
-- **The meter is the interesting part and it costs nothing.** The audio
-- server's mix already touches every sample to sum them, so the loudest one it saw is
-- free on the way past - and it is measured *before* the gain, which is
-- what makes it answer "who is sending audio" rather than "how loud is it".
-- A muted stream still shows a moving meter, which is the whole point when
-- you are looking for the program that will not shut up.
--
-- Nothing here plays anything. It asks `/Devices/audio` what exists and tells
-- it what to change, which is the same relationship the Deskbar has with
-- the window manager: the authority is in the server and this is a view of
-- it.

local ui    = use("/Kosmos/Libraries/ui.lua")
local audio = use("/Kosmos/Libraries/audio.lua")

local theme = ui.theme

local W, H = 460, 540

local win, err = ui.window{ title = "Mixer", w = W, h = H, x = 150, y = 110,
                            header = true }

if not win then
  print("mixer: " .. tostring(err))
  return
end

local L = ui.layout
local fmt = audio.format()

local master = 256
local master_muted = false
local rows = {}                 -- what the server last said
local mixes = 0                 -- periods the server has mixed, ever
local devices = {}              -- every output, as `audio.devices()` says
local chosen = ""               -- the name chosen; "" for the newest

--------------------------------------------------------------------------
-- The window, as `docs/apps.html` draws it (`roadmap.md` 5zp).
--
-- **A header and a page of cards**: the rate beside the title and Test
-- tone as the verb, the master in a card of its own, and a row per stream
-- in the card under it. It was a list drawn by hand from x = 6 with a
-- sunken track, a raised knob and a box for mute - BeOS's fader, in a
-- window whose every neighbour had moved to the drawings' rail and knob.
--
-- **The meter stays, and goes under the rail.** It is the part of this
-- window worth having (the header of this file says why), and the drawing
-- has no meter only because a drawing has nothing playing.
--------------------------------------------------------------------------

local VALUE_W = 40              -- "100%", right-aligned so digits hold still
local SWITCH_W = 40             -- the kit's switch pill
local KNOB = 16                 -- the slider's knob, as `ui.slider` draws it

--
-- What a strip shows, looked up by its key on every paint: the master's
-- gain, or a stream's row as the server last described it. So a tick that
-- brings new numbers repaints the strips without rebuilding them - which a
-- drag in progress would not survive.
--
local function state(key)
  if key == "master" then
    return { gain = master, muted = master_muted }
  end

  for _, r in ipairs(rows) do
    if r.stream == key then return r end
  end

  return nil
end

local function set_gain(key, g)
  g = math.min(math.max(g, 0), 256)

  if key == "master" then
    audio.set{ master = g }
    master = g
  else
    local r = state(key)

    audio.set{ stream = key, gain = g }
    if r then r.gain = g end
  end
end

local function set_muted(key, muted)
  local r = state(key)

  if key == "master" then
    audio.set{ master_muted = muted }
    master_muted = muted
  elseif r then
    audio.set{ stream = key, muted = muted }
    r.muted = muted
  end
end

--
-- **A strip**: the fader, the level beside it, and a switch that is on
-- while it is heard - one control, so a card's row can hold it with its
-- name on the left. The rail and knob are `ui.slider`'s, drawn here rather
-- than composed because the meter lives inside the fader's box and a
-- switch that means "heard" belongs to the same row's state.
--
local function strip(key)
  local v = ui.view{ h = 31 }

  v.fill = true
  v.focusable = true

  local function fader_w(self)
    return self.w - L.row_in - VALUE_W - L.row_in - SWITCH_W
  end

  local function gain_at(self, x)
    local span = math.max(1, fader_w(self) - KNOB)

    return ((x - KNOB // 2) * 256) // span
  end

  function v:draw(g)
    local r = state(key)

    if not r then return end

    local fw = fader_w(self)
    local span = fw - KNOB
    local ry = self.h // 2 - 2
    local at = KNOB // 2 + span * (r.gain or 0) // 256

    g:fill_round(KNOB // 2, ry, span, 4, theme.track, 2)
    g:fill_round(KNOB // 2, ry, at - KNOB // 2, 4,
                 r.muted and theme.text_dim or theme.accent, 2)

    --
    -- The meter: the loudest sample of the last period, before the gain,
    -- as a line three high under the rail - green, and red in its last
    -- fifth, the colours every level meter uses. Nothing is drawn at
    -- silence, so a quiet row is a plain fader.
    --
    if r.peak and r.peak > 0 then
      local lit = math.min(span, (r.peak * span) // 32767)

      g:fill_round(KNOB // 2, self.h - 4, lit, 3,
                   lit > (span * 4) // 5 and theme.bad or theme.good, 1)
    end

    local ky = (self.h - KNOB) // 2

    g:fill_round(at - KNOB // 2, ky + 1, KNOB, KNOB, 0x30000000, KNOB // 2)
    g:fill_round(at - KNOB // 2, ky, KNOB, KNOB, 0xffffffff, KNOB // 2)
    g:frame_round(at - KNOB // 2, ky, KNOB, KNOB,
                  self.focused and self.keyed and theme.ring
                  or theme.line_soft, KNOB // 2)

    local pct = ("%d%%"):format(((r.gain or 0) * 100) // 256)

    g:text(fw + L.row_in + VALUE_W - gfx.measure(pct),
           (self.h - gfx.height()) // 2, pct, theme.text_dim, nil, "ui")

    do
      local sx = self.w - SWITCH_W
      local sy = (self.h - 23) // 2
      local heard = not r.muted
      local kx = heard and (SWITCH_W - 18 - 3) or 3

      g:fill_round(sx, sy, SWITCH_W, 23, heard and theme.accent or theme.track,
                   11)
      g:fill_round(sx + kx, sy + 3, 18, 18, 0x38000000, 9)
      g:fill_round(sx + kx, sy + 2, 18, 18, 0xffffffff, 9)
    end
  end

  function v:key(c)
    local r = state(key)

    if not r then return false end

    if c == -3 or c == -1 then set_gain(key, r.gain + 13) return true end
    if c == -4 or c == -2 then set_gain(key, r.gain - 13) return true end

    if c == 32 or c == 10 or c == 13 then
      set_muted(key, not r.muted)
      return true
    end

    return false
  end

  -- The switch is a button: it turns on the release over it (`ui.click`;
  -- Diego, 9 October: a click is a press and a release, "in all kosmos"),
  -- from the state as it is then. The fader is a drag and starts on the
  -- press.
  local function switch_at(x, y)
    if x < v.w - SWITCH_W or x >= v.w or y < 0 or y >= v.h then return nil end

    return "mute", function()
      local now = state(key)

      if now then set_muted(key, not now.muted) end
    end
  end

  function v:mouse(action, x, y)
    local r = state(key)

    if not r then return true end

    if action == "press" then
      self.dragging = x < fader_w(self)
    end

    if not self.dragging then
      ui.click(self, action, x, y, switch_at)
    end

    if self.dragging and (action == "press" or action == "move") then
      set_gain(key, gain_at(self, x))
    end

    if action == "release" then self.dragging = false end

    return true
  end

  return v
end

--------------------------------------------------------------------------
-- **Output: which device plays, and at what** (`docs/usbaudio.md` U5, as
-- `docs/usbaudio.html` draws it). A dropdown of every output, the one
-- playing shown; choosing one moves every program's sound to it at once,
-- and is kept (`audio.choose`). Under it three tiles - the sample size,
-- the rate and the channels the device runs at, with what else it offers
-- beneath each - and, when the device's rate is not the mix's, a line
-- saying the sound is converted there. A format is chosen at U7; here the
-- tiles say what is.
--------------------------------------------------------------------------

local function playing_device()
  for _, d in ipairs(devices) do
    if d.playing then return d end
  end

  return devices[1]
end

local function khz(rate)
  local s = ("%.1f"):format(rate / 1000):gsub("%.0$", "")

  return s .. " kHz"
end

local function facts(d)
  if d.kind == "machine" then return "This machine" end

  return ("USB · %04x:%04x"):format(d.vendor or 0, d.product or 0)
end

local function also(list, now, say)
  local out = {}

  for _, x in ipairs(list) do
    if x ~= now then out[#out + 1] = say(x) end
  end

  return #out > 0 and ("also " .. table.concat(out, ", ")) or "only"
end

local TILE_GAP = 8

-- A tile is as tall as its three lines in the desktop's faces, with 6
-- above and below: it was 54 pixels, which held them at QEMU's sizes and
-- not at the M700's, where the third line ran out of the tile and over
-- "Mixed at" (Diego, 23 November 2026).
local function tile_h()
  return 6 + 2 * gfx.height("ui") + gfx.height("label") + 6
end

local function tiles()
  local v = ui.view{ h = tile_h() + 6 + gfx.height("ui") + 4 }

  v.fill = true

  function v:draw(g)
    local d = playing_device()

    if not d then return end

    local w = (self.w - 2 * TILE_GAP) // 3
    local TILE_H = tile_h()
    local lines = 2 * gfx.height("ui") + gfx.height("label")

    -- Once a size: how tall a tile is and its three lines, which the USB
    -- speaker's suite holds the one to the other (`usb_audio_mixer`).
    if self.said_h ~= TILE_H then
      self.said_h = TILE_H
      print(("mixer: a tile %d tall, its lines %d"):format(TILE_H, lines))
    end
    local items = {
      { "Sample", ("%d bit"):format(d.bits or 16),
        also(d.offers.bits, d.bits, function(b) return tostring(b) end) },
      { "Rate", khz(d.rate or 0),
        also(d.offers.rates, d.rate, function(r) return khz(r):gsub(" kHz", "") end) },
      { "Channels", tostring(d.channels or 2),
        d.channels == 2 and "stereo" or (d.channels == 1 and "mono" or "") },
    }

    for i, t in ipairs(items) do
      local x = (i - 1) * (w + TILE_GAP)

      g:fill_round(x, 0, w, TILE_H, theme.raised, 7)
      g:frame_round(x, 0, w, TILE_H, theme.line_soft, 7)
      g:text(x + 10, 6, t[1], theme.text_dim, nil, "ui")
      g:text(x + 10, 6 + gfx.height("ui"), t[2], theme.text, nil, "label")
      g:text(x + 10, 6 + gfx.height("ui") + gfx.height("label"), t[3], theme.text_dim, nil, "ui")
    end

    if fmt.rate > 0 and d.rate and d.rate ~= fmt.rate then
      g:text(0, TILE_H + 6, ("Mixed at %s, converted to %s here"):format(khz(fmt.rate), khz(d.rate)),
             theme.text_dim, nil, "ui")
    end
  end

  return v
end

local function chooser()
  local choices, now = {}, playing_device()

  for _, d in ipairs(devices) do
    choices[#choices + 1] = { d.name, d.name }
  end

  return ui.dropdown{
    choices = choices,
    value = now and now.name or nil,
    on_change = function(_, name)
      local list, said = audio.choose(name)

      if list then devices, chosen = list, said end
      print(("mixer: chose %s; playing on %s"):format(name,
            (playing_device() or {}).name or "nothing"))
    end,
  }
end

local header = ui.header{
  x = 0, y = 0, w = W, title = "Mixer",
  sub = (fmt.period == 0) and "no sound device"
        or ("%d Hz · %s"):format(fmt.rate,
                                 fmt.channels == 2 and "stereo"
                                 or (tostring(fmt.channels) .. " channels")),
  right = {
    --
    -- A test tone, because a mixer with nothing playing shows nothing.
    --
    -- Every hardware mixer has one and the reason is the same here: the
    -- meters, the faders and the mute are only observable while something
    -- is making a noise, and arranging for that from outside means starting
    -- a program at the right moment and hoping the window is up in time. It
    -- was not - the window manager takes long enough to start that a
    -- five-second tone was over before this window first drew, which is why
    -- the rows all said "idle" and looked like a bug.
    --
    -- It asks the window manager to run `beep`, rather than playing
    -- anything itself. This program is a *view* of the audio server and
    -- giving it a voice of its own would make it a participant in what it
    -- is meant to be showing.
    --
    ui.button{ text = "Test tone", hidden = (fmt.period == 0),
               on_click = function()
                 fs.send("/Running/wm", { type = "launch", program = "beep",
                                      args = "440 3000" })
               end },
  },
  title_bar = true,
}

local cards = ui.cards{ x = 0, y = L.head, w = W, h = H - L.head }

--
-- What the server has mixed, ever, as the page's note.
--
-- Kept after it stopped being a debugging aid, because it is the one
-- number that distinguishes "nothing is playing" from "the server is not
-- running" - and telling those apart took an hour once. A label of its own
-- rather than the cards' foot, because it changes every tick and a page is
-- rebuilt, not edited.
--
local mixed = ui.label{ x = L.page_side + 3, y = 0, w = W - 2 * L.page_side,
                        text = "", color = "text_dim", role = "ui",
                        follow = { "left", "right", "top" } }

--
-- The rows, built again only when the streams themselves change - one
-- starting or ending - and not when their numbers do.
--
local shown = nil

local function rebuild()
  local playing = {}
  local key = {}

  for _, r in ipairs(rows) do
    --
    -- Playing is something in its ring or a sample above silence in the
    -- last period; the server keeps no flag of its own for it.
    --
    local on = (r.queued or 0) > 0 or (r.peak or 0) > 0

    key[#key + 1] = tostring(r.stream) .. "=" .. tostring(r.name)
                    .. (on and "+" or "-")

    playing[#playing + 1] = { label = tostring(r.name or "?"),
                              note = on and "playing" or "idle",
                              control = strip(r.stream) }
  end

  -- The devices too: one plugged in, pulled out or chosen is a new page.
  for _, d in ipairs(devices) do
    key[#key + 1] = ("%s:%s:%d:%d:%d"):format(d.name, d.playing and "on" or "off",
                                              d.rate or 0, d.bits or 0, d.channels or 0)
  end

  key = table.concat(key, " ")

  if key == shown then return end

  shown = key

  --
  -- **One column for every name**, so the faders start at one x down the
  -- whole page and a level can be read against the one above it. A row's
  -- fader would otherwise begin wherever its own name ended.
  --
  local name_w = gfx.measure("Master", "label")

  for _, row in ipairs(playing) do
    name_w = math.max(name_w, gfx.measure(row.label, "label"))
  end

  name_w = math.min(name_w, 140)

  for _, row in ipairs(playing) do row.name_w = name_w end

  if #playing == 0 then
    playing[1] = { label = "Nothing is playing",
                   note = "Test tone plays three seconds of A." }
  end

  local output = {}
  local now = playing_device()

  if now then
    output[#output + 1] = { label = "Device", note = facts(now), control = chooser() }
    output[#output + 1] = { control = tiles() }
  end

  output[#output + 1] = { label = "Master", name_w = name_w, control = strip("master") }

  cards:set({
    { name = "Output", rows = output },
    { name = "Playing", rows = playing },
  })

  if now then
    print(("mixer: output %s, %d bit, %d Hz, %d channels; %d devices"):format(
          now.name, now.bits or 0, now.rate or 0, now.channels or 0, #devices))
  end

  mixed.y = L.head + cards.content_h + 10
end

--
-- Asked on every tick, because a meter that updates when you click is not a
-- meter. This is one message a tick to a server that is already awake while
-- anything is playing.
--
local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

--
-- **Through `/Kosmos/Libraries/audio.lua`**, which speaks the server's declared struct.
-- This asked `/Devices/audio` with a table, which the server stopped taking
-- when it moved to `audioproto.h` - so every reply was a refusal, the rows
-- were always empty, and the window said "nothing is playing" while
-- something was.
--
function ticker:tick()
  local list, st = audio.streams()

  rows = list or {}

  if st then
    master = st.master or master
    master_muted = st.master_muted == true
    mixes = st.mixes or 0
  end

  rebuild()
  mixed.text = ("%d periods mixed"):format(mixes)
end

if fmt.period == 0 then
  --
  -- No device is a sentence in the middle of the page, as Video's empty
  -- state is - not a card with nothing in it.
  --
  local lines = { { "No sound device", "label", "text" },
                  { "This machine has nothing the system can play through.",
                    "text", "text_dim" } }
  local step = gfx.height("text") + 6
  local y = L.head + (H - L.head - #lines * step) // 2

  for _, line in ipairs(lines) do
    local w = gfx.measure(line[1], line[2])

    win:add(ui.label{ x = (W - w) // 2, y = y, w = w + 2, text = line[1],
                      role = line[2], color = line[3] })
    y = y + step
  end

  win:add(header)
  win:run()
  return
end

--
-- The devices asked for once, and again whenever the server says they
-- changed - a speaker plugged in, pulled out, or chosen from elsewhere -
-- by a pulse to this window rather than on the tick (`win:watch_code`).
--
local function heard_devices()
  local list, said = audio.devices()

  if list then devices, chosen = list, said end
  rebuild()
end

do
  local code, door = win:watch_code(heard_devices)

  if code then
    local ok, why = audio.watch(code, door)

    pcall(sys.release, door)
    if not ok then
      win:drop_code(code)
      print("mixer: not told when a device changes: " .. tostring(why))
    end
  end
end

win:add(cards)
win:add(mixed)
win:add(ticker)
win:add(header)

do
  local list, said = audio.devices()

  if list then devices, chosen = list, said end
end

ticker:tick()
win:run()
