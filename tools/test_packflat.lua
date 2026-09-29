-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The disk server's reader of `sys.pack`'s tables, held to the serialiser.
--
--   build/host/lua tools/test_packflat.lua
--
-- **One format, two readings of it** (`docs/diskfs.md` step 3). A file's
-- attributes are stored as the bytes `sys.pack` makes, and the disk server in
-- C reads them without Lua to merge a `setattr` and to answer a query:
-- `user/servers/packflat.c`. The host's Lua carries both - `kpack`, the
-- machine's own `lua/kosmos/serialize.c`, and `packflat` - so what one writes
-- the other has to read, and a change made in C has to come out of the real
-- `unpack` as the table it describes.

local kpack = require("kpack")
local packflat = require("packflat")

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- Two flat tables with the same keys, values and types - 1 and 1.0 differ.
local function same(a, b)
  if type(a) ~= "table" or type(b) ~= "table" then return false end

  for k, v in pairs(a) do
    if b[k] ~= v or math.type(b[k]) ~= math.type(v) then return false end
  end

  for k in pairs(b) do
    if a[k] == nil then return false end
  end

  return true
end

-- What attributes are, and the awkward ends of each.
local TABLES = {
  {},
  { kind = "launcher", program = "/Home/Apps/Quake/quake.lua", args = "" },
  { desktop_x = 120, desktop_y = -40, rating = 0 },
  { title = "Nine Inch Nails - Hurt", year = 1994, rating = 4.5, favourite = true,
    played = false },
  { big = math.maxinteger, small = math.mininteger, zero = 0.0, half = 0.5 },
  { ["with space"] = "a\0b", [""] = "empty name", unicode = "Caf\xc3\xa9" },
  { [1] = "one", [2] = "two", name = "mixed keys" },
}

-- 1. What the serialiser makes, read and written again here, unpacks the same.
for i, t in ipairs(TABLES) do
  local bytes = assert(kpack.pack(t))
  local again, why = packflat.rewrite(bytes)

  check(again ~= nil and same(kpack.unpack(again), t),
        ("table %d read and written in C unpacks as it went in: %s"):format(
          i, tostring(why)))
end

-- 2. A change made in C is the change the table describes.
do
  local t = { kind = "launcher", program = "/Kosmos/Apps/calc.lua", x = 3 }
  local bytes = assert(kpack.pack(t))

  bytes = assert(packflat.set(bytes, "program", "/Kosmos/Apps/clock.lua"))
  bytes = assert(packflat.set(bytes, "icon", "Clock"))
  bytes = assert(packflat.set(bytes, "x", nil))
  bytes = assert(packflat.set(bytes, "rating", 5))
  bytes = assert(packflat.set(bytes, "scale", 1.25))
  bytes = assert(packflat.set(bytes, "hidden", true))
  bytes = assert(packflat.set(bytes, "nothing", nil))

  check(same(kpack.unpack(bytes), { kind = "launcher", program = "/Kosmos/Apps/clock.lua",
                                    icon = "Clock", rating = 5, scale = 1.25,
                                    hidden = true }),
        "a value replaced, three added, one taken out and one not there taken "
        .. "out come out of the serialiser as that table")
end

-- 3. A value as `tostring` writes it, which is how a query compares.
do
  local values = { 0, 5, -3, math.maxinteger, math.mininteger, 1.5, 1.0, -2.0,
                   1e100, 0.1, 3.14159265358979, 1/3, 2^53, -0.5, true, false,
                   "abc", "", "5" }
  local all = true

  for _, v in ipairs(values) do
    local bytes = assert(kpack.pack({ v = v }))
    local text = packflat.text(bytes, "v")

    if text ~= tostring(v) then
      all = false
      print(("    %s: C says %q, Lua says %q"):format(math.type(v) or type(v),
                                                     tostring(text), tostring(v)))
    end
  end

  check(all, "every value is written as tostring writes it")
  check(packflat.text(assert(kpack.pack({ a = 1 })), "b") == nil,
        "and a name that is not there has no value")
end

-- 4. What is not a flat table, or not a value at all, is refused.
do
  local nested = assert(kpack.pack({ a = { b = 1 } }))
  local good = assert(kpack.pack({ a = "x", b = 2 }))
  local _, why

  _, why = packflat.rewrite(nested)
  check(why == "not flat", "a table inside is refused: " .. tostring(why))

  _, why = packflat.rewrite(good:sub(1, #good - 1))
  check(why == "malformed", "a table with no end is refused: " .. tostring(why))

  _, why = packflat.rewrite(good .. "\0")
  check(why == "malformed", "bytes after the table are refused: " .. tostring(why))

  _, why = packflat.rewrite(assert(kpack.pack("not a table")))
  check(why == "malformed", "a value that is not a table is refused: " .. tostring(why))

  _, why = packflat.rewrite(good:sub(1, 5))
  check(why == "malformed", "a string cut short is refused: " .. tostring(why))

  local many = {}

  for i = 1, 129 do many["k" .. i] = i end

  _, why = packflat.rewrite(assert(kpack.pack(many)))
  check(why == "too many", "a table of 129 entries is refused, not half read: "
                           .. tostring(why))

  local long = { text = string.rep("x", 5000) }

  _, why = packflat.rewrite(assert(kpack.pack(long)))
  check(why == "too big", "one that does not fit in a block is refused: "
                          .. tostring(why))
end

if failed > 0 then
  print(("FAIL: %d of %d checks on reading sys.pack's tables in C.")
        :format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on reading sys.pack's tables in C (every table the "
       .. "serialiser made read and written back the same, changes made in C "
       .. "come out as the table they describe, values written as tostring "
       .. "writes them, and what is not a flat table refused).")
      :format(passed))
