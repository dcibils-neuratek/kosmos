-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The WAV header walker, tested on this machine instead of the target.
--
--   build/host/lua tools/test_wav.lua
--
-- `wav.lua` reads a chunked header through a `read(offset, n)` function and
-- touches no samples, so it has nothing to say to the system and can be
-- exercised here in a fraction of a second. Building the headers by hand is
-- the point: the interesting ones are the shapes a generator does not
-- produce.
--
-- **Every case below except the first two came from a file that broke it.**
-- Four hand-built headers passed while a real track - converted by
-- `afconvert`, which pads with a 4044-byte `FLLR` chunk so the samples
-- begin at 4096 - was refused as having no data chunk. The parser had been
-- reading the first kilobyte and searching it, which works for every header
-- anybody writes to test a parser.

local wav = assert(loadfile("user/lib/wav.lua"))()

local pass, fail = 0, 0

local function ok(name, cond, detail)
  if cond then
    pass = pass + 1
    print("ok " .. (pass + fail) .. " - " .. name)
  else
    fail = fail + 1
    print("not ok " .. (pass + fail) .. " - " .. name
          .. (detail and ("  (" .. tostring(detail) .. ")") or ""))
  end
end

--------------------------------------------------------------------------
-- Building headers.
--------------------------------------------------------------------------

local function u32(n)
  return string.char(n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255)
end

local function u16(n)
  return string.char(n & 255, (n >> 8) & 255)
end

local function chunk(id, body)
  -- Odd-length chunks are padded to even, and a walker that forgets it
  -- reads the next chunk one byte out.
  return id .. u32(#body) .. body .. ((#body % 2 == 1) and "\0" or "")
end

local function fmt(format, channels, rate, bits)
  return chunk("fmt ", u16(format) .. u16(channels) .. u32(rate)
                    .. u32(rate * channels * (bits // 8))
                    .. u16(channels * (bits // 8)) .. u16(bits))
end

local function riff(...)
  local body = "WAVE" .. table.concat({ ... })

  return "RIFF" .. u32(#body) .. body
end

-- A reader over a string, which is what the file is here.
local function reader(s)
  return function(at, n)
    if at >= #s then return nil end

    return s:sub(at + 1, at + n)
  end
end

--------------------------------------------------------------------------
-- The shapes that must work.
--------------------------------------------------------------------------

local plain = riff(fmt(1, 2, 44100, 16), chunk("data", string.rep("\0", 400)))
local got, why = wav.scan(reader(plain))

ok("a plain 44100 stereo 16-bit header parses", got ~= nil, why)
ok("the rate is read", got and got.rate == 44100)
ok("the channel count is read", got and got.channels == 2)
ok("the sample width is read", got and got.bits == 16)
ok("a frame is four bytes", got and got.frame == 4)
ok("the data length is read", got and got.bytes == 400)
ok("the data offset points past the chunk header",
   got and plain:sub(got.offset - 7, got.offset - 4) == "data", got and got.offset)

got = wav.scan(reader(riff(fmt(1, 1, 11025, 8),
                           chunk("data", string.rep("\0", 100)))))
ok("8-bit mono parses", got ~= nil)
ok("a mono 8-bit frame is one byte", got and got.frame == 1)
ok("the length in seconds is right", got and math.abs(got.seconds - 100/11025) < 1e-9)

--------------------------------------------------------------------------
-- The shapes real files actually have.
--------------------------------------------------------------------------

-- The one that started this. `afconvert` writes it on every file.
local filler = riff(fmt(1, 2, 44100, 16),
                    chunk("FLLR", string.rep("\0", 4044)),
                    chunk("data", string.rep("\0", 400)))
got, why = wav.scan(reader(filler))
ok("a 4044-byte FLLR chunk before the data is walked past", got ~= nil, why)
ok("and the data offset is past it, not inside it",
   got and got.offset > 4044, got and got.offset)

got = wav.scan(reader(riff(fmt(1, 2, 44100, 16),
                           chunk("LIST", "INFOINAM" .. u32(6) .. "Kosmos"),
                           chunk("data", string.rep("\0", 400)))))
ok("a LIST metadata chunk before the data is walked past", got ~= nil)

-- An odd-length chunk. The pad byte is not counted in the size, so a walker
-- that adds only the size lands one byte short and reads a garbage id.
got = wav.scan(reader(riff(fmt(1, 2, 44100, 16),
                           chunk("junk", string.rep("x", 5)),
                           chunk("data", string.rep("\0", 400)))))
ok("an odd-length chunk is padded to even", got ~= nil)

-- More metadata than any window would have covered.
got = wav.scan(reader(riff(fmt(1, 2, 44100, 16),
                           chunk("FLLR", string.rep("\0", 200000)),
                           chunk("data", string.rep("\0", 400)))))
ok("two hundred kilobytes of padding is still walked past", got ~= nil)

-- fmt after the padding rather than before it: legal, and the order nobody
-- writes a parser for.
got = wav.scan(reader(riff(chunk("FLLR", string.rep("\0", 1000)),
                           fmt(1, 2, 22050, 16),
                           chunk("data", string.rep("\0", 400)))))
ok("fmt may come after other chunks", got ~= nil and got.rate == 22050)

--------------------------------------------------------------------------
-- The shapes that must be refused, by name rather than by playing noise.
--------------------------------------------------------------------------

local function refused(name, s)
  local r, err = wav.scan(reader(s))
  ok(name, r == nil and type(err) == "string", r and "accepted it")
end

refused("a file that is not RIFF", "MTHd" .. u32(6) .. "WAVEfmt ")
refused("a RIFF file that is not WAVE",
        "RIFF" .. u32(20) .. "AVI " .. chunk("data", "xxxx"))
refused("a file too short to hold a header", "RIF")
refused("compressed audio, by format number",
        riff(fmt(0xfffe, 2, 44100, 16), chunk("data", "xxxx")))
refused("IEEE float samples",
        riff(fmt(3, 2, 44100, 32), chunk("data", "xxxx")))
refused("24-bit samples",
        riff(fmt(1, 2, 44100, 24), chunk("data", "xxxx")))
refused("more channels than two",
        riff(fmt(1, 6, 44100, 16), chunk("data", "xxxx")))
refused("a data chunk before its fmt",
        riff(chunk("data", "xxxx"), fmt(1, 2, 44100, 16)))
refused("a header that ends without a data chunk",
        riff(fmt(1, 2, 44100, 16), chunk("LIST", "INFO")))
refused("a truncated fmt chunk",
        "RIFF" .. u32(16) .. "WAVE" .. "fmt " .. u32(16) .. "\1\0\2\0")

--------------------------------------------------------------------------
-- `wav.parse` is the same walker with the file already in memory.
--------------------------------------------------------------------------

got = wav.parse(filler)
ok("parse() agrees with scan() on the same bytes",
   got ~= nil and got.offset > 4044)

print()

if fail > 0 then
  print(("FAIL: %d of %d WAV header checks"):format(fail, pass + fail))
  os.exit(1)
end

print(("PASS: %d checks on the WAV header walker, on this machine.")
      :format(pass))
