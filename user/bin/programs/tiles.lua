-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- tiles: ask the map's tiles server, from the prompt or a script.
--
--   tiles fetch png https://.../{z}/{x}/{y}.png 12/1200/1500 12/1201/1500
--   tiles fetch png URL 12/1/2 and jpg URL 12/1/2 12/1/3
--   tiles arrived 3
--
-- kosmos: needs tiles
--
-- `fetch` sets up one source or several - a format, an address and the
-- tiles wanted from it, each group after `and` - asks for them all at once,
-- and says each tile as it comes and where it is kept, until all have come
-- or a minute has gone. `arrived N` asks what source N has fetched; a
-- source there is none of is refused.
--
-- It is what `tools/run_tiles.py` opens, and what to reach for when a map
-- shows grey and the question is whether the network gave anything. It
-- needs `/Tiles`, which the window manager holds and the prompt's shell
-- does not: `open tiles ...` runs it from a prompt, its lines on the
-- console.

local tiles = use("/Kosmos/Libraries/tiles.lua")

local words = {}

for w in args:gmatch("%S+") do words[#words + 1] = w end

local function usage()
  print("tiles: tiles fetch FORMAT URL Z/X/Y... [and FORMAT URL Z/X/Y...]")
  print("       tiles arrived N")
end

if words[1] == "arrived" then
  local n = tonumber(words[2] or "")

  if not n then return usage() end

  local came, why = tiles.numbered(n):arrived()

  if came then
    print(("tiles: source %d has %d tiles come"):format(n, #came))
  else
    print(("tiles: source %d refused: %s"):format(n, tostring(why)))
  end

  return
end

if words[1] ~= "fetch" then return usage() end

-- The groups: format, address, tiles.
local groups, at = {}, 2

while words[at] do
  local g = { format = words[at], url = words[at + 1], ids = {}, count = 0, got = 0 }

  if not g.url then return usage() end

  at = at + 2

  while words[at] and words[at] ~= "and" do
    local z, x, y = words[at]:match("^(%d+)/(%d+)/(%d+)$")

    if not z then return usage() end

    local ids = g.ids
    ids[#ids + 1], ids[#ids + 2], ids[#ids + 3] = tonumber(z), tonumber(x), tonumber(y)
    g.count = g.count + 1
    at = at + 1
  end

  if words[at] == "and" then at = at + 1 end

  groups[#groups + 1] = g
end

if #groups == 0 then return usage() end

for _, g in ipairs(groups) do
  local src, why = tiles.source(g.url, g.format)

  if not src then
    print(("tiles: %s as .%s refused: %s"):format(g.url, g.format, tostring(why)))
    return
  end

  g.src = src
  print(("tiles: source %d is %s as .%s, kept in %s"):format(src.number, g.url, g.format,
                                                             src.cache))
end

for _, g in ipairs(groups) do
  local ok, why = g.src:want(g.ids)

  if not ok then print(("tiles: source %d refused the list: %s"):format(g.src.number, why)) end
end

-- Asked on this program's own clock, a tenth of a second apart - in
-- scheduler ticks, which is what `sys.sleep` counts.
local tenth = math.max(1, ((sys.info() or {}).tick_hz or 250) // 10)
local left = 0

for _, g in ipairs(groups) do left = left + g.count end

for _ = 1, 600 do
  for _, g in ipairs(groups) do
    if g.got < g.count then
      local came = g.src:arrived() or {}

      for _, t in ipairs(came) do
        g.got, left = g.got + 1, left - 1
        print(("tiles: came %d %d/%d/%d %s"):format(g.src.number, t.z, t.x, t.y,
                                                    g.src:path(t.z, t.x, t.y)))
      end

      if g.got >= g.count then
        print(("tiles: source %d done, %d tiles"):format(g.src.number, g.got))
      end
    end
  end

  if left <= 0 then break end

  sys.sleep(tenth)
end

for _, g in ipairs(groups) do
  if g.got < g.count then
    print(("tiles: source %d: %d of %d came in a minute; %d failed, %s"):format(
          g.src.number, g.got, g.count, g.src.failed, g.src.why))
  end
end

print(left <= 0 and "tiles: all came" or "tiles: not all came")
