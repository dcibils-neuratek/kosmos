-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- An MP4 file's index: its tracks, and where every frame of each one is.
--
-- The first piece of the video player (`roadmap.md` 4e). Diego, 19
-- September: "add a video player with mp4 support". An MP4 - ISO/IEC
-- 14496-12, the ISO base media file format, and 14496-14 for the MP4 part -
-- is a tree of boxes: a 32-bit size and a four-letter type, then the box's
-- bytes, some of them boxes in turn. The frames are in `mdat`, as bytes
-- nobody labelled; `moov` is the index that says where each one is, how big,
-- and when it is shown. This reads `moov` and nothing else.
--
-- **Structure, not bytes, so it is Lua**: a few hundred boxes, a table per
-- track of a few thousand entries, read once when a film is opened. The
-- frames themselves are never touched here - a decoder is handed them by
-- offset and size - which is where the loops over bytes are, and where C
-- belongs (`CLAUDE.md`, *Language split*).
--
-- **Read through a function, not a string**: `mp4.open(read, size)` asks
-- `read(offset, length)` for what it wants, so a film of a gigabyte is
-- opened by reading its box headers and its `moov`, a few kilobytes to a
-- few megabytes, whether `moov` is at the front or - as a camera writes it -
-- at the end.
--
-- What a track says, for a decoder:
--
--   kind       "video" or "audio"
--   codec      the sample entry's type: "avc1" for H.264, "mp4a" for AAC
--   timescale  ticks a second, which every time below is in
--   duration   in those ticks
--   video      width, height; and from `avcC` (14496-15 5.3.3.1): profile,
--              level, nal_length (bytes before each NAL unit), sps, pps
--   audio      channels, rate; and from `esds` (14496-1 7.2.6): object
--              (0x40 is MPEG-4 audio), aot (2 is AAC-LC), config - the
--              AudioSpecificConfig's bytes, as an AAC decoder wants them
--   samples    in decoding order, each { at, size, dts, pts, key }: its
--              offset in the file, its length, when it is decoded and when
--              it is shown (they differ where there are B-frames, `ctts`),
--              and whether it is a sync sample a decoder can start at

local mp4 = {}

-- Boxes whose contents are only boxes (14496-12's container boxes).
local CONTAINER = {
  moov = true, trak = true, mdia = true, minf = true, stbl = true,
  edts = true, dinf = true,
}

local function u32(s, at) return (string.unpack(">I4", s, at)) end
local function u16(s, at) return (string.unpack(">I2", s, at)) end

--
-- The boxes in `s` from `from` to `to`, as { type, at, size, body } where
-- `body` is where the contents start. A size of 1 means a 64-bit one
-- follows; 0 means "to the end". A box that says it is longer than what is
-- left, or shorter than its own header, ends the list rather than the read.
--
local function boxes(s, from, to)
  local out, at = {}, from

  while at + 8 <= to + 1 do
    local size, kind = u32(s, at), s:sub(at + 4, at + 7)
    local body = at + 8

    if size == 1 then
      if at + 16 > to + 1 then break end
      size = string.unpack(">I8", s, at + 8)
      body = at + 16
    elseif size == 0 then
      size = to - at + 1
    end

    if size < body - at or at + size - 1 > to then break end

    out[#out + 1] = { type = kind, at = at, size = size, body = body }
    at = at + size
  end

  return out
end

local function find(list, kind)
  for _, b in ipairs(list) do
    if b.type == kind then return b end
  end
end

local function children(s, box)
  return boxes(s, box.body, box.at + box.size - 1)
end

-- A full box's version, and where its contents start after it and its flags.
local function full(s, box)
  return s:byte(box.body), box.body + 4
end

--
-- `avcC`, 14496-15 5.3.3.1: version, profile, compatibility, level, the NAL
-- length's size less one in the low two bits, then the sequence parameter
-- sets and the picture parameter sets, each with a 16-bit length.
--
local function avcc(s, box, track)
  local at = box.body

  if box.size < 15 then return end

  track.profile = s:byte(at + 1)
  track.compat = s:byte(at + 2)
  track.level = s:byte(at + 3)
  track.nal_length = (s:byte(at + 4) & 3) + 1
  track.sps, track.pps = {}, {}

  local n = s:byte(at + 5) & 0x1F
  at = at + 6

  for _ = 1, n do
    local len = u16(s, at)
    track.sps[#track.sps + 1] = s:sub(at + 2, at + 1 + len)
    at = at + 2 + len
  end

  n = s:byte(at) or 0
  at = at + 1

  for _ = 1, n do
    local len = u16(s, at)
    track.pps[#track.pps + 1] = s:sub(at + 2, at + 1 + len)
    at = at + 2 + len
  end
end

-- A descriptor's length: up to four bytes, seven bits each, the top bit
-- saying another follows (14496-1 8.3.3).
local function descriptor_length(s, at)
  local n = 0

  for _ = 1, 4 do
    local b = s:byte(at)
    at = at + 1
    n = (n << 7) | (b & 0x7F)
    if b < 0x80 then break end
  end

  return n, at
end

--
-- `esds`: the ES_Descriptor (tag 3), inside it the DecoderConfigDescriptor
-- (tag 4) with the object type, and inside that the DecoderSpecificInfo
-- (tag 5), which for AAC is the AudioSpecificConfig - whose first five bits
-- are the audio object type (14496-3 1.6.2.1).
--
local function esds(s, box, track)
  local _, at = full(s, box)
  local stop = box.at + box.size - 1

  if s:byte(at) ~= 3 then return end

  local _, after = descriptor_length(s, at + 1)
  local flags = s:byte(after + 2)
  at = after + 3

  if flags & 0x80 ~= 0 then at = at + 2 end               -- depends on an ES
  if flags & 0x40 ~= 0 then at = at + 1 + s:byte(at) end  -- a URL
  if flags & 0x20 ~= 0 then at = at + 2 end               -- an OCR stream

  if at > stop or s:byte(at) ~= 4 then return end

  _, at = descriptor_length(s, at + 1)
  track.object = s:byte(at)
  at = at + 13

  if at > stop or s:byte(at) ~= 5 then return end

  local len
  len, at = descriptor_length(s, at + 1)
  track.config = s:sub(at, at + len - 1)

  if #track.config >= 1 then
    track.aot = track.config:byte(1) >> 3
  end
end

-- The first sample entry of `stsd`: what the track holds and how.
local function stsd(s, box, track)
  local _, at = full(s, box)
  local entries = boxes(s, at + 4, box.at + box.size - 1)
  local e = entries[1]

  if not e then return end

  track.codec = e.type

  if track.kind == "video" and e.size >= 8 + 78 then
    -- The visual sample entry, 14496-12 12.1.3: six reserved and the data
    -- reference, sixteen more, then the width and height.
    track.width = u16(s, e.body + 24)
    track.height = u16(s, e.body + 26)

    local inner = boxes(s, e.body + 78, e.at + e.size - 1)
    local c = find(inner, "avcC")

    if c then avcc(s, c, track) end

    --
    -- **And `esds` here too, which only the audio branch read.**
    --
    -- A video sample entry of `mp4v` says "MPEG-4 systems describes this",
    -- and *which* codec is the object type inside its `esds`: 0x20 is
    -- MPEG-4 Visual, 0x6c is JPEG. Reading it only for audio meant every
    -- `mp4v` film looked alike from here, and a reader that cannot tell
    -- Motion JPEG from MPEG-4 Visual hands both to the same decoder - so
    -- one of them fails as "would not decode" rather than as "this system
    -- has no decoder for that", which is a different sentence and the
    -- honest one.
    --
    local d = find(inner, "esds")

    if d then esds(s, d, track) end
  elseif track.kind == "audio" and e.size >= 8 + 28 then
    -- The audio sample entry, 12.2.3: the channels at 16, the rate a 16.16
    -- number at 24.
    track.channels = u16(s, e.body + 16)
    track.rate = u32(s, e.body + 24) >> 16

    local inner = boxes(s, e.body + 28, e.at + e.size - 1)
    local d = find(inner, "esds")

    if d then esds(s, d, track) end
  end
end

-- A table box's entries: a count, then `width` 32-bit words each.
local function table_of(s, box, width, skip)
  local _, at = full(s, box)
  local out = {}

  at = at + (skip or 0)

  local count = u32(s, at)
  at = at + 4

  for i = 1, count do
    if at + width * 4 - 1 > box.at + box.size - 1 then break end

    local row = {}

    for k = 1, width do
      row[k] = u32(s, at)
      at = at + 4
    end

    out[i] = row
  end

  return out
end

--
-- The sample table, 14496-12 8.5 to 8.7: sizes (`stsz`), which chunk each
-- sample is in (`stsc`, by runs), where each chunk is (`stco` or `co64`),
-- how long each lasts (`stts`, by runs), how far each is shown after it is
-- decoded (`ctts`, by runs), and which can be decoded alone (`stss`; when
-- there is none, every sample can).
--
local function samples(s, stbl, track)
  local list = children(s, stbl)
  local sz = find(list, "stsz")

  if not sz then return {} end

  local _, at = full(s, sz)
  local fixed, count = u32(s, at), u32(s, at + 4)
  local sizes = {}

  for i = 1, count do
    sizes[i] = fixed ~= 0 and fixed or u32(s, at + 8 + (i - 1) * 4)
  end

  local chunks = {}
  local co = find(list, "stco")

  if co then
    for i, row in ipairs(table_of(s, co, 1)) do chunks[i] = row[1] end
  else
    local co64 = find(list, "co64")

    if co64 then
      local _, a = full(s, co64)
      local n = u32(s, a)

      for i = 1, n do
        chunks[i] = string.unpack(">I8", s, a + 4 + (i - 1) * 8)
      end
    end
  end

  local out, sample = {}, 1
  local sc = find(list, "stsc")
  local runs = sc and table_of(s, sc, 3) or {}

  for r, run in ipairs(runs) do
    local last = runs[r + 1] and runs[r + 1][1] - 1 or #chunks

    for chunk = run[1], last do
      local offset = chunks[chunk]

      if not offset then break end

      for _ = 1, run[2] do
        if sample > count then break end

        out[sample] = { at = offset, size = sizes[sample], key = true }
        offset = offset + sizes[sample]
        sample = sample + 1
      end
    end
  end

  -- Times: decoding by `stts`, shown by `ctts` on top (version 1's offsets
  -- are signed, version 0's are not).
  local tt = find(list, "stts")
  local dts, i = 0, 1

  for _, run in ipairs(tt and table_of(s, tt, 2) or {}) do
    for _ = 1, run[1] do
      if out[i] then out[i].dts, out[i].pts = dts, dts end
      dts = dts + run[2]
      i = i + 1
    end
  end

  local ct = find(list, "ctts")

  if ct then
    local version = full(s, ct)

    i = 1

    for _, run in ipairs(table_of(s, ct, 2)) do
      local offset = run[2]

      if version == 1 and offset >= 0x80000000 then
        offset = offset - 0x100000000
      end

      for _ = 1, run[1] do
        if out[i] then out[i].pts = out[i].dts + offset end
        i = i + 1
      end
    end
  end

  local ss = find(list, "stss")

  if ss then
    for _, row in ipairs(out) do row.key = false end

    for _, row in ipairs(table_of(s, ss, 1)) do
      if out[row[1]] then out[row[1]].key = true end
    end
  end

  return out
end

local function track_of(s, trak)
  local track = {}
  local mdia = find(children(s, trak), "mdia")

  if not mdia then return nil end

  local inside = children(s, mdia)
  local hd, hdlr = find(inside, "mdhd"), find(inside, "hdlr")

  if hdlr then
    local handler = s:sub(hdlr.body + 8, hdlr.body + 11)

    track.kind = handler == "vide" and "video"
                 or handler == "soun" and "audio" or handler
  end

  if hd then
    local version, at = full(s, hd)

    if version == 1 then
      track.timescale = u32(s, at + 16)
      track.duration = string.unpack(">I8", s, at + 20)
    else
      track.timescale = u32(s, at + 8)
      track.duration = u32(s, at + 12)
    end
  end

  local minf = find(inside, "minf")
  local stbl = minf and find(children(s, minf), "stbl")

  if stbl then
    local d = find(children(s, stbl), "stsd")

    if d then stsd(s, d, track) end

    track.samples = samples(s, stbl, track)
  else
    track.samples = {}
  end

  return track
end

--
-- The film at `read`, `size` bytes long: its top-level boxes found by their
-- headers, `moov` read whole, and every track in it. Answers { brand,
-- tracks, video, audio } - `video` and `audio` the first of each - or nil
-- and why.
--
function mp4.open(read, size)
  local at, moov, brand = 0, nil, nil

  while at + 8 <= size do
    local head = read(at, 16)

    if not head or #head < 8 then break end

    local len, kind = u32(head, 1), head:sub(5, 8)

    if len == 1 and #head >= 16 then
      len = string.unpack(">I8", head, 9)
    elseif len == 0 then
      len = size - at
    end

    if len < 8 then return nil, "a box at " .. at .. " is shorter than its header" end

    if kind == "ftyp" then
      brand = head:sub(9, 12)
    elseif kind == "moov" then
      moov = read(at, len)

      if not moov or #moov ~= len then
        return nil, "moov could not be read whole"
      end
    end

    at = at + len
  end

  if not moov then return nil, "no moov box - not an MP4, or not a whole one" end

  local film = { brand = brand, tracks = {} }
  local top = boxes(moov, 1, #moov)

  for _, trak in ipairs(children(moov, top[1])) do
    if trak.type == "trak" then
      local track = track_of(moov, trak)

      if track then
        film.tracks[#film.tracks + 1] = track

        if track.kind == "video" and not film.video then film.video = track end
        if track.kind == "audio" and not film.audio then film.audio = track end
      end
    end
  end

  return film
end

-- A track's time in seconds.
function mp4.seconds(track, ticks)
  return ticks / (track.timescale or 1)
end

mp4.CONTAINER = CONTAINER

return mp4
