-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Plays a WAV file.
--
--   play /home/music/thing.wav
--
-- A program rather than an application, because it does one thing and
-- stops. The window with the playlist in it is `Music`; this is the part
-- underneath, and having it separate means the decoding can be tested from
-- a prompt without a window in the way.
--
-- **Streamed, not loaded.** Three minutes of CD audio is thirty megabytes
-- and a process has forty-eight, so this reads a window at a time, converts
-- it to the board's format, and hands it over. Nothing anywhere holds the
-- whole song.

local audio = use("/lib/audio.lua")
local wav   = use("/lib/wav.lua")

local path = (args or ""):match("^%s*(%S+)")

if not path then
  print("play: which file?")
  return
end

local fmt = audio.format()

if fmt.period == 0 then
  print("play: this machine has no sound device")
  return
end

local attrs, why = fs.getattr(path)

if not attrs then
  print("play: " .. path .. ": " .. tostring(why))
  return
end

--
-- The header first, walked rather than guessed at.
--
-- One page is plenty to read a chunk header into; the scan seeks through
-- the file eight bytes at a time and never assumes the samples start
-- within any particular distance of the front. They do not: a track from
-- `afconvert` has four kilobytes of padding before its first sample.
--
local window = sys.memory(1)

local info, err = wav.scan(function(at, n)
  local got = fs.read_into(path, window, at, n)

  if not got or got == 0 then return nil end

  return sys.region_read(window, 0, got)
end)

if not info then
  print("play: " .. path .. ": " .. tostring(err))
  return
end

print(("play: %s, %d Hz %s %d-bit, %.1f s")
      :format(path, info.rate, info.channels == 2 and "stereo" or "mono",
              info.bits, info.seconds))

local out, oops = audio.open(path:match("([^/]+)$") or "play")

if not out then
  print("play: " .. tostring(oops))
  return
end

--
-- A window of the file at a time.
--
-- 64 KB is about a third of a second of CD audio: big enough that the
-- reads are not the cost, small enough that converting one is not a pause
-- somebody hears. The leftover matters as much as the window - `sys.pcm`
-- stops at the last frame it can interpolate *from*, so what it did not
-- consume is carried forward and prepended to the next read.
--
local WINDOW = 64 * 1024
local buffer = sys.memory(WINDOW // 4096)

if not buffer then
  print("play: no room for a read buffer")
  return
end

local at = info.offset
local last = info.offset + info.bytes
local carry, phase = "", 0.0
local pending = ""

while at < last or #pending > 0 or #carry > 0 do
  -- Top up: convert more only when there is nothing left to send.
  if #pending == 0 then
    if at < last and #carry < WINDOW then
      local want = math.min(WINDOW - #carry, last - at)
      local n = fs.read_into(path, buffer, at, want)

      if not n or n == 0 then break end

      carry = carry .. sys.region_read(buffer, 0, n)
      at = at + n
    end

    if #carry < info.frame * 2 then break end

    local pcm, used
    pcm, used, phase = sys.pcm(carry, info.rate, info.channels, info.bits,
                               phase, fmt.period * 8)

    if used == 0 or #pcm == 0 then break end

    carry = carry:sub(used + 1)
    pending = pcm
  end

  -- `write` rather than `play`: there is nothing else to do with the time,
  -- and calling `play` in a loop is a spin rather than a wait.
  local ok, oops2 = out:write(pending:sub(1, fmt.period))

  if not ok then
    print("play: " .. tostring(oops2))
    break
  end

  pending = pending:sub(fmt.period + 1)
end

--
-- Wait for what was handed over to actually be played before letting go.
--
-- Closing drops whatever the server has not mixed yet, which is a fifth of
-- a second of the end of every song.
--
for _ = 1, 100 do
  local list = audio.streams()
  local mine = nil

  for _, one in ipairs(list or {}) do
    if one.stream == out.id then mine = one end
  end

  if not mine or (mine.queued or 0) == 0 then break end

  sys.sleep(1)
end

out:close()
print("play: done")
