-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Tags: what an audio file says about itself.
--
--   local tags = use("/Kosmos/Libraries/tags.lua")
--   local t = tags.read(read, size)    -- read(offset, bytes) -> string or nil
--   t.title  t.artist  t.album  t.album_artist  t.composer  t.genre  t.year
--   t.track = "7/15"   t.track_number = 7   t.track_total = 15
--   t.disc  = "1/2"    t.disc_number  = 1   t.disc_total  = 2
--   t.compilation = true
--   t.cover = { mime = "image/jpeg", offset = 1234, bytes = 56789 }
--   t.gapless = { delay = 576, padding = 1260, length = 44100,
--                 source = "LAME" }                -- or "iTunSMPB"
--   t.replaygain = { track_gain = -6.54, track_peak = 0.98,
--                    album_gain = -7.01, album_peak = 1.0 }
--
-- **Read from the file, and never written onto it** - Diego, 14 September
-- 2026: "cant we just gget the attributes from the mp3 file with a reader?"
-- So Music's Library reads the start of each file as it builds its list, and
-- a person's files are left as they were.
--
-- What it reads, by the file's first bytes:
--
--   - an MP3's **ID3v2** tag at its start - versions 2.2, 2.3 and 2.4 -
--     with **ID3v1**'s last 128 bytes filling what it leaves out, or standing
--     alone; and the **Xing or Info frame** after it, where LAME (and
--     FFmpeg, which writes the same header) say how many samples of silence
--     the encoder put in front and behind;
--   - a **FLAC** file's metadata blocks: its **Vorbis comments** and its
--     **`PICTURE`** block - and an ID3v2 tag some taggers put in front of
--     one, filling what the comments leave out;
--   - an **Ogg** file's comment packet, for Vorbis and for Opus;
--   - an **MP4** (`.m4a`, AAC or ALAC): `moov/udta/meta/ilst`'s atoms, its
--     `covr`, and its freeform `----` atoms, which is where iTunes keeps
--     `iTunSMPB` and taggers keep ReplayGain;
--   - an **AIFF**'s `ID3 ` chunk, with its `NAME` and `AUTH` filling in;
--   - a **WAV**'s **`LIST INFO`** chunk, and an `id3 ` chunk if it has one.
--
-- ReplayGain is read wherever a file keeps it: a Vorbis comment, an ID3
-- `TXXX` frame or an MP4 freeform atom named `REPLAYGAIN_TRACK_GAIN` and its
-- three siblings, in any case. Gains are decibels and peaks are a fraction of
-- full scale, as numbers.
--
-- **The gapless numbers are what the file says, not what a decoder skips.**
-- `delay` and `padding` are samples a channel, as the encoder wrote them; an
-- MP3 decoder adds its own 529 samples of delay on top (FFmpeg's
-- `528 + 1`), and that sum is the Playback Kit's to make, since it is a fact
-- about the decoder and not the file. `length` is the samples of the music
-- itself, where the file says: `iTunSMPB`'s own field, or the Xing frame's
-- count of frames times the samples in one, less the delay and the padding.
-- In an MP3 the LAME header wins over an `iTunSMPB` comment, being the
-- encoder's own word.
--
-- The cover is where it is in the file and how long, not its bytes, so a
-- list of a thousand songs does not read a thousand pictures to show ten.
-- A FLAC file with several pictures gives the front cover if it names one,
-- and the first otherwise; its `width`, `height` and `kind` come with it.
--
-- **Over a read function**, as `wav.scan` is, so `tools/test_tags.lua` gives
-- it bytes built by hand on the host and the same file runs on the machine.
-- Header parsing, once a file, with nothing waiting on it: Lua, which cannot
-- read past the end of what it was given. The read function is asked
-- through a window of 4 KB, so walking a header a few bytes at a time costs
-- one read of the file and not one for each field.
--
-- **A size is a claim, and one that points outside what holds it is
-- refused.** An MP4 atom longer than its parent, a FLAC block or a Vorbis
-- comment past the end of what contains it, an AIFF or WAV chunk past its
-- form: the walk stops there rather than following it, and what was read
-- before it stands. Following one would read the wrong bytes as a tag at
-- best, and at worst walk the whole of a song's audio a header at a time.
--
-- **What it does not do, and says so.** A genre given only as a number -
-- ID3v1's, or MP4's `gnre` - is left out rather than named, since the table
-- of names would be written here from memory. A frame marked compressed or
-- encrypted is skipped rather than guessed at. A tag whose whole body is
-- unsynchronised has its text read but no cover, because taking the
-- stuffing bytes out moves every offset after them. An Ogg file's picture
-- is base64 inside a comment, which has no offset in the file to give, so
-- it has no cover here. Opus's `R128_TRACK_GAIN` is not ReplayGain and is
-- not turned into it. LAME's own ReplayGain fields are left for the
-- `REPLAYGAIN_*` tags, which is where every tagger that writes them puts
-- them. And an MP4's edit list, the other place it keeps its priming, is
-- the demuxer's: FFmpeg reads it as it reads the samples.

local tags = {}

-- The most of one text field read: a title is words, and a field longer
-- than this is not one worth showing.
local TEXT_MOST = 4096

-- The most of an unsynchronised tag read whole to undo it.
local UNSYNC_MOST = 256 * 1024

-- How much of the file is read at once, for the walks that read a few bytes
-- at a time.
local WINDOW = 4096

-- Bounds on how many things are walked, so a file of nonsense sizes cannot
-- keep a walk going: atoms in one MP4 container, FLAC metadata blocks, Vorbis
-- comments, Ogg pages, and the most of an Ogg comment packet put together.
local ATOMS_MOST    = 1024
local BLOCKS_MOST   = 128
local COMMENTS_MOST = 1024
local PAGES_MOST    = 64
local PACKET_MOST   = 64 * 1024

-- Where a file with no size given is taken to end, for the walks that need
-- an end: further than any file.
local NO_END = 1 << 62

--------------------------------------------------------------------------
-- Reading.
--------------------------------------------------------------------------

--
-- The read function, through one window: a request inside the last window
-- read is answered from it, a small one outside reads a new window from
-- where it starts, and a large one goes straight through. What it returns is
-- what `read` would have: at most `n` bytes, and nil past the end.
--
local function buffered(read)
  local wat, wdata = -1, ""

  return function(offset, n)
    if n <= 0 then return "" end

    if offset >= wat and offset + n <= wat + #wdata then
      return wdata:sub(offset - wat + 1, offset - wat + n)
    end

    if n > WINDOW then return read(offset, n) end

    local got = read(offset, WINDOW)

    if not got or #got == 0 then return nil end

    wat, wdata = offset, got

    local s = got:sub(1, n)

    return (#s > 0) and s or nil
  end
end

-- A string behind the same kind of function, for what has been read whole.
local function over(s)
  return function(offset, n)
    if offset >= #s then return nil end

    return s:sub(offset + 1, offset + n)
  end
end

-- Fill what `into` leaves out from `from`.
local function fill(into, from)
  for k, v in pairs(from or {}) do
    if into[k] == nil then into[k] = v end
  end

  return into
end

--------------------------------------------------------------------------
-- Text.
--------------------------------------------------------------------------

--
-- Latin-1 as UTF-8 - unless it already was UTF-8, which is what it usually
-- turns out to be when a byte above 127 appears: many taggers write UTF-8
-- into fields the format says are Latin-1. A Latin-1 byte is its own code
-- point.
--
-- **UTF-8 is Lua's own `utf8` here**, both ways: `utf8.char` writes a code
-- point, and `utf8.len` is nil for bytes that are not UTF-8 - strictly, so a
-- surrogate or a character spelt long is not taken for it, where the check
-- written out by hand that this was let both through.
--
local function single_byte(s)
  if utf8.len(s) then return s end

  return (s:gsub("[\128-\255]", function(c) return utf8.char(c:byte()) end))
end

-- UTF-16, with a byte order mark or in the order given, as UTF-8; a
-- surrogate without its other half is U+FFFD. Stops at the first NUL.
local function utf16(s, big)
  local out, i = {}, 1

  if #s >= 2 then
    local a, b = s:byte(1, 2)

    if a == 0xFF and b == 0xFE then big, i = false, 3
    elseif a == 0xFE and b == 0xFF then big, i = true, 3 end
  end

  local function unit(at)
    local x, y = s:byte(at, at + 1)

    return big and ((x << 8) | y) or ((y << 8) | x)
  end

  while i + 1 <= #s do
    local u = unit(i)

    i = i + 2

    if u == 0 then break end

    if u >= 0xD800 and u <= 0xDBFF and i + 1 <= #s then
      local lo = unit(i)

      if lo >= 0xDC00 and lo <= 0xDFFF then
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)
        i = i + 2
      else
        u = 0xFFFD
      end
    elseif u >= 0xD800 and u <= 0xDFFF then
      u = 0xFFFD
    end

    out[#out + 1] = utf8.char(u)
  end

  return table.concat(out)
end

-- A field's bytes in the encoding its first byte names, trimmed, or nil for
-- an encoding there is no such thing as, or nothing left. The numbers are
-- ID3's, and the other formats borrow them: 0 Latin-1, 1 UTF-16 with a byte
-- order mark, 2 UTF-16 big-endian, 3 UTF-8.
local function text(encoding, s)
  if not s then return nil end

  if encoding == 0 then
    s = single_byte((s:gsub("%z.*$", "")))
  elseif encoding == 1 then
    s = utf16(s, false)
  elseif encoding == 2 then
    s = utf16(s, true)
  elseif encoding == 3 then
    s = (s:gsub("%z.*$", ""))
  else
    return nil
  end

  s = s:gsub("^%s+", ""):gsub("%s+$", "")

  return (#s > 0) and s or nil
end

-- Two strings ended as `encoding` ends a string - a description and its
-- value - or nil when the first is never ended. UTF-16's end is two NULs on
-- a unit's boundary.
local function split(encoding, s)
  if encoding == 1 or encoding == 2 then
    local j = 1

    while j + 1 <= #s do
      if s:byte(j) == 0 and s:byte(j + 1) == 0 then
        return s:sub(1, j - 1), s:sub(j + 2)
      end

      j = j + 2
    end

    return nil
  end

  local z = s:find("%z")

  if not z then return nil end

  return s:sub(1, z - 1), s:sub(z + 1)
end

-- What a field is shown as. A genre of "(17)Rock" is Rock, and one that is
-- only a number is left out (see the top); a year is its first four digits,
-- since version 2.4 gives a whole date; a compilation is a 1, and anything
-- else says it is not one.
local function tidy(field, s)
  if not s then return nil end

  if field == "genre" then
    local rest = s:match("^%(%d+%)(.*)$")

    if rest then s = rest end
    if s:match("^%d+$") or #s == 0 then return nil end
  elseif field == "year" then
    return s:match("^(%d%d%d%d)")
  elseif field == "compilation" then
    return (s:match("^1") or s:lower() == "true") and true or nil
  end

  return s
end

-- "7/15" as 7 and 15; "7" as 7 and nil.
local function numbers(s)
  local a, b = s:match("^%s*(%d+)%s*/%s*(%d+)")

  if a then return tonumber(a), tonumber(b) end

  a = s:match("^%s*(%d+)")

  return a and tonumber(a) or nil, nil
end

--------------------------------------------------------------------------
-- What several formats keep under a name of their own: ReplayGain, and
-- iTunes's gapless note.
--------------------------------------------------------------------------

local REPLAYGAIN = { replaygain_track_gain = "track_gain",
                     replaygain_track_peak = "track_peak",
                     replaygain_album_gain = "album_gain",
                     replaygain_album_peak = "album_peak" }

-- "-6.54 dB" as -6.54, and "0.988" as 0.988: the number a value starts
-- with, and nil when it does not start with one.
local function leading_number(s)
  return tonumber(s:match("^%s*([+-]?%d*%.?%d+)") or "")
end

--
-- iTunes's `iTunSMPB`: words in hexadecimal, of which the second is the
-- encoder's delay, the third its padding and the fourth the samples of the
-- music itself. The first is always nought, and the rest say nothing a
-- player needs.
--
local function smpb(s)
  local w = {}

  for x in s:gmatch("%x+") do
    if #x > 16 then return nil end

    w[#w + 1] = tonumber(x, 16)

    if #w == 4 then break end
  end

  if #w < 4 then return nil end

  return { delay = w[2], padding = w[3], length = w[4], source = "iTunSMPB" }
end

-- A value kept under a name: a Vorbis comment no field claims, an ID3
-- `TXXX`, an MP4 `----` atom. The first of each wins, as everywhere here.
local function freeform(out, key, value)
  if not key or not value then return end

  key = key:lower()

  local rg = REPLAYGAIN[key]

  if rg then
    local x = leading_number(value)

    if x then
      out.replaygain = out.replaygain or {}

      if out.replaygain[rg] == nil then out.replaygain[rg] = x end
    end
  elseif key == "itunsmpb" and out.gapless == nil then
    out.gapless = smpb(value)
  elseif (key == "tcmp" or key == "compilation") and out.compilation == nil then
    -- FFmpeg writes iTunes's TCMP frame as a TXXX of that name.
    out.compilation = tidy("compilation", value)
  end
end

--------------------------------------------------------------------------
-- ID3v2.
--------------------------------------------------------------------------

-- Frame names, by version. 2.2's are three letters. `txxx` and `comm` are
-- frames of a name and a value, more than one to a tag.
local V22 = { TT2 = "title", TP1 = "artist", TAL = "album", TCO = "genre",
              TYE = "year", TRK = "track", PIC = "cover",
              TP2 = "album_artist", TPA = "disc", TCM = "composer",
              TCP = "compilation", TXX = "txxx", COM = "comm" }

local V23 = { TIT2 = "title", TPE1 = "artist", TALB = "album",
              TCON = "genre", TYER = "year", TDRC = "year", TRCK = "track",
              APIC = "cover", TPE2 = "album_artist", TPOS = "disc",
              TCOM = "composer", TCMP = "compilation", TXXX = "txxx",
              COMM = "comm" }

-- Seven bits a byte, the top bit of each clear: a size that cannot be
-- mistaken for the start of an MP3 frame.
local function syncsafe(s, i)
  local a, b, c, d = s:byte(i, i + 3)

  if not d or ((a | b | c | d) & 0x80) ~= 0 then return nil end

  return (a << 21) | (b << 14) | (c << 7) | d
end

-- Where a cover's picture is, from the start of its frame: the encoding, the
-- picture's type - a MIME type, or 2.2's three letters - the picture's kind,
-- a description ended as its encoding ends a string, and then the picture.
local function picture(major, lead, base, total)
  local encoding = lead:byte(1)
  local mime, i

  if major == 2 then
    local f = lead:sub(2, 4):upper()

    mime = (f == "PNG") and "image/png" or (f == "JPG") and "image/jpeg" or nil
    i = 5
  else
    local z = lead:find("%z", 2)

    if not z then return nil end

    mime = lead:sub(2, z - 1):lower()
    i = z + 1
  end

  i = i + 1                                  -- the picture's kind

  if encoding == 1 or encoding == 2 then
    local j = i

    while j + 1 <= #lead and not (lead:byte(j) == 0 and lead:byte(j + 1) == 0) do
      j = j + 2
    end

    if j + 1 > #lead then return nil end

    i = j + 2
  else
    local z = lead:find("%z", i)

    if not z then return nil end

    i = z + 1
  end

  if i > total then return nil end

  if mime == "jpg" then mime = "image/jpeg" end

  return { mime = mime, offset = base + i - 1, bytes = total - (i - 1) }
end

--
-- A `TXXX` - an encoding, a description, a value - or a `COMM`, which has a
-- language of three letters before its description. A comment is read only
-- for iTunes's gapless note, which iTunes keeps in one; the rest of what
-- people write in comments is not a tag.
--
local function named(out, field, data)
  local encoding = data:byte(1)
  local body = (field == "comm") and data:sub(5) or data:sub(2)
  local d, v = split(encoding, body)

  if not d then return end

  local key = text(encoding, d)

  if field == "comm" and (not key or key:lower() ~= "itunsmpb") then return end

  freeform(out, key, text(encoding, v))
end

-- The frames from file offset `at` to `stop`, through `get(offset, bytes)`.
local function frames(get, at, stop, major, covers)
  local out = {}
  local head_len = (major == 2) and 6 or 10
  local names = (major == 2) and V22 or V23

  while at + head_len <= stop do
    local h = get(at, head_len)

    -- A NUL where a frame's name would be is padding, and the end.
    if not h or #h < head_len or h:byte(1) == 0 then break end

    local id = h:sub(1, (major == 2) and 3 or 4)
    local n, flags

    if major == 2 then
      n, flags = string.unpack(">I3", h, 4), 0
    elseif major == 3 then
      n = string.unpack(">I4", h, 5)
      flags = string.unpack(">I2", h, 9)
    else
      n = syncsafe(h, 5)
      flags = string.unpack(">I2", h, 9)
    end

    if not n or n == 0 or at + head_len + n > stop then break end

    -- Compressed, encrypted, grouped - or, in 2.4, unsynchronised or with a
    -- length in front: bytes that are not the field as it reads.
    local transformed = (major == 3 and (flags & 0x00E0) ~= 0)
                        or (major == 4 and (flags & 0x004F) ~= 0)
    local field = names[id]

    if field and not transformed then
      if field == "txxx" or field == "comm" then
        local data = get(at + head_len, math.min(n, TEXT_MOST))

        if data and #data > 1 then named(out, field, data) end
      elseif out[field] ~= nil then
        -- The first of a field is the one kept.
      elseif field == "cover" then
        if covers then
          local lead = get(at + head_len, math.min(n, 1024))

          if lead then out.cover = picture(major, lead, at + head_len, n) end
        end
      else
        local data = get(at + head_len, math.min(n, TEXT_MOST))

        if data and #data > 1 then
          out[field] = tidy(field, text(data:byte(1), data:sub(2)))
        end
      end
    end

    at = at + head_len + n
  end

  return out
end

--
-- An ID3v2 tag at `base` - the start of a file, or of an AIFF's or a WAV's
-- chunk, which `limit` ends - and where the tag ends, which is where an MP3's
-- first frame or a FLAC file's `fLaC` is looked for.
--
local function id3v2(read, base, limit)
  base = base or 0

  local head = read(base, 10)

  if not head or #head < 10 or head:sub(1, 3) ~= "ID3" then return nil end

  local major, flags = head:byte(4), head:byte(6)
  local length = syncsafe(head, 7)

  if not length or major < 2 or major > 4 then return nil end

  local stop = base + 10 + length
  local ends = stop + ((major == 4 and (flags & 0x10) ~= 0) and 10 or 0)

  if limit and stop > limit then stop = limit end

  -- The whole tag unsynchronised, as 2.2 and 2.3 do it: every FF 00 was FF.
  -- Undone in memory, and read without a cover (see the top).
  if (flags & 0x80) ~= 0 and major < 4 then
    local body = read(base + 10, math.min(stop - base - 10, UNSYNC_MOST))

    if not body then return nil end

    body = body:gsub("\255%z", "\255")

    local function get(off, n)
      return body:sub(off - base - 10 + 1, off - base - 10 + n)
    end

    return frames(get, base + 10, base + 10 + #body, major, false), ends
  end

  local at = base + 10

  -- An extended header: its size counts itself in 2.4, and not in 2.3.
  if (flags & 0x40) ~= 0 and major >= 3 then
    local e = read(base + 10, 4)

    if not e or #e < 4 then return nil end

    if major == 3 then
      at = at + 4 + string.unpack(">I4", e)
    else
      local n = syncsafe(e, 1)

      if not n then return nil end

      at = at + n
    end
  end

  return frames(read, at, stop, major, true), ends
end

--------------------------------------------------------------------------
-- ID3v1.
--------------------------------------------------------------------------

local function id3v1(read, size)
  if not size or size < 128 then return nil end

  local t = read(size - 128, 128)

  if not t or #t < 128 or t:sub(1, 3) ~= "TAG" then return nil end

  local function field(i, n)
    return text(0, t:sub(i, i + n - 1))
  end

  local out = { title = field(4, 30), artist = field(34, 30),
                album = field(64, 30), year = tidy("year", field(94, 4)) }

  -- ID3v1.1: a zero as the comment's last byte but one, and the track in its
  -- last.
  if t:byte(126) == 0 and t:byte(127) ~= 0 then
    out.track = tostring(t:byte(127))
  end

  return out
end

--------------------------------------------------------------------------
-- An MP3's first frame: the Xing or Info frame, and LAME's header in it.
--------------------------------------------------------------------------

--
-- The frame at `at`, if it is one, and if it carries a Xing or an Info
-- header - which sits where the frame's audio would start, after the
-- four-byte header and the side information, whose size depends on the
-- version and on mono. After its fields, which its flags say are there, comes
-- LAME's: nine bytes naming the encoder, and at its 21st byte the delay and
-- the padding, twelve bits each.
--
local function xing(get, at, version, b4)
  local mpeg1 = (version == 3)
  local mono = (b4 >> 6) == 3
  local side = mpeg1 and (mono and 17 or 32) or (mono and 9 or 17)
  local x = get(at + 4 + side, 156)

  if not x or #x < 8 then return nil end

  local id = x:sub(1, 4)

  if id ~= "Xing" and id ~= "Info" then return nil end

  local flags = string.unpack(">I4", x, 5)
  local p, count = 9, nil

  if (flags & 1) ~= 0 then
    if #x < p + 3 then return nil end

    count = string.unpack(">I4", x, p)
    p = p + 4
  end

  if (flags & 2) ~= 0 then p = p + 4 end
  if (flags & 4) ~= 0 then p = p + 100 end
  if (flags & 8) ~= 0 then p = p + 4 end

  if #x < p + 23 then return nil end

  -- LAME's header, or FFmpeg's, which writes the same fields under its own
  -- name. Anything else in those bytes is not a header to read them from.
  local encoder = x:sub(p, p + 3)

  if encoder ~= "LAME" and encoder ~= "Lavf" and encoder ~= "Lavc" then
    return nil
  end

  local d = string.unpack(">I3", x, p + 21)
  local out = { delay = d >> 12, padding = d & 0xFFF, source = "LAME" }

  if count then
    local length = count * (mpeg1 and 1152 or 576) - out.delay - out.padding

    if length >= 0 then out.length = length end
  end

  return out
end

-- The first frame from `at`, past the NULs some taggers leave after a tag:
-- a sync, a version that exists, layer III, a bit rate and a sample rate
-- that are not the reserved ones.
local function lame(get, at)
  local piece = get(at, 2048)

  if not piece then return nil end

  local i = piece:find("[^%z]")

  if not i or i + 3 > #piece then return nil end

  local b1, b2, b3, b4 = piece:byte(i, i + 3)

  if b1 ~= 0xFF or (b2 & 0xE0) ~= 0xE0 then return nil end

  local version, layer = (b2 >> 3) & 3, (b2 >> 1) & 3

  if version == 1 or layer ~= 1 or (b3 >> 4) == 15 or ((b3 >> 2) & 3) == 3 then
    return nil
  end

  return xing(get, at + i - 1, version, b4)
end

--------------------------------------------------------------------------
-- Vorbis comments: FLAC's, and Ogg's.
--------------------------------------------------------------------------

local VORBIS = { TITLE = "title", ARTIST = "artist", ALBUM = "album",
                 ALBUMARTIST = "album_artist", ["ALBUM ARTIST"] = "album_artist",
                 ALBUM_ARTIST = "album_artist", GENRE = "genre",
                 DATE = "year", YEAR = "year", TRACKNUMBER = "track",
                 TRACKTOTAL = "track_total", TOTALTRACKS = "track_total",
                 DISCNUMBER = "disc", DISCTOTAL = "disc_total",
                 TOTALDISCS = "disc_total", COMPOSER = "composer",
                 COMPILATION = "compilation" }

--
-- A vendor string, a count, and that many `NAME=value` strings, each with
-- its length in front - little-endian, unlike the rest of FLAC. From `at` to
-- `stop`, which is the end of the block or the packet, and a comment whose
-- length runs past it is refused, with what was read before it kept.
--
local function comments(get, at, stop, out)
  local h = get(at, 4)

  if not h or #h < 4 then return end

  at = at + 4 + string.unpack("<I4", h)

  local c = (at + 4 <= stop) and get(at, 4)

  if not c or #c < 4 then return end

  local count = string.unpack("<I4", c)
  local totals = {}

  at = at + 4

  for _ = 1, math.min(count, COMMENTS_MOST) do
    local l = get(at, 4)

    if not l or #l < 4 then break end

    local n = string.unpack("<I4", l)

    if at + 4 + n > stop then break end

    local s = get(at + 4, math.min(n, TEXT_MOST)) or ""
    local eq = s:find("=", 1, true)

    if eq then
      local key, value = s:sub(1, eq - 1):upper(), text(3, s:sub(eq + 1))
      local field = VORBIS[key]

      if field == "track_total" or field == "disc_total" then
        if totals[field] == nil then totals[field] = value end
      elseif field then
        if out[field] == nil then out[field] = tidy(field, value) end
      else
        freeform(out, key, value)
      end
    end

    at = at + 4 + n
  end

  -- A number and its total in two comments, as one field the way ID3 has it.
  for field, total in pairs({ track = totals.track_total,
                              disc = totals.disc_total }) do
    if out[field] and not out[field]:find("/", 1, true) and total then
      out[field] = out[field] .. "/" .. total
    end
  end
end

--------------------------------------------------------------------------
-- FLAC.
--------------------------------------------------------------------------

-- A `PICTURE` block from `at` to `stop`: its kind, its MIME type and its
-- description, each length in front of it, its width and height, its depth
-- and colours, and then the picture's length and the picture. A MIME type of
-- "-->" is a picture somewhere else, named by a link, and not one here.
local function flac_picture(get, at, stop)
  local function u32(p)
    local s = get(p, 4)

    if not s or #s < 4 then return nil end

    return string.unpack(">I4", s)
  end

  local kind, ml = u32(at), u32(at + 4)

  if not kind or not ml or at + 8 + ml > stop then return nil end

  local mime = (get(at + 8, math.min(ml, 256)) or ""):lower()
  local p = at + 8 + ml
  local dl = u32(p)

  if not dl or p + 4 + dl + 20 > stop then return nil end

  p = p + 4 + dl

  local width, height, n = u32(p), u32(p + 4), u32(p + 16)

  if not n or p + 20 + n > stop or mime == "-->" then return nil end

  if mime == "image/jpg" then mime = "image/jpeg" end

  return { mime = mime, offset = p + 20, bytes = n, width = width,
           height = height, kind = kind }
end

-- The metadata blocks after `fLaC` at `at`: a byte whose top bit says it is
-- the last and whose rest its kind, and a 24-bit length.
local function flac(get, at, size, out)
  at = at + 4

  for _ = 1, BLOCKS_MOST do
    local h = get(at, 4)

    if not h or #h < 4 then break end

    local kind, n = h:byte(1) & 0x7F, string.unpack(">I3", h, 2)
    local body, finish = at + 4, at + 4 + n

    if finish > (size or NO_END) then break end

    if kind == 4 then
      comments(get, body, finish, out)
    elseif kind == 6 and not (out.cover and out.cover.kind == 3) then
      local p = flac_picture(get, body, finish)

      if p and (not out.cover or p.kind == 3) then out.cover = p end
    end

    if (h:byte(1) & 0x80) ~= 0 then break end

    at = finish
  end

  return out
end

--------------------------------------------------------------------------
-- Ogg.
--------------------------------------------------------------------------

--
-- The comment packet is the second packet of the first stream, and a packet
-- is laced across pages: each page has a table of segment lengths, and a
-- packet ends at a segment shorter than 255. Put together up to
-- `PACKET_MOST`, past which a picture is all it can be, and read as far as
-- it goes.
--
local function ogg(get)
  local at, serial, index, parts, have = 0, nil, 0, {}, 0

  local function parse()
    local p = table.concat(parts)
    local out, from = {}, nil

    if p:sub(1, 7) == "\3vorbis" then from = 7
    elseif p:sub(1, 8) == "OpusTags" then from = 8 end

    if from then comments(over(p), from, #p, out) end

    return out
  end

  for _ = 1, PAGES_MOST do
    local h = get(at, 27)

    if not h or #h < 27 or h:sub(1, 4) ~= "OggS" then break end

    local s, count = string.unpack("<I4", h, 15), h:byte(27)
    local lace = get(at + 27, count) or ""

    if #lace < count then break end

    local pos = at + 27 + count

    serial = serial or s

    for i = 1, count do
      local len = lace:byte(i)

      if s == serial then
        if index == 1 and len > 0 and have < PACKET_MOST then
          local piece = get(pos, math.min(len, PACKET_MOST - have)) or ""

          parts[#parts + 1] = piece
          have = have + #piece
        end

        if len < 255 then
          if index == 1 then return parse() end

          index = index + 1
        end
      end

      pos = pos + len
    end

    at = pos
  end

  return parse()
end

--------------------------------------------------------------------------
-- MP4.
--------------------------------------------------------------------------

--
-- An atom at `at` inside a parent that ends at `stop`: its kind, where its
-- body starts and where it ends. A size of 1 is 64 bits after the kind, and
-- 0 is "to the end of the parent". **A size shorter than its own header or
-- running past its parent is refused** - nil - and the walk over the parent
-- stops there.
--
local function atom(get, at, stop)
  local h = get(at, 8)

  if not h or #h < 8 then return nil end

  local n, head = string.unpack(">I4", h), 8

  if n == 1 then
    local big = get(at + 8, 8)

    if not big or #big < 8 then return nil end

    n, head = string.unpack(">I8", big), 16
  elseif n == 0 then
    n = stop - at
  end

  if n < head or at + n > stop then return nil end

  return h:sub(5, 8), at + head, at + n
end

-- Each child of a container from `start` to `stop`, until `fn` says it has
-- what it wanted or a child is refused.
local function each(get, start, stop, fn)
  local at = start

  for _ = 1, ATOMS_MOST do
    if at + 8 > stop then return end

    local kind, body, finish = atom(get, at, stop)

    if not kind or fn(kind, body, finish) then return end

    at = finish
  end
end

-- The first child of a kind.
local function child(get, start, stop, want)
  local found

  each(get, start, stop, function(kind, body, finish)
    if kind == want then found = { body, finish } return true end
  end)

  if found then return found[1], found[2] end
end

-- An item's `data` atom: its type - 1 UTF-8, 2 UTF-16, 13 JPEG, 14 PNG,
-- 21 a signed integer, 0 whatever the item's name implies - after a byte of
-- version, then four bytes of locale, then the value.
local function data(get, start, stop)
  local body, finish = child(get, start, stop, "data")

  if not body or finish - body < 8 then return nil end

  local h = get(body, 8)

  if not h or #h < 8 then return nil end

  return string.unpack(">I3", h, 2), body + 8, finish
end

local function value_text(get, code, at, stop)
  local s = get(at, math.min(stop - at, TEXT_MOST))

  return text((code == 2) and 2 or 3, s)
end

local ILST = { ["\169nam"] = "title", ["\169ART"] = "artist",
               ["aART"] = "album_artist", ["\169alb"] = "album",
               ["\169day"] = "year", ["\169gen"] = "genre",
               ["\169wrt"] = "composer" }

local IMAGE = { [13] = "image/jpeg", [14] = "image/png", [27] = "image/bmp" }

local function ilst(get, start, stop, out)
  each(get, start, stop, function(kind, body, finish)
    local field = ILST[kind]

    if field then
      local code, at, ends = data(get, body, finish)

      if code and out[field] == nil then
        out[field] = tidy(field, value_text(get, code, at, ends))
      end
    elseif kind == "trkn" or kind == "disk" then
      -- Two bytes of nought, the number, the total.
      local code, at, ends = data(get, body, finish)
      local v = code and ends - at >= 6 and get(at, 6)

      if v and #v == 6 then
        local number, total = string.unpack(">I2I2", v, 3)
        local field = (kind == "trkn") and "track" or "disc"

        if number > 0 and out[field] == nil then
          out[field] = (total > 0) and (number .. "/" .. total) or tostring(number)
        end
      end
    elseif kind == "cpil" then
      local code, at, ends = data(get, body, finish)
      local v = code and ends > at and get(at, 1)

      if v and v:byte(1) ~= 0 then out.compilation = true end
    elseif kind == "covr" and out.cover == nil then
      local code, at, ends = data(get, body, finish)

      if code and ends > at then
        local mime = IMAGE[code]

        if not mime then
          local m = get(at, 4) or ""

          mime = (m:sub(1, 2) == "\255\216") and "image/jpeg"
                 or (m == "\137PNG") and "image/png" or nil
        end

        out.cover = { mime = mime, offset = at, bytes = ends - at }
      end
    elseif kind == "----" then
      -- A freeform item: `mean`, whose domain is iTunes's or a tagger's,
      -- `name`, and `data`. Both strings have four bytes of version and
      -- flags in front.
      local nb, nf = child(get, body, finish, "name")
      local code, at, ends = data(get, body, finish)

      if nb and code and nf - nb > 4 then
        local key = get(nb + 4, math.min(nf - nb - 4, 256))

        freeform(out, key, value_text(get, code, at, ends))
      end
    end
  end)
end

--
-- `moov/udta/meta/ilst`, and `moov/meta/ilst` where a tagger put it there.
-- `meta` is a full atom - four bytes of version and flags before its
-- children - except in QuickTime's own files, where its first child's kind
-- is where the version would be.
--
local function meta(get, start, stop, out)
  local body, finish = child(get, start, stop, "meta")

  if not body then return end

  if get(body + 4, 4) ~= "hdlr" then body = body + 4 end

  local lb, lf = child(get, body, finish, "ilst")

  if lb then ilst(get, lb, lf, out) end
end

local function mp4(get, size)
  local out = {}
  local mb, mf = child(get, 0, size or NO_END, "moov")

  if not mb then return out end

  local ub, uf = child(get, mb, mf, "udta")

  if ub then meta(get, ub, uf, out) end

  meta(get, mb, mf, out)

  return out
end

--------------------------------------------------------------------------
-- AIFF, and a WAV's INFO.
--------------------------------------------------------------------------

--
-- A form's chunks from `at` to `stop`: four letters, a size in the form's
-- own byte order, the body padded to an even length. `fn` is given each, and
-- a chunk running past the form is refused.
--
local function chunks(get, at, stop, order, fn)
  for _ = 1, 64 do
    if at + 8 > stop then return end

    local h = get(at, 8)

    if not h or #h < 8 then return end

    local id, n = h:sub(1, 4), string.unpack(order .. "I4", h, 5)

    if at + 8 + n > stop then return end

    fn(id, at + 8, n)

    at = at + 8 + n + (n % 2)
  end
end

local function aiff(get, size)
  local h = get(0, 12)
  local stop = math.min(8 + string.unpack(">I4", h, 5), size or NO_END)
  local out, info = nil, {}

  chunks(get, 12, stop, ">", function(id, at, n)
    if (id == "ID3 " or id == "id3 ") and not out then
      out = id3v2(get, at, at + n)
    elseif id == "NAME" or id == "AUTH" then
      local field = (id == "NAME") and "title" or "artist"

      info[field] = info[field] or text(0, get(at, math.min(n, TEXT_MOST)))
    end
  end)

  return fill(out or {}, info)
end

local INFO = { INAM = "title", IART = "artist", IPRD = "album",
               IGNR = "genre", ICRD = "year", ITRK = "track" }

local function wav_info(read, size)
  local riff = read(0, 12)

  if not riff or #riff < 12 or riff:sub(1, 4) ~= "RIFF"
     or riff:sub(9, 12) ~= "WAVE" then
    return nil
  end

  local info, id3 = nil, nil

  chunks(read, 12, size or NO_END, "<", function(id, at, n)
    if id == "LIST" and n >= 4 and read(at, 4) == "INFO" and not info then
      local body = read(at + 4, math.min(n - 4, TEXT_MOST * 8)) or ""
      local i = 1

      info = {}

      while i + 8 <= #body do
        local sid, sn = body:sub(i, i + 3), string.unpack("<I4", body, i + 4)
        local field = INFO[sid]

        if field and info[field] == nil then
          info[field] = tidy(field, text(0, body:sub(i + 8, i + 7 + sn)))
        end

        i = i + 8 + sn + (sn % 2)
      end
    elseif (id == "id3 " or id == "ID3 ") and not id3 then
      id3 = id3v2(read, at, at + n)
    end
  end)

  if not info and not id3 then return nil end

  return fill(id3 or {}, info)
end

--------------------------------------------------------------------------

-- An MP3, or what has no other header: ID3v2 - with a FLAC file behind one
-- read as FLAC - then ID3v1 filling in, then the first frame's LAME header.
local function mpeg(get, size)
  local t, ends = id3v2(get, 0)

  if ends and get(ends, 4) == "fLaC" then
    return fill(flac(get, ends, size, {}), t)
  end

  t = fill(t or {}, id3v1(get, size))

  local g = lame(get, ends or 0)

  if g then t.gapless = g end

  return t
end

--
-- Everything a file says about itself that this knows how to read, as one
-- table - empty when it says nothing. `size` is the file's length, which
-- ID3v1 needs to find the end and every walk uses to refuse a size past it.
--
function tags.read(read, size)
  local get = buffered(read)
  local head = get(0, 12) or ""
  local t

  if head:sub(1, 4) == "fLaC" then
    t = flac(get, 0, size, {})
  elseif head:sub(1, 4) == "OggS" then
    t = ogg(get)
  elseif head:sub(5, 8) == "ftyp" then
    t = mp4(get, size)
  elseif #head == 12 and head:sub(1, 4) == "FORM"
         and (head:sub(9, 12) == "AIFF" or head:sub(9, 12) == "AIFC") then
    t = aiff(get, size)
  elseif head:sub(1, 4) == "RIFF" then
    t = fill(wav_info(get, size) or {}, id3v1(get, size))
  else
    t = mpeg(get, size)
  end

  -- A number and its total, as numbers, from wherever they came.
  for _, field in ipairs({ "track", "disc" }) do
    if t[field] then
      t[field .. "_number"], t[field .. "_total"] = numbers(t[field])
    end
  end

  return t
end

return tags
