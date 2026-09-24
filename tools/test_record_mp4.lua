-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The camera's recording, read by the video player's own reader.
--
--   build/host/lua tools/test_record_mp4.lua
--
-- `tools/test_record.c` records sixty frames of the camera's test pattern -
-- 640 by 480, thirty a second, two seconds - with the Record Kit's own code,
-- and writes the file to `build/host/test_record.mp4`; FFmpeg decodes it
-- there. This reads the same file with `/lib/mp4.lua`, which is how Video
-- will open a recording once it decodes H.264 (`roadmap.md` 4e): a writer
-- and a reader that agree only with themselves would not be a file format.

local mp4 = assert(loadfile("user/lib/mp4.lua"))()

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local path = "build/host/test_record.mp4"
local clip = io.open(path, "rb")

if not clip then
  print("FAIL: no " .. path .. " - `build/host/test_record` writes it")
  os.exit(1)
end

local size = clip:seek("end")
local film = mp4.open(function(at, n) clip:seek("set", at) return clip:read(n) end,
                      size)

local video

for _, t in ipairs(film.tracks or {}) do
  if t.kind == "video" then video = t end
end

check(#(film.tracks or {}) == 1 and video ~= nil,
      "one track, and it is video")
check(video and video.codec == "avc1", "H.264: an avc1 sample entry")
check(video and video.width == 640 and video.height == 480, "640 by 480")
check(video and video.profile == 66,
      "Baseline, which is what minih264e writes")
check(video and #video.samples == 60, "sixty samples, a frame each: "
      .. tostring(video and #video.samples))
check(video and video.samples[1] and video.samples[1].key,
      "the first a sync sample a decoder can start at")
check(video and video.timescale and video.duration
      and math.abs(video.duration / video.timescale - 2.0) < 0.05,
      "two seconds long")

-- And every sample inside the file, where the index says.
local inside = video ~= nil

for _, s in ipairs(video and video.samples or {}) do
  inside = inside and s.at + s.size <= size and s.size > 0
end

check(inside, "every sample within the file")

clip:close()

if failed > 0 then
  print(("FAIL: %d of %d checks on a recording read back"):format(failed,
        passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the camera's recording, read by the video "
       .. "player's own MP4 reader"):format(passed))
