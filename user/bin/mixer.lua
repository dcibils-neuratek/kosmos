-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Mixer: a fader for every program making a noise.
-- kosmos: application
-- kosmos: section preferences
--
--   wm mixer
--
-- One row per open stream, plus a master at the top. Drag a fader, click
-- the box to mute, drag the balance under it.
--
-- **The meter is the interesting part and it costs nothing.** `sys.mix`
-- already touches every sample to sum them, so the loudest one it saw is
-- free on the way past - and it is measured *before* the gain, which is
-- what makes it answer "who is sending audio" rather than "how loud is it".
-- A muted stream still shows a moving meter, which is the whole point when
-- you are looking for the program that will not shut up.
--
-- Nothing here plays anything. It asks `/dev/audio` what exists and tells
-- it what to change, which is the same relationship the Deskbar has with
-- the window manager: the authority is in the server and this is a view of
-- it.

local ui    = use("/lib/ui.lua")
local audio = use("/lib/audio.lua")

local theme = ui.theme

local W, H = 460, 340

local win, err = ui.window{ title = "Mixer", w = W, h = H, x = 150, y = 110 }

if not win then
  print("mixer: " .. tostring(err))
  return
end

local fmt = audio.format()

local master = 256
local rows = {}                 -- what the server last said
local mixes = 0                 -- periods the server has mixed, ever

--
-- Row geometry, in one place because the drawing and the hit test both need
-- it and disagreeing is the bug where you drag one fader and another moves.
-- The same reason `boxes_x` exists in the window manager.
--
local ROW_H   = 44
local NAME_W  = 110
local VALUE_W = 44
local MUTE_W  = 22

local function fader_rect(view)
  local x = NAME_W + 8
  local w = view.w - x - VALUE_W - MUTE_W - 16

  return x, w
end

local function gain_at(view, x)
  local fx, fw = fader_rect(view)

  if fw <= 0 then return nil end

  local g = ((x - fx) * 256) // fw

  return math.min(math.max(g, 0), 256)
end

--
-- The list, drawn by hand rather than as an `ui.list`.
--
-- A list formats one string per item, and a row here is a name, a fader, a
-- number, a button and a meter - five fields that have to line up in
-- columns. `ui.md` 16.2: a view draws itself, and what it draws with comes
-- from the kit.
--
local view = ui.view{ x = 8, y = 8, w = W - 16, h = H - 76,
                      follow = { "left", "right", "top", "bottom" } }

view.focusable = true

local function draw_row(g, y, label, gain, muted, peak, playing, is_master)
  local fx, fw = fader_rect(view)

  g:text(6, y + 6, label, is_master and theme.text or theme.text,
         theme.window)

  if playing ~= nil then
    g:text(6, y + 6 + gfx.font.h, playing and "playing" or "idle",
           playing and theme.good or theme.text_dim, theme.window)
  end

  --
  -- The track, then the fill, then the knob.
  --
  -- Sunken track and a raised knob: `ui.md` 16.8b, and here it is doing
  -- real work rather than decoration - which part of a fader you can drag
  -- is exactly the question the two edges answer.
  --
  g:sunken(fx, y + 8, fw, 10, "sunken")

  local at = (gain * (fw - 12)) // 256

  if gain > 0 then
    g:fill(fx + 2, y + 10, at + 8, 6,
           muted and theme.line_soft or theme.accent)
  end

  g:raised(fx + at, y + 5, 12, 16, "raised")

  -- The number, right-aligned so the digits do not move as they change.
  local pct = ("%d%%"):format((gain * 100) // 256)


  g:text(fx + fw + 8 + VALUE_W - gfx.measure(pct) - 4, y + 6, pct,
         theme.text, theme.window)

  -- Mute, as a box that is filled when it is on.
  local mx = fx + fw + 8 + VALUE_W + 4

  g:sunken(mx, y + 6, MUTE_W - 6, MUTE_W - 6, "sunken")

  if muted then
    g:fill(mx + 3, y + 9, MUTE_W - 12, MUTE_W - 12, theme.bad)
  end

  --
  -- The meter, under the fader.
  --
  -- Full scale is 32767 and the bar is drawn from the peak directly rather
  -- than in decibels, which is the honest simplification: a dB scale is
  -- what you want for mixing and this is for *finding* the program that is
  -- making the noise, where linear is easier to read at a glance.
  --
  if peak then
    local lit = math.min(fw, (peak * fw) // 32767)

    g:fill(fx, y + 22, fw, 4, theme.sunken)

    if lit > 0 then
      g:fill(fx, y + 22, lit, 4,
             lit > (fw * 4) // 5 and theme.bad or theme.good)
    end
  end
end

function view:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  if fmt.period == 0 then
    g:text(6, 6, "this machine has no sound device", theme.text_dim,
           theme.window)

    return
  end

  draw_row(g, 0, "Master", master, false, nil, nil, true)
  g:groove(0, ROW_H - 6, self.w, 2)

  local y = ROW_H

  for _, s in ipairs(rows) do
    if y + ROW_H > self.h then break end

    draw_row(g, y, s.name, s.gain, s.muted, s.peak, s.playing)
    y = y + ROW_H
  end

  if #rows == 0 then
    g:text(6, ROW_H + 6, "nothing is playing", theme.text_dim, theme.window)
  end

  --
  -- What the server has mixed, ever.
  --
  -- Kept after it stopped being a debugging aid, because it is the one
  -- number that distinguishes "nothing is playing" from "the server is not
  -- running" - and telling those apart took an hour once.
  --
  g:text(6, self.h - gfx.font.h - 2, ("%d periods mixed"):format(mixes),
         theme.text_dim, theme.window)
end

--
-- Which row a point is in, and what part of it.
--
-- Returns the stream (or nil for the master) and what was hit.
--
local function hit(x, y)
  local fx, fw = fader_rect(view)
  local mx = fx + fw + 8 + VALUE_W + 4

  local which, row
  if y < ROW_H then
    which = nil                                   -- the master
    row = 0
  else
    row = (y - ROW_H) // ROW_H
    which = rows[row + 1]

    if not which then return nil end
  end

  local top = (row == 0 and y < ROW_H) and 0 or (ROW_H + row * ROW_H)
  local ry = y - ((y < ROW_H) and 0 or (ROW_H + row * ROW_H))

  local _ = top

  if x >= mx and x < mx + MUTE_W and ry >= 4 and ry < 4 + MUTE_W then
    return which, "mute"
  end

  if x >= fx and x < fx + fw and ry < 22 then
    return which, "gain"
  end

  return which, nil
end

local dragging = nil

function view:mouse(action, x, y)
  if action == "press" or (action == "move" and dragging) then
    local who, what = hit(x, y)

    if action == "press" then
      if what == "mute" then
        if who then
          audio.set{ stream = who.stream, muted = not who.muted }
        end

        return true
      end

      dragging = (what == "gain") and { who = who } or nil
    end

    if dragging then
      local g = gain_at(view, x)

      if g then
        if dragging.who then
          audio.set{ stream = dragging.who.stream, gain = g }
          dragging.who.gain = g
        else
          audio.set{ master = g }
          master = g
        end
      end
    end

    return true
  end

  if action == "release" then dragging = nil end

  return true
end

--
-- Asked on every tick, because a meter that updates when you click is not a
-- meter. This is one message a tick to a server that is already awake while
-- anything is playing.
--
local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function ticker:tick()
  local r = fs.send("/dev/audio", { type = "streams" })

  if r then
    rows = r.streams or {}
    master = r.master or master
    mixes = r.mixes or 0
  end
end

--
-- A test tone, because a mixer with nothing playing shows nothing.
--
-- Every hardware mixer has one and the reason is the same here: the meters,
-- the faders and the mute are only observable while something is making a
-- noise, and arranging for that from outside means starting a program at
-- the right moment and hoping the window is up in time. It was not - the
-- window manager takes long enough to start that a five-second tone was
-- over before this window first drew, which is why the rows all said
-- "idle" and looked like a bug.
--
-- It asks the window manager to run `beep`, rather than playing anything
-- itself. This program is a *view* of the audio server and giving it a
-- voice of its own would make it a participant in what it is meant to be
-- showing.
--
win:add(ui.button{
  x = 8, y = H - 34, w = 90, h = 24, text = "Test tone",
  on_click = function()
    fs.send("/app/wm", { type = "launch", program = "beep",
                         args = "440 3000" })
  end,
})

win:add(view)
win:add(ticker)

ticker:tick()
win:run()
