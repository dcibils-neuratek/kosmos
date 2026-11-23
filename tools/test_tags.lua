-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What an audio file says about itself, read on this machine.
--
--   build/host/lua tools/test_tags.lua
--
-- `tags.lua` is pure over a read function, so each case here is a file built
-- by hand as the format describes it - ID3v2.2, 2.3 and 2.4, ID3v1, a WAV's
-- INFO, FLAC's blocks, Ogg's pages, MP4's atoms, AIFF's chunks, LAME's
-- header - and read back through the same code Music runs.
--
-- **Built from the format rather than from a tagger's output**, and that is
-- a known weakness as well as a strength: a field written at the wrong
-- offset here would be read at the same wrong offset and agree. So the
-- other half is files this Mac's FFmpeg writes and tags, made fresh in a
-- directory of their own each run, with FFmpeg's own reading of them -
-- `ffprobe`'s start and length of an MP3 - as the other opinion on the
-- gapless numbers. Without `ffmpeg` that half is skipped, and says so.
--
-- **Every size a file claims is a check with a control**: a size pointing
-- outside what holds it - an MP4 atom past its parent, a FLAC block past the
-- file, a Vorbis comment past its block, a chunk past its form - is built
-- here with a field behind it that only following it would find. Each was
-- seen to fail with its guard taken out of `tags.lua`.

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

--------------------------------------------------------------------------
-- ID3v2: the fields P2 added - album artist, disc, composer, compilation -
-- and the frames of a name and a value: ReplayGain in TXXX, in UTF-16 and in
-- Latin-1, and iTunes's gapless note in a COMM.
--------------------------------------------------------------------------

local SMPB = " 00000000 00000840 000001CA 0000000000D1A36E 00000000 00000000"

do
  local body = frame23("TIT2", "\0Joined")
            .. frame23("TPE2", "\0Various Artists")
            .. frame23("TPOS", "\0" .. "2/3")
            .. frame23("TCOM", "\0A Composer")
            .. frame23("TCMP", "\0" .. "1")
            .. frame23("TXXX", "\1" .. utf16le_bom("replaygain_track_gain")
                               .. utf16le_bom("-3.25 dB"))
            .. frame23("TXXX", "\0REPLAYGAIN_TRACK_PEAK\0" .. "0.912345")
            .. frame23("TXXX", "\0REPLAYGAIN_ALBUM_GAIN\0+1.50 dB")
            .. frame23("COMM", "\0eng\0Just a comment")
            .. frame23("COMM", "\0engiTunSMPB\0" .. SMPB)
  local t = read_all(tag(3, 0, body) .. audio)

  check(t.album_artist == "Various Artists" and t.composer == "A Composer",
        "2.3: TPE2 is the album artist and TCOM the composer")
  check(t.disc == "2/3" and t.disc_number == 2 and t.disc_total == 3,
        "2.3: TPOS is the disc, and its numbers: " .. tostring(t.disc))
  check(t.compilation == true, "2.3: TCMP 1 is a compilation")
  check(t.replaygain and t.replaygain.track_gain == -3.25
        and t.replaygain.track_peak == 0.912345
        and t.replaygain.album_gain == 1.5 and t.replaygain.album_peak == nil,
        "2.3: ReplayGain from TXXX, UTF-16 and Latin-1, a name in any case")
  check(t.gapless and t.gapless.source == "iTunSMPB" and t.gapless.delay == 0x840
        and t.gapless.padding == 0x1CA and t.gapless.length == 0xD1A36E,
        "2.3: iTunSMPB in a COMM: delay, padding and length")
  check(t.track == nil and t.track_number == nil,
        "no track, no track number: the table says only what the file says")

  -- Control: a comment is read for iTunes's note and nothing else, even
  -- one named as ReplayGain is, which belongs in a TXXX.
  local other = read_all(tag(3, 0, frame23("COMM", "\0engREPLAYGAIN_TRACK_GAIN\0-9 dB"))
                         .. audio)

  check(other.replaygain == nil, "control: a COMM is read only when it is named iTunSMPB")
end

--------------------------------------------------------------------------
-- LAME's header in an MP3's first frame.
--------------------------------------------------------------------------

-- A Xing or Info frame: the frame header, side information of `side`
-- bytes, the tag, flags for frames, bytes, a table of contents and a
-- quality, and LAME's 36 bytes with the delay and padding at its 21st.
local function info_frame(header, side, id, count, delay, padding, encoder)
  local lame = (encoder or "LAME3.100"):sub(1, 9)
  lame = lame .. string.rep("\0", 9 - #lame) .. string.rep("\0", 12)
         .. string.pack(">I3", (delay << 12) | padding)
         .. string.rep("\0", 12)
  local frame = header .. string.rep("\0", side) .. id
                .. string.pack(">I4I4I4", 0x0F, count, 12345)
                .. string.rep("\1", 100) .. string.pack(">I4", 50) .. lame

  return frame .. string.rep("\0", 417 - #frame)
end

do
  local stereo = "\255\251\144\100"                 -- MPEG-1, layer III, joint
  local first = info_frame(stereo, 32, "Info", 100, 576, 1000)
  local file = tag(3, 0, frame23("TIT2", "\0Gapless") .. string.rep("\0", 20))
               .. first .. audio
  local t = read_all(file)

  check(t.title == "Gapless" and t.gapless and t.gapless.source == "LAME"
        and t.gapless.delay == 576 and t.gapless.padding == 1000
        and t.gapless.length == 100 * 1152 - 576 - 1000,
        "LAME: delay, padding and length after an ID3v2 tag: "
        .. (t.gapless and (t.gapless.delay .. " " .. t.gapless.padding .. " "
                           .. tostring(t.gapless.length)) or "none"))

  -- MPEG-2, mono: nine bytes of side information and 576 samples a frame,
  -- in a file with no ID3 tag at all.
  local mono = "\255\243\144\192"
  local m = read_all(info_frame(mono, 9, "Xing", 40, 1105, 300) .. audio)

  check(m.gapless and m.gapless.delay == 1105 and m.gapless.padding == 300
        and m.gapless.length == 40 * 576 - 1105 - 300,
        "LAME: MPEG-2 mono, with no tag in front")

  -- LAME wins over an iTunSMPB comment in the same file.
  local both = tag(3, 0, frame23("COMM", "\0engiTunSMPB\0" .. SMPB)) .. first .. audio

  check(read_all(both).gapless.source == "LAME", "LAME's header wins over iTunSMPB in an MP3")

  -- Controls: the tag where mono's side information would put it in a
  -- stereo frame is not there; an encoder that is not LAME's or FFmpeg's
  -- has no delay to read; and nothing in front of the first frame but NULs
  -- is skipped, anything else is not.
  check(read_all(info_frame(stereo, 17, "Info", 100, 576, 1000)).gapless == nil,
        "control: an Info tag at the wrong offset for the frame is not read")
  check(read_all(info_frame(stereo, 32, "Info", 100, 576, 1000, "GOGO1.00")).gapless == nil,
        "control: an encoder that is not LAME's has no delay read")
  check(read_all("junk" .. first).gapless == nil,
        "control: bytes other than NULs before the first frame are not skipped")
end

--------------------------------------------------------------------------
-- FLAC: Vorbis comments and PICTURE blocks.
--------------------------------------------------------------------------

local function le_comment(s) return string.pack("<I4", #s) .. s end

local function vorbis_block(list, vendor)
  local out = { le_comment(vendor or "reference libFLAC 1.4.3"), string.pack("<I4", #list) }

  for _, c in ipairs(list) do out[#out + 1] = le_comment(c) end

  return table.concat(out)
end

local function flac_block(kind, body, last)
  return string.char(kind | (last and 0x80 or 0)) .. string.pack(">I3", #body) .. body
end

local function flac_pic(kind, mime, desc, w, h, data)
  return string.pack(">I4", kind) .. string.pack(">I4", #mime) .. mime
         .. string.pack(">I4", #desc) .. desc
         .. string.pack(">I4I4I4I4", w, h, 24, 0)
         .. string.pack(">I4", #data) .. data
end

local STREAMINFO = string.rep("\0", 34)

do
  local back = string.rep("B", 700)
  local front = string.rep("F", 900)
  local comments = vorbis_block({
    "TITLE=Lossless", "artist=Someone", "ALBUM=Album", "ALBUMARTIST=Band",
    "TRACKNUMBER=3", "TRACKTOTAL=12", "DISCNUMBER=1", "DISCTOTAL=2",
    "DATE=2018-02-03", "GENRE=Ambient", "COMPOSER=Writer", "COMPILATION=1",
    "REPLAYGAIN_TRACK_GAIN=-7.10 dB", "REPLAYGAIN_TRACK_PEAK=0.99",
    "replaygain_album_gain=-8.00 dB", "REPLAYGAIN_ALBUM_PEAK=1.000000",
    "TITLE=Second title, ignored" })
  local file = "fLaC" .. flac_block(0, STREAMINFO)
               .. flac_block(4, comments)
               .. flac_block(6, flac_pic(4, "image/png", "back", 10, 10, back))
               .. flac_block(6, flac_pic(3, "image/jpeg", "front", 640, 480, front))
               .. flac_block(1, string.rep("\0", 100), true) .. audio
  local t = read_all(file)

  check(t.title == "Lossless" and t.artist == "Someone" and t.album == "Album"
        and t.album_artist == "Band" and t.composer == "Writer" and t.genre == "Ambient",
        "FLAC: Vorbis comments, names in any case, the first of each kept")
  check(t.track == "3/12" and t.track_number == 3 and t.track_total == 12
        and t.disc == "1/2" and t.year == "2018" and t.compilation == true,
        "FLAC: TRACKNUMBER and TRACKTOTAL as one, the year from a date: " .. tostring(t.track))

  local rg = t.replaygain or {}

  check(rg.track_gain == -7.1 and rg.track_peak == 0.99 and rg.album_gain == -8
        and rg.album_peak == 1.0, "FLAC: all four ReplayGain values")

  local at = file:find(front, 1, true) - 1

  check(t.cover and t.cover.kind == 3 and t.cover.mime == "image/jpeg"
        and t.cover.offset == at and t.cover.bytes == #front
        and t.cover.width == 640 and t.cover.height == 480,
        "FLAC: the front cover chosen over a back one before it, found in place")

  -- One picture, not the front: it is the cover.
  local only = read_all("fLaC" .. flac_block(0, STREAMINFO)
                        .. flac_block(6, flac_pic(0, "image/png", "", 1, 1, back), true))

  check(only.cover and only.cover.kind == 0 and only.cover.bytes == #back,
        "FLAC: a picture of another kind is the cover when it is the only one")

  -- Controls. A comment whose length runs past its block is refused, with
  -- the comments before it kept and the one inside it never read; a block
  -- whose length runs past the file is refused; a picture whose length runs
  -- past its block is no cover.
  local body = le_comment("vendor") .. string.pack("<I4", 2)
               .. le_comment("ARTIST=Before")
               .. string.pack("<I4", 200) .. "TITLE=Inside"
  local cut = read_all("fLaC" .. flac_block(4, body, true)
                       .. string.rep("\0", 300) .. audio)

  check(cut.artist == "Before" and cut.title == nil,
        "control: a Vorbis comment longer than its block is refused, not followed")

  local long = "fLaC" .. string.char(4 | 0x80) .. string.pack(">I3", 5000)
               .. vorbis_block({ "TITLE=Past the end" })

  check(read_all(long).title == nil, "control: a FLAC block longer than the file is refused")

  local pic = flac_pic(3, "image/png", "", 1, 1, string.rep("P", 50))
  pic = pic:sub(1, -51 - 4) .. string.pack(">I4", 5000) .. string.rep("P", 50)

  check(read_all("fLaC" .. flac_block(6, pic, true) .. string.rep("\0", 6000)).cover == nil,
        "control: a picture longer than its block is no cover")

  -- FLAC behind an ID3v2 tag: the comments first, the tag filling in.
  local behind = tag(3, 0, frame23("TIT2", "\0From ID3") .. frame23("TPE1", "\0ID3 Artist"))
                 .. "fLaC" .. flac_block(4, vorbis_block({ "TITLE=From FLAC" }), true)
  local b = read_all(behind)

  check(b.title == "From FLAC" and b.artist == "ID3 Artist",
        "FLAC behind an ID3v2 tag: its comments first, the tag filling in")
end

--------------------------------------------------------------------------
-- Ogg: a comment packet laced across pages, with another stream between.
--------------------------------------------------------------------------

local function ogg_page(serial, sequence, lacing, data)
  return "OggS\0\0" .. string.rep("\0", 8) .. string.pack("<I4I4", serial, sequence)
         .. "\0\0\0\0" .. string.char(#lacing) .. string.char(table.unpack(lacing)) .. data
end

-- A packet's segment lengths: 255s, then what is left, which is shorter.
local function lace(packet)
  local out, n = {}, #packet

  while n >= 255 do out[#out + 1] = 255 n = n - 255 end

  out[#out + 1] = n

  return out
end

local function ogg_file(head, comment)
  local pages = { ogg_page(7, 0, lace(head), head) }
  local segs, pos, seq = lace(comment), 1, 1

  -- Two segments a page, so the packet crosses pages; a page of another
  -- stream after the first, which is not this stream's.
  for i = 1, #segs, 2 do
    local l = { segs[i], segs[i + 1] }
    local n = l[1] + (l[2] or 0)

    pages[#pages + 1] = ogg_page(7, seq, l, comment:sub(pos, pos + n - 1))
    pos, seq = pos + n, seq + 1

    if i == 1 then pages[#pages + 1] = ogg_page(9, 0, { 10 }, "\3vorbis\0\0\0") end
  end

  return table.concat(pages) .. ogg_page(7, seq, { 4 }, "aaaa")
end

do
  local filler = "COMMENT=" .. string.rep("x", 900)
  local comment = "\3vorbis" .. vorbis_block({ filler, "TITLE=Over pages",
                                              "ARTIST=Ogg", "REPLAYGAIN_TRACK_GAIN=-2 dB" },
                                            "Xiph.Org libVorbis") .. "\1"
  local t = read_all(ogg_file("\1vorbis" .. string.rep("\0", 23), comment))

  check(t.title == "Over pages" and t.artist == "Ogg"
        and t.replaygain and t.replaygain.track_gain == -2,
        "Ogg Vorbis: a comment packet laced over four pages, another stream skipped")

  local opus = read_all(ogg_file("OpusHead" .. string.rep("\0", 11),
                                 "OpusTags" .. vorbis_block({ "TITLE=Opus title" }, "libopus")))

  check(opus.title == "Opus title", "Ogg Opus: OpusTags")
end

--------------------------------------------------------------------------
-- MP4: moov/udta/meta/ilst.
--------------------------------------------------------------------------

local function box(kind, body) return string.pack(">I4", 8 + #body) .. kind .. body end

local function databox(code, value) return box("data", string.pack(">I4I4", code, 0) .. value) end

local function item(kind, code, value) return box(kind, databox(code, value)) end

local function free(name, value, mean)
  return box("----", box("mean", "\0\0\0\0" .. (mean or "com.apple.iTunes"))
                     .. box("name", "\0\0\0\0" .. name) .. databox(1, value))
end

local function m4a(ilst_body, opts)
  opts = opts or {}

  local hdlr = box("hdlr", string.rep("\0", 8) .. "mdirappl" .. string.rep("\0", 9))
  local meta = opts.quicktime and box("meta", hdlr .. box("ilst", ilst_body) .. (opts.after or ""))
               or box("meta", "\0\0\0\0" .. hdlr .. box("ilst", ilst_body) .. (opts.after or ""))
  local moov = box("moov", box("mvhd", string.rep("\0", 100))
                           .. box("trak", string.rep("\0", 300)) .. box("udta", meta))

  -- `mdat` in front of `moov`, as a file written in one pass has it, and in
  -- 64-bit form, so the walk has to step over a size of 1.
  local payload = string.rep("\0", 2000)
  local mdat = string.pack(">I4", 1) .. "mdat" .. string.pack(">I8", 16 + #payload) .. payload

  return box("ftyp", "M4A \0\0\2\0M4A mp42isom") .. mdat .. moov
end

do
  local png = "\137PNG\r\n\26\n" .. string.rep("\5", 600)
  local ilst = item("\169nam", 1, "Apple Song")
            .. item("\169ART", 1, "Apple Artist")
            .. item("aART", 1, "Apple Album Artist")
            .. item("\169alb", 1, "Apple Album")
            .. item("\169day", 1, "2019-03-04T08:00:00Z")
            .. item("gnre", 0, "\0\18")                  -- a number: left out
            .. item("\169wrt", 1, "Apple Composer")
            .. item("trkn", 0, "\0\0\0\5\0\11\0\0")
            .. item("disk", 0, "\0\0\0\2\0\2")
            .. item("cpil", 21, "\1")
            .. item("covr", 14, png)
            .. free("iTunSMPB", SMPB)
            .. free("replaygain_album_gain", "-4.20 dB", "org.hydrogenaudio.replaygain")
            .. free("REPLAYGAIN_ALBUM_PEAK", "0.5")
  local file = m4a(ilst)
  local t = read_all(file)

  check(t.title == "Apple Song" and t.artist == "Apple Artist"
        and t.album_artist == "Apple Album Artist" and t.album == "Apple Album"
        and t.composer == "Apple Composer" and t.year == "2019" and t.genre == nil,
        "MP4: ilst's text atoms, the year from a date, a numbered genre left out")
  check(t.track == "5/11" and t.track_number == 5 and t.track_total == 11
        and t.disc == "2/2" and t.compilation == true,
        "MP4: trkn, disk and cpil: " .. tostring(t.track) .. " " .. tostring(t.disc))
  check(t.cover and t.cover.mime == "image/png" and t.cover.bytes == #png
        and t.cover.offset == file:find(png, 1, true) - 1,
        "MP4: covr found in place, past a 64-bit mdat")
  check(t.gapless and t.gapless.source == "iTunSMPB" and t.gapless.delay == 0x840
        and t.gapless.padding == 0x1CA and t.gapless.length == 0xD1A36E,
        "MP4: iTunSMPB from a freeform atom")
  check(t.replaygain and t.replaygain.album_gain == -4.2 and t.replaygain.album_peak == 0.5,
        "MP4: ReplayGain from freeform atoms, whoever's domain")

  -- QuickTime's meta, with no version in front of its children.
  local q = read_all(m4a(item("\169nam", 1, "QuickTime"), { quicktime = true }))

  check(q.title == "QuickTime", "MP4: a meta atom with no version and flags")

  -- **The control the design names**: an atom whose size points past its
  -- parent. `©nam`'s size here runs past the end of `ilst` and into a `free`
  -- atom after it, inside `meta`, so following it would find a title; it is
  -- refused, and the atom before it stands.
  local inner = databox(1, "Followed")
  local bad = string.pack(">I4", 8 + #inner + 64) .. "\169nam" .. inner
  local c = read_all(m4a(item("\169ART", 1, "Kept") .. bad,
                         { after = box("free", string.rep("\0", 80)) }))

  check(c.artist == "Kept" and c.title == nil,
        "control: an atom whose size points past its parent is refused, not followed: "
        .. tostring(c.title))

  -- The same one level up: udta's size runs past moov, into what follows.
  local hdlr = box("hdlr", string.rep("\0", 25))
  local meta = box("meta", "\0\0\0\0" .. hdlr .. box("ilst", item("\169nam", 1, "Followed")))
  local udta = string.pack(">I4", 8 + #meta + 40) .. "udta" .. meta
  local moov = box("moov", box("mvhd", string.rep("\0", 100)) .. udta)
  local up = box("ftyp", "M4A \0\0\0\0") .. moov .. box("free", string.rep("\0", 64))

  check(read_all(up).title == nil, "control: udta past the end of moov is refused")

  -- And an atom shorter than its own header: a size of 4, where taking it
  -- would step into the middle of the atom and find the one hidden there.
  local tiny = string.pack(">I4", 4) .. item("\169nam", 1, "Tiny")

  check(read_all(m4a(tiny)).title == nil, "control: an atom shorter than its header is refused")

  -- Reading a whole MP4's tags is a handful of reads of the file.
  local calls, f2 = 0, file
  local counted = tags.read(function(o, n)
    calls = calls + 1
    if o >= #f2 then return nil end
    return f2:sub(o + 1, o + n)
  end, #f2)

  check(counted.title == "Apple Song" and calls <= 4,
        "MP4: the walk is read through a window, in " .. calls .. " reads")
end

--------------------------------------------------------------------------
-- AIFF's ID3 chunk, and a WAV's.
--------------------------------------------------------------------------

local function chunk(order, id, body)
  return id .. string.pack(order .. "I4", #body) .. body .. ((#body % 2 == 1) and "\0" or "")
end

do
  local picture = string.rep("J", 333)
  local id3 = tag(4, 0, frame24("TIT2", "\3AIFF Title")
                        .. frame24("APIC", "\0image/jpeg\0\3\0" .. picture))
  local body = "AIFF" .. chunk(">", "COMM", string.rep("\0", 18))
               .. chunk(">", "NAME", "Name Chunk")
               .. chunk(">", "AUTH", "Auth Chunk")
               .. chunk(">", "SSND", string.rep("\0", 101))
               .. chunk(">", "ID3 ", id3)
  local file = "FORM" .. string.pack(">I4", #body) .. body
  local t = read_all(file)

  check(t.title == "AIFF Title" and t.artist == "Auth Chunk",
        "AIFF: the ID3 chunk first, AUTH filling in, after an odd-length chunk")
  check(t.cover and t.cover.bytes == #picture
        and t.cover.offset == file:find(picture, 1, true) - 1,
        "AIFF: the cover's offset is in the file, not in the chunk")

  -- Control: a chunk whose size runs past the form is refused.
  local over = "AIFF" .. chunk(">", "NAME", "Before")
               .. "AUTH" .. string.pack(">I4", 900) .. "Past the form"
  local o = read_all("FORM" .. string.pack(">I4", #over) .. over .. string.rep("\0", 1000))

  check(o.title == "Before" and o.artist == nil,
        "control: an AIFF chunk past the end of its form is refused")

  local wid3 = tag(3, 0, frame23("TIT2", "\0WAV ID3") .. frame23("TALB", "\0WAV Album"))
  local wbody = "WAVE" .. chunk("<", "fmt ", string.rep("\0", 16))
                .. chunk("<", "LIST", "INFO" .. chunk("<", "INAM", "Info Title\0")
                                             .. chunk("<", "IART", "Info Artist\0"))
                .. chunk("<", "data", string.rep("\0", 8))
                .. chunk("<", "id3 ", wid3)
  local w = read_all("RIFF" .. string.pack("<I4", #wbody) .. wbody)

  check(w.title == "WAV ID3" and w.album == "WAV Album" and w.artist == "Info Artist",
        "WAV: an id3 chunk first, LIST INFO filling in")
end

--------------------------------------------------------------------------
-- Files this Mac's FFmpeg writes and tags.
--------------------------------------------------------------------------

local generated = "files from FFmpeg skipped: no ffmpeg on this machine"

local function shell(cmd)
  local ok = os.execute(cmd .. " >/dev/null 2>&1")

  return ok == true or ok == 0
end

local function slurp(path)
  local f = io.open(path, "rb")

  if not f then return nil end

  local s = f:read("a")

  f:close()

  return s
end

-- What a command prints, through a file: the Lua built here has no popen.
local function output(cmd, file)
  local ok = os.execute(cmd .. " > '" .. file .. "' 2>/dev/null")

  if not (ok == true or ok == 0) then return nil end

  return slurp(file)
end

if shell("command -v ffmpeg") and shell("command -v ffprobe") then
  -- A directory of its own: a name tmpname made, and the directory beside it.
  local mark = os.tmpname()
  local dir = mark .. ".tags"

  shell("mkdir '" .. dir .. "'")

  local made = 0
  local tone = "-f lavfi -i sine=d=1:sample_rate=44100"
  local art = "-f lavfi -i color=red:s=64x48:d=1 -map 0 -map 1 -frames:v 1 "
              .. "-disposition:v attached_pic"
  local meta = "-metadata title='Generated' -metadata artist='FFmpeg' "
               .. "-metadata album='Lavfi' -metadata album_artist='Various' "
               .. "-metadata track=4/10 -metadata disc=1/2 -metadata date=2020 "
               .. "-metadata composer='Sine' -metadata genre='Test' "
               .. "-metadata compilation=1"

  local function make(name, args)
    local path = dir .. "/" .. name

    if not shell("ffmpeg -hide_banner -loglevel error -y " .. args .. " " .. path) then
      check(false, "ffmpeg could not write " .. name)
      return nil
    end

    made = made + 1

    return path, slurp(path)
  end

  local function common(t, what)
    check(t.title == "Generated" and t.artist == "FFmpeg" and t.album == "Lavfi",
          what .. ": title, artist and album: " .. tostring(t.title))
  end

  -- A picture's first bytes where the cover says it starts.
  local function cover_in(file, t, magic)
    return t.cover and t.cover.bytes > 0
           and file:sub(t.cover.offset + 1, t.cover.offset + #magic) == magic
           and t.cover.offset + t.cover.bytes <= #file
  end

  local _, flac = make("x.flac", tone .. " " .. art .. " -c:v png " .. meta
                       .. " -metadata REPLAYGAIN_TRACK_GAIN='-6.54 dB'"
                       .. " -metadata REPLAYGAIN_ALBUM_PEAK=0.75 -c:a flac")

  if flac then
    local t = read_all(flac)

    common(t, "FFmpeg's FLAC")
    check(t.album_artist == "Various" and t.track_number == 4 and t.track_total == 10
          and t.disc_number == 1 and t.year == "2020" and t.compilation == true,
          "FFmpeg's FLAC: album artist, track and disc, year, compilation")
    check(t.replaygain and t.replaygain.track_gain == -6.54 and t.replaygain.album_peak == 0.75,
          "FFmpeg's FLAC: ReplayGain")
    check(cover_in(flac, t, "\137PNG") and t.cover.width == 64 and t.cover.height == 48,
          "FFmpeg's FLAC: its PICTURE block found in place, 64 by 48")
  end

  for _, codec in ipairs({ "alac", "aac" }) do
    local _, m = make("x-" .. codec .. ".m4a", tone .. " " .. art .. " -c:v mjpeg "
                      .. meta .. " -c:a " .. codec)

    if m then
      local t = read_all(m)

      common(t, "FFmpeg's " .. codec .. " in .m4a")
      check(t.album_artist == "Various" and t.track == "4/10" and t.disc == "1/2"
            and t.year == "2020" and t.composer == "Sine" and t.genre == "Test"
            and t.compilation == true,
            "FFmpeg's " .. codec .. ": aART, trkn, disk, ©day, ©wrt, ©gen, cpil")
      check(cover_in(m, t, "\255\216") and t.cover.mime == "image/jpeg",
            "FFmpeg's " .. codec .. ": covr found in place")
    end
  end

  -- FFmpeg's own reading of an MP3 is the other opinion on its gapless
  -- numbers: it skips the delay and its decoder's 529, and says the length
  -- that is left, both in its stream's time base.
  local function agrees(path, g, what)
    local said = output("ffprobe -v error -select_streams a -show_entries "
                        .. "stream=start_pts,duration_ts,time_base -of default=nw=1 "
                        .. path, dir .. "/probe.txt") or ""
    local start = tonumber(said:match("start_pts=(%d+)"))
    local dur = tonumber(said:match("duration_ts=(%d+)"))
    local den = tonumber(said:match("time_base=1/(%d+)"))

    check(g and g.source == "LAME" and start and dur and den
          and g.delay + 529 == start * 44100 // den and g.length == dur * 44100 // den,
          ("%s: LAME's delay and length agree with ffprobe (ours %s, its %s)"):format(
            what, g and (g.delay .. "+529, " .. tostring(g.length)) or "none",
            (start and dur and den) and ((start * 44100 // den) .. ", " .. (dur * 44100 // den))
            or "nothing"))
  end

  local path, mp3 = make("x.mp3", tone .. " " .. art .. " -c:v mjpeg " .. meta
                         .. " -metadata REPLAYGAIN_TRACK_GAIN='-1.25 dB'"
                         .. " -c:a libmp3lame -b:a 128k")

  if mp3 then
    local t = read_all(mp3)

    common(t, "FFmpeg's MP3")
    check(t.album_artist == "Various" and t.disc == "1/2" and t.compilation == true
          and t.composer == "Sine",
          "FFmpeg's MP3: TPE2, TPOS, TCOM, and compilation as FFmpeg's TXXX:TCMP")
    check(t.replaygain and t.replaygain.track_gain == -1.25, "FFmpeg's MP3: ReplayGain in TXXX")
    check(cover_in(mp3, t, "\255\216"), "FFmpeg's MP3: APIC found in place")

    -- With a picture attached, FFmpeg 9.0.2's muxer writes a frame count of
    -- two into the Info frame, and its own reader then says the song is 26
    -- ms long. What is checked is that this reads what the file says, as
    -- FFmpeg does; the length is the next file's to check.
    agrees(path, t.gapless, "FFmpeg's MP3 with a picture")
  end

  local plain = make("plain.mp3", tone .. " -c:a libmp3lame -b:a 128k")

  if plain then
    local g = read_all(slurp(plain)).gapless

    agrees(plain, g, "FFmpeg's MP3")
    check(g and g.length == 44100, "FFmpeg's MP3: a second of sound is 44100 samples long: "
          .. tostring(g and g.length))
  end

  local _, ogg = make("x.ogg", tone .. " " .. meta .. " -c:a vorbis -strict -2 -ac 2")

  if ogg then common(read_all(ogg), "FFmpeg's Ogg Vorbis") end

  local _, opus = make("x.opus", tone .. " " .. meta .. " -c:a libopus")

  if opus then
    local t = read_all(opus)

    common(t, "FFmpeg's Opus")
    check(t.album_artist == "Various" and t.compilation == true, "FFmpeg's Opus: the rest of its comments")
  end

  local _, aiff = make("x.aiff", tone .. " " .. meta .. " -write_id3v2 1 -c:a pcm_s16be")

  if aiff then common(read_all(aiff), "FFmpeg's AIFF, its ID3 chunk") end

  local _, wav = make("x.wav", tone .. " " .. meta .. " -c:a pcm_s16le")

  if wav then
    local t = read_all(wav)

    check(t.title == "Generated" and t.artist == "FFmpeg", "FFmpeg's WAV: its LIST INFO")
  end

  shell("rm -rf '" .. dir .. "' '" .. mark .. "'")

  generated = made .. " files FFmpeg wrote"
end

if failed > 0 then
  print(("FAIL: %d of %d checks on audio tags."):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on audio tags, on this machine (ID3v2.2, 2.3 and 2.4, "
       .. "ID3v1, a WAV's INFO, UTF-16 pairs, FLAC, Ogg, MP4, AIFF, LAME and "
       .. "iTunSMPB, ReplayGain, covers found in place, sizes past their parent "
       .. "refused; %s)."):format(passed, generated))
