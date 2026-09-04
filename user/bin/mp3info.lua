-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- What an MP3 says about itself, and how fast it decodes.
--
-- A program rather than a test because the interesting number is a rate:
-- the decoder has to produce samples faster than the device drains them,
-- and "faster" here means with enough margin that a busy desktop does not
-- eat it. This prints the ratio.

local mp3 = use("/kits/mp3")

local path = (args or ""):match("^%s*(%S+)")

if not path then print("usage: mp3info <path>") return end

local attrs = fs.getattr(path)

if not attrs then print("mp3info: no such file: " .. path) return end

local page = sys.memory(8)                      -- 32 KB window

if not page then print("mp3info: no memory") return end

local function window(off, n)
  local got = fs.read_into(path, page, off, n)

  if not got or got == 0 then return nil end

  return sys.region_read(page, 0, got)
end

local head = window(0, 32768)

if not head then print("mp3info: cannot read it") return end

local info, why = mp3.probe(head)

if not info then print("mp3info: " .. tostring(why)) return end

print(("%s: %d Hz, %s, %d kbps, layer %d, audio starts at %d")
      :format(path, info.rate,
              info.channels == 2 and "stereo" or "mono",
              info.bitrate, info.layer, info.offset))

--
-- Decode a stretch of it and time that against the audio it represents.
--
-- The device drains `rate * channels * 2` bytes a second, so the ratio of
-- decoded bytes to elapsed time is the headroom. Under one and the music
-- stutters no matter what the scheduler does.
--
local dec = mp3.decoder()
local at, decoded, carry = info.offset, 0, ""
local size = attrs.size or 0
local WANT = 512 * 1024                         -- source bytes to chew on

local started = sys.ticks()

while at < size and (at - info.offset) < WANT do
  local n = math.min(32768, size - at)
  local piece = window(at, n)

  if not piece then break end

  carry = carry .. piece
  at = at + n

  while #carry > 0 do
    local pcm, used = dec:decode(carry, 64 * 1024)

    if used == 0 then break end

    decoded = decoded + #pcm
    carry = carry:sub(used + 1)
  end
end

local elapsed = sys.ticks() - started

--
-- `sys.ticks` is the generic timer counter, not scheduler ticks, and the two
-- differ by a factor of about 250,000 on this machine. Dividing by TICK_HZ
-- here reported a 0.2 second decode as fourteen hours.
--
-- The frequency comes from `/dev/cpu` rather than a constant, because it is
-- 62 MHz under QEMU and 54 on a Pi 5, and a number compiled in would make
-- every measurement on real hardware quietly wrong.
--
local hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

if elapsed < 1 then elapsed = 1 end

local seconds = elapsed / hz
local audio = decoded / (info.rate * info.channels * 2)

print(("decoded %d KB of PCM (%.1f s of audio) in %.3f s: %.0fx real time")
      :format(decoded // 1024, audio, seconds, audio / seconds))

--
-- The number that decides whether this can play, rather than the ratio.
--
-- One period is 5.8 ms of audio, and the device wants one every 5.8 ms. If
-- decoding a period takes longer than that, no amount of scheduling helps.
--
print(("one 256-frame period costs %.3f ms to decode; the deadline is 5.8")
      :format(seconds * 1000 * (256 / (audio * info.rate))))
