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
-- know** (`CLAUDE.md`, on kits). Today this decodes Motion JPEG, because
-- `gfx.jpeg` is already in the image; when libavcodec is ported it will
-- decode H.264, and nothing written against this file changes - not one
-- call site. That is the same promise `use("/kits/pdf")` makes about a
-- scanner that moved from Lua to C.
--
-- What it does *not* do, deliberately, is own the clock. An application
-- has a loop already - it is answering the pointer and the keyboard in it -
-- and a kit that took the loop away would have to give back a way to do
-- everything the application was doing in it. So the caller says what
-- moment it wants and this answers with the frame for that moment, which
-- is also exactly what a seek is.
--
local mp4 = use("/lib/mp4.lua")

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

local decoders = {
  -- QuickTime's two names for a track of JPEGs, which say so themselves.
  jpeg = MOTION_JPEG,
  mjpa = MOTION_JPEG,
}

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
  if codec == "avc1" or named == "H.264" then
    return "this film is H.264, and this system has no H.264 decoder yet"
  end

  if named then
    return "this film is " .. named .. ", which this system cannot decode"
  end

  return "this film is " .. tostring(codec) .. ", which this system cannot decode"
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

  local size = (fs.getattr(path) or {}).size

  if not size or size == 0 then
    return nil, "there is no film at " .. tostring(path)
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

  if not movie then return nil, tostring(why) end

  local track, sound

  for _, t in ipairs(movie.tracks) do
    if t.kind == "video" and not track then track = t end
    if t.kind == "audio" and not sound then sound = t end
  end

  if not track then return nil, "this film has no picture in it" end

  local decoder, named = decoder_for(track)

  if not decoder then return nil, no_decoder(track.codec, named) end

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
  local samples = self.track.samples
  local scale = self.track.timescale or 1
  local ticks = when * scale

  if #samples == 0 then return nil end

  local at = self.shown or 1

  -- Backwards first: a seek to the beginning is common and cheap.
  while at > 1 and (samples[at].pts or 0) > ticks do
    at = at - 1
  end

  while at < #samples and (samples[at + 1].pts or 0) <= ticks do
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
  local s = samples and samples[n]

  if not s then return nil, "there is no frame " .. tostring(n) end

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

  local picture = (n == self.shown) and self.picture or self:frame(n)

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

function film:close()
  if self.picture then self.picture:free() self.picture = nil end

  self.track, self.movie, self.shown = nil, nil, nil
end

return video
