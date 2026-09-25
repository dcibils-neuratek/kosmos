-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- An MP4's first audio track, as `/lib/mp4.lua` reads it, printed for a
-- test written in C (`tools/test_aac.c`): the track's description on one
-- line, then each sample's offset and length, in decoding order.
--
--   track <codec> <object> <aot> <rate> <channels> <timescale> <config-hex>
--   <at> <size>
--   ...
--
-- So the C test decodes what the video player would hand its decoder,
-- found by the reader the player uses, rather than by a second reader
-- written for the test.
--
-- Usage: lua tools/mp4index.lua FILE

local mp4 = dofile("user/lib/mp4.lua")

local path = arg and arg[1]

if not path then
  io.stderr:write("usage: lua tools/mp4index.lua FILE\n")
  os.exit(2)
end

local f = assert(io.open(path, "rb"))
local size = f:seek("end")

local function read(at, n)
  f:seek("set", at)
  return f:read(n)
end

local movie, why = mp4.open(read, size)

if not movie then
  io.stderr:write("mp4index: " .. tostring(why) .. "\n")
  os.exit(1)
end

for _, t in ipairs(movie.tracks) do
  if t.kind == "audio" then
    local hex = (t.config or ""):gsub(".", function(c)
      return ("%02x"):format(c:byte())
    end)

    print(("track %s %d %d %d %d %d %s"):format(tostring(t.codec),
          t.object or 0, t.aot or 0, t.rate or 0, t.channels or 0,
          t.timescale or 0, hex ~= "" and hex or "-"))

    for _, s in ipairs(t.samples or {}) do
      print(("%d %d"):format(s.at, s.size))
    end

    os.exit(0)
  end
end

io.stderr:write("mp4index: no audio track in " .. path .. "\n")
os.exit(1)
