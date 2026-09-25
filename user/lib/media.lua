-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Media: a file played, whoever is playing it.
--
--   local media = use("/lib/media.lua")
--   local p, why = media.open("/home/song.mp3")
--   p:play()                     -- and p:tick() on the caller's own tick
--   p:seek(90)   p:position()   p:volume(0.5)   p:finished()   p:close()
--   media.tags("/home/song.mp3")   -- { title, artist, album, ..., cover }
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
-- **It never blocks.** `tick` hands over what the server will take and
-- returns at "full", because whoever calls it is a window with a pointer to
-- answer. WAV and MP3.

local audio = use("/lib/audio.lua")
local wav   = use("/lib/wav.lua")
local mp3   = use("/kits/mp3")
local tags  = use("/lib/tags.lua")

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
local READ = 32768

--
-- How many periods one tick may hand over: sixteen, against the 172 a second
-- the device drains and the seventeen ticks a second a window actually gets -
-- 272 of headroom over 172 needed, which is margin rather than a fit.
--
-- A ceiling rather than "until full", so that a server which wrongly always
-- answered "taken" would make the audio run ahead - visible and recoverable -
-- instead of reading the whole file inside one tick and taking the window
-- still with it.
--
local FEED_MAX = 16

local player = {}
player.__index = player

--
-- Everything between the file and the stream emptied, and reading picked up
-- again at byte `at` - for a start, and for a seek.
--
local function restart(p, at)
  p.at = at
  p.last = p.info.offset + p.info.bytes
  p.carry, p.samples, p.pending, p.phase = "", "", "", 0.0

  if p.decoder then p.decoder:reset() end
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
    return use("/lib/video.lua").open(path, options)
  end

  local fmt = audio.format()

  if fmt.period == 0 then return nil, "this machine has no sound device" end

  -- Sized to the read, which is not a detail: `fs.read_into` writes what it
  -- is asked for, and asking for 32 KB into one page is a buffer overrun with
  -- the length written three lines away from the allocation.
  local page = sys.memory(READ // 4096)

  if not page then return nil, "no memory for a read buffer" end

  local function read_at(off, n)
    local got = fs.read_into(path, page, off, n)

    if not got or got == 0 then return nil end

    return sys.region_read(page, 0, got)
  end

  local format = path:lower():match("%.mp3$") and "MP3" or "WAV"
  local info, why, decoder

  if format == "MP3" then
    --
    -- An MP3 has no header, only a run of frames, so the first frame *is* the
    -- header - and finding it means decoding one. `mp3.probe` does that with
    -- a decoder of its own, so the real one does not start life holding half
    -- a frame of somebody else's overlap. 32 KB is plenty to find it in: a
    -- frame is at most 1728 bytes, and an ID3v2 tag ahead of it is usually a
    -- few kilobytes of cover.
    --
    local head = read_at(0, READ)

    if not head then
      sys.release(page)
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
      decoder = mp3.decoder()
    end
  else
    info, why = wav.scan(read_at)
  end

  if not info then
    sys.release(page)
    return nil, tostring(why)
  end

  info.format = format

  local name = path:match("([^/]+)$") or path
  local stream, serr = audio.open(name)

  if not stream then
    sys.release(page)
    return nil, tostring(serr)
  end

  local p = setmetatable({
    path = path, name = name, info = info, fmt = fmt, page = page,
    stream = stream, decoder = decoder, playing = false, base = 0,
  }, player)

  restart(p, info.offset)
  return p
end

function player:play()
  self.playing = true
end

-- Nothing more is handed over; what the ring and the device hold, about
-- 70 ms, plays out, and the position stops where the sound did.
function player:pause()
  self.playing = false
end

--
-- Hand over as much as the server will take, and not one period more. Call
-- it on every tick; it returns how many periods went.
--
function player:tick()
  if not self.stream or not self.playing then return 0 end

  local fmt, info = self.fmt, self.info
  local fed = 0

  for _ = 1, FEED_MAX do
    if #self.pending == 0 then
      if self.at < self.last and #self.carry < READ then
        local n = fs.read_into(self.path, self.page, self.at,
                               math.min(READ, self.last - self.at))

        if not n or n == 0 then
          self.at = self.last
          break
        end

        self.carry = self.carry .. sys.region_read(self.page, 0, n)
        self.at = self.at + n
      end

      --
      -- Decode, for a format that needs it. A WAV's bytes are already samples
      -- and go straight through.
      --
      if self.decoder then
        if #self.samples < fmt.period * 4 and #self.carry > 0 then
          local pcm, used = self.decoder:decode(self.carry, fmt.period * 8)

          if used == 0 then
            -- Not a whole frame yet. If the file is finished there will never
            -- be one, so stop rather than spin on the same bytes.
            if self.at >= self.last then self.carry = "" end
            break
          end

          self.samples = self.samples .. pcm
          self.carry = self.carry:sub(used + 1)
        end
      else
        self.samples, self.carry = self.carry, ""
      end

      --
      -- The end of the file is the end of the input: everything read, and
      -- nothing left to decode. Said to `sys.pcm`, so the final frame is
      -- played rather than held for a neighbour that never comes - and one
      -- frame is then enough to convert, where two are needed while there
      -- is more to come. The last sample of every song went unplayed until
      -- `run_film.py` heard a film's go missing.
      --
      local last = self.at >= self.last
                   and (not self.decoder or #self.carry < self.info.frame * 2)
      local frame_bytes = info.channels * (info.bits // 8)

      if #self.samples < frame_bytes * (last and 1 or 2) then break end

      local pcm, used
      pcm, used, self.phase = sys.pcm(self.samples, info.rate, info.channels,
                                      info.bits, self.phase, fmt.period * 4,
                                      last)

      if used == 0 or #pcm == 0 then break end

      self.samples = self.samples:sub(used + 1)

      -- What the resampler did not take goes back, so a WAV keeps its one
      -- buffer and the next turn reads on from where this one stopped.
      if not self.decoder then self.carry, self.samples = self.samples, "" end

      self.pending = pcm
    end

    local took, why = self.stream:play(self.pending:sub(1, fmt.period))

    if not took then
      if why ~= "full" then
        self.error = tostring(why)
        self.playing = false
      end

      return fed                  -- the server is ahead; come back next tick
    end

    self.pending = self.pending:sub(fmt.period + 1)
    fed = fed + 1
  end

  return fed
end

--
-- Seconds heard: where the last seek landed, and the frames out of the
-- speaker since - at the device's rate, since `sys.pcm` has converted to it.
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
-- the decoder is reset there and finds the next frame by itself, since
-- `decode` consumes bytes that are not a frame and gives no samples for them.
--
-- **The stream is closed and opened again**, because nothing can drop what a
-- stream has queued: there is no flush in `audioproto.h`. The ring and the
-- device hold about 70 ms of the old place, which a new stream discards and
-- carrying on would play.
--
function player:seek(seconds)
  local info = self.info

  seconds = math.max(0, math.min(seconds, info.seconds))

  local at

  if self.decoder then
    at = info.offset + math.floor(seconds * (info.bitrate or 0) * 1000 / 8)
  else
    at = info.offset + math.floor(seconds * info.rate) * info.frame
  end

  at = math.min(at, info.offset + info.bytes)

  if self.stream then self.stream:close() end

  local fresh, why = audio.open(self.name)

  self.stream = fresh

  if not fresh then
    self.error = tostring(why)
    self.playing = false
    return false, self.error
  end

  if self.gain then audio.set{ stream = fresh.id, gain = self.gain } end

  self.base = seconds
  restart(self, at)
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
-- All of it handed over, and all of it played. Not the same thing: the
-- server still holds up to four periods after the last one is handed over,
-- and letting go at the first would cut the last twenty milliseconds off
-- every file.
--
function player:finished()
  if not self.stream then return false end

  return self.at >= self.last and #self.pending == 0
         and #self.carry < self.info.frame * 2
         and self.stream:queued() == 0
end

--
-- What a file says about itself - title, artist, album, genre, year, track,
-- and where its cover is - read from the file and never written onto it
-- (`/lib/tags.lua`). An empty table when it says nothing, and nil with why
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
  local reply, err = fs.send("/app/wm",
                             { type = "picture", name = name,
                               mime = t.cover.mime, bytes = bytes }, region)

  -- The compressed copy was scratch: the compositor holds the pixels now.
  sys.release(region)

  if not reply then return nil, tostring(err) end

  return name, reply.w, reply.h
end

-- The stream, and the pages it read through, given back.
function player:close()
  if self.stream then self.stream:close() end
  if self.page then sys.release(self.page) end

  self.stream, self.page, self.playing = nil, nil, false
end

return media
