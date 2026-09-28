-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Novation Launchkey Mini MK3 in its DAW mode, for Groove (`roadmap.md`
-- 6zg): PulseMusic's `launchkey.lua`, converted. Pure logic - it needs only
-- `midi.outputNames()` and `midi.send(port, status, d1, d2)`, which
-- `groove/midiport.lua` gives it over `/Devices/midi`.
--
-- Session pad mode: the top row is the eight tracks' clips in the selected
-- scene, the bottom row scenes 1 to 8. Drum pad mode: the sixteen pads play
-- the drum track, and their lights follow its hits, played or sequenced.
--
-- A port is named by its device and its jack; a Launchkey whose jacks carry
-- no names is "<device> 2" on its second cable, which `isDawPort` takes as
-- the DAW port as it took Windows's "MIDIIN2".

local LK = { active = false, port = nil, padMode = 2, cache = {} }

local BRIGHT = { 5, 9, 13, 21, 33, 45, 49, 57 }   -- palette entries matching the 8 track colors
local DIM    = { 7, 11, 15, 23, 35, 47, 51, 59 }
local TOP, BOTTOM = 96, 112                       -- session pad notes, left to right
local DRUM_TOP    = { 40, 41, 42, 43, 48, 49, 50, 51 }
local DRUM_BOTTOM = { 36, 37, 38, 39, 44, 45, 46, 47 }

local function isDawPort(name)
  local n = name:lower()
  if not n:find("launchkey") then return false end
  return n:find("daw") or n:find("midi 2") or n:find("midiin2") or n:find("midiout2") or n:find("mk3 2")
end
LK.isDawPort = isDawPort

function LK.attach(midi)
  LK.midi, LK.active, LK.port, LK.cache = midi, false, nil, {}
  for _, name in ipairs(midi.outputNames()) do
    if isDawPort(name) then LK.port = name; break end
  end
  if not LK.port then return false end
  if not midi.send(LK.port, 0x9F, 12, 127) then return false end  -- enter DAW mode
  midi.send(LK.port, 0xBF, 3, 2)                                  -- pads: session mode
  LK.padMode, LK.active = 2, true
  return true
end

function LK.detach()
  if LK.active then LK.midi.send(LK.port, 0x9F, 12, 0) end        -- hand the unit back
  LK.active = false
end

-- Returns an action table when the message belongs to the control surface, else nil.
function LK.handle(kind, ch, d1, d2, port)
  if not LK.active or not port or not isDawPort(port) then return nil end
  if kind == "cc" then
    if ch == 16 and d1 == 3 then LK.padMode = d2; LK.cache = {}; return { type = "padmode", mode = d2 } end
    if ch == 16 and d1 == 9 then return { type = "knobmode", mode = d2 } end
    if d2 == 0 then
      if d1 == 104 or d1 == 105 or d1 == 102 or d1 == 103 or d1 == 115 or d1 == 117 or d1 == 108 then return { type = "release" } end
      return nil
    end
    if d1 == 104 then return { type = "launchSelected" } end
    if d1 == 105 then return { type = "stopAll" } end
    if d1 == 103 then return { type = "track", delta = -1 } end
    if d1 == 102 then return { type = "track", delta = 1 } end
    if d1 == 115 then return { type = "play" } end
    if d1 == 117 then return { type = "rec" } end
    return nil
  end
  if (kind == "on" or kind == "off") and LK.padMode == 2 and ch == 1 then
    if d1 >= TOP and d1 < TOP + 8 then return { type = kind == "on" and "clip" or "release", track = d1 - TOP + 1 } end
    if d1 >= BOTTOM and d1 < BOTTOM + 8 then return { type = kind == "on" and "scene" or "release", scene = d1 - BOTTOM + 1 } end
  end
  return nil
end

local function led(key, status, d1, color, flashBase)
  local sig = status * 256 + color
  if LK.cache[key] == sig then return end
  LK.cache[key] = sig
  -- a flashing LED alternates with the last solid color, so set that first
  if flashBase then LK.midi.send(LK.port, flashBase[1], d1, flashBase[2]) end
  LK.midi.send(LK.port, status, d1, color)
end

-- state = { clips[1..8] = "empty"|"has"|"playing"|"queued", scenes[1..8] = "empty"|"has"|"playing",
--           selScene, drumColor (1..8), rowLit[1..8] = bool, playing, rec }
function LK.refresh(state)
  if not LK.active then return end
  if LK.padMode == 2 then
    for i = 1, 8 do
      local st, note = state.clips[i], TOP + i - 1
      if st == "playing" then led("t" .. i, 0x92, note, BRIGHT[i])            -- pulsing
      elseif st == "queued" then led("t" .. i, 0x91, note, BRIGHT[i], { 0x90, DIM[i] })
      elseif st == "has" then led("t" .. i, 0x90, note, DIM[i])
      else led("t" .. i, 0x90, note, 0) end
      local sc, n2 = state.scenes[i], BOTTOM + i - 1
      if sc == "playing" then led("b" .. i, 0x90, n2, 21)
      elseif i == state.selScene then led("b" .. i, 0x90, n2, 3)
      elseif sc == "has" then led("b" .. i, 0x90, n2, 1)
      else led("b" .. i, 0x90, n2, 0) end
    end
  elseif LK.padMode == 1 then
    local c = state.drumColor or 1
    for i = 1, 8 do
      local color = state.rowLit[i] and 3 or DIM[c]
      led("dt" .. i, 0x99, DRUM_TOP[i], color)
      led("db" .. i, 0x99, DRUM_BOTTOM[i], color)
    end
  end
  led("play", 0xB0, 115, state.playing and 127 or 0)
  if state.rec then led("rec", 0xB1, 117, 127, { 0xB0, 0 }) else led("rec", 0xB0, 117, 0) end
end

return LK
