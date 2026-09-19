-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `mp4.lua` on films built here: the boxes an MP4 is made of, written out
-- by hand, and every fact the reader gives back held to what was written.
--
--   build/host/lua tools/test_mp4.lua
--
-- Built rather than carried, because no film goes in the repository. The
-- one Diego tests with, `~/Kosmos/home/magicword-clip.mp4`, is read too
-- when it is there, and what it must say is written below from what the
-- Mac's own tools say of it - H.264 Main at level 3.0, 640x360, AAC-LC.

local mp4 = assert(loadfile("user/lib/mp4.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- A box, and a full box: a size and a type, and a version and flags.
local function box(kind, body)
  return string.pack(">I4", 8 + #body) .. kind .. body
end

local function fullbox(kind, version, body)
  return box(kind, string.pack(">I1I3", version, 0) .. body)
end

local function words(...)
  local out = {}

  for i, v in ipairs({ ... }) do out[i] = string.pack(">I4", v) end

  return table.concat(out)
end

local SPS = "\x67\x4d\x40\x1e\xaa\xbb"
local PPS = "\x68\xee\x3c\x80"

local function video_trak(offsets, co64, ctts_version)
  local avcC = box("avcC", "\x01\x4d\x40\x1e\xff\xe1"
                   .. string.pack(">I2", #SPS) .. SPS .. "\x01"
                   .. string.pack(">I2", #PPS) .. PPS)
  local visual = string.rep("\0", 6) .. string.pack(">I2", 1)
                 .. string.rep("\0", 16) .. string.pack(">I2I2", 640, 360)
                 .. string.rep("\0", 78 - 28)
  local stsd = fullbox("stsd", 0, words(1) .. box("avc1", visual .. avcC))

  -- Five samples: sizes 100, 200, 300, 400, 500. Chunks: two samples in
  -- each of the first two, one in the third.
  local stsz = fullbox("stsz", 0, words(0, 5, 100, 200, 300, 400, 500))
  local stsc = fullbox("stsc", 0, words(2, 1, 2, 1, 3, 1, 1))
  local chunks

  if co64 then
    chunks = fullbox("co64", 0, words(3)
                     .. string.pack(">I8I8I8", offsets[1], offsets[2], offsets[3]))
  else
    chunks = fullbox("stco", 0, words(3, offsets[1], offsets[2], offsets[3]))
  end

  -- 1000 ticks each; shown in the order I P B B P - the B-frames a
  -- decoding step early, the P after them later.
  local stts = fullbox("stts", 0, words(1, 5, 1000))
  local ctts

  if ctts_version == 1 then
    ctts = fullbox("ctts", 1, words(4, 1, 0, 1, 2000, 2, 0xFFFFFC18, 1, 0))
  else
    ctts = fullbox("ctts", 0, words(4, 1, 1000, 1, 3000, 2, 0, 1, 1000))
  end

  local stss = fullbox("stss", 0, words(2, 1, 5))
  local stbl = box("stbl", stsd .. stts .. ctts .. stss .. stsc .. stsz .. chunks)
  local mdhd = fullbox("mdhd", 0, words(0, 0, 30000, 5000) .. "\0\0\0\0")
  local hdlr = fullbox("hdlr", 0, words(0) .. "vide" .. string.rep("\0", 12) .. "V\0")

  return box("trak", box("mdia", mdhd .. hdlr .. box("minf", stbl)))
end

local function audio_trak(offset)
  local asc = "\x12\x10"
  local es = "\x03\x19" .. "\x00\x01" .. "\x00"
             .. "\x04\x11" .. "\x40\x15" .. "\0\0\0" .. words(0, 0)
             .. "\x05\x02" .. asc .. "\x06\x01\x02"
  local sound = string.rep("\0", 6) .. string.pack(">I2", 1) .. string.rep("\0", 8)
                .. string.pack(">I2I2I2I2", 2, 16, 0, 0)
                .. string.pack(">I4", 44100 << 16)
  local stsd = fullbox("stsd", 0, words(1) .. box("mp4a", sound .. fullbox("esds", 0, es)))
  local stsz = fullbox("stsz", 0, words(64, 3))       -- three of 64 bytes
  local stsc = fullbox("stsc", 0, words(1, 1, 3, 1))
  local stco = fullbox("stco", 0, words(1, offset))
  local stts = fullbox("stts", 0, words(1, 3, 1024))
  local stbl = box("stbl", stsd .. stts .. stsc .. stsz .. stco)
  local mdhd = fullbox("mdhd", 1, string.pack(">I8I8I4I8", 0, 0, 44100, 3072) .. "\0\0\0\0")
  local hdlr = fullbox("hdlr", 0, words(0) .. "soun" .. string.rep("\0", 12) .. "S\0")

  return box("trak", box("mdia", mdhd .. hdlr .. box("minf", stbl)))
end

--
-- A film: `ftyp`, then `moov` and `mdat` in either order; `mdat` with a
-- 64-bit size when asked. The chunk offsets are where the samples really
-- are, worked out from the order.
--
local function film(options)
  local ftyp = box("ftyp", "isom" .. words(0x200) .. "isomavc1")
  local payload = string.rep("V", 100 + 200) .. string.rep("W", 300 + 400)
                  .. string.rep("X", 500) .. string.rep("A", 192)

  local function mdat()
    if options.wide then
      return string.pack(">I4", 1) .. "mdat" .. string.pack(">I8", 16 + #payload) .. payload
    end

    return box("mdat", payload)
  end

  local function moov(first_byte)
    local v = { first_byte, first_byte + 300, first_byte + 1000 }

    return box("moov", box("mvhd", string.rep("\0", 100))
               .. video_trak(v, options.co64, options.ctts_version)
               .. audio_trak(first_byte + 1500))
  end

  local head = options.wide and 16 or 8

  if options.moov_last then
    local first = #ftyp + head
    return ftyp .. mdat() .. moov(first)
  end

  -- `moov` first: its length does not depend on the offsets inside it.
  local length = #moov(0)
  return ftyp .. moov(#ftyp + length + head) .. mdat()
end

local function open(bytes)
  return mp4.open(function(at, n) return bytes:sub(at + 1, at + n) end, #bytes)
end

local function holds(bytes, sample, fill)
  return bytes:sub(sample.at + 1, sample.at + sample.size)
         == string.rep(fill, sample.size)
end

for _, case in ipairs({
  { name = "moov first, stco, ctts version 0" },
  { name = "moov last, co64, ctts version 1, a 64-bit mdat",
    moov_last = true, co64 = true, ctts_version = 1, wide = true },
}) do
  local bytes = film(case)
  local f, why = open(bytes)

  check(f ~= nil, case.name .. ": did not open: " .. tostring(why))

  if f then
    local v, a = f.video, f.audio

    check(f.brand == "isom" and #f.tracks == 2 and v and a,
          case.name .. ": not an isom film with a video and an audio track")

    check(v.codec == "avc1" and v.width == 640 and v.height == 360
          and v.timescale == 30000 and v.duration == 5000,
          case.name .. ": the video's codec, size, timescale or duration")

    check(v.profile == 77 and v.level == 30 and v.nal_length == 4
          and v.sps[1] == SPS and v.pps[1] == PPS,
          case.name .. ": avcC's profile, level, NAL length, SPS or PPS")

    local s = v.samples
    check(#s == 5 and s[1].size == 100 and s[5].size == 500,
          case.name .. ": the five samples and their sizes")

    check(holds(bytes, s[1], "V") and holds(bytes, s[2], "V")
          and holds(bytes, s[3], "W") and holds(bytes, s[4], "W")
          and holds(bytes, s[5], "X"),
          case.name .. ": a sample's offset is not where its bytes are - "
          .. "the chunks and their runs were read wrong")

    -- Version 1's offsets are version 0's less a frame - what signed
    -- offsets are for, the first frame shown at 0 - so the same order,
    -- a thousand earlier, with two of them negative.
    local dts, pts = {}, {}
    local shown = case.ctts_version == 1 and "0,3000,1000,2000,4000"
                  or "1000,4000,2000,3000,5000"
    for i = 1, 5 do dts[i], pts[i] = s[i].dts, s[i].pts end
    check(table.concat(dts, ",") == "0,1000,2000,3000,4000"
          and table.concat(pts, ",") == shown,
          case.name .. ": decoding and showing times were " .. table.concat(dts, ",")
          .. " and " .. table.concat(pts, ",") .. " - ctts, signed or not")

    check(s[1].key and not s[2].key and not s[4].key and s[5].key,
          case.name .. ": the sync samples are not 1 and 5 alone")

    check(a.codec == "mp4a" and a.channels == 2 and a.rate == 44100
          and a.timescale == 44100 and a.duration == 3072,
          case.name .. ": the audio's codec, channels, rate or duration "
          .. "(mdhd version 1)")

    check(a.object == 0x40 and a.aot == 2 and a.config == "\x12\x10",
          case.name .. ": esds did not say MPEG-4 audio, AAC-LC, config 12 10")

    check(#a.samples == 3 and holds(bytes, a.samples[3], "A")
          and a.samples[3].dts == 2048 and a.samples[2].key,
          case.name .. ": the audio's samples, times, or every one a sync")
  end
end

-- Not films.
check(select(2, open(box("ftyp", "isom") .. box("free", "xx"))) ~= nil
      and open(box("ftyp", "isom")) == nil,
      "a file with no moov was opened")

local whole = film({})
check(open(whole:sub(1, 200)) == nil,
      "a film cut off inside its moov was opened")

check(open(string.pack(">I4", 4) .. "junk") == nil,
      "a box shorter than its own header was believed")

-- Diego's clip, when it is on this Mac: what the Mac's tools say of it.
local path = (os.getenv("HOME") or "") .. "/Kosmos/home/magicword-clip.mp4"
local clip = io.open(path, "rb")

if clip then
  local size = clip:seek("end")
  local f = mp4.open(function(at, n) clip:seek("set", at) return clip:read(n) end, size)

  check(f and f.video and f.video.profile == 77 and f.video.level == 30
        and f.video.width == 640 and f.video.height == 360
        and #f.video.samples == 727 and f.audio and f.audio.aot == 2
        and f.audio.rate == 44100 and #f.audio.samples == 1045,
        "magicword-clip.mp4 was not H.264 Main 3.0 at 640x360 with 727 "
        .. "frames, and AAC-LC at 44100 Hz with 1045")

  clip:close()
end

if failed > 0 then
  print(("FAIL: %d of %d checks on mp4.lua"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on mp4.lua (two films built here - moov first and "
       .. "last, stco and co64, ctts signed and not, a 64-bit mdat - every "
       .. "sample found where its bytes are, the H.264 and AAC set-up read, "
       .. "and three files that are not films refused%s)"):format(passed,
       clip and "; and Diego's clip, as the Mac's tools describe it" or ""))
