-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- `regions.lua`, on this machine.
--
--   build/host/lua tools/test_regions.lua
--
-- The one door every region goes through since step 3 after 0.11 (5 October
-- 2026), held against a `sys` and an `fs` that keep regions and files as
-- strings, so where each byte lands can be looked at:
--
-- - **a file read a window at a time lands each window at its own offset**:
--   `fs.read_into` always writes at the region's start, and the loops this
--   replaced in the window manager and `pdfbench` would have written every
--   window over the last - Doom's WAD began "02_8" where "IWAD" belongs
--   until that was found in Doom's own copy;
-- - the whole of a file in a region of its own, and no scratch region kept;
-- - a file the filesystem will not hand over in pages read through a string;
-- - a string written through pages, and through `fs.write` where a file
--   will not take pages;
-- - a region that will not map given back rather than kept;
-- - and after all of it, no capability still held.

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- Regions as strings, by capability; files as strings, by path.
local held, next_cap, refuse_map = {}, 10, false
local files = {}
local pages_only = {}         -- files `read_into` and `write_from` serve

local function splice(s, at, data)
  return s:sub(1, at) .. data .. s:sub(at + #data + 1)
end

sys = {
  memory = function(pages)
    next_cap = next_cap + 1
    held[next_cap] = string.rep("\0", pages * 4096)
    return next_cap
  end,
  memory_map = function(cap)
    if refuse_map then return nil, "the kernel would not map it" end
    return held[cap] and 0x100000 * cap or nil, "no such region"
  end,
  release = function(cap)
    held[cap] = nil
    return true
  end,
  region_write = function(cap, at, data)
    local r = held[cap]
    if not r or at + #data > #r then return nil, "past the end" end
    held[cap] = splice(r, at, data)
    return #data
  end,
  region_read = function(cap, at, n)
    local r = held[cap]
    if not r then error("no such region") end
    return r:sub(at + 1, at + n)
  end,
  region_copy = function(to, to_at, from, from_at, n)
    local a, b = held[to], held[from]
    if not a or not b or to_at + n > #a then return nil, "bad copy" end
    held[to] = splice(a, to_at, b:sub(from_at + 1, from_at + n))
    return n
  end,
}

fs = {
  getattr = function(path)
    if not files[path] then return nil, "no such path" end
    return { size = #files[path] }
  end,
  read = function(path) return files[path] end,
  write = function(path, data)
    files[path] = data
    return true
  end,
  -- At the region's start, always, whatever the file offset: as the
  -- protocol has it.
  read_into = function(path, cap, offset, n)
    local f = files[path]
    if not f or not pages_only[path] then return nil, "no pages here" end
    local piece = f:sub(offset + 1, offset + n)
    held[cap] = splice(held[cap], 0, piece)
    return #piece
  end,
  write_from = function(path, cap, size)
    if not pages_only[path] then return nil, "no pages here" end
    files[path] = held[cap]:sub(1, size)
    return size
  end,
}

local regions = dofile("user/lib/regions.lua")

local function live()
  local n = 0
  for _ in pairs(held) do n = n + 1 end
  return n
end

-- A file of 200 KB and some, every byte telling where it is.
local parts = {}
for i = 0, 200 * 1024 + 122 do parts[#parts + 1] = string.char((i * 31 + i // 251) % 256) end
local body = table.concat(parts)

files["/Home/Apps/Doom/doom1.wad"] = "IWAD" .. body
pages_only["/Home/Apps/Doom/doom1.wad"] = true

-- ---- a window at a time, each at its own offset ----
local r, size = regions.read_whole("/Home/Apps/Doom/doom1.wad", 16 * 1024)

check(r ~= nil and size == #body + 4, "the whole file, in a region its size")
check(r and held[r.cap]:sub(1, 4) == "IWAD",
      "it begins as the file does, not with its last window")
check(r and held[r.cap]:sub(1, size) == "IWAD" .. body,
      "every window at its own offset, byte for byte")
check(live() == 1, "and no scratch region kept")

regions.free(r)
check(live() == 0, "free gives it back")

-- ---- a file that will not hand over pages, through a string ----
files["/Temporary/note"] = "a note in memory"

local q = regions.make(4096)
local got = regions.read_file("/Temporary/note", q, 4096)

check(got == #"a note in memory"
      and held[q.cap]:sub(1, got) == "a note in memory",
      "a file with no pages to hand over read through a string")

regions.free(q)

-- ---- a string written through pages, and through fs.write ----
pages_only["/Home/log.txt"] = true
check(regions.write_string("/Home/log.txt", "written through pages") == 21
      and files["/Home/log.txt"] == "written through pages",
      "a string written to a file through pages")
check(regions.write_string("/Temporary/log.txt", "written as a string") == 19
      and files["/Temporary/log.txt"] == "written as a string",
      "and as a string where a file takes no pages")
check(live() == 0, "neither keeps its region")

-- ---- a region that will not map is given back ----
refuse_map = true
local m, why = regions.make(8192)
refuse_map = false

check(m == nil and tostring(why):find("would not map", 1, true) ~= nil,
      "a region that will not map is refused, with the kernel's reason")
check(live() == 0, "and given back rather than kept")

-- ---- read_whole on something that is not there ----
local none, said = regions.read_whole("/Home/nothing.wad")
check(none == nil and tostring(said):find("/Home/nothing.wad", 1, true),
      "a file that is not there is named in the refusal")
check(live() == 0, "and nothing held after it")

if failed > 0 then
  print(("FAIL: %d of %d checks on regions.lua"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on regions.lua - each window at its own offset, "
       .. "and every region given back"):format(passed))
