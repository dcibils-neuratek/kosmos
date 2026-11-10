-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Media: a file played, whoever is playing it.
--
--   local media = use("/Kosmos/Libraries/media.lua")
--   local p, why = media.open("/Home/song.mp3")
--   p:play()                     -- and p:tick() on the caller's own tick
--   p:seek(90)   p:position()   p:volume(0.5)   p:finished()   p:close()
--   media.tags("/Home/song.mp3")   -- { title, artist, album, ..., cover }
--
-- **The engine under Music, and under a video app later** (`docs/music.html`,
-- decided with Diego on 14 September 2026). Playing a file is the same work
-- whatever window is around it: read it a window at a time, decode it,
-- convert it to what the device takes, feed the audio server without waiting
-- on it, keep time, and let a person seek. That work lived inside `music.lua`;
-- it lives here so a second app adds to it rather than copying it.
--
-- **Time is the sound's.** `position` is frames that came out of the
-- speaker, as the audio server publishes them into the stream's ring, plus
-- where the last seek landed - not what was handed over, which runs ahead by
-- the ring and the device together. A picture timed to anything else drifts.
--
-- **The sound is played on a thread of its own** (`docs/playback.md`): the
-- Playback Kit's, in the audio band when the program declared `kosmos:
-- needs audio`, decoding, converting and writing into the stream's ring.
-- This file's part is the file: `tick` reads it a piece at a time and hands
-- the pieces to the kit while it has room - a megabyte, a minute of a song
-- - so a window held away for seconds is not heard. It was `media.feed`,
-- in Lua, a period at a time from the window's own loop (`roadmap.md` 6zw).
--
-- **It never blocks.** `tick` hands over what the kit will take and returns,
-- because whoever calls it is a window with a pointer to answer. WAV and MP3.

local audio = use("/Kosmos/Libraries/audio.lua")
local wav   = use("/Kosmos/Libraries/wav.lua")
local mp3   = use("/Kosmos/Kits/mp3")
local tags  = use("/Kosmos/Libraries/tags.lua")
local regions = use("/Kosmos/Libraries/regions.lua")

local media = {}

--
-- How much of the file to fetch at a time: 32 KB, about 190 ms of audio,
-- fetched roughly five times a second.
--
-- This was one page, on the reasoning that a window has a tick to keep and a
-- big read inside one is a stutter. The instrument said otherwise: a page is
-- a tenth of a second of audio, so it meant forty-odd round trips to the disk
-- a second, and each tick that did one took long enough that the tick rate
-- collapsed from 250 a second to ten. Twelve periods a tick times ten ticks is
-- 120 a second against the 172 the device drains, and the music played at two
-- thirds speed. Bigger and rarer wins: most ticks touch no disk at all.
--
local READ = 65536

--
-- How much of it goes to the kit as one piece: 16 KB, the most an MP3's or
-- a WAV's piece may be (`playback_kosmos.c` says why).
--
local PIECE = 16384

local player = {}
player.__index = player

local function format_is_mp3(info)
  return info.format == "MP3"
end

--
-- Reading picked up again at byte `at` - for a start, and for a seek. What
-- the kit held of the old place it forgets at its `seek`.
--
local function restart(p, at)
  p.at = at
  p.last = p.info.offset + p.info.bytes
  p.held, p.held_n, p.ended = 0, 0, false
end

--
-- **The kit's input kept full**: the file a window at a time into the read
-- buffer, and from there to the kit in pieces of `PIECE`, while it has room.
-- An MP3's and a WAV's bytes are a stream, so a piece is any length; the kit
-- takes no larger one (`playback_kosmos.c`). Returns how many pieces went.
--
local function fill(p)
  local put = 0

  while true do
    if p.held_n == 0 then
      if p.at >= p.last then break end

      local n = fs.read_into(p.path, p.page.cap, p.at, math.min(READ, p.last - p.at))

      if not n or n == 0 then
        p.at = p.last
        break
      end

      p.at, p.held, p.held_n = p.at + n, 0, n
    end

    local n = math.min(PIECE, p.held_n)

    if not p.player:frame(p.page.at + p.held, n) then return put end

    p.held, p.held_n = p.held + n, p.held_n - n
    put = put + 1
  end

  if not p.ended then p.ended = p.player:ended() end

  return put
end

--
-- A file, and a stream to play it on - paused until `play`. Nil and why when
-- it cannot be either.
--
--
-- **Films come through this door too**, and that is a decision rather than
-- a convenience. Diego, 20 September, asked whether the video work was "a
-- video kit or media kit that also has audio decoding and playing
-- abilities", and left the answer to me. It is one Media Kit, for one
-- reason that outweighs the rest: **a film's picture and its sound have to
-- agree on a clock, and neither half can hold that agreement alone.** Two
-- kits would mean every application that plays a film writes its own sync -
-- which is the wheel this exists so that nobody reinvents. BeOS called the
-- same thing the Media Kit for the same reason, and this system already
-- borrows that shelf of names.
--
-- **The picture half is a file of its own** (`video.lua`), loaded here only
-- when a film is opened - so Music, which opens songs, never pays for an
-- MP4 reader or a JPEG decoder it will not use. One door to learn, and no
-- cost to what comes through the other side of it.
--
local FILMS = { mp4 = true, m4v = true, mov = true }

function media.open(path, options)
  local kind = tostring(path):lower():match("%.(%w+)$")

  if kind and FILMS[kind] then
    return use("/Kosmos/Libraries/video.lua").open(path, options)
  end

  local fmt = audio.format()

  if fmt.period == 0 then return nil, "this machine has no sound device" end

  -- Sized to the read, which is not a detail: `fs.read_into` writes what it
  -- is asked for, and asking for 64 KB into one page is a buffer overrun with
  -- the length written three lines away from the allocation.
  local page = regions.make(READ)

  if not page then return nil, "no memory for a read buffer" end

  local function read_at(off, n)
    local got = fs.read_into(path, page.cap, off, n)

    if not got or got == 0 then return nil end

    return sys.region_read(page.cap, 0, got)
  end

  local format = path:lower():match("%.mp3$") and "MP3" or "WAV"
  local info, why

  if format == "MP3" then
    --
    -- An MP3 has no header, only a run of frames, so the first frame *is* the
    -- header - and finding it means decoding one. `mp3.probe` does that with
    -- a decoder of its own. 64 KB is plenty to find it in: a frame is at most
    -- 1728 bytes, and an ID3v2 tag ahead of it is usually a few kilobytes of
    -- cover.
    --
    local head = read_at(0, READ)

    if not head then
      regions.free(page)
      return nil, "cannot read " .. path
    end

    info, why = mp3.probe(head)

    if info then
      --
      -- The whole file after the tag is audio. How long it runs comes from a
      -- Xing or Info header when the encoder wrote one - `mp3.probe` reads
      -- its frame count, exact, and the average bitrate beside it - and from
      -- the bitrate otherwise: exact for a constant-bitrate file and an
      -- estimate for a variable one, because an MP3 is not required to write
      -- its length down anywhere.
      --
      info.bytes = ((fs.getattr(path) or {}).size or 0) - info.offset
      info.frame = 1

      if not info.seconds then
        info.seconds = (info.bitrate > 0)
                       and (info.bytes * 8 / (info.bitrate * 1000)) or 0
      end
    end
  else
    info, why = wav.scan(read_at)
  end

  if not info then
    regions.free(page)
    return nil, tostring(why)
  end

  info.format = format

  --
  -- The kit that plays it: told an MP3 is one, which finds its own rate in
  -- its frames, and told a WAV's format, since its bytes are samples.
  --
  local playback = use("/Kosmos/Kits/playback")
  local kit, kwhy

  if format == "MP3" then
    kit, kwhy = playback.open{ kind = "mp3" }
  else
    kit, kwhy = playback.open{ kind = "wav", rate = info.rate,
                               channels = info.channels, bits = info.bits }
  end

  if not kit then
    regions.free(page)
    return nil, tostring(kwhy)
  end

  local name = path:match("([^/]+)$") or path
  local stream, serr = audio.open(name)

  if not stream then
    kit:close()
    regions.free(page)
    return nil, tostring(serr)
  end

  local ok, swhy = pcall(kit.seek, kit, stream.ring)

  if not ok then
    kit:close()
    stream:close()
    regions.free(page)
    return nil, tostring(swhy)
  end

  local p = setmetatable({
    path = path, name = name, info = info, fmt = fmt, page = page,
    stream = stream, player = kit, heard = {}, playing = false, base = 0,
    rate = info.rate, channels = info.channels, bits = info.bits,
  }, player)

  restart(p, info.offset)
  fill(p)
  return p
end

function player:play()
  self.playing = true
  self.player:play()
end

-- Nothing more is written; what the ring and the device hold, about 70 ms,
-- plays out, and the position stops where the sound did.
function player:pause()
  self.playing = false
  self.player:pause()
end

--
-- The kit's input kept full. Call it on every tick; it returns how many
-- pieces of the file went. A frame the kit would not decode is said in
-- `error`, and the song goes on past it.
--
function player:tick()
  if not self.stream then return 0 end

  local put = fill(self)
  local h = self.player:state(self.heard)

  if h.refused > 0 and h.why ~= "" then self.error = h.why end

  return put
end

--
-- Seconds heard: where the last seek landed, and the frames out of the
-- speaker since - at the device's rate, which the kit converts to.
--
function player:position()
  if not self.stream then return self.base end

  return self.base + self.stream:position() / self.fmt.rate
end

--
-- Somewhere else in the file.
--
-- A WAV's place is exact. An MP3's comes from the bitrate, as its length
-- does - exact for a constant bitrate, an estimate for a variable one - and
-- the kit's decoder starts again there and finds the next frame by itself.
--
-- **The stream is closed and opened again**, because nothing can drop what a
-- stream has queued: there is no flush in `audioproto.h`. The ring and the
-- device hold about 70 ms of the old place, which a new stream discards and
-- carrying on would play. The kit is told first, and has let go of the old
-- ring before it is closed.
--
function player:seek(seconds)
  local info = self.info

  seconds = math.max(0, math.min(seconds, info.seconds))

  local at

  if format_is_mp3(info) then
    at = info.offset + math.floor(seconds * (info.bitrate or 0) * 1000 / 8)
  else
    at = info.offset + math.floor(seconds * info.rate) * info.frame
  end

  at = math.min(at, info.offset + info.bytes)

  local fresh, why = audio.open(self.name)

  if not fresh then
    self.error = tostring(why)
    self.playing = false
    self.player:pause()
    return false, self.error
  end

  if self.gain then audio.set{ stream = fresh.id, gain = self.gain } end

  local ok, swhy = pcall(self.player.seek, self.player, fresh.ring)

  if not ok then
    fresh:close()
    self.error = tostring(swhy)
    return false, self.error
  end

  if self.stream then self.stream:close() end

  self.stream = fresh
  self.base = seconds
  restart(self, at)
  fill(self)
  return true
end

--
-- Loudness, 0 to 1: this stream's share of the audio server's gain, kept
-- across a seek.
--
function player:volume(level)
  self.gain = math.floor(math.max(0, math.min(1, level)) * 256 + 0.5)

  if not self.stream then return false, "nothing is playing" end

  return audio.set{ stream = self.stream.id, gain = self.gain }
end

--
-- How loud what is coming out is, 0 to 1, off the server's own peak - the
-- number the Mixer draws, and the honest answer to "is this really coming
-- out", which a moving bar is not. A round trip, so ask it when painting
-- rather than on every tick.
--
function player:peak()
  if not self.stream then return 0 end

  for _, one in ipairs(audio.streams() or {}) do
    if one.stream == self.stream.id then return (one.peak or 0) / 32767 end
  end

  return 0
end

--
-- All of it handed over, written and played. Not the same thing: the ring
-- and the server still hold periods after the last one is written, and
-- letting go at the first would cut the last of every file.
--
function player:finished()
  if not self.stream then return false end

  return self.ended and self.player:state(self.heard).finished
         and self.stream:queued() == 0
end

--
-- What a file says about itself - title, artist, album, genre, year, track,
-- and where its cover is - read from the file and never written onto it
-- (`/Kosmos/Libraries/tags.lua`). An empty table when it says nothing, and nil with why
-- when it cannot be read at all.
--
-- Through one page, read a window at a time as `tags.lua` asks: a tag's
-- frames are small, and a cover is found in place rather than read, so a
-- Library of a thousand songs reads a thousand beginnings and no pictures.
--
function media.tags(path)
  local size = (fs.getattr(path) or {}).size

  if not size then return nil, "no such file: " .. tostring(path) end

  local page = sys.memory(1)

  if not page then return nil, "no memory for a read buffer" end

  local function read(offset, n)
    local out = {}

    while n > 0 do
      local want = math.min(n, 4096)
      local got = fs.read_into(path, page, offset, want)

      if not got or got == 0 then break end

      out[#out + 1] = sys.region_read(page, 0, got)
      offset, n = offset + got, n - got

      if got < want then break end
    end

    return (#out > 0) and table.concat(out) or nil
  end

  local ok, t = pcall(tags.read, read, size)

  sys.release(page)

  if not ok then return nil, tostring(t) end

  return t
end

--
-- **The picture inside a file, handed to the window manager as pages.**
--
-- `tags.read` says where the cover is and how long it is and stops there, so
-- that listing a library costs no pictures. This is the other half, asked for
-- one song at a time: the bytes into a region, the region to the compositor
-- with a name, and `ui.image` draws that name like any other picture.
--
-- Returns the name, and its size, so a caller can place it before it is
-- drawn.
--
function media.cover(path)
  local t, why = media.tags(path)

  if not t then return nil, why end

  if not t.cover or not t.cover.bytes then
    return nil, "this file carries no picture"
  end

  local bytes = tonumber(t.cover.bytes) or 0

  if bytes <= 0 then return nil, "the picture in this file is empty" end

  local region = sys.memory((bytes + 4095) // 4096)

  if not region then return nil, "no memory for a picture that size" end

  --
  -- One read, from where the tag says the picture begins. `read_into` puts
  -- what it read at the region's start rather than at the offset it was
  -- given, so a second call would land on top of the first instead of after
  -- it - a short read is said rather than stitched.
  --
  local got = fs.read_into(path, region, t.cover.offset, bytes)

  if not got or got < bytes then
    sys.release(region)

    return nil, "could only read " .. tostring(got) .. " of " .. bytes
                .. " bytes of the picture"
  end

  local name = "cover:" .. path
  local reply, err = fs.send("/Running/wm",
                             { type = "picture", name = name,
                               mime = t.cover.mime, bytes = bytes }, region)

  -- The compressed copy was scratch: the compositor holds the pixels now.
  sys.release(region)

  if not reply then return nil, tostring(err) end

  return name, reply.w, reply.h
end

-- The stream, and the pages it read through, given back.
-- The kit's thread stopped first, so nothing writes into the ring as the
-- stream goes.
function player:close()
  if self.player then self.player:close() end
  if self.stream then self.stream:close() end
  if self.page then regions.free(self.page) end

  self.player, self.stream, self.page, self.playing = nil, nil, nil, false
end

return media
