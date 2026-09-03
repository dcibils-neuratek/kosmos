-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- RIFF/WAVE: where the samples are, and what shape they are in.
--
-- **The header is parsed in Lua and not one sample is touched here.** That
-- is the split `CLAUDE.md` describes and this file is a clean example of
-- it: reading a chunked header is a dozen decisions about a few dozen
-- bytes, which is Lua's work, and converting a million frames is a byte
-- loop, which is `sys.pcm`'s. Nothing in between.
--
-- WAV is a container: a `RIFF` header, then chunks with a four-character
-- name and a length. Only two matter - `fmt ` says what the samples are and
-- `data` is where they start - and everything else is skipped by its own
-- length, which is what a chunked format is for.

local wav = {}

local function u16(s, at)
  return s:byte(at) | (s:byte(at + 1) << 8)
end

local function u32(s, at)
  return s:byte(at) | (s:byte(at + 1) << 8)
       | (s:byte(at + 2) << 16) | (s:byte(at + 3) << 24)
end

--
-- Walk the chunks, reading only the headers.
--
-- `read(offset, n)` returns up to `n` bytes at `offset`, or nil at the end
-- of the file. It is a *reader* rather than a string of the first however
-- many bytes, and that is the whole design decision here.
--
-- The first version took the first kilobyte and searched it, on the
-- reasoning that the chunks that matter are at the front. They usually are.
-- Then a real file arrived - a track converted by `afconvert`, which pads
-- the header with a 4044-byte `FLLR` chunk so that the samples begin
-- exactly at 4096 - and the parser said there was no data chunk in a file
-- that is nothing but data. Four tests with hand-built headers had all
-- passed, because a header somebody generates to test a parser is a header
-- with nothing surprising in it.
--
-- **A chunked format does not have a header size, so nothing may assume
-- one.** Seeking eight bytes at a time costs a handful of reads and is
-- correct for any file, including one whose padding is a megabyte.
--
function wav.scan(read)
  local riff = read(0, 12)

  if not riff or #riff < 12 then return nil, "too short to be a WAV" end

  if riff:sub(1, 4) ~= "RIFF" or riff:sub(9, 12) ~= "WAVE" then
    return nil, "not a RIFF/WAVE file"
  end

  local at = 12
  local format, channels, rate, bits

  while true do
    local hdr = read(at, 8)

    if not hdr or #hdr < 8 then
      return nil, "the file ended before its data chunk"
    end

    local id = hdr:sub(1, 4)
    local size = u32(hdr, 5)

    if id == "fmt " then
      local body = read(at + 8, 16)

      if not body or #body < 16 then return nil, "a truncated fmt chunk" end

      format   = u16(body, 1)
      channels = u16(body, 3)
      rate     = u32(body, 5)
      bits     = u16(body, 15)
    elseif id == "data" then
      if not rate then return nil, "a data chunk before its fmt" end

      --
      -- Format 1 is PCM. 3 is IEEE float, 6 and 7 are the companded
      -- telephone codecs, 0xfffe is "look in the extension" - all real and
      -- none of them handled. Refusing by name is friendlier than playing
      -- noise.
      --
      if format ~= 1 then
        return nil, ("compressed WAV (format %d) is not supported")
                    :format(format)
      end

      if bits ~= 8 and bits ~= 16 then
        return nil, ("%d bits a sample is not supported"):format(bits)
      end

      if channels ~= 1 and channels ~= 2 then
        return nil, ("%d channels is not supported"):format(channels)
      end

      return {
        rate = rate, channels = channels, bits = bits,
        offset = at + 8,
        bytes = size,
        frame = channels * (bits // 8),
        seconds = size / (rate * channels * (bits // 8)),
      }
    end

    -- Chunks are padded to an even length, which is the detail that turns a
    -- working parser into one that reads garbage on the third file.
    at = at + 8 + size + (size % 2)
  end
end

--
-- The same, for a header already in memory.
--
-- Convenient for a caller that has the bytes anyway, and implemented on top
-- of `scan` rather than beside it so there is one walker rather than two
-- that agree until somebody fixes only one.
--
function wav.parse(head)
  return wav.scan(function(at, n)
    if at >= #head then return nil end

    return head:sub(at + 1, at + n)
  end)
end

return wav
