-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Playing a film, for anybody's application.
--
-- **This is a kit, and the Video app is one of its callers** - which is
-- Diego's decision, on 20 September, asked as a question: "Let's say I
-- build an app that plays video. Can I use the videokit you are working on
-- to include those capabilities in my app?" Yes, and building it that way
-- round is the point rather than a courtesy: "the idea is that all these
-- also helps us design and develop the kits that will make making new apps
-- super enjoyable", "and so the user that creates app don't reinvent the
-- wheel every time". So the measure of this file is how short the
-- application on top of it is, and a player that opens a window and plays a
-- film is fifteen lines (`user/bin/play.lua`).
--
-- **Which decoder is behind it is not a fact its user should have to
-- know** (`CLAUDE.md`, on kits). It decoded Motion JPEG first, because
-- `gfx.jpeg` was already in the image; it decodes H.264 now, through
-- FFmpeg's decoder in `/kits/h264` (`roadmap.md` 4e), and nothing written
-- against this file changed - not one call site. That is the same promise
-- `use("/kits/pdf")` makes about a scanner that moved from Lua to C.
--
-- **It owns the clock and not the loop.** An application has a loop
-- already - it is answering the pointer and the keyboard in it - and a kit
-- that took the loop away would have to give back a way to do everything
-- the application was doing in it. So the application calls `film:tick()`
-- on each pass, asks `film:position()` what moment it is, and draws the
-- frame for that moment, which is also exactly what a seek is.
--
-- This paragraph said it did not own the clock either, and that stopped
-- being true the day a film had sound: **time is the sound's** - frames
-- that came out of the speaker, as `media.lua` has it for Music - and a
-- picture timed by anything else drifts away from it. Only the kit hears
-- the sound, so only the kit can keep the time. A film without sound, or
-- on a machine without a device, keeps it by the counter instead, and the
-- caller cannot tell which.
--
local mp4 = use("/lib/mp4.lua")
local audio = use("/lib/audio.lua")

local video = {}

local film = {}
film.__index = film

-- Four megabytes: enough for any `moov` this will meet, and for a single
-- frame of anything it will be asked to decode - a 4K Motion JPEG frame is
-- about a megabyte and a half at a decent quality. Reads go through a
-- region because that is what `fs.read_into` fills, and because the
-- decoder reads the region directly (`jpeg_frame`).
local READ = 4 * 1024 * 1024

--
-- **The decoders, by the sample entry that names them.**
--
-- A table rather than a chain of ifs, because this is the list that grows:
-- `avc1` joins it with libavcodec, and whoever adds it should have to write
-- one line here and one decoder, not find every place a codec is named.
--
-- `mp4v` is what ffmpeg writes for Motion JPEG in an MP4; `jpeg` and `mjpa`
-- are what QuickTime writes for the same thing. All three are a sequence of
-- ordinary JPEG images, one to a sample, which is why this kit can exist
-- before a video decoder does.
--
--
-- **A frame is an address and a length, never a Lua string.**
--
-- `gfx.jpeg` has taken `(at, length)` since the window manager needed it
-- for wallpapers, and its comment says why: bytes about to be thrown away
-- should not be bytes the collector has to walk. This kit read each frame
-- into a Lua string anyway - ten kilobytes of garbage thirty times a
-- second, 300 KB a second, which is the audio server's mistake wearing
-- different clothes and exactly what `mp4.lua`'s own header claims does
-- not happen here.
--
-- Diego asked why an MP4 is read in Lua at all, which is what found it.
-- The *index* is Lua and should be: a few hundred boxes, read once, and it
-- runs on the host so the format is tested without booting. The *frames*
-- were never supposed to be.
--
local function jpeg_frame(at, length) return gfx.jpeg(at, length) end

local MOTION_JPEG = { name = "Motion JPEG", frame = jpeg_frame }

--
-- **H.264 is a conversation rather than a function**, which is why it has
-- no `frame` here and a path of its own below (`film:h264_to`). A picture
-- may be built from pictures before *and after* it, so samples go into the
-- decoder in the order they are decoded and pictures come out in the order
-- they are shown, a few behind; only the kit's decoder object can hold
-- that. `avc3` is the same stream with its parameter sets allowed in the
-- samples as well as in `avcC`.
--
local H264 = { name = "H.264", stateful = true }

local decoders = {
  -- QuickTime's two names for a track of JPEGs, which say so themselves.
  jpeg = MOTION_JPEG,
  mjpa = MOTION_JPEG,
  avc1 = H264,
  avc3 = H264,
}

--
-- The kit, on first use and only where the image has it: `FULL=0` builds
-- without FFmpeg, and there a film of H.264 is one this system cannot
-- decode rather than an error in this file.
--
local h264_kit

local function h264()
  if h264_kit == nil then
    local ok, kit = pcall(use, "/kits/h264")

    h264_kit = (ok and type(kit) == "table") and kit or false
  end

  return h264_kit or nil
end

--
-- **`mp4v` is not a codec**, and reading it as one was a guess that
-- happened to be right about the file in front of me. It means "MPEG-4
-- systems describes this", and which codec is the object type in the
-- sample entry's `esds`: **0x6c is JPEG** and 0x20 is MPEG-4 Visual, which
-- is a different thing entirely and would be handed to `gfx.jpeg` to fail
-- as "would not decode". `mp4.lua` reads that byte for video now - it used
-- to read `esds` only for audio - so this can tell them apart.
--
local OBJECT = {
  [0x6c] = MOTION_JPEG,
}

local NAMED = {
  [0x20] = "MPEG-4 Visual",
  [0x21] = "H.264",
  [0x23] = "H.265",
}

local function decoder_for(track)
  if track.codec == "mp4v" then
    return OBJECT[track.object or 0], NAMED[track.object or 0]
  end

  return decoders[track.codec], nil
end

--
-- **What the sound is, in the words a person reads.**
--
-- A film's audio track is described here even though this kit does not
-- play it yet, because describing it costs nothing and an application that
-- wants to say what it is playing should not have to read an `esds` to find
-- out. Diego asked for the audio codec in the player's overlay on 20
-- September, and the answer was three lines away from being free.
--
-- The numbers are 14496-1's object type indications: 0x40 is MPEG-4 audio,
-- whose flavour is in the AudioSpecificConfig beside it, and 0x6b is plain
-- MPEG-1 Layer III.
--
local function sound_name(track)
  local object, aot = track.object, track.aot

  if object == 0x6b then return "MP3" end

  if object == 0x40 then
    if aot == 2 then return "AAC-LC" end
    if aot == 5 then return "HE-AAC" end

    return "MPEG-4 audio"
  end

  return tostring(track.codec)
end

-- What a film says when it cannot be played, in the words a person reads.
local function no_decoder(codec, named)
  if named == "H.264" then
    return "this film is H.264 in an MPEG-4 systems track, which this "
           .. "system cannot read yet"
  end

  if named then
    return "this film is " .. named .. ", which this system cannot decode"
  end

  return "this film is " .. tostring(codec) .. ", which this system cannot decode"
end

--------------------------------------------------------------------------
-- **The sound**: a film's audio track, decoded and fed to the audio
-- server - AAC through `/kits/aac`, MP3 through `/kits/mp3` - and the
-- clock the picture follows.
--------------------------------------------------------------------------

--
-- How much of the file to read at a time for the sound: a run of samples
-- that sit next to each other, up to this. An MP4 keeps a film's sound in
-- chunks between the picture's, a second or so each, so one read is a
-- second of sound rather than one frame of it - a round trip to the disk
-- server costs more than decoding the frame (`roadmap.md` 6k).
--
local SOUND_READ = 256 * 1024

--
-- **How deep a film's ring is: 32 periods, 186 ms** - where Music's is the
-- default 8. `audio.lua` says why a video player is the other end of that
-- choice: a film would far rather be 200 ms behind than skip, because a gap
-- desynchronises everything after it, and the picture follows what is
-- heard whatever the delay. And a film's window goes away for as long as a
-- picture takes to decode - up to 40 ms of H.264 - which a 46 ms ring does
-- not survive with the device's own queue on top.
--
local SOUND_PERIODS = 32

--
-- How many periods one tick may hand over: the whole ring, so that a pass
-- after a long decode fills it again at once; and a ceiling, so that a
-- server that wrongly always took would make the sound run ahead rather
-- than taking the window with it (`media.lua`'s reasoning).
--
local FEED_MAX = SOUND_PERIODS

local voice = {}
voice.__index = voice

--
-- The kit for a track, and a decoder from it: `decode` is AAC's from an
-- address, and MP3's from a string, and this is the one place that knows.
--
local function sound_decoder(track)
  local object = track.object or 0

  if track.codec == "mp4a" and object == 0x40 then
    local ok, kit = pcall(use, "/kits/aac")

    if not ok or type(kit) ~= "table" then
      return nil, "this film's sound is AAC, and this system was built "
                  .. "without its AAC decoder (FULL=0)"
    end

    if not track.config then
      return nil, "this film's sound does not describe itself (no config)"
    end

    local d, why = kit.decoder(track.config, track.rate or 0,
                               track.channels or 0)

    if not d then return nil, tostring(why) end

    return function(at, n)
      return d:decode(at, n)
    end, function() d:reset() end, function() d:close() end
  end

  if track.codec == "mp4a" and (object == 0x6b or object == 0x69) then
    local mp3 = use("/kits/mp3")
    local d = mp3.decoder()

    return function(_, n, bytes)
      local pcm, _, rate, channels = d:decode(bytes, 1 << 20)

      return pcm, rate, channels
    end, function() d:reset() end, function() end
  end

  return nil, "this film's sound is " .. tostring(track.codec)
              .. (object ~= 0 and (" (object 0x%02x)"):format(object) or "")
              .. ", which this system cannot play"
end

--
-- A film's sound, ready to start: its decoder, a read buffer of its own,
-- and nothing playing yet. Nil and why when it cannot be heard.
--
local function open_voice(path, track, name)
  local fmt = audio.format()

  if not fmt or (fmt.period or 0) == 0 then
    return nil, "this machine has no sound device"
  end

  local decode, reset, close = sound_decoder(track)

  if not decode then return nil, reset end

  local page = sys.memory(SOUND_READ // 4096)

  if not page then
    close()
    return nil, "no memory for the sound's read buffer"
  end

  return setmetatable({
    path = path, track = track, name = name, fmt = fmt, page = page,
    mapped = sys.memory_map(page),
    decode = decode, reset_decoder = reset, close_decoder = close,
    samples = track.samples or {}, scale = track.timescale or 1,
    next = 1, run_first = 0, run_last = -1, run_at = 0,
    decoded = "", pending = "", phase = 0.0, rate = 0, channels = 0,
    base = 0, gain = nil, frames_out = 0, frames_in = 0,
  }, voice)
end

--
-- Start hearing it at `at` seconds: a stream of its own, and the sample
-- that holds that moment. **A new stream for every start**, as Music
-- seeks: nothing can drop what a stream has queued - `audioproto.h` has no
-- flush - and the ring and the device hold about 70 ms of the old place.
--
function voice:start(at)
  local samples, ticks = self.samples, at * self.scale
  local lo, hi = 1, #samples

  -- The last sample that starts at or before `at`: the frames' times only
  -- rise, so halving finds it.
  while lo < hi do
    local mid = (lo + hi + 1) // 2

    if (samples[mid].pts or 0) <= ticks then lo = mid else hi = mid - 1 end
  end

  if self.stream then self.stream:close() end

  local stream, why = audio.open(self.name, SOUND_PERIODS)

  self.stream = stream

  if not stream then return false, tostring(why) end

  if self.gain then audio.set{ stream = stream.id, gain = self.gain } end

  self.reset_decoder()
  self.next = (#samples > 0) and lo or 1
  self.base = (#samples > 0) and ((samples[lo].pts or 0) / self.scale) or 0
  self.run_first, self.run_last = 0, -1
  self.decoded, self.pending, self.phase = "", "", 0.0
  self.fed_all, self.frames_out, self.frames_in = false, 0, 0
  return true
end

--
-- Sample `i` of the track, where the decoder can reach it: the address
-- inside the read buffer and its length, reading the run of samples that
-- sit next to it in the file when it is not there already.
--
function voice:sample_at(i)
  local samples = self.samples

  if i < self.run_first or i > self.run_last then
    local first = samples[i]
    local last, bytes = i, first.size

    while samples[last + 1]
          and samples[last + 1].at == samples[last].at + samples[last].size
          and bytes + samples[last + 1].size <= SOUND_READ do
      last = last + 1
      bytes = bytes + samples[last].size
    end

    if bytes > SOUND_READ then return nil, "a sound frame larger than the read buffer" end

    local got = fs.read_into(self.path, self.page, first.at, bytes)

    if not got or got < bytes then return nil, "the film stopped being readable" end

    self.run_first, self.run_last, self.run_at = i, last, first.at
  end

  local s = samples[i]

  return self.mapped + (s.at - self.run_at), s.size, s.at - self.run_at
end

--
-- Hand over what the server will take, and not a period more - `media.lua`'s
-- `tick`, over a film's frames instead of a file's bytes.
--
function voice:feed()
  local stream, fmt = self.stream, self.fmt

  if not stream then return 0 end

  local fed = 0

  for _ = 1, FEED_MAX do
    if #self.pending == 0 then
      -- Enough decoded to make a few periods of, or the end.
      while #self.decoded < fmt.period * 4 and self.next <= #self.samples do
        local at, n, offset = self:sample_at(self.next)

        if not at then
          self.error = tostring(n)
          self.next = #self.samples + 1
          break
        end

        local bytes = nil

        if self.track.object ~= 0x40 then
          bytes = sys.region_read(self.page, offset, n)
        end

        local pcm, rate, channels = self.decode(at, n, bytes)

        self.next = self.next + 1

        --
        -- A frame that will not decode is passed over, as a damaged
        -- picture is: a click in the sound, not the end of it.
        --
        if pcm and #pcm > 0 then
          self.decoded = self.decoded .. pcm
          self.rate, self.channels = rate, channels
          self.frames_in = self.frames_in + #pcm // (2 * channels)
        elseif not pcm then
          self.error = tostring(rate)
        end
      end

      if self.next > #self.samples and #self.decoded < 4 then
        self.fed_all = true
        break
      end

      --
      -- The end of the track is the end of the input, so its final frame
      -- comes out rather than waiting for a neighbour (`sys.pcm`'s `last`)
      -- - and one frame is then enough to convert, where two are needed
      -- while there is more to come. Both halves were missing: the last
      -- sample of a film never played (`testing.md` 18.185).
      --
      local last = self.next > #self.samples

      if self.rate == 0
         or #self.decoded < self.channels * 2 * (last and 1 or 2) then
        break
      end

      local pcm, used
      pcm, used, self.phase = sys.pcm(self.decoded, self.rate, self.channels,
                                      16, self.phase, fmt.period * 4, last)

      if used == 0 or #pcm == 0 then
        if self.next > #self.samples then
          self.fed_all = true
          self.decoded = ""
        end
        break
      end

      self.decoded = self.decoded:sub(used + 1)
      self.pending = pcm
      self.frames_out = self.frames_out + #pcm // 4
    end

    local took, why = stream:play(self.pending:sub(1, fmt.period))

    if not took then
      if why ~= "full" then self.error = tostring(why) end
      break
    end

    self.pending = self.pending:sub(fmt.period + 1)
    fed = fed + 1
  end

  return fed
end

-- Seconds heard: where this start began, and the frames out of the speaker
-- since, at the device's rate - which `sys.pcm` has converted to.
function voice:position()
  if not self.stream then return self.base end

  return self.base + self.stream:position() / self.fmt.rate
end

-- All of it handed over, and all of it played.
function voice:done()
  return self.fed_all and #self.pending == 0
         and (not self.stream or self.stream:queued() == 0)
end

function voice:volume(level)
  self.gain = math.floor(math.max(0, math.min(1, level)) * 256 + 0.5)

  if self.stream then
    return audio.set{ stream = self.stream.id, gain = self.gain }
  end

  return true
end

function voice:close()
  if self.stream then self.stream:close() self.stream = nil end
  if self.page then sys.release(self.page) self.page = nil end

  self.close_decoder()
end

--
-- A film, or nil and why.
--
--
-- **`options.debuginfo` puts the overlay in the kit, not in the app.**
--
-- Diego, 20 September: "i mean debug as a parameter when instantiating the
-- video player in lua code", "like debuginfo=true". He is right and my
-- first attempt was wrong: I had built the badge and the box inside
-- `play.lua`, which makes every application that wants to know what it is
-- playing write the same forty lines again - the exact thing a kit exists
-- to stop ("so the user that creates app don't reinvent the wheel every
-- time").
--
-- So the kit draws it, the kit keeps the numbers, and an application asks
-- for it with one word at `open` and forwards a click with one line. An
-- application that does not ask gets a film with nothing drawn over it.
--
function video.open(path, options)
  options = options or {}

  local page = sys.memory(READ // 4096)

  if not page then return nil, "no memory for a read buffer" end

  --
  -- Every refusal from here gives the buffer back: four megabytes held
  -- for a film that was never opened is four megabytes an application
  -- that tries a folder of files never sees again.
  --
  local function refuse(why)
    sys.release(page)
    return nil, why
  end

  local size = (fs.getattr(path) or {}).size

  if not size or size == 0 then
    return refuse("there is no film at " .. tostring(path))
  end

  --
  -- Two ways to read, and the difference is who the bytes are for.
  --
  -- `read_at` gives a string, for `mp4.open`: it is parsing a box tree in
  -- Lua, once, and Lua is what it parses with. `frame_at` gives the
  -- *address* the bytes landed at, for a decoder that is C.
  --
  local mapped = sys.memory_map(page)

  local function read_at(off, n)
    local got = fs.read_into(path, page, off, n)

    if not got or got == 0 then return nil end

    return sys.region_read(page, 0, got)
  end

  local function frame_at(off, n)
    if n > READ then return nil, "a frame larger than the read buffer" end

    local got = fs.read_into(path, page, off, n)

    if not got or got == 0 then return nil, "the film stopped being readable" end

    return mapped, got
  end

  local movie, why = mp4.open(read_at, size)

  if not movie then return refuse(tostring(why)) end

  local track, sound

  for _, t in ipairs(movie.tracks) do
    if t.kind == "video" and not track then track = t end
    if t.kind == "audio" and not sound then sound = t end
  end

  if not track then return refuse("this film has no picture in it") end

  local decoder, named = decoder_for(track)

  if not decoder then return refuse(no_decoder(track.codec, named)) end

  local f = setmetatable({
    path = path, page = page, read_at = read_at, frame_at = frame_at,
    movie = movie, track = track, decoder = decoder,
    width = track.width or 0, height = track.height or 0,
    codec = decoder.name,
    frames = #(track.samples or {}),
    -- Seconds, from the track's own clock. A timescale is ticks a second
    -- and every time in the track is in them, so this is the one place the
    -- division happens - the same rule the system has about ticks.
    duration = (track.duration or 0) / (track.timescale or 1),
    shown = nil,

    --
    -- **What a frame costs, in counter ticks, kept by the kit** because it
    -- is the only place that can see the three parts apart: reading the
    -- sample off the disk, decoding it, and putting it on the screen.
    --
    -- Ticks rather than milliseconds, and `_ticks` in the name, because a
    -- number that leaves here arrives somewhere that has to divide by a
    -- rate this file has no business reading (`CLAUDE.md`, two clocks).
    -- The caller reads `counter_hz` next to its own sum.
    --
    read_ticks = 0, decode_ticks = 0, blit_ticks = 0, decoded = 0,

    --
    -- The overlay's own bookkeeping, and the counter's rate beside it: this
    -- is the one place in the kit that turns ticks into milliseconds, and
    -- it reads `counter_hz` rather than assuming one (`CLAUDE.md`, two
    -- clocks).
    --
    debuginfo = options.debuginfo and true or false,
    info_open = options.debuginfo and options.open_now and true or false,
    hz = (fs.read("/dev/cpu") or {}).counter_hz or 1,
    drawn = 0, dropped = 0, rate = 0,
    rate_at = sys.ticks(), rate_frames = 0,
    ms_read = 0, ms_decode = 0, ms_screen = 0,
  }, film)

  f.fps = (f.frames > 0 and f.duration > 0) and (f.frames / f.duration) or 0

  --
  -- **The frames in the order they are shown**, which is not the order
  -- they are stored in once a film has B-frames: a picture that depends on
  -- one after it is stored after it. Positions from `index_at`, and the
  -- frame numbers a caller sees, count in this order; `order[n]` is where
  -- the n-th shown frame sits among the samples. A film without B-frames -
  -- every Motion JPEG, every camera recording - is already in order, and
  -- is only checked.
  --
  local samples = track.samples or {}
  local order, sorted = {}, true

  for i = 1, #samples do
    order[i] = i

    if i > 1 and (samples[i].pts or 0) < (samples[i - 1].pts or 0) then
      sorted = false
    end
  end

  if not sorted then
    table.sort(order, function(a, b)
      local pa, pb = samples[a].pts or 0, samples[b].pts or 0

      if pa ~= pb then return pa < pb end

      return a < b
    end)
  end

  f.order = order

  --
  -- **When the first frame is shown**, which is the film's zero. An MP4
  -- with B-frames usually shows its first picture a frame or two after its
  -- clock starts - the encoder had to decode ahead - and an edit list says
  -- to skip that. `mp4.lua` does not read edit lists, so this does the one
  -- thing they are almost always used for: it starts the film at its first
  -- picture rather than at a moment of black.
  --
  f.base = (#order > 0) and (samples[order[1]].pts or 0) or 0

  if decoder.stateful then
    local kit = h264()

    if not kit then
      return refuse("this film is H.264, and this system was built without "
                    .. "its H.264 decoder (FULL=0)")
    end

    if not track.avcc then
      return refuse("this film does not describe its H.264 stream (no avcC)")
    end

    local stream, serr = kit.decoder(track.avcc)

    if not stream then return refuse(tostring(serr)) end

    f.stream, f.next = stream, 1
  end

  --
  -- **Bits a second, from the samples themselves** rather than from
  -- anything the file claims. An MP4 has no field that must hold it, and a
  -- sum over a few hundred sizes costs nothing once, at open - so this is
  -- a fact the kit answers rather than one every application works out for
  -- itself, which is the whole argument for a kit. Diego asked for it on
  -- 20 September, for the player's debug overlay.
  --
  local bytes = 0

  for _, sample in ipairs(track.samples or {}) do
    bytes = bytes + (sample.size or 0)
  end

  f.bytes = bytes
  f.bitrate = (f.duration > 0) and (bytes * 8 / f.duration) or 0

  --
  -- The sound, described and not yet played. `track` is kept so that the
  -- half of this kit that plays it has what it needs without opening the
  -- film twice.
  --
  if sound then
    f.sound = {
      codec = sound_name(sound),
      rate = sound.rate or 0,
      channels = sound.channels or 0,
      frames = #(sound.samples or {}),
      track = sound,
    }
  end

  --
  -- **The clock**: stopped at the start, until `play`. `at` is where it
  -- was when it last started or stopped, and `since` the counter then.
  --
  f.clock = { playing = false, at = 0, since = sys.ticks() }
  f.name = path:match("([^/]+)$") or path

  return f
end

--
-- Which frame is on screen at `when` seconds.
--
-- A search rather than `when * fps`, because a film's frames are not evenly
-- spaced in general - a camera drops them, an editor cuts on them - and the
-- sample table says exactly when each one is shown. Linear from where we
-- are, which is one step for playing and a handful for a nudge; a seek to
-- the far end of a film walks it, and that is a thing to make cleverer when
-- something asks for it rather than now.
--
function film:index_at(when)
  local samples, order = self.track.samples, self.order
  local scale = self.track.timescale or 1
  local ticks = when * scale + self.base

  if #order == 0 then return nil end

  local at = self.shown or 1

  -- Backwards first: a seek to the beginning is common and cheap.
  while at > 1 and (samples[order[at]].pts or 0) > ticks do
    at = at - 1
  end

  while at < #order and (samples[order[at + 1]].pts or 0) <= ticks do
    at = at + 1
  end

  return at
end

--
-- Frame `n` as a surface, decoded now.
--
-- The caller owns nothing: the surface belongs to the film and is replaced
-- when another frame is asked for, which is what keeps a film from making a
-- surface a frame and asking the collector to notice.
--
function film:frame(n)
  local samples = self.track.samples
  local s = samples and self.order[n] and samples[self.order[n]]

  if not s then return nil, "there is no frame " .. tostring(n) end

  if self.stream then return self:h264_to(n) end

  local before = sys.ticks()
  local where, got = self.frame_at(s.at, s.size)

  if not where then return nil, tostring(got) end

  local read_done = sys.ticks()
  local ok, picture = pcall(self.decoder.frame, where, got)

  if not ok or not picture then
    return nil, "frame " .. n .. " would not decode: " .. tostring(picture)
  end

  self.read_ticks = self.read_ticks + (read_done - before)
  self.decode_ticks = self.decode_ticks + (sys.ticks() - read_done)
  self.decoded = self.decoded + 1

  if self.picture then self.picture:free() end

  self.picture, self.shown = picture, n

  return picture
end

--
-- **How long one call may spend decoding**, in seconds. A seek lands on the
-- frame before the one asked for that can be decoded alone - a key frame -
-- and decodes forward from there, which can be a second of film; and a
-- machine that decodes slower than the film plays falls behind. Neither is
-- allowed to take the window with it: a call decodes for this long and
-- shows the latest picture it has, and the next call goes on from there.
-- A machine that cannot keep up shows fewer frames, counted as dropped,
-- rather than stopping to answer the pointer.
--
local DECODE_BUDGET = 0.040

--
-- The last key frame at or before sample `i`, in storage order.
--
local function key_before(samples, i)
  while i > 1 and not samples[i].key do i = i - 1 end

  return i
end

--
-- **Shown frame `n` of an H.264 film, as a surface.**
--
-- Two surfaces: the one on screen, and the one the next picture is
-- converted into as it comes out of the decoder - which is often a picture
-- for later, since pictures come out in the order they are shown and a
-- sample may be sent well before its picture is due. It waits in the
-- second surface until its moment and the two are swapped; no picture is
-- converted twice.
--
function film:h264_to(n)
  local samples, stream = self.track.samples, self.stream
  local target = self.order[n]
  local want = samples[target].pts or 0

  if not self.picture then
    self.picture = gfx.surface{ w = self.width, h = self.height }
    self.spare = gfx.surface{ w = self.width, h = self.height }

    if not self.picture or not self.spare then
      return nil, "no memory for a " .. self.width .. " by " .. self.height
                  .. " picture"
    end
  end

  --
  -- Start again from a key frame when going backwards, or when the key
  -- frame this picture needs has not been reached yet - decoding up to it
  -- would be decoding pictures nobody will see.
  --
  local key = key_before(samples, target)

  if (self.shown_pts and want < self.shown_pts) or key > self.next then
    stream:flush()
    self.next, self.pending_pts, self.shown_pts = key, nil, nil
    self.finished = false
  end

  local limit = sys.ticks() + DECODE_BUDGET * self.hz

  while true do
    --
    -- A picture that has come out is shown once it is due - or at once
    -- when nothing is shown yet, since the first picture after a start or a
    -- seek is better than black.
    --
    if self.pending_pts then
      if self.pending_pts <= want or not self.shown_pts then
        self.picture, self.spare = self.spare, self.picture
        self.shown_pts, self.pending_pts = self.pending_pts, nil
      else
        break
      end
    end

    if (self.shown_pts and self.shown_pts >= want) or sys.ticks() > limit then
      break
    end

    local before = sys.ticks()
    local pts, said = stream:picture(self.spare)

    if pts then
      self.pending_pts = pts
      self.decoded = self.decoded + 1
      self.decode_ticks = self.decode_ticks + (sys.ticks() - before)
    elseif said == "none" then
      if self.next <= #samples then
        local s = samples[self.next]
        local where, got = self.frame_at(s.at, s.size)

        if not where then return nil, tostring(got) end

        local read_done = sys.ticks()
        local ok, why = stream:send(where, got, s.pts or 0)

        self.read_ticks = self.read_ticks + (read_done - before)
        self.decode_ticks = self.decode_ticks + (sys.ticks() - read_done)

        --
        -- A sample that will not decode is passed over rather than
        -- stopping the film: the decoder conceals what it can, and a
        -- damaged frame in the middle of a film is a blemish, not an end.
        -- What it said is kept for whoever asks (`film.error`).
        --
        if ok or why ~= "full" then self.next = self.next + 1 end
        if not ok and why ~= "full" then self.error = tostring(why) end
      elseif not self.finished then
        stream:finish()
        self.finished = true
      else
        break
      end
    elseif said == "end" then
      break
    else
      self.error = tostring(said)
      break
    end
  end

  if not self.shown_pts then
    return nil, self.error or "no picture has come out yet"
  end

  return self.picture
end

--
-- **The whole of playing, for a caller that has a surface and a clock.**
--
-- Draws the frame for `when` seconds into `dest`, scaled to `w` by `h` at
-- `x`, `y` - or at the film's own size when those are left out. Answers
-- true when it drew and false when there was nothing to draw.
--
-- It draws every time it is asked, rather than skipping when the frame has
-- not changed, and that is deliberate: a direct window has *two* buffers
-- and `commit` swaps them, so "the same frame is already there" is a
-- question about the buffer in hand rather than about the film. Getting
-- that wrong shows one frame of the one before it, which is a flicker
-- nobody can find later. A blit of a decoded frame is cheap next to
-- decoding it; when something measures this and wants it back, the answer
-- is a frame number kept per buffer, not a cleverer guess here.
--
function film:draw(dest, when, x, y, w, h)
  local n = self:index_at(when or 0)

  if not n then return false end

  -- **A jump of more than one frame is a frame nobody saw**, and counting
  -- it here rather than in the caller is the difference between a number
  -- every application gets right and one each of them works out again.
  if self.drew_frame and n > self.drew_frame + 1 then
    self.dropped = self.dropped + (n - self.drew_frame - 1)
  end

  local picture

  if self.stream then
    picture = self:h264_to(n)
    self.shown = n
  else
    picture = (n == self.shown) and self.picture or self:frame(n)
  end

  if not picture then return false end

  x, y = x or 0, y or 0
  w, h = w or self.width, h or self.height

  local before = sys.ticks()

  if w == self.width and h == self.height then
    dest:blit(picture, 0, 0, self.width, self.height, x, y)
  else
    dest:stretch(picture, 0, 0, self.width, self.height, x, y, w, h)
  end

  self.blit_ticks = self.blit_ticks + (sys.ticks() - before)
  self.drew_frame, self.drawn = n, self.drawn + 1
  self.rate_frames = self.rate_frames + 1

  -- What it is really managing, every two seconds. Kept whether or not
  -- anything is drawn with it, because `stats` answers for a caller that
  -- wants the numbers without an overlay over its film.
  local now = sys.ticks()

  if now - self.rate_at >= 2 * self.hz then
    local seconds = (now - self.rate_at) / self.hz
    local each = (self.decoded > 0) and self.decoded or 1

    self.rate = self.rate_frames / seconds
    self.ms_read = self.read_ticks * 1000 / self.hz / each
    self.ms_decode = self.decode_ticks * 1000 / self.hz / each
    self.ms_screen = self.blit_ticks * 1000 / self.hz / each

    self.rate_at, self.rate_frames = now, 0
    self.read_ticks, self.decode_ticks, self.blit_ticks = 0, 0, 0
    self.decoded = 0
  end

  if self.debuginfo then self:info(dest, x, y, w, h) end

  return true
end

--
-- **What it is playing and how well**, for a caller that wants the numbers
-- without anything drawn over its film.
--
function film:stats()
  return {
    codec = self.codec, width = self.width, height = self.height,
    fps = self.fps, rate = self.rate,
    frames = self.frames, dropped = self.dropped,
    bitrate = self.bitrate, duration = self.duration,
    ms_read = self.ms_read, ms_decode = self.ms_decode,
    ms_screen = self.ms_screen,
    sound = self.sound,
  }
end

--
-- **The badge, and the box it opens.**
--
-- Drawn by the kit over the frame it just drew, so there is nothing to
-- undraw and nothing to keep in step: the film is painted again every
-- frame and this goes on top of it.
--
local BADGE = 22

--
-- **One black pixel, stretched.**
--
-- Diego, 20 September, having tried the overlay: "make it black with 60%
-- transparency so it does not block the video behind". `fill` writes the
-- colour it is given and does not blend, so a translucent panel is not a
-- fill at all - but `stretch` takes an alpha, and stretching a single black
-- pixel over the box composites it over the frame underneath at whatever
-- alpha is asked for. No new primitive, and the loop stays in C where every
-- pixel loop here lives.
--
-- Made once and kept: a surface a frame would be a surface a frame for the
-- collector to walk (`gfx.md` 19.1).
--
local SHADE = 153                       -- of 255: the box is six tenths black

local dark

local function shade(dest, x, y, w, h)
  if not dark then
    dark = gfx.surface{ w = 1, h = 1 }

    if not dark then return false end

    dark:fill(0, 0, 1, 1, 0xff000000)
  end

  dest:stretch(dark, 0, 0, 1, 1, x, y, w, h, SHADE)

  return true
end

function film:info(dest, x, y, w, h)
  local white, on = 0xffffffff, 0xff3b6ea5
  local bx, by = x + w - 8 - BADGE // 2, y + 8 + BADGE // 2

  dest:disc(bx, by, BADGE // 2, self.info_open and on or 0xaa000000)
  dest:text(bx - 2, by - gfx.height() // 2, "i", white)

  if not self.info_open then return end

  local sound = self.sound
             and ("%s, %d Hz, %s"):format(self.sound.codec, self.sound.rate,
                   (self.sound.channels == 2) and "stereo" or
                   (self.sound.channels == 1) and "mono" or
                   (self.sound.channels .. " channels"))
             or "no sound track"

  --
  -- What Diego asked it to say, in his words: "fps of the video, frames
  -- dropped, actual fps, audio codec", the resolution, and the bitrate.
  --
  local lines = {
    ("Video     %s, %d x %d"):format(self.codec, self.width, self.height),
    ("Audio     %s"):format(sound),
    ("Rate      %.1f a second, of %.1f"):format(self.rate, self.fps),
    ("Dropped   %d of %d frames"):format(self.dropped, self.frames),
    ("Bitrate   %.0f kbit/s over %.1f s"):format(self.bitrate / 1000,
                                                 self.duration),
    ("Frame     %.1f ms read, %.1f ms decode, %.1f ms to screen")
      :format(self.ms_read, self.ms_decode, self.ms_screen),
  }

  local line_h = gfx.height() + 4
  local box_w, box_h = 0, #lines * line_h + 12

  for _, line in ipairs(lines) do
    local wide = gfx.measure(line)

    if wide > box_w then box_w = wide end
  end

  box_w = box_w + 20

  local ox, oy = x + 8, y + 8 + BADGE + 8

  -- Six tenths black over the film, so what is behind still shows.
  if not shade(dest, ox, oy, box_w, box_h) then
    dest:fill(ox, oy, box_w, box_h, 0xdd000000)
  end

  dest:fill(ox, oy, box_w, 1, on)

  for i, line in ipairs(lines) do
    dest:text(ox + 10, oy + 6 + (i - 1) * line_h, line, white)
  end
end

--
-- **A click, for the badge.** True when it was the badge's, so a caller can
-- say `if not film:pointer(x, y) then ... end` and go on treating the rest
-- of the picture as its own.
--
function film:pointer(px, py, x, y, w, h)
  if not self.debuginfo then return false end

  x, y = x or 0, y or 0
  w, h = w or self.width, h or self.height

  local bx, by = x + w - 8 - BADGE // 2, y + 8 + BADGE // 2
  local dx, dy = px - bx, py - by

  if dx * dx + dy * dy > (BADGE // 2 + 2) ^ 2 then return false end

  self.info_open = not self.info_open

  return true
end

--
-- The size a film should be drawn at inside `w` by `h`, keeping its shape.
--
-- Here rather than in each application because every one of them wants it -
-- `video.html`'s "Fit to the Screen" is this - and because getting it
-- wrong is a film with a squashed face that nobody notices in a mockup.
--
function film:fit(w, h)
  if self.width == 0 or self.height == 0 then return 0, 0, 0, 0 end

  local scale = math.min(w / self.width, h / self.height)
  local dw = math.floor(self.width * scale)
  local dh = math.floor(self.height * scale)

  return (w - dw) // 2, (h - dh) // 2, dw, dh
end

--------------------------------------------------------------------------
-- **Playing, and the time.**
--------------------------------------------------------------------------

--
-- The sound, on first need: opened when the film first plays rather than
-- when it is opened, so a program that only wants frames - a thumbnail, a
-- test - never holds a stream. Why it cannot be heard is kept, for
-- `stats` and the overlay, and the film plays by the counter instead.
--
function film:voice()
  if self.voice_state == nil then
    if not self.sound then
      self.voice_state = false
      self.silent = "this film has no sound"
    else
      local v, why = open_voice(self.path, self.sound.track, self.name)

      self.voice_state = v or false
      self.silent = not v and tostring(why) or nil
      if v and self.gain_level then v:volume(self.gain_level) end
    end
  end

  return self.voice_state or nil
end

--
-- Seconds into the film, as it is heard: the sound's clock while there is
-- sound to hear, and the counter's otherwise - before a film has sound,
-- after its sound has run out, and on a machine with none.
--
function film:position()
  local c = self.clock

  if not c.playing then return c.at end

  local v = self.voice_state

  if v and v.stream and not c.by_counter then
    if v:done() then
      -- The sound has finished and the picture has not: carry on by the
      -- counter from where the sound left off.
      c.at, c.since, c.by_counter = v:position(), sys.ticks(), true
    else
      return v:position()
    end
  end

  return c.at + (sys.ticks() - c.since) / self.hz
end

function film:playing()
  return self.clock.playing
end

--
-- From `at` seconds, or from where it stopped. Resuming does not start the
-- sound again: it goes on from what it had queued, which the device has
-- played out while it was paused, and the clock goes on from what it heard.
--
function film:play(at)
  local c = self.clock

  if at then
    self:seek(at)
  elseif not c.playing and not self.started then
    self:seek(c.at)
  end

  c.since = sys.ticks()
  c.playing = true
  return true
end

-- Stopped where it is. What the device holds, about 70 ms, plays out.
function film:pause()
  local c = self.clock

  c.at = self:position()
  c.playing = false
end

--
-- Somewhere else in the film. The sound starts again from the frame that
-- holds that moment, and the picture follows it.
--
function film:seek(at)
  local c = self.clock

  at = math.max(0, math.min(at or 0, self.duration))
  c.at, c.since, c.by_counter = at, sys.ticks(), false
  self.started = true

  local v = self:voice()

  if v then
    local ok, why = v:start(at)

    if not ok then
      v:close()
      self.voice_state, self.silent = false, why
    else
      -- What is heard starts at the frame that holds `at`, a few
      -- milliseconds before it: the picture waits for the sound.
      c.at = v.base
    end
  end
end

--
-- The sound fed, on every pass of the caller's loop. A film that is not
-- playing, or has no sound, costs nothing here.
--
function film:tick()
  local v = self.voice_state

  if not v or not self.clock.playing then return 0 end

  return v:feed()
end

-- Loudness, 0 to 1, kept across a seek and applied once there is sound.
function film:volume(level)
  self.gain_level = math.max(0, math.min(1, level))

  local v = self.voice_state

  if v then return v:volume(self.gain_level) end

  return true
end

-- Whether it can be heard, and why not when it cannot.
function film:audible()
  self:voice()

  return self.voice_state and true or false, self.silent
end

function film:close()
  if self.voice_state then self.voice_state:close() end
  self.voice_state = false

  if self.picture then self.picture:free() self.picture = nil end
  if self.spare then self.spare:free() self.spare = nil end
  if self.stream then self.stream:close() self.stream = nil end
  if self.page then sys.release(self.page) self.page = nil end

  self.track, self.movie, self.shown = nil, nil, nil
end

return video
