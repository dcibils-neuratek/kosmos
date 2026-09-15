-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What an audio file says about itself, read on this machine.
--
--   build/host/lua tools/test_tags.lua
--
-- `tags.lua` is pure over a read function, so each case here is a file built
-- by hand as the format describes it - ID3v2.2, 2.3 and 2.4, ID3v1, a WAV's
-- INFO - and read back through the same code Music runs.
--
-- **Built from the format rather than from a tagger's output**, and that is
-- a known weakness as well as a strength: a field written at the wrong
-- offset here would be read at the same wrong offset and agree. The files
-- people have, tagged by real software, are the other half, and the first of
-- them is the one on Diego's disk.

local tags = assert(loadfile("user/lib/tags.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- A file, as a string, behind the read function tags.read takes.
local function reader(bytes)
  return function(offset, n)
    if offset >= #bytes then return nil end

    return bytes:sub(offset + 1, offset + n)
  end
end

local function read_all(bytes)
  return tags.read(reader(bytes), #bytes)
end

local function syncsafe(n)
  return string.char((n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F)
end

local function tag(major, flags, body)
  return "ID3" .. string.char(major, 0, flags) .. syncsafe(#body) .. body
end

local function frame23(id, data, flags)
  return id .. string.pack(">I4I2", #data, flags or 0) .. data
end

local function frame24(id, data, flags)
  return id .. syncsafe(#data) .. string.pack(">I2", flags or 0) .. data
end

local function frame22(id, data)
  return id .. string.pack(">I3", #data) .. data
end

local function utf16le_bom(s)
  -- ASCII only, enough for the names used here.
  local out = { "\255\254" }

  for i = 1, #s do out[#out + 1] = s:sub(i, i) .. "\0" end

  return table.concat(out) .. "\0\0"
end

local audio = string.rep("\255\251\144\0", 64)       -- stands for MP3 frames

--------------------------------------------------------------------------
-- ID3v2.3: Latin-1, UTF-16 with a byte order mark, a numbered genre with a
-- name after it, a year, a track of a total, and a cover found in place.
--------------------------------------------------------------------------

do
  local picture = string.rep("\1\2\3\4", 1250)       -- 5000 bytes
  local apic = "\0" .. "image/jpeg\0" .. "\3" .. "front\0" .. picture
  local body = frame23("TIT2", "\0Basket Case")
            .. frame23("TPE1", "\1" .. utf16le_bom("Green Day"))
            .. frame23("TALB", "\0Dookie\0")
            .. frame23("TCON", "\0(43)Punk")
            .. frame23("TYER", "\0" .. "1994")
            .. frame23("TRCK", "\0" .. "7/15")
            .. frame23("APIC", apic)
            .. string.rep("\0", 64)                    -- padding
  local file = tag(3, 0, body) .. audio
  local t = read_all(file)

  check(t.title == "Basket Case", "2.3: a Latin-1 title: " .. tostring(t.title))
  check(t.artist == "Green Day", "2.3: a UTF-16 artist with a byte order mark: " .. tostring(t.artist))
  check(t.album == "Dookie", "2.3: an album ended by a NUL: " .. tostring(t.album))
  check(t.genre == "Punk", "2.3: \"(43)Punk\" is Punk: " .. tostring(t.genre))
  check(t.year == "1994" and t.track == "7/15", "2.3: the year and the track")

  local at = file:find("image/jpeg", 1, true)
  local expect = at - 1 + #"image/jpeg\0" + 1 + #"front\0"

  check(t.cover and t.cover.mime == "image/jpeg" and t.cover.bytes == #picture
        and t.cover.offset == expect
        and file:sub(t.cover.offset + 1, t.cover.offset + 8) == picture:sub(1, 8),
        ("2.3: the cover is where its bytes are, %d long at %d: %s"):format(
          #picture, expect, t.cover and (t.cover.offset .. " " .. t.cover.bytes) or "none"))
end

--------------------------------------------------------------------------
-- ID3v2.4: syncsafe frame sizes, UTF-8, a whole date, a genre that is only a
-- number, and a compressed frame skipped rather than read as text.
--------------------------------------------------------------------------

do
  local body = frame24("TIT2", "\3Caf\195\169")
            .. frame24("TPE1", "\3Example Artist", 0x0008)   -- compressed
            .. frame24("TDRC", "\3" .. "2024-05-01")
            .. frame24("TCON", "\3" .. "17")
  local t = read_all(tag(4, 0, body) .. audio)

  check(t.title == "Caf\195\169", "2.4: a UTF-8 title: " .. tostring(t.title))
  check(t.artist == nil, "2.4: a compressed frame is skipped, not read: " .. tostring(t.artist))
  check(t.year == "2024", "2.4: the year out of a whole date: " .. tostring(t.year))
  check(t.genre == nil, "2.4: a genre that is only a number is left out: " .. tostring(t.genre))

  -- A frame of 128 bytes or more, which is where a syncsafe size and a plain
  -- one stop being the same bytes: a cover, and a frame after it that can
  -- only be found if the cover's size was read as seven bits a byte.
  local picture = string.rep("\9", 1000)
  local big = frame24("APIC", "\0image/png\0\3\0" .. picture)
              .. frame24("TALB", "\3After the cover")
  local file = tag(4, 0, big) .. audio
  local b = read_all(file)

  check(b.album == "After the cover",
        "2.4: a frame after one of 1000 bytes is found, so sizes are syncsafe: "
        .. tostring(b.album))
  check(b.cover and b.cover.bytes == #picture
        and file:sub(b.cover.offset + 1, b.cover.offset + #picture) == picture,
        "2.4: a 1000-byte cover found in place")
end

--------------------------------------------------------------------------
-- ID3v2.3 with an extended header, and a frame bigger than the frame header
-- says the tag is.
--------------------------------------------------------------------------

do
  local ext = string.pack(">I4", 6) .. "\0\0\0\0\0\0"
  local body = ext .. frame23("TIT2", "\0After the extended header")
  local t = read_all(tag(3, 0x40, body) .. audio)

  check(t.title == "After the extended header", "2.3: an extended header is stepped over: " .. tostring(t.title))

  local short = "ID3" .. string.char(3, 0, 0) .. syncsafe(100000)
                .. frame23("TIT2", "\0Cut off")
  local ok, cut = pcall(read_all, short)

  check(ok and cut.title == "Cut off",
        "a tag that says it is longer than the file is read as far as it goes: "
        .. tostring(ok and cut.title or cut))
end

--------------------------------------------------------------------------
-- ID3v2.2: three-letter frames, and a picture named by three letters.
--------------------------------------------------------------------------

do
  local picture = string.rep("P", 300)
  local body = frame22("TT2", "\0Old Tag")
            .. frame22("TP1", "\0Old Artist")
            .. frame22("PIC", "\0PNG\3\0" .. picture)
  local file = tag(2, 0, body) .. audio
  local t = read_all(file)

  check(t.title == "Old Tag" and t.artist == "Old Artist", "2.2: three-letter frames")
  check(t.cover and t.cover.mime == "image/png" and t.cover.bytes == #picture
        and file:sub(t.cover.offset + 1, t.cover.offset + #picture) == picture,
        "2.2: a PNG cover found in place")
end

--------------------------------------------------------------------------
-- UTF-16 beyond the first plane, and a surrogate on its own.
--------------------------------------------------------------------------

do
  -- U+1F600 is D83D DE00; then a lone D800.
  local grin = "\1\255\254" .. "H\0i\0 \0" .. "\61\216\0\222" .. "\0\216" .. "\0\0"
  local t = read_all(tag(3, 0, frame23("TIT2", grin)) .. audio)

  check(t.title == "Hi \240\159\152\128\239\191\189",
        "UTF-16: a pair is one character, and a lone half is U+FFFD: " .. tostring(t.title))
end

--------------------------------------------------------------------------
-- ID3v1, alone and filling in.
--------------------------------------------------------------------------

local function v1(title, artist, album, year, track)
  local function pad(s, n) return (s .. string.rep("\0", n)):sub(1, n) end

  local comment = pad("a comment", 28) .. "\0" .. string.char(track or 0)

  return "TAG" .. pad(title, 30) .. pad(artist, 30) .. pad(album, 30)
         .. pad(year, 4) .. comment .. "\255"
end

do
  local t = read_all(audio .. v1("Only Version One", "An Artist", "An Album", "1999", 3))

  check(t.title == "Only Version One" and t.artist == "An Artist"
        and t.album == "An Album" and t.year == "1999",
        "ID3v1 alone: its fixed fields")
  check(t.track == "3", "ID3v1.1: the track in the comment's last byte: " .. tostring(t.track))

  local both = tag(3, 0, frame23("TIT2", "\0From Version Two"))
               .. audio .. v1("Ignored", "Filled In", "", "", 0)
  local b = read_all(both)

  check(b.title == "From Version Two" and b.artist == "Filled In",
        "ID3v2 first, and ID3v1 filling what it left out")
end

--------------------------------------------------------------------------
-- A WAV's INFO.
--------------------------------------------------------------------------

do
  local function sub(id, s)
    local data = s .. "\0"
    return id .. string.pack("<I4", #data) .. data .. ((#data % 2 == 1) and "\0" or "")
  end

  local info = "INFO" .. sub("INAM", "A Tone") .. sub("IART", "Kosmos") .. sub("IGNR", "Test")
  local fmt = "fmt " .. string.pack("<I4", 16) .. string.pack("<I2I2I4I4I2I2", 1, 2, 44100, 176400, 4, 16)
  local list = "LIST" .. string.pack("<I4", #info) .. info
  local data = "data" .. string.pack("<I4", 8) .. string.rep("\0", 8)
  local body = "WAVE" .. fmt .. list .. data
  local file = "RIFF" .. string.pack("<I4", #body) .. body
  local t = read_all(file)

  check(t.title == "A Tone" and t.artist == "Kosmos" and t.genre == "Test",
        "WAV: LIST INFO's name, artist and genre, odd lengths padded")
end

--------------------------------------------------------------------------
-- Nothing to say.
--------------------------------------------------------------------------

do
  local t = read_all(audio)

  check(type(t) == "table" and next(t) == nil, "a file with no tag says nothing, and does not fail")

  local latin = read_all(tag(3, 0, frame23("TIT2", "\0Caf\233")) .. audio)

  check(latin.title == "Caf\195\169", "Latin-1 that is not UTF-8 becomes UTF-8: " .. tostring(latin.title))
end

if failed > 0 then
  print(("FAIL: %d of %d checks on audio tags."):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on audio tags, on this machine (ID3v2.2, 2.3 and 2.4, "
       .. "ID3v1, a WAV's INFO, UTF-16 pairs, and covers found in place)."):format(passed))
