-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Tags: what an audio file says about itself.
--
--   local tags = use("/lib/tags.lua")
--   local t = tags.read(read, size)    -- read(offset, bytes) -> string or nil
--   t.title  t.artist  t.album  t.genre  t.year  t.track
--   t.cover = { mime = "image/jpeg", offset = 1234, bytes = 56789 }
--
-- **Read from the file, and never written onto it** - Diego, 14 September
-- 2026: "cant we just gget the attributes from the mp3 file with a reader?"
-- So Music's Library reads the start of each file as it builds its list, and
-- a person's files are left as they were.
--
-- An MP3's **ID3v2** tag at its start - versions 2.2, 2.3 and 2.4 - with
-- **ID3v1**'s last 128 bytes filling what it leaves out, or standing alone;
-- and a WAV's **`LIST INFO`** chunk. The cover is where it is in the file and
-- how long, not its bytes, so a list of a thousand songs does not read a
-- thousand pictures to show ten.
--
-- **Over a read function**, as `wav.scan` is, so `tools/test_tags.lua` gives
-- it bytes built by hand on the host and the same file runs on the machine.
-- Header parsing, once a file, with nothing waiting on it: Lua, which cannot
-- read past the end of what it was given.
--
-- **What it does not do, and says so.** A genre given only as ID3v1's number
-- is left out rather than named, since the table of names would be written
-- here from memory. A frame marked compressed or encrypted is skipped rather
-- than guessed at. And a tag whose whole body is unsynchronised has its text
-- read but no cover, because taking the stuffing bytes out moves every
-- offset after them.

local tags = {}

-- The most of one text field read: a title is words, and a field longer
-- than this is not one worth showing.
local TEXT_MOST = 4096

-- The most of an unsynchronised tag read whole to undo it.
local UNSYNC_MOST = 256 * 1024

--------------------------------------------------------------------------
-- Text.
--------------------------------------------------------------------------

local function utf8_of(u)
  if u < 0x80 then return string.char(u) end

  if u < 0x800 then
    return string.char(0xC0 | (u >> 6), 0x80 | (u & 0x3F))
  end

  if u < 0x10000 then
    return string.char(0xE0 | (u >> 12), 0x80 | ((u >> 6) & 0x3F),
                       0x80 | (u & 0x3F))
  end

  return string.char(0xF0 | (u >> 18), 0x80 | ((u >> 12) & 0x3F),
                     0x80 | ((u >> 6) & 0x3F), 0x80 | (u & 0x3F))
end

-- Whether a string is already UTF-8, as many taggers write into fields the
-- format says are Latin-1.
local function valid_utf8(s)
  local i, n = 1, #s

  while i <= n do
    local c = s:byte(i)
    local more

    if c < 0x80 then more = 0
    elseif c >= 0xC2 and c <= 0xDF then more = 1
    elseif c >= 0xE0 and c <= 0xEF then more = 2
    elseif c >= 0xF0 and c <= 0xF4 then more = 3
    else return false end

    for j = 1, more do
      local d = s:byte(i + j)

      if not d or d < 0x80 or d > 0xBF then return false end
    end

    i = i + more + 1
  end

  return true
end

-- Latin-1 as UTF-8 - unless it already was UTF-8, which is what it usually
-- turns out to be when a byte above 127 appears.
local function single_byte(s)
  if valid_utf8(s) then return s end

  return (s:gsub("[\128-\255]", function(c)
    local b = c:byte()

    return string.char(0xC0 | (b >> 6), 0x80 | (b & 0x3F))
  end))
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

    out[#out + 1] = utf8_of(u)
  end

  return table.concat(out)
end

-- A field's bytes in the encoding its first byte names, trimmed, or nil for
-- an encoding there is no such thing as, or nothing left.
local function text(encoding, s)
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

-- What a field is shown as. A genre of "(17)Rock" is Rock, and one that is
-- only a number is left out (see the top); a year is its first four digits,
-- since version 2.4 gives a whole date.
local function tidy(field, s)
  if not s then return nil end

  if field == "genre" then
    local rest = s:match("^%(%d+%)(.*)$")

    if rest then s = rest end
    if s:match("^%d+$") or #s == 0 then return nil end
  elseif field == "year" then
    return s:match("^(%d%d%d%d)")
  end

  return s
end

--------------------------------------------------------------------------
-- ID3v2.
--------------------------------------------------------------------------

-- Frame names, by version. 2.2's are three letters.
local V22 = { TT2 = "title", TP1 = "artist", TAL = "album", TCO = "genre",
              TYE = "year", TRK = "track", PIC = "cover" }

local V23 = { TIT2 = "title", TPE1 = "artist", TALB = "album",
              TCON = "genre", TYER = "year", TDRC = "year", TRCK = "track",
              APIC = "cover" }

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

    if field and not transformed and out[field] == nil then
      if field == "cover" then
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

local function id3v2(read)
  local head = read(0, 10)

  if not head or #head < 10 or head:sub(1, 3) ~= "ID3" then return nil end

  local major, flags = head:byte(4), head:byte(6)
  local length = syncsafe(head, 7)

  if not length or major < 2 or major > 4 then return nil end

  local stop = 10 + length

  -- The whole tag unsynchronised, as 2.2 and 2.3 do it: every FF 00 was FF.
  -- Undone in memory, and read without a cover (see the top).
  if (flags & 0x80) ~= 0 and major < 4 then
    local body = read(10, math.min(length, UNSYNC_MOST))

    if not body then return nil end

    body = body:gsub("\255%z", "\255")

    local function get(off, n)
      return body:sub(off - 10 + 1, off - 10 + n)
    end

    return frames(get, 10, 10 + #body, major, false)
  end

  local at = 10

  -- An extended header: its size counts itself in 2.4, and not in 2.3.
  if (flags & 0x40) ~= 0 and major >= 3 then
    local e = read(10, 4)

    if not e or #e < 4 then return nil end

    if major == 3 then
      at = at + 4 + string.unpack(">I4", e)
    else
      local n = syncsafe(e, 1)

      if not n then return nil end

      at = at + n
    end
  end

  return frames(read, at, stop, major, true)
end

--------------------------------------------------------------------------
-- ID3v1, and a WAV's INFO.
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

local INFO = { INAM = "title", IART = "artist", IPRD = "album",
               IGNR = "genre", ICRD = "year", ITRK = "track" }

local function wav_info(read, size)
  local riff = read(0, 12)

  if not riff or #riff < 12 or riff:sub(1, 4) ~= "RIFF"
     or riff:sub(9, 12) ~= "WAVE" then
    return nil
  end

  local at, steps = 12, 0

  -- Chunks are padded to an even length; a bound on how many are walked, so a
  -- file of nonsense sizes cannot keep this going.
  while (not size or at + 8 <= size) and steps < 64 do
    steps = steps + 1

    local h = read(at, 8)

    if not h or #h < 8 then break end

    local id, n = h:sub(1, 4), string.unpack("<I4", h, 5)

    if id == "LIST" and n >= 4 and read(at + 8, 4) == "INFO" then
      local body = read(at + 12, math.min(n - 4, TEXT_MOST * 8)) or ""
      local out, i = {}, 1

      while i + 8 <= #body do
        local sid, sn = body:sub(i, i + 3), string.unpack("<I4", body, i + 4)
        local field = INFO[sid]

        if field and out[field] == nil then
          out[field] = tidy(field, text(0, body:sub(i + 8, i + 7 + sn)))
        end

        i = i + 8 + sn + (sn % 2)
      end

      return out
    end

    at = at + 8 + n + (n % 2)
  end

  return nil
end

--------------------------------------------------------------------------

--
-- Everything a file says about itself that this knows how to read, as one
-- table - empty when it says nothing. `size` is the file's length, which
-- ID3v1 needs to find the end.
--
function tags.read(read, size)
  local t = id3v2(read)

  if t and next(t) then
    local v1 = id3v1(read, size)

    if v1 then
      for k, v in pairs(v1) do
        if t[k] == nil then t[k] = v end
      end
    end

    return t
  end

  return id3v1(read, size) or wav_info(read, size) or {}
end

return tags
