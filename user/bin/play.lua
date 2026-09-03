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
-- A window of the file at a time, and not a byte of it in a Lua string.
--
-- **This is the shape `CLAUDE.md`'s rule asks for.** The filesystem reads
-- into a region; `sys.pcm_into` converts from that region straight into the
-- audio ring the server is reading from; nothing is allocated per period,
-- so the collector has no reason to run inside the deadline. The old loop
-- built a Lua string per read, sliced another per period, and sent every
-- one of them as a message.
--
-- The read window is matched to the ring rather than chosen for being a
-- round number. `pcm_into` stops when the ring is full, and whatever it did
-- not consume is simply read again next time - so a window much larger than
-- the ring would re-read most of itself on every pass. Eight periods of
-- ring is 8 KB of source at this rate; sixteen gives slack without turning
-- the loop into a re-reader.
--
local READ = 16 * 1024
local bufcap = sys.memory(READ // 4096)

if not bufcap then
  print("play: no room for a read buffer")
  return
end

local addr = sys.memory_map(bufcap)

if not addr then
  print("play: could not map the read buffer")
  return
end

local pos = info.offset
local last = info.offset + info.bytes
local phase, partial = 0.0, 0

while pos < last do
  if out:space() == 0 then
    -- Ahead of the device, which is the good problem. Sleep rather than
    -- spin: yielding here would be a busy wait dressed as a wait.
    sys.sleep(1)
  else
    local want = math.min(READ, last - pos)
    local n = fs.read_into(path, bufcap, pos, want)

    if not n or n == 0 then break end

    local wrote, used
    wrote, used, phase, partial =
        sys.pcm_into(addr, n, out.ring,
                     info.rate, info.channels, info.bits, phase, partial)

    --
    -- The file position carries the leftover.
    --
    -- `pcm_into` stops at the last frame it can interpolate *from*, so what
    -- it did not consume is still needed - and rather than shuffling those
    -- bytes down the buffer, the next read simply starts where the
    -- conversion stopped. A few bytes are read twice and no primitive has
    -- to exist for moving them.
    --
    pos = pos + used

    if used == 0 then
      if n < info.frame * 2 then break end     -- the tail of the file
      if wrote == 0 then sys.sleep(1) end
    end
  end
end

--
-- Wait for what was handed over to actually be played before letting go.
--
-- Closing drops whatever has not been mixed yet, which is the last fifth of
-- a second of every song.
--
for _ = 1, 400 do
  if out:queued() == 0 then break end

  sys.sleep(1)
end

out:close()
print("play: done")
