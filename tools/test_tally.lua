-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- How much is in a folder, counted a little at a time - on this computer.
--
-- `user/lib/tally.lua` is what Info's Size and Contains come from. The
-- tree below is made to have every shape a walk can get wrong: a folder
-- inside a folder inside a folder, an empty one, one that will not list,
-- and files of sizes that add up to a number nobody could reach by
-- counting only one level.
--
--   build/host/lua tools/test_tally.lua

local tally = dofile("user/lib/tally.lua")

local checks, failed = 0, 0

local function check(ok, what)
  checks = checks + 1

  if not ok then
    failed = failed + 1
    print("not ok - " .. what)
  end
end

--
-- /r
--   a.txt 10
--   b
--     c.bin 200
--     d
--       e.png 3000
--       f.txt 40000
--     empty
--   locked          (lists as nothing: refused)
--   g.lua 500000
--
local tree = {
  ["/r"] = { kind = "directory" },
  ["/r/a.txt"] = { kind = "file", size = 10 },
  ["/r/b"] = { kind = "directory" },
  ["/r/b/c.bin"] = { kind = "file", size = 200 },
  ["/r/b/d"] = { kind = "directory" },
  ["/r/b/d/e.png"] = { kind = "file", size = 3000 },
  ["/r/b/d/f.txt"] = { kind = "file", size = 40000 },
  ["/r/b/empty"] = { kind = "directory" },
  ["/r/locked"] = { kind = "directory" },
  ["/r/g.lua"] = { kind = "file", size = 500000 },
}

local lists = 0

local store = {
  getattr = function(path) return tree[path] end,
  list = function(path)
    lists = lists + 1

    if path == "/r/locked" then return nil end

    local out = {}

    for p in pairs(tree) do
      local parent, name = p:match("^(.*)/([^/]+)$")

      if parent == path then out[#out + 1] = name end
    end

    table.sort(out)
    return out
  end,
}

--
-- One folder: what is inside it, not the folder itself.
--
local t = tally.new(store, { "/r" })
local passes = 0

while not t:step(2) do passes = passes + 1 end

check(t.files == 5 and t.folders == 4 and t.bytes == 543210,
      ("one folder: 5 files in 4 folders, 543210 bytes, got %d files in %d "
       .. "folders, %d bytes"):format(t.files, t.folders, t.bytes))

check(t.unreadable == 1,
      "a folder that would not list is counted as one that would not, "
      .. "not as empty: " .. t.unreadable)

check(passes > 3,
      "a step of two does not finish a tree this size in one go - the walk "
      .. "stops and comes back: " .. passes .. " passes")

--
-- Stopping and starting gives the same answer as one long step.
--
local once = tally.new(store, { "/r" })

once:step(1000000)

check(once.done and once.files == t.files and once.bytes == t.bytes
      and once.folders == t.folders,
      "one long step and many short ones agree")

--
-- Several: each folder given counts too.
--
local several = tally.new(store, { "/r/a.txt", "/r/b", "/r/g.lua" })

several:step(1000)

check(several.files == 5 and several.folders == 3
      and several.bytes == 543210,
      ("several - a file, a folder, a file: 5 files in 3 folders (b and the "
       .. "two in it), got %d in %d, %d bytes"):format(several.files,
                                                      several.folders,
                                                      several.bytes))

--
-- A file on its own, and an empty folder.
--
local file = tally.new(store, { "/r/b/d/e.png" })

check(file.done and file.files == 1 and file.bytes == 3000,
      "a file is done before any step: one file, its size")

local empty = tally.new(store, { "/r/b/empty" })

empty:step(10)

check(empty.done and empty.files == 0 and empty.folders == 0,
      "an empty folder is empty")

if failed == 0 then
  print(("PASS: %d checks on counting what is in a folder (every level of "
         .. "it, a step at a time and the same as all at once, several "
         .. "things together, and a folder that will not list said so)."):format(checks))
else
  print(("FAIL: %d of %d checks"):format(failed, checks))
  os.exit(1)
end
