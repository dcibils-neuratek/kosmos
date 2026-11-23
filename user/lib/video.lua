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
-- film is fifteen lines (`user/bin/programs/play.lua`).
--
-- **Which decoder is behind it is not a fact its user should have to
-- know** (`CLAUDE.md`, on kits). It decoded Motion JPEG first, because
-- `gfx.jpeg` was already in the image, then H.264 through a closure of
-- FFmpeg's decoder vendored for it. Since M6 (`docs/media.md`) the film is
-- the Media Kit's: its container read and its streams decoded by the whole
-- FFmpeg in `media.elf`, through the kit's Lua door (`/Kosmos/Kits/media`)
-- - MP4, WebM and Matroska; H.264, VP9, VP8 and AV1; AAC, Opus, Vorbis,
-- FLAC and MP3 - and nothing written against this file changed: not one
-- call site.
--
-- **So it plays only in `media.elf`**, the image the kit is in. A program
-- that plays a film says `-- kosmos: image media` in its header, as Video
-- and `listen` do; asked from any other image, `video.open` refuses and
-- says so.
--
-- **It owns the clock and not the loop.** An application has a loop
-- already - it is answering the pointer and the keyboard in it - and a kit
-- that took the loop away would have to give back a way to do everything
-- the application was doing in it. So the application calls `film:tick()`
-- on each pass, asks `film:position()` what moment it is, and draws the
-- frame for that moment, which is also exactly what a seek is.
--
-- **Time is the sound's**: frames that came out of the speaker, as
-- `media.lua` has it for Music, because a picture timed by anything else
-- drifts away from it. Only the kit hears the sound, so only the kit can
-- keep the time. A film without sound, or on a machine without a device,
-- keeps it by the counter instead, and the caller cannot tell which.
--
local audio = use("/Kosmos/Libraries/audio.lua")

local video = {}

local film = {}
film.__index = film

--
-- **The Media Kit's door**, on first use, and only where the image has it.
--
local kit

local function door()
  if kit == nil then
    local ok, k = pcall(use, "/Kosmos/Kits/media")

    kit = (ok and type(k) == "table") and k or false
  end

  return kit or nil
end

local NO_KIT = "a film plays in media.elf, the image with the Media Kit; "
               .. "this program is not in it (its header wants "
               .. "`-- kosmos: image media`)"

--
-- **What a codec is called, in the words a person reads**, from the name
-- the Media Kit gives it - FFmpeg's.
--
local NAMES = {
  h264 = "H.264", hevc = "H.265", vp9 = "VP9", vp8 = "VP8", av1 = "AV1",
  mjpeg = "Motion JPEG", mpeg4 = "MPEG-4 Visual",
  aac = "AAC", opus = "Opus", vorbis = "Vorbis", flac = "FLAC", mp3 = "MP3",
}

--
-- AAC says its profile, which is the difference a person can hear between
-- one and another: "AAC-LC", "HE-AAC", as this kit said before M6.
--
local function named(codec, profile)
  codec = tostring(codec)

  if codec == "aac" and profile == "LC" then return "AAC-LC" end
  if codec == "aac" and profile and profile:match("^HE%-AAC") then return profile end
  if NAMES[codec] then return NAMES[codec] end
  if codec:match("^pcm_") then return "PCM" end

  return codec
end

--
-- **Motion JPEG is decoded by `gfx.jpeg`**, as it always was: one JPEG to
-- a packet, from the packet's own bytes - its address, never a Lua string.
-- The Media Kit's FFmpeg has no JPEG decoder of its own, and needs none:
-- something already supplies one (`CLAUDE.md`, *What this is aiming at*).
--
local MOTION_JPEG = "mjpeg"

--
-- **How many packets wait for the other stream**, at most. One demuxer
-- serves both halves: a packet read for the picture that is the sound's
-- waits in the sound's queue, and the other way round. A film interleaves
-- its streams a second or so apart, so this is far past what one holds;
-- past it, the oldest is let go and counted (`film.overflow`) rather than
-- a queue growing without end behind a stream nobody is taking.
--
local QUEUE = 4096

local function queue() return { first = 1, last = 0 } end

local function push(q, p)
  q.last = q.last + 1
  q[q.last] = p
end

local function pop(q)
  if q.first > q.last then return nil end

  local p = q[q.first]

  q[q.first] = nil
  q.first = q.first + 1
  return p
end

local function drop_all(q)
  while true do
    local p = pop(q)

    if not p then break end

    p:free()
  end
end

--------------------------------------------------------------------------
-- **The sound**: a film's audio stream, decoded by the Media Kit into
-- 16-bit PCM and played by the Playback Kit on a thread of its own in the
-- audio band (`docs/playback.md`), and the clock the picture follows.
--
-- The window's part is the decoding, on every pass of its loop, as far
-- ahead as the kit's input ring takes - seconds - so a picture's decode, a
-- collection or a busy desktop holding the loop away is no gap in the
-- sound. Converting to what the device takes and writing the stream's
-- ring is the kit's thread's.
--------------------------------------------------------------------------

--
-- **How deep a film's ring is: 32 periods, 186 ms** - where Music's is the
-- default 8. `audio.lua` says why a video player is the other end of that
-- choice: a film would far rather be 200 ms behind than skip, because a gap
-- desynchronises everything after it, and the picture follows what is
-- heard whatever the delay.
--
local SOUND_PERIODS = 32

--
-- **A WAV's piece is 16 KB at most** (`playback_player.c`, `PB_PIECE`): a
-- decoded frame larger than that - FLAC's are 16 KB in stereo, more in
-- six channels - goes in pieces, each a whole number of sample frames.
--
local PIECE = 16 * 1024

local voice = {}
voice.__index = voice

--
-- A film's sound, ready to start: a decoder and a player, and nothing
-- playing yet. Nil and why when it cannot be heard.
--
local function open_voice(f)
  local fmt = audio.format()

  if not fmt or (fmt.period or 0) == 0 then
    return nil, "this machine has no sound device"
  end

  local st = f.sound
  local dec, why = f.src:decoder(f.ai)

  if not dec then return nil, tostring(why) end

  local playback = use("/Kosmos/Kits/playback")
  local player, perr = playback.open{ kind = "wav", rate = st.rate,
                                      channels = st.channels, bits = 16 }

  if not player then
    dec:close()
    return nil, tostring(perr)
  end

  local frame_bytes = st.channels * 2

  return setmetatable({
    film = f, dec = dec, player = player, fmt = fmt, heard = {},
    name = f.name, rate = st.rate, channels = st.channels,
    piece = PIECE - PIECE % frame_bytes,
    base = 0, gain = nil, frames_out = 0, frames_in = 0,
    out0 = 0, in0 = 0, ended = false,
  }, voice)
end

--
-- Start hearing it at `at` seconds: a stream of its own, the decoder let go
-- of what it held, and the sound from the frame that holds that moment. The
-- caller has put the film's reading there first (`film:reposition`).
--
-- **A new stream for every start**, as Music seeks: nothing can drop what a
-- stream has queued - `audioproto.h` has no flush - and the ring and the
-- device hold about 200 ms of the old place. The kit is told first and has
-- let go of the old ring before it is closed; then the input ring is
-- filled, so the thread has the new place before it is asked to play it.
--
-- **A seek lands on a key picture**, which is before `at` by as much as a
-- second of film, and the sound read from there is too: frames that end
-- before `at` are decoded and passed over, so what is heard begins at the
-- frame that holds `at` - a few milliseconds before it.
--
function voice:start(at)
  local stream, why = audio.open(self.name, SOUND_PERIODS)

  if not stream then return false, tostring(why) end

  if self.gain then audio.set{ stream = stream.id, gain = self.gain } end

  local ok, serr = pcall(self.player.seek, self.player, stream.ring)

  if not ok then
    stream:close()
    return false, tostring(serr)
  end

  if self.stream then self.stream:close() end

  if self.held then self.held:free() self.held = nil end

  self.dec:flush()
  self.stream = stream
  self.from_us = math.floor(at * 1e6 + 0.5)
  self.base, self.based = at, false
  self.pending, self.sent_end, self.drained = nil, false, false
  self.ended = false

  local h = self.player:state(self.heard)

  self.out0, self.in0 = h.frames_out, h.frames_in
  self.frames_out, self.frames_in = 0, 0
  self:feed()
  return true
end

--
-- **The input ring kept full**, on every pass of the caller's loop: the
-- stream's next frames decoded and handed to the kit while it has room, and
-- the end said once the last has gone. A frame that will not go waits for
-- the next pass - in the decoder's own memory, which stays its until the
-- next call - and the kit holds seconds, so a pass that comes late costs
-- nothing heard. Returns how many pieces went.
--
function voice:feed()
  if not self.stream then return 0 end

  local p, put = self.player, 0

  for _ = 1, 4096 do
    local pending = self.pending

    if pending then
      local n = math.min(pending.len - pending.off, self.piece)

      if not p:frame(pending.at + pending.off, n) then break end

      pending.off = pending.off + n
      put = put + 1

      if pending.off >= pending.len then self.pending = nil end
    elseif self.drained then
      break
    else
      local at, len, pts, frames = self.dec:sound()

      if at == false then
        local packet = self.held or self.film:pull(self.film.ai)

        self.held = nil

        if packet then
          local ok, why = self.dec:send(packet)

          if ok == false then
            self.held = packet
          else
            packet:free()

            -- A packet that will not decode is passed over: the decoder
            -- conceals what it can, and a blemish is not the end.
            if ok == nil then self.error = tostring(why) end
          end
        elseif not self.sent_end then
          self.dec:send()
          self.sent_end = true
        else
          self.drained = true
        end
      elseif at == nil then
        if len then self.error = tostring(len) end

        self.drained = true
      elseif len > 0 then
        local known = pts ~= math.mininteger

        if not known or pts + frames * 1000000 // self.rate > self.from_us then
          if not self.based then
            self.base = known and pts / 1e6 or self.from_us / 1e6
            self.based = true
          end

          self.pending = { at = at, len = len, off = 0 }
        end
      end
    end
  end

  if self.drained and not self.pending and not self.ended then
    self.ended = p:ended()
  end

  self:hear()
  return put
end

-- What the kit has heard, since this start: frames converted and written,
-- which `run_film.py` reads, and why a frame would not go.
function voice:hear()
  local h = self.player:state(self.heard)

  self.frames_out = h.frames_out - self.out0
  self.frames_in = h.frames_in - self.in0

  if h.refused > 0 and h.why ~= "" then self.error = h.why end

  return h
end

-- Seconds heard: where this start began, and the frames out of the speaker
-- since, at the device's rate - which the kit has converted to.
function voice:position()
  if not self.stream then return self.base end

  return self.base + self.stream:position() / self.fmt.rate
end

-- All of it decoded, handed over, written and played.
function voice:done()
  return self.ended and self:hear().finished
         and (not self.stream or self.stream:queued() == 0)
end

function voice:play() self.player:play() end
function voice:pause() self.player:pause() end

function voice:volume(level)
  self.gain = math.floor(math.max(0, math.min(1, level)) * 256 + 0.5)

  if self.stream then
    return audio.set{ stream = self.stream.id, gain = self.gain }
  end

  return true
end

-- The thread stopped first, so nothing is writing into the ring when the
-- stream goes.
function voice:close()
  if self.player then self.player:close() self.player = nil end
  if self.stream then self.stream:close() self.stream = nil end
  if self.held then self.held:free() self.held = nil end
  if self.dec then self.dec:close() self.dec = nil end
end

--
-- A film, or nil and why.
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

  local media = door()

  if not media then return nil, NO_KIT end

  local size = (fs.getattr(path) or {}).size

  if not size or size == 0 then
    return nil, "there is no film at " .. tostring(path)
  end

  local src, why = media.open(path)

  if not src then return nil, tostring(why) end

  local picture, sound

  for i, st in ipairs(src:streams()) do
    if st.kind == "video" and not picture then picture = st picture.index = i - 1 end
    if st.kind == "audio" and not sound then sound = st sound.index = i - 1 end
  end

  if not picture and not sound then
    src:close()
    return nil, "this film has neither a picture nor a sound in it"
  end

  local longest = 0

  for _, st in ipairs({ picture or {}, sound or {} }) do
    if (st.duration_us or 0) > longest then longest = st.duration_us end
  end

  local f = setmetatable({
    path = path, src = src,
    vi = picture and picture.index, ai = sound and sound.index,
    width = picture and picture.width or 0, height = picture and picture.height or 0,
    codec = picture and named(picture.codec) or "none",
    duration = longest / 1e6,
    fps = (picture and (picture.fps_den or 0) > 0)
          and picture.fps_num / picture.fps_den or 0,
    vq = queue(), aq = queue(), overflow = 0,

    --
    -- **What a frame costs, in counter ticks, kept by the kit** because it
    -- is the only place that can see the parts apart: taking the packet off
    -- the file, decoding it onto a surface, and putting it on the screen.
    --
    -- Ticks rather than milliseconds, and `_ticks` in the name, because a
    -- number that leaves here arrives somewhere that has to divide by a
    -- rate this file has no business reading (`CLAUDE.md`, two clocks).
    -- The caller reads `counter_hz` next to its own sum.
    --
    read_ticks = 0, decode_ticks = 0, blit_ticks = 0, decoded = 0,

    debuginfo = options.debuginfo and true or false,
    info_open = options.debuginfo and options.open_now and true or false,
    hz = (fs.read("/Devices/cpu") or {}).counter_hz or 1,
    drawn = 0, dropped = 0, rate = 0,
    rate_at = sys.ticks(), rate_frames = 0,
    ms_read = 0, ms_decode = 0, ms_screen = 0,
  }, film)

  f.frames = math.floor(f.duration * f.fps + 0.5)

  --
  -- **Bits a second, from the file's size over its length**: an MP4 or a
  -- WebM has no field that must hold it, and an average over the whole is
  -- what the overlay means by it. Diego asked for it on 20 September.
  --
  f.bytes = size
  f.bitrate = (f.duration > 0) and (size * 8 / f.duration) or 0

  if picture and picture.codec ~= MOTION_JPEG then
    local dec, derr = src:decoder(f.vi)

    if not dec then
      src:close()
      return nil, "this film is " .. f.codec .. ", which will not decode: " .. tostring(derr)
    end

    f.vdec = dec
  end

  --
  -- The sound, described and not yet played.
  --
  if sound then
    f.sound = {
      codec = named(sound.codec, sound.profile),
      rate = sound.rate or 0,
      channels = sound.channels or 0,
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
-- **The next packet of stream `index`**, from its queue or read from the
-- file: what is read for the other stream waits in that stream's queue -
-- the sound's only while it is being heard. Nil at the end.
--
function film:pull(index)
  local mine = (index == self.vi) and self.vq or self.aq
  local p = pop(mine)

  if p then return p end

  if self.eof then return nil end

  while true do
    local packet, why = self.src:read()

    if not packet then
      self.eof = true
      if why then self.error = tostring(why) end
      return nil
    end

    local s = packet.stream

    if s == index then return packet end

    local other = (s == self.vi) and self.vq
                  or (s == self.ai and self.voice_state) and self.aq
                  or nil

    if other then
      push(other, packet)

      if other.last - other.first >= QUEUE then
        pop(other):free()
        self.overflow = self.overflow + 1
      end
    else
      packet:free()
    end
  end
end

--
-- **The film's reading put at `us` microseconds**: the file sought to the
-- key picture at or before it, the decoders and both queues let go of what
-- they held. The sound starts again from there itself (`voice:start`).
--
function film:reposition(us)
  self.src:seek(math.max(0, us))

  drop_all(self.vq)
  drop_all(self.aq)

  if self.held then self.held:free() self.held = nil end
  if self.vdec then self.vdec:flush() end

  self.eof, self.finished = false, false
  self.pending_pts, self.shown_pts = nil, nil
end

--
-- Which frame is on screen at `when` seconds, counted from one: what an
-- application compares to decide that the picture has changed.
--
function film:index_at(when)
  if self.frames == 0 then return nil end

  return math.max(1, math.min(self.frames, math.floor((when or 0) * self.fps) + 1))
end

--
-- **How long one call may spend decoding**, in seconds. A seek lands on the
-- key picture before the moment asked for and decodes forward from there,
-- which can be a second of film; and a machine that decodes slower than the
-- film plays falls behind. Neither is allowed to take the window with it: a
-- call decodes for this long and shows the latest picture it has, and the
-- next call goes on from there. A machine that cannot keep up shows fewer
-- frames, counted as dropped, rather than stopping to answer the pointer.
--
local DECODE_BUDGET = 0.040

--
-- **How far ahead of what is shown a moment may be** before the film is
-- sought rather than decoded up to: decoding three seconds of pictures
-- nobody will see costs more than starting again from a key picture.
--
local FAR_US = 3000000

--
-- **The next picture out of the film**, into the spare surface: its time,
-- or false when there is none yet and nothing more to send, or nil and why.
--
function film:next_picture()
  local before = sys.ticks()

  if not self.vdec then
    -- Motion JPEG: one packet, one picture, decoded by `gfx.jpeg`.
    local packet = self:pull(self.vi)

    if not packet then return false end

    local read_done = sys.ticks()
    local ok, pic = pcall(gfx.jpeg, packet.at, packet.size)
    local pts = packet.pts_us or 0

    packet:free()

    if not ok or not pic then
      self.error = "a picture would not decode: " .. tostring(pic)
      return nil, self.error
    end

    if self.spare then self.spare:free() end

    self.spare = pic
    self.read_ticks = self.read_ticks + (read_done - before)
    self.decode_ticks = self.decode_ticks + (sys.ticks() - read_done)
    self.decoded = self.decoded + 1
    return pts
  end

  for _ = 1, 64 do
    local pts, why = self.vdec:picture(self.spare)

    if pts then
      self.decoded = self.decoded + 1
      self.decode_ticks = self.decode_ticks + (sys.ticks() - before)
      return pts ~= math.mininteger and pts or 0
    end

    if pts == nil then
      if why then self.error = tostring(why) end
      return false
    end

    -- It wants another packet.
    local packet = self.held or self:pull(self.vi)

    self.held = nil

    if packet then
      local ok, serr = self.vdec:send(packet)

      if ok == false then
        self.held = packet
      else
        packet:free()

        -- A packet that will not decode is passed over rather than
        -- stopping the film: the decoder conceals what it can, and a
        -- damaged frame in the middle of a film is a blemish, not an end.
        -- What it said is kept for whoever asks (`film.error`).
        if ok == nil then self.error = tostring(serr) end
      end
    elseif not self.finished then
      self.vdec:send()
      self.finished = true
    else
      return false
    end
  end

  return false
end

--
-- **The picture for `when` seconds, as a surface.**
--
-- Two surfaces: the one on screen, and the spare the next picture is
-- decoded into as it comes out - often a picture for later, since pictures
-- come out in the order they are shown and the decoder works a few ahead.
-- It waits in the spare until its moment and the two are swapped; no
-- picture is decoded twice.
--
function film:picture_at(when)
  if not self.vi then return nil, "this film has no picture" end

  local want = math.floor(when * 1e6 + 0.5)

  if self.vdec and not self.picture then
    self.picture = gfx.surface{ w = self.width, h = self.height }
    self.spare = gfx.surface{ w = self.width, h = self.height }

    if not self.picture or not self.spare then
      return nil, "no memory for a " .. self.width .. " by " .. self.height
                  .. " picture"
    end
  end

  --
  -- Start again from a key picture when going backwards, or when the
  -- moment is far past what has come out.
  --
  if self.shown_pts and (want < self.shown_pts or want > self.shown_pts + FAR_US) then
    self:reposition(want)

    if self.voice_state and self.voice_state.stream then
      self.voice_state:start(when)
    end
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

    local pts = self:next_picture()

    if not pts then break end

    self.pending_pts = pts
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
-- decoding it.
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

  local picture = self:picture_at(when or 0)

  self.shown = n

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
-- **Whether a point is on the badge**, opening or closing nothing: what a
-- caller asks on the press, to remember that the badge was what was
-- pressed, and again on the release.
--
function film:badge_at(px, py, x, y, w, h)
  if not self.debuginfo then return false end

  x, y = x or 0, y or 0
  w = w or self.width          -- the badge is in the top right; `h` is not needed

  local bx, by = x + w - 8 - BADGE // 2, y + 8 + BADGE // 2
  local dx, dy = px - bx, py - by

  return dx * dx + dy * dy <= (BADGE // 2 + 2) ^ 2
end

--
-- **A click, for the badge.** True when it was the badge's, so a caller can
-- say `if not film:pointer(x, y) then ... end` and go on treating the rest
-- of the picture as its own.
--
-- A click is a press and a release (Diego, 9 October 2026: "mouse click is
-- working in mouse down not in a real click which is mouse down+mouse
-- up"), so this is called on the *release*, by a caller that saw the press
-- land on the badge too (`badge_at`): the panel opens or closes when the
-- button comes up over it, and not if it is let go anywhere else.
--
function film:pointer(px, py, x, y, w, h)
  if not self:badge_at(px, py, x, y, w, h) then return false end

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
      local v, why = open_voice(self)

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

  if self.voice_state then self.voice_state:play() end

  return true
end

-- Stopped where it is. What the ring and the device hold, about 200 ms,
-- plays out.
function film:pause()
  local c = self.clock

  c.at = self:position()
  c.playing = false

  if self.voice_state then self.voice_state:pause() end
end

--
-- Somewhere else in the film. The reading goes back to the key picture
-- before it, the sound starts again from the frame that holds that moment,
-- and the picture follows it.
--
function film:seek(at)
  local c = self.clock

  at = math.max(0, math.min(at or 0, self.duration))
  c.at, c.since, c.by_counter = at, sys.ticks(), false
  self.started = true
  self:reposition(math.floor(at * 1e6 + 0.5))

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
-- The kit's input ring kept full, on every pass of the caller's loop. A
-- film that is not playing, or has no sound, costs nothing here.
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

--
-- **What the sound's thread has heard** (`docs/playback.md`), for a caller
-- that wants to say how the sound held: periods written, how often the
-- input ran dry, the longest the thread was away between two passes (in
-- counter ticks), whether it is in the audio band and whether all of it
-- has been written. Nil while the film has no sound playing.
--
function film:heard()
  local v = self.voice_state

  if not v or not v.player then return nil end

  return v:hear()
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
  if self.held then self.held:free() self.held = nil end

  if self.vq then drop_all(self.vq) end
  if self.aq then drop_all(self.aq) end

  if self.vdec then self.vdec:close() self.vdec = nil end
  if self.src then self.src:close() self.src = nil end

  self.shown = nil
end

return video
