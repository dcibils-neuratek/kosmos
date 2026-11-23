-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The play queue: songs decoded by the Media Kit and played by the
-- Playback Kit one after another, joined without a gap.
--
--   local playqueue = use("/Kosmos/Libraries/playqueue.lua")
--   local q, why = playqueue.open{ name = "Music" }
--   q:set(songs, 3)          -- { { path =, title = ... }, ... }, from the third
--   q:play()  q:pause()  q:toggle()
--   q:next()  q:previous()  q:seek(seconds)  q:jump(index)
--   q:repeating("off" | "all" | "one")  q:shuffle(true | false)
--   q:volume(0.8)
--   q:tick()                 -- on the caller's clock: decode ahead, follow
--   q:now()                  -- what is heard: { index, song, at, length,
--                            --   playing, ended, source, stream, why }
--   q:stream_id()            -- the stream, for `audio.streams()`'s `exact`
--   q.on_heard = function(q, index, was, natural) end
--   q:close()
--   playqueue.facts(path)    -- a song's tags and its stream's format, for
--                            -- `library.open`'s `tags_of`
--
-- **The door `docs/player.md` §3 draws for the Playback Kit's queue**
-- (`playback.queue{}`, "the door also exists in C"), in Lua, until the kit
-- has it. §6 item 4 puts the queue in the kit, its feeder a C thread at
-- NORMAL; P3 built the kit's half of it - decoded songs, their records,
-- the trimming and the joins (`playback.h`) - and not the feeder. So this
-- is the feeder, on the window's clock, as `video.lua` feeds a film's
-- sound: decoded on every pass of the caller's loop as far ahead as the
-- kit's input ring takes, which is **eight seconds** (`PLAYBACK_AHEAD_
-- SECONDS`), so a window held away by a paint, a collection or a busy
-- machine for less than that is never heard. When the C door comes the
-- calls above stay, and what they reach moves under them.
--
-- **What the feeder does**, and nothing more, since everything per sample
-- is C (`player.md`, *C or Lua*):
--
--   - a song opened by the Media Kit (`media.open`), its sound decoded
--     **untrimmed** (decision 10) in the decoder's own format - `s16`, or
--     `s32` with a 24-bit source in its top bits, or `f32` for a lossy
--     codec whose output is float - and never through float for an
--     integer source (`player.md` §4, 3);
--   - its record handed to the kit first - the format, the rate, and the
--     encoder's delay and the song's length as `tags.lua` read them: LAME's
--     delay plus the decoder's 529 for an MP3, `iTunSMPB`'s priming as
--     written for an AAC, nothing for FLAC and WAV - and its samples after,
--     as the decoder's own memory, never a Lua string (`p:frame(at, n)`);
--   - the kit trims, joins songs of one format sample for sample, and marks
--     each song at its first frame heard; a song of another format stops it
--     (`state.change`), and the feeder opens a stream at the new format and
--     goes on with `continue` once the old one has played out - the one
--     place a gap is allowed (`player.md` §6, Playback Kit 5);
--   - what is heard is the kit's marks against the stream's own count of
--     frames out of the speaker, so the song the window shows changes at the
--     sample it changes.
--
-- **A new stream for every start** - a play, a seek, the next song chosen -
-- as `video.lua` does: nothing can drop what a stream holds (`audioproto.h`
-- has no flush), so the kit is told first and lets go of the old ring, and
-- the old stream is closed.
--
-- **Bounded on the caller's clock.** `tick` decodes for a few milliseconds
-- at most, or until the ring is full, and returns; it never waits on the
-- disk for long - a packet is a few kilobytes of a file the Storage Kit
-- reads in pieces - and never on the audio server but at a start, where the
-- stream is opened.
--
-- In `media.elf` only, where the Media Kit is: a program using it says
-- `-- kosmos: image media` and `-- kosmos: needs audio` in its header.

local audio = use("/Kosmos/Libraries/audio.lua")

local playqueue = {}

-- How long one `tick` may decode for, in milliseconds, before it gives the
-- caller its loop back. The ring holds seconds; a pass that comes back
-- with it not full only means the next pass goes on.
playqueue.BUDGET_MS = 6

-- How deep a song's stream ring is: Music's default, 8 periods (`audio.lua`
-- says why a player of songs is the other end of a film's 32).
local PERIODS = 8

-- The format the kit's input ring is sized for: the largest a queue is
-- expected to play, 96 kHz stereo held as 32 bits - 6.9 MB for eight
-- seconds. A 192 kHz song is played with four seconds ahead
-- (`playback_open_pcm`).
local RING = { format = "s32", rate = 96000, channels = 2,
               seconds = 8 }

-- An MP3 decoded untrimmed starts this much later than LAME's delay says
-- (`PLAYBACK_MP3_DECODER_DELAY`, which the kit's Lua door does not
-- publish yet: `playback.h`'s number, written here once).
local MP3_DECODER_DELAY = 529

-- Pressing Previous this far into a song starts it again instead.
local AGAIN_S = 3

--------------------------------------------------------------------------
-- The doors, on first use, and only where the image has them.
--------------------------------------------------------------------------

local kits = {}

local function kit(name)
  if kits[name] == nil then
    local ok, k = pcall(use, "/Kosmos/Kits/" .. name)

    kits[name] = (ok and type(k) == "table") and k or false
  end

  return kits[name] or nil
end

local NO_KIT = "the play queue decodes with the Media Kit, which is in media.elf; "
               .. "this program is not in it (its header wants `-- kosmos: image media`)"

--------------------------------------------------------------------------
-- A song's sound, as a person reads it.
--------------------------------------------------------------------------

--
-- **The format's name, from the codec the Media Kit names** (FFmpeg's)
-- and the file's extension where the codec is only PCM: a WAV and an AIFF
-- are both `pcm_*`.
--
local CODECS = { flac = "FLAC", alac = "ALAC", aac = "AAC", mp3 = "MP3",
                 mp3float = "MP3", vorbis = "Vorbis", opus = "Opus" }
local PCM_BY_EXTENSION = { wav = "WAV", aif = "AIFF", aiff = "AIFF", aifc = "AIFF" }

function playqueue.format_name(codec, path)
  codec = tostring(codec or "")

  if CODECS[codec] then return CODECS[codec] end

  if codec:match("^pcm_") then
    local ext = tostring(path or ""):lower():match("%.(%w+)$")

    return PCM_BY_EXTENSION[ext or ""] or "PCM"
  end

  return codec ~= "" and codec:upper() or nil
end

-- The first sound in a source, with its index (0 first, as the kit counts).
local function sound_of(src)
  for i, st in ipairs(src:streams()) do
    if st.kind == "audio" then return st, i - 1 end
  end

  return nil
end

--
-- **The format a song is decoded in**: what the decoder hands back by
-- itself where the kit has heard it say (`pcm`); otherwise an integer
-- source as integers - 16 bits as `s16`, deeper as `s32` - and float for a
-- float source or a lossy one, whose decoder's output is float.
--
local function decoded_format(st)
  if st.pcm then return st.pcm end
  if st.source_float then return "f32" end

  if st.lossless then
    return (st.source_bits or 16) > 16 and "s32" or "s16"
  end

  return "f32"
end

--
-- **A song's facts, for the library** (`library.open{ tags_of = ... }`):
-- what `tags.lua` reads of the file - through `media.tags`, the one door
-- that reads a file's start for it - and what only the stream can say,
-- from the Media Kit: the format, its rate, depth and channels, and the
-- length. The library keeps them as attributes, so Tracker and the Open
-- window read them without this image (`player.md`, decision 3).
--
function playqueue.facts(path)
  local t = use("/Kosmos/Libraries/media.lua").tags(path) or {}
  local mk = kit("media")

  if not mk then return t end

  local src = mk.open(path)

  if not src then return t end

  local st = sound_of(src)

  if st then
    t.format = t.format or playqueue.format_name(st.codec, path)
    t.codec = t.codec or st.codec
    t.rate = t.rate or st.rate
    t.channels = t.channels or st.channels
    t.bits = t.bits or ((st.lossless and (st.source_bits or 0) > 0) and st.source_bits or nil)

    if st.duration_us and st.duration_us > 0 then
      t.duration_ms = t.duration_ms or st.duration_us // 1000
    end
  end

  src:close()
  return t
end

--------------------------------------------------------------------------
-- The queue.
--------------------------------------------------------------------------

local Queue = {}
Queue.__index = Queue

--
-- A queue named `name` in the Mixer, with nothing in it. Nil and why where
-- nothing can be played: no Media Kit in this image, no sound device.
--
function playqueue.open(o)
  o = o or {}

  if not kit("media") or not kit("playback") then return nil, NO_KIT end

  if (audio.format().period or 0) == 0 then
    return nil, "this machine has no sound device"
  end

  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

  return setmetatable({
    name = o.name or "Music",
    hz = hz,
    list = {},            -- the songs, as given
    order = {},           -- the order they play in: indices into `list`
    pos = nil,            -- where in `order` the song heard is
    repeat_mode = "off",
    shuffled = false,
    gain = nil,           -- 0 to 256 once `volume` is set
    serial = 0,           -- every song handed to the kit has a number of its own
    ids = {},             -- that number -> { pos, index, rate }
    heard = {},           -- the kit's state, read into one table
    playing = false,
    ended = false,
  }, Queue)
end

-- The list index of the song at place `pos` of the order.
function Queue:at_pos(pos)
  return pos and self.order[pos]
end

-- The place after `pos`, as repeat and the order say; nil at the end.
function Queue:after(pos)
  if not pos then return nil end
  if self.repeat_mode == "one" then return pos end
  if pos < #self.order then return pos + 1 end
  if self.repeat_mode == "all" and #self.order > 0 then return 1 end

  return nil
end

-- The order, from `first` on: in the list's order, or shuffled with
-- `first` kept at the front so what is playing goes on playing.
local function make_order(n, first, shuffled)
  local order = {}

  if not shuffled then
    for i = 1, n do order[i] = i end
    return order, first
  end

  local rest = {}

  for i = 1, n do
    if i ~= first then rest[#rest + 1] = i end
  end

  -- Fisher and Yates, on the counter's low bits as a seed: an order that
  -- differs each time it is asked for, which is all a shuffle owes anyone.
  local seed = sys.ticks()

  for i = #rest, 2, -1 do
    seed = (seed * 1103515245 + 12345) & 0x7fffffff
    local j = seed % i + 1

    rest[i], rest[j] = rest[j], rest[i]
  end

  order[1] = first

  for _, i in ipairs(rest) do order[#order + 1] = i end

  return order, 1
end

--------------------------------------------------------------------------
-- Decoding one song.
--------------------------------------------------------------------------

--
-- **A song opened to be decoded** from `from_s` seconds: its source, its
-- decoder untrimmed, the format it comes out in and the record the kit is
-- handed before its samples. Nil and why for a song that cannot be
-- played, which the queue passes over and says.
--
function Queue:open_song(pos, from_s)
  local index = self.order[pos]
  local song = self.list[index]
  local mk = kit("media")
  local path = type(song) == "table" and song.path or song

  local src, why = mk.open(path)

  if not src then return nil, tostring(why) end

  local st, si = sound_of(src)

  if not st then
    src:close()
    return nil, "there is no sound in " .. tostring(path)
  end

  if (st.channels or 0) < 1 or st.channels > 2 then
    src:close()
    return nil, ("%d channels: the Playback Kit plays one or two, and mixing down "
                 .. "is not built yet"):format(st.channels or 0)
  end

  local dec, derr = src:decoder(si, 1, "untrimmed")

  if not dec then
    src:close()
    return nil, tostring(derr)
  end

  local fmt = decoded_format(st)
  local bits = (st.lossless and (st.source_bits or 0) > 0) and st.source_bits or 0
  local playback = kit("playback")

  --
  -- The encoder's delay and the song's length, as the file says them
  -- (`tags.lua`, `gapless`), and the decoder's own delay added for an MP3:
  -- frames counted in the decoded stream, priming included (`playback.h`).
  --
  local t = use("/Kosmos/Libraries/media.lua").tags(path) or {}
  local g = type(t.gapless) == "table" and t.gapless or nil
  local delay, length = 0, 0

  if g then
    local codec = tostring(st.codec)

    if codec == "mp3" or codec == "mp3float" then
      delay = (g.source == "LAME") and ((g.delay or 0) + MP3_DECODER_DELAY) or (g.delay or 0)
    else
      delay = g.delay or 0
    end

    length = g.length or 0
  end

  local from = 0

  if from_s and from_s > 0 then
    from = math.floor(from_s * st.rate + 0.5)
    src:seek(math.floor(from_s * 1e6))
  end

  self.serial = self.serial + 1

  local source = {
    format = playqueue.format_name(st.codec, path), codec = st.codec,
    rate = st.rate, channels = st.channels, bits = bits ~= 0 and bits or nil,
    lossless = st.lossless and true or false, bitrate = st.bitrate,
    duration = (st.duration_us or 0) / 1e6,
  }

  if type(song) == "table" then song.source = source end

  self.ids[self.serial] = { pos = pos, index = index, rate = st.rate, source = source }

  return {
    pos = pos, index = index, path = path, src = src, dec = dec, si = si,
    fmt = fmt, rate = st.rate, channels = st.channels, bits = bits,
    stream_bits = playback.stream_bits(fmt, bits),
    record = { song = self.serial, format = fmt, rate = st.rate, channels = st.channels,
               bits = bits, delay = delay, length = length, first = 0, from = from },
    from = from, sent = false, pending = nil, held = nil, sent_end = false,
    drained = false, source = source,
  }
end

function Queue:close_song(d)
  if not d then return end
  if d.held then d.held:free() d.held = nil end
  if d.dec then d.dec:close() d.dec = nil end
  if d.src then d.src:close() d.src = nil end
end

--
-- Where the decoder landed, in decoded frames, from the first sound's time:
-- 0 for a song from its start - where the time it says is taken as its
-- origin, since an MP4's edit list makes a song's first time the priming's
-- and not nought - and, after a seek, its time less that origin.
--
local function first_frame(d, song, pts)
  local known = pts ~= nil and pts ~= math.mininteger
  local frames = known and math.floor(pts * d.rate / 1e6 + 0.5) or nil

  if d.from == 0 then
    if type(song) == "table" then song.origin = frames or 0 end
    return 0
  end

  if not frames then return d.record.delay + d.from end

  local origin = type(song) == "table" and song.origin or 0

  return math.max(0, frames - origin)
end

--
-- **The kit's input kept full**, for at most `deadline`: the song's next
-- sound decoded and handed over - its record first - and, when a song has
-- all gone, the next one opened; the end said after the last.
--
function Queue:feed(deadline)
  local p = self.player

  while sys.ticks() < deadline do
    local d = self.decoding

    if not d then
      if self.decoded_last then
        if not self.ended_sent then self.ended_sent = p:ended() end
        return
      end

      local next_pos = self:after(self.decode_pos)

      if not next_pos then
        self.decoded_last = true
      else
        local opened, why = self:open_song(next_pos, 0)

        self.decode_pos = next_pos

        if opened then
          self.decoding = opened
        else
          self:said(why, next_pos)

          -- A song that would not open is passed over - and so is the
          -- whole of a list where none will, rather than going round it.
          self.failed = (self.failed or 0) + 1
          if self.failed >= #self.order then self.decoded_last = true end
        end
      end
    elseif d.pending then
      if not d.sent then
        if not p:song(d.record) then return end
        d.sent = true
      end

      local pend = d.pending

      if not p:frame(pend.at, pend.len) then return end

      d.pending = nil
      self.failed = 0
    elseif d.drained then
      self:close_song(d)
      self.decoding = nil
    else
      local at, len, pts, _, fname = d.dec:sound(d.fmt)

      if at == false then
        local packet, why = d.held, nil

        d.held = nil

        if not packet then
          -- The next packet of this song's sound; any other stream's - a
          -- cover stored as a picture stream - let go.
          repeat
            packet, why = d.src:read()
            if packet and packet.stream ~= d.si then packet:free() packet = "other" end
          until packet ~= "other"
        end

        if packet then
          local ok, serr = d.dec:send(packet)

          if ok == false then
            d.held = packet
          else
            packet:free()
            if ok == nil then self:said(serr, d.pos) end
          end
        elseif why then
          self:said(why, d.pos)
          d.drained = true
        elseif not d.sent_end then
          d.dec:send()
          d.sent_end = true
        else
          d.drained = true
        end
      elseif at == nil then
        if len then self:said(len, d.pos) end
        d.drained = true
      elseif len > 0 then
        if fname and fname ~= d.fmt then
          self:said(("the decoder gave %s where %s was asked"):format(fname, d.fmt), d.pos)
          d.drained = true
        else
          if not d.sent then
            d.record.first = first_frame(d, self.list[d.index], pts)
          end

          d.pending = { at = at, len = len }
        end
      end
    end
  end
end

-- What went wrong, kept for the caller to say, and printed once.
function Queue:said(why, pos)
  local song = pos and self.list[self.order[pos]]
  local name = type(song) == "table" and (song.title or song.path) or tostring(song)

  self.why = tostring(why)
  print(("playqueue: %s: %s"):format(tostring(name), self.why))
end

--------------------------------------------------------------------------
-- Starting, and starting again.
--------------------------------------------------------------------------

--
-- **Everything begun again at place `pos`, from `from_s` seconds**: the old
-- song's decoding let go, a stream at this song's format, the kit sought
-- into it - which forgets whatever it held - and the old stream closed.
-- The songs after it are decoded after it, as they always are.
--
function Queue:restart(pos, from_s)
  local p = self.player

  self:close_song(self.decoding)
  self.decoding, self.decoded_last, self.ended_sent = nil, false, false
  self.failed, self.why = 0, nil
  self.ended = false

  if not pos then
    self.playing = false
    return false
  end

  -- A song that will not open is said and passed over, to the next that
  -- will - once round the list at most.
  local d, why

  for _ = 1, math.max(1, #self.order) do
    d, why = self:open_song(pos, from_s)

    if d then break end

    self:said(why, pos)
    from_s = 0
    pos = self:after(pos)

    if not pos then break end
  end

  if not d then
    self.pos, self.playing = pos or self.pos, false
    return false, why
  end

  local stream, serr = audio.open(self.name, PERIODS,
                                  { rate = d.rate, bits = d.stream_bits, channels = d.channels })

  if not stream then
    self:close_song(d)
    self.why = tostring(serr)
    return false, self.why
  end

  if self.gain then audio.set{ stream = stream.id, gain = self.gain } end

  local ok, perr = pcall(p.seek, p, stream.ring, d.rate, d.stream_bits, d.channels)

  if not ok then
    stream:close()
    self:close_song(d)
    self.why = tostring(perr)
    return false, self.why
  end

  if self.stream then self.stream:close() end

  self.stream = stream
  self.stream_format = { rate = d.rate, bits = d.stream_bits, channels = d.channels }
  self.changing = nil
  self.decoding, self.decode_pos = d, pos
  self.pos = pos
  self.start_at = from_s or 0
  self.heard_id = nil

  -- What the kit said of the old start is not this one's: read afresh on
  -- the next tick, and until then the song begun is the one shown.
  self.heard = {}

  if self.playing then p:play() end

  self:feed(sys.ticks() + self.hz * playqueue.BUDGET_MS // 1000)
  return true
end

--
-- **The songs, and where to start**: `songs` a list of `{ path = ... }`
-- (any other fields are the caller's, handed back by `now`), `first` the
-- one to start with. Playing at once.
--
function Queue:set(songs, first)
  if not self.player then
    local playback = kit("playback")
    local p, why = playback.open{ kind = "pcm", format = RING.format, rate = RING.rate,
                                  channels = RING.channels, seconds = RING.seconds }

    if not p then return false, tostring(why) end

    self.player = p
  end

  self.list = songs or {}

  local start

  self.order, start = make_order(#self.list, first or 1, self.shuffled)
  self.playing = true

  return self:restart(#self.order > 0 and start or nil, 0)
end

function Queue:play()
  if not self.player then return false end

  if self.ended and self.pos then return self:restart(self.pos, 0) end

  self.playing = true
  return self.player:play()
end

function Queue:pause()
  if not self.player then return false end

  self.playing = false
  return self.player:pause()
end

function Queue:toggle()
  if self.playing then return self:pause() end
  return self:play()
end

-- The song heard, from `seconds` into it.
function Queue:seek(seconds)
  if not self.pos then return false end

  return self:restart(self.pos, math.max(0, seconds or 0))
end

-- The song at `index` of the list, from its start.
function Queue:jump(index)
  for pos, i in ipairs(self.order) do
    if i == index then
      self.playing = true
      return self:restart(pos, 0)
    end
  end

  return false
end

function Queue:next()
  if not self.pos then return false end

  -- Next always moves, even repeating one song: it is a person's choice.
  local pos = self.pos < #self.order and self.pos + 1
              or (self.repeat_mode ~= "off" and 1 or nil)

  if not pos then return false end

  return self:restart(pos, 0)
end

function Queue:previous()
  if not self.pos then return false end

  local now = self:now()

  if (now.at or 0) > AGAIN_S or self.pos == 1 then return self:restart(self.pos, 0) end

  return self:restart(self.pos - 1, 0)
end

-- Off, all, or one. The songs already decoded ahead - a few seconds of the
-- next - stay as they were decoded.
function Queue:repeating(mode)
  if mode == "off" or mode == "all" or mode == "one" then self.repeat_mode = mode end

  return self.repeat_mode
end

-- Shuffle on or off, the song heard kept where it is.
function Queue:shuffle(on)
  self.shuffled = on and true or false

  local index = self:at_pos(self.pos)

  if not index then return self.shuffled end

  local order, start = make_order(#self.list, index, self.shuffled)

  if not self.shuffled then
    start = index
  end

  self.order, self.pos = order, start

  -- What is being decoded ahead is renumbered into the new order, so the
  -- song after it follows the new one.
  local d = self.decoding

  for pos, i in ipairs(order) do
    if d and i == d.index then d.pos = pos self.decode_pos = pos end
  end

  if not d then self.decode_pos = self.pos end

  return self.shuffled
end

function Queue:volume(level)
  self.gain = math.floor(math.max(0, math.min(1, level or 1)) * 256 + 0.5)

  if self.stream then return audio.set{ stream = self.stream.id, gain = self.gain } end

  return true
end

function Queue:stream_id()
  return self.stream and self.stream.id
end

--------------------------------------------------------------------------
-- Following what is heard.
--------------------------------------------------------------------------

--
-- **A song of another format waits** (`state.change`): a stream at its
-- format once the old one has played out, and on with `continue`. The old
-- ring's last periods are heard first; the device's own few milliseconds
-- and the new stream's start are the gap, which is the one a change of
-- format costs (`player.md` §2, Gapless).
--
function Queue:follow_change(h)
  if not h.change then return end

  if self.stream and self.stream:queued() > 0 then return end

  local fmt = { rate = h.change_rate, bits = h.change_bits, channels = h.change_channels }
  local stream, why = audio.open(self.name, PERIODS, fmt)

  if not stream then
    self.why = tostring(why)
    return
  end

  if self.gain then audio.set{ stream = stream.id, gain = self.gain } end

  local ok, perr = pcall(self.player.continue, self.player, stream.ring,
                         fmt.rate, fmt.bits, fmt.channels)

  if not ok then
    stream:close()
    self.why = tostring(perr)
    return
  end

  if self.stream then self.stream:close() end

  self.stream, self.stream_format = stream, fmt
end

--
-- **On the caller's clock**: decode ahead for a few milliseconds, go on at
-- a change of format, and say which song is heard - `on_heard(q, index,
-- was, natural)` when it changes, `natural` when the one before played to
-- its end rather than being left.
--
function Queue:tick()
  if not self.player or not self.pos then return end

  local deadline = sys.ticks() + self.hz * playqueue.BUDGET_MS // 1000

  if self.decoding or not self.decoded_last or not self.ended_sent then
    self:feed(deadline)
  end

  local h = self.player:state(self.heard)

  if h.refused > 0 and h.why ~= "" then self.why = h.why end

  self:follow_change(h)

  local now = self:now()

  if now.id and now.id ~= self.heard_id then
    local was = self.heard_id and self.ids[self.heard_id]

    self.heard_id = now.id
    self.pos = now.pos

    if self.on_heard then
      self.on_heard(self, now.index, was and was.index, was ~= nil)
    end
  end

  -- The end: everything decoded and written, and the ring played out.
  if self.ended_sent and h.finished and not h.change
     and (not self.stream or self.stream:queued() == 0) and not self.ended then
    self.ended, self.playing = true, false

    if self.on_heard then self.on_heard(self, nil, now.index, true) end
  end
end

--
-- **What is heard now**: which song, how far into it in seconds, from the
-- kit's marks against the frames the stream has had out of the speaker -
-- the newest mark in this stream once its frame has been played, the one
-- before it until then.
--
function Queue:now()
  local h = self.heard
  local out = { playing = self.playing, ended = self.ended, why = self.why,
                stream = self.stream_format }

  if not self.player or not self.pos then return out end

  local played = self.stream and self.stream:position() or 0
  local id, at, from

  if (h.marks or 0) > 0 and h.song_ring == h.ring and played >= h.song_at then
    id, at, from = h.song, h.song_at, h.song_from
  elseif (h.marks or 0) > 1 and h.previous_ring == h.ring then
    id, at, from = h.previous, h.previous_at, h.previous_from
  end

  local known = id and self.ids[id]

  if known then
    out.id, out.pos, out.index = id, known.pos, known.index
    out.at = (from + math.max(0, played - at)) / known.rate
    out.source = known.source
  else
    -- Nothing of this start heard yet: the song it starts with, where it
    -- starts.
    out.pos, out.index = self.pos, self.order[self.pos]
    out.at = self.start_at or 0

    local song = self.list[out.index]

    out.source = type(song) == "table" and song.source or nil
  end

  out.song = self.list[out.index]
  out.length = out.source and out.source.duration or 0

  return out
end

-- The kit's thread stopped first, so nothing writes into the ring as the
-- stream goes.
function Queue:close()
  self:close_song(self.decoding)
  self.decoding = nil

  if self.player then self.player:close() self.player = nil end
  if self.stream then self.stream:close() self.stream = nil end

  self.playing = false
end

return playqueue
