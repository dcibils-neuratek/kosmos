-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The filesystem's two implementations, held to each other block for block.
--
--   build/host/kfs-lua tools/test_kfs_cross.lua
--
-- **One format, two readings of it, and nothing may tell them apart**
-- (`docs/diskfs.md` step 1). `kfs.lua` has made every disk there is, and
-- `kfs.c` is taking over from it, so the C must place every byte where the
-- Lua would: a disk the Lua made is then one the C was always going to
-- make, and nothing about moving from one to the other can show on it.
--
-- `test_kfs.lua` asks each of them the same questions and checks the
-- answers. This checks the disks. A few hundred operations - files of every
-- awkward size, directories, renames within one and across two, names in
-- another case, attributes set and cleared, removals, transactions of
-- several operations and ones rolled back, and the power lost after a
-- commit - run four times over a fresh disk: all by the Lua, all by the C,
-- taking turns, and at random. **The four disks must be the same, block for
-- block, and every operation must have come out the same way.** Taking
-- turns is the part that says each reads what the other wrote: each
-- operation starts from what the other one left.
--
-- Then each reads every file on each disk, and they must agree about all
-- of it. And three things the random run is unlikely to reach, done on
-- purpose: a file too fragmented for its twelve extents, a full disk, and
-- a directory moved into itself.

local MOST = 31 * 4096
local SECTORS = 16 * 1024 * 1024 // 512

local disk = {}

sys = {}

function sys.disk()
  return { sectors = SECTORS, sector_size = 512, bytes = SECTORS * 512,
           most = MOST }
end

function sys.disk_read(sector, bytes)
  assert(bytes <= MOST, "a disk read of more than one call moves")

  local out = {}

  for i = 0, bytes // 4096 - 1 do
    out[#out + 1] = disk[sector // 8 + i] or string.rep("\0", 4096)
  end

  return table.concat(out)
end

function sys.disk_write(sector, data)
  assert(#data <= MOST, "a disk write of more than one call moves")

  for i = 0, #data // 4096 - 1 do
    disk[sector // 8 + i] = data:sub(i * 4096 + 1, (i + 1) * 4096)
  end

  return true
end

-- Flat tables of strings and numbers, as `test_kfs.lua` packs them.
function sys.pack(value)
  local parts = {}

  for k, v in pairs(value) do
    parts[#parts + 1] = ("%s=%s:%s"):format(k, type(v), tostring(v))
  end

  table.sort(parts)
  return table.concat(parts, "\n")
end

function sys.unpack(text)
  local out = {}

  for line in tostring(text):gmatch("[^\n]+") do
    local k, kind, v = line:match("^([^=]+)=([^:]+):(.*)$")

    if k then out[k] = (kind == "number") and tonumber(v) or v end
  end

  return out
end

function sys.fnv1a(bytes, seed)
  local h = seed or 0x811c9dc5

  for i = 1, #bytes do
    h = ((h ~ bytes:byte(i)) * 16777619) & 0xffffffff
  end

  return h
end

local IMPL = {
  lua = assert(loadfile("user/lib/kfs.lua"))(),
  c = require("kfsc"),
}

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

--------------------------------------------------------------------------
-- The operations, made once and run four times.
--------------------------------------------------------------------------

local seed = 20260929

local function rand(n)
  seed = (seed * 1103515245 + 12345) % 2147483648
  return seed % n
end

local function pick(list) return list[rand(#list) + 1] end

-- Bytes that differ from file to file and from block to block, so a block
-- in the wrong place cannot pass for the right one.
local function bytes_of(size, tag)
  local unit = ("%08d|"):format(tag)
  local s = string.rep(unit, size // #unit + 1)

  return s:sub(1, size)
end

local SIZES = { 0, 1, 100, 4095, 4096, 4097, 8192, 12000, 70001, 131072,
                300000, 1300000 }
local NAMES = { "a", "Notes.txt", "Song.mp3", "IMG_0001.jpg", "x",
                "Read me first.txt" }
local DIRS = { "Music", "photos", "deep", "Work" }

-- A model of the tree, keyed by the path in lower case as the disk finds
-- names, so most operations are aimed at something that is there. Each one
-- says whether it expects to succeed, and changes the model only if so; an
-- operation that fails fails the same way in both, and that is checked like
-- everything else.
local function inside(dir, path)
  return path:sub(1, #dir + 1) == dir .. "/"
end

local function case_of(path)
  if rand(3) ~= 0 then return path end

  return (path:gsub("%a", function(c)
    return rand(2) == 0 and c:upper() or c:lower()
  end))
end

local function values(t)
  local list = {}

  for _, v in pairs(t) do list[#list + 1] = v end
  table.sort(list)
  return list
end

local function parent(path) return path:match("^(.*)/[^/]*$") end

local function taken(m, path)
  return m.dirs[path:lower()] or m.files[path:lower()]
end

local function empty(m, dir)
  for k in pairs(m.files) do
    if inside(dir:lower(), k) then return false end
  end

  for k in pairs(m.dirs) do
    if inside(dir:lower(), k) then return false end
  end

  return true
end

-- Everything at or under `from` moved to `to`.
local function move(m, from, to)
  local lf = from:lower()

  for _, set in ipairs({ m.dirs, m.files }) do
    local moved = {}

    for k, v in pairs(set) do
      if k == lf or inside(lf, k) then
        moved[#moved + 1] = { k, to .. v:sub(#from + 1) }
      end
    end

    for _, pair in ipairs(moved) do
      set[pair[1]] = nil
      set[pair[2]:lower()] = pair[2]
    end
  end
end

local function one_op(m)
  local r = rand(100)
  local files = values(m.files)
  local dirs = values(m.dirs)

  if r < 35 or #files == 0 then
    local path = pick(dirs) .. "/" .. pick(NAMES)
    local size = SIZES[rand(rand(4) == 0 and #SIZES or #SIZES - 1) + 1]

    if m.dirs[path:lower()] then return { "store", path, size, 1, false }, false end

    m.files[path:lower()] = m.files[path:lower()] or path
    return { "store", case_of(path), size, rand(100000), rand(2) == 0 }, true
  elseif r < 43 then
    local path = pick(dirs) .. "/" .. pick(DIRS)

    if taken(m, path) then return { "mkdir", case_of(path) }, false end

    m.dirs[path:lower()] = path
    return { "mkdir", case_of(path) }, true
  elseif r < 55 then
    local path = pick(files)

    m.files[path:lower()] = nil
    return { "unlink", case_of(path) }, true
  elseif r < 70 then
    local from = pick(files)
    local to, target

    if rand(2) == 0 then
      to = rand(3) == 0 and from:match("[^/]+$"):upper() or pick(NAMES)
      target = parent(from) .. "/" .. to
    else
      target = pick(dirs) .. "/" .. pick(NAMES)
      to = case_of(target)
    end

    if target:lower() ~= from:lower() and taken(m, target) then
      return { "rename", case_of(from), to }, false
    end

    move(m, from, target)
    return { "rename", case_of(from), to }, true
  elseif r < 76 then
    local dir = pick(dirs)

    if dir == "" then return { "list", "/" }, true end

    local to_dir = pick(dirs)
    local to, target

    if to_dir == dir or inside(dir:lower(), to_dir:lower()) then
      to = pick(DIRS) .. " old"
      target = parent(dir) .. "/" .. to
    else
      target = to_dir .. "/" .. pick(DIRS) .. " moved"
      to = target
    end

    if target:lower() ~= dir:lower() and taken(m, target) then
      return { "rename", dir, to }, false
    end

    move(m, dir, target)
    return { "rename", dir, to }, true
  elseif r < 86 then
    local attrs = {}

    if rand(4) ~= 0 then
      attrs.title = pick(NAMES)
      attrs.rating = rand(6)
      if rand(2) == 0 then attrs.kind = "text/plain" end
    end

    return { "attrs", case_of(pick(files)), attrs }, true
  elseif r < 94 then
    return { "list", case_of(pick(dirs)) }, true
  else
    local dir = pick(dirs)

    if dir == "" or not empty(m, dir) then return { "unlink", dir }, false end

    m.dirs[dir:lower()] = nil
    return { "unlink", case_of(dir) }, true
  end
end

-- Grouped as the disk server groups them - most in a transaction of their
-- own, some several to one, some with none, some rolled back whatever
-- happened, and some lost to the power after their commit.
local groups = {}
local model = { dirs = { [""] = "" }, files = {} }

local function copy(m)
  local c = { dirs = {}, files = {} }

  for k, v in pairs(m.dirs) do c.dirs[k] = v end
  for k, v in pairs(m.files) do c.files[k] = v end
  return c
end

for _ = 1, 200 do
  local r = rand(100)
  local g = { ops = {} }
  local m = copy(model)
  local all = true

  g.txn = r >= 10
  g.rollback = r >= 10 and r < 16
  g.crash = r >= 16 and r < 22

  for _ = 1, (r >= 80) and 2 + rand(3) or 1 do
    local op, ok = one_op(m)

    g.ops[#g.ops + 1] = op
    all = all and ok
  end

  -- A transaction with a failure in it is rolled back, and so is one that
  -- was always going to be; without one, what happened stays.
  if not g.txn or (all and not g.rollback) then model = m end

  groups[#groups + 1] = g
end

--------------------------------------------------------------------------
-- Running them.
--------------------------------------------------------------------------

-- An operation, by one implementation: what it said, as a string both can be
-- compared by. Messages are left out - they are words, not the format.
local function run_op(kfs, sb, op)
  local kind = op[1]

  if kind == "store" then
    local now = op[5] and kfs.stamp(1790532000 + op[4], op[4] % 7) or op[4]
    local n = kfs.store(sb, op[2], bytes_of(op[3], op[4]), now)

    return n ~= nil, "store " .. tostring(n)
  elseif kind == "mkdir" then
    return kfs.mkdir(sb, op[2], 7) ~= nil, "mkdir"
  elseif kind == "unlink" then
    return kfs.unlink(sb, op[2]) ~= nil, "unlink"
  elseif kind == "rename" then
    return kfs.rename(sb, op[2], op[3]) ~= nil, "rename"
  elseif kind == "attrs" then
    local number, node = kfs.find(sb, op[2])

    if not number then return false, "attrs: no file" end

    return kfs.write_attrs(sb, number, node, op[3]) ~= nil, "attrs"
  elseif kind == "list" then
    local names = kfs.list(sb, op[2])

    return names ~= nil, "list " .. table.concat(names or {}, ",")
  end

  error("no operation " .. kind)
end

-- Every file and directory, what each holds and what is said about it.
local function tree(kfs, sb)
  local out = {}

  local function walk(path)
    for _, name in ipairs(assert(kfs.list(sb, path == "" and "/" or path))) do
      local full = path .. "/" .. name
      local number, node = kfs.find(sb, full)
      local attrs = kfs.read_attrs(sb, node)
      local keys = {}

      for k, v in pairs(attrs) do keys[#keys + 1] = k .. "=" .. tostring(v) end
      table.sort(keys)

      out[#out + 1] = ("%s #%d kind %d links %d size %d mtime %d {%s}"):format(
        full, number, node.kind, node.links, node.size, node.mtime,
        table.concat(keys, ","))

      if node.kind == kfs.KIND_DIR then
        walk(full)
      else
        local bytes = kfs.read_file(sb, node)

        out[#out + 1] = ("  %d bytes, sum %08x"):format(#bytes, sys.fnv1a(bytes))
      end
    end
  end

  walk("")
  return table.concat(out, "\n")
end

-- Four ways of choosing who does each step.
local RUNS = {
  { name = "all by the Lua", by = function() return "lua" end },
  { name = "all by the C", by = function() return "c" end },
  { name = "taking turns", by = function(i) return i % 2 == 0 and "lua" or "c" end },
  { name = "at random", by = function(i) return (i * 7919 % 13) % 2 == 0 and "c" or "lua" end },
}

local results = {}

for _, run in ipairs(RUNS) do
  local step = 0

  local function next_impl()
    step = step + 1
    return IMPL[run.by(step)], run.by(step)
  end

  disk = {}

  local maker = next_impl()

  assert(maker.mkfs(SECTORS, 1))

  local sb = assert(next_impl().mount())
  local said = {}

  for gi, g in ipairs(groups) do
    local kfs = next_impl()
    local ok = true

    if g.txn then assert(kfs.begin()) end

    for _, op in ipairs(g.ops) do
      local fine, what = run_op(kfs, sb, op)

      said[#said + 1] = ("%d %s %s: %s, %s"):format(gi, op[1], tostring(op[2]),
                                                    fine and "done" or "refused", what)
      ok = ok and fine
    end

    if g.txn then
      if g.rollback or not ok then
        kfs.rollback()
      elseif g.crash then
        assert(kfs.commit(sb, "after-commit"))

        -- The machine stops; whoever mounts next finishes the job.
        local mounter = next_impl()

        sb = assert(mounter.mount())
        said[#said + 1] = ("%d replayed %d"):format(gi, mounter.recover(sb))
      else
        assert(kfs.commit(sb))
      end
    end
  end

  local blocks = {}

  for n, bytes in pairs(disk) do blocks[n] = bytes end

  results[#results + 1] = { run = run, disk = blocks, said = table.concat(said, "\n"),
                            sb = sb }
end

local first = results[1]

check(#first.said > 300, ("the operations ran: %d answers"):format(#first.said))

do
  local stored = 0

  for line in first.said:gmatch("[^\n]+") do
    if line:find(": done, store %d") then stored = stored + 1 end
  end

  check(stored > 40, ("and most of the stores took: %d"):format(stored))
end

for i = 2, #results do
  local r = results[i]
  local differ, where = 0, nil

  for n = 0, SECTORS // 8 - 1 do
    if (first.disk[n] or string.rep("\0", 4096))
       ~= (r.disk[n] or string.rep("\0", 4096)) then
      differ = differ + 1
      where = where or n
    end
  end

  check(r.said == first.said,
        ("every operation came out the same %s as all by the Lua"):format(r.run.name))
  check(differ == 0,
        ("the disk %s is the Lua's, block for block: %d blocks differ, the "
         .. "first %s"):format(r.run.name, differ, tostring(where)))

  if r.said ~= first.said then
    local a, b = {}, {}

    for line in first.said:gmatch("[^\n]+") do a[#a + 1] = line end
    for line in r.said:gmatch("[^\n]+") do b[#b + 1] = line end

    for k = 1, math.max(#a, #b) do
      if a[k] ~= b[k] then
        print(("    first difference, answer %d:\n      lua: %s\n      %s: %s")
              :format(k, tostring(a[k]), r.run.name, tostring(b[k])))
        break
      end
    end
  end
end

-- What is on the disk, read by each: they must see the same files.
disk = first.disk

local by_lua = tree(IMPL.lua, assert(IMPL.lua.mount()))
local by_c = tree(IMPL.c, assert(IMPL.c.mount()))

check(#by_lua > 200 and by_lua == by_c,
      ("the Lua and the C read the same tree off the disk: %d lines and %d")
      :format(select(2, by_lua:gsub("\n", "")) + 1,
              select(2, by_c:gsub("\n", "")) + 1))

--------------------------------------------------------------------------
-- What the random run is unlikely to reach, on purpose, by both.
--------------------------------------------------------------------------

local function both(steps)
  local disks, saids = {}, {}

  for _, name in ipairs({ "lua", "c" }) do
    local kfs = IMPL[name]

    disk = {}

    local said = steps(kfs)
    local blocks = {}

    for n, bytes in pairs(disk) do blocks[n] = bytes end
    disks[name], saids[name] = blocks, said
  end

  local same = true

  for n = 0, SECTORS // 8 - 1 do
    same = same and (disks.lua[n] or "") == (disks.c[n] or "")
  end

  return same, saids.lua, saids.c
end

-- Twelve extents and no more: a disk of one-block files with every other
-- one removed, and then a file wanting more than twelve of the holes.
do
  local same, lua_said, c_said = both(function(kfs)
    local sb = assert(kfs.mkfs(8 * 1024, 1))

    sb = assert(kfs.mount())

    for i = 1, 40 do
      assert(kfs.store(sb, "/f" .. i, bytes_of(4096, i), 1))
    end

    for i = 1, 40, 2 do
      assert(kfs.unlink(sb, "/f" .. i))
    end

    assert(kfs.begin())

    local n, why = kfs.store(sb, "/big", bytes_of(15 * 4096, 99), 2)

    kfs.rollback()
    return tostring(n) .. " " .. tostring(why and why:find("fragmented") ~= nil)
  end)

  check(same, "a file too fragmented to store leaves the same disk from both")
  check(lua_said == "nil true" and c_said == "nil true",
        ("and both refuse it for its extents: %s, %s"):format(lua_said, c_said))
end

-- Full.
do
  local same, lua_said, c_said = both(function(kfs)
    local sb = assert(kfs.mkfs(8 * 512, 1))

    sb = assert(kfs.mount())
    assert(kfs.begin())

    local n, why = kfs.store(sb, "/Home/huge", bytes_of(4 * 1024 * 1024, 5), 1)

    kfs.rollback()

    local free = kfs.free_blocks(sb)

    return tostring(n) .. " " .. tostring(why and why:find("full") ~= nil)
           .. " " .. tostring(free)
  end)

  check(same, "a file too large for the disk leaves the same disk from both")
  check(lua_said == c_said and lua_said:find("^nil true") ~= nil,
        ("and both refuse it as full, the same blocks free: %s, %s")
        :format(lua_said, c_said))
end

-- Into itself.
do
  local same, lua_said, c_said = both(function(kfs)
    local sb = assert(kfs.mkfs(8 * 1024, 1))

    sb = assert(kfs.mount())
    assert(kfs.mkdir(sb, "/Home/a", 1))
    assert(kfs.mkdir(sb, "/Home/a/b", 1))

    local ok = kfs.rename(sb, "/Home/a", "/Home/a/b/a")

    -- Nor through a path in another case, which the Lua's check let past
    -- until the C was written against it; nor to a name no path reaches.
    local other = kfs.rename(sb, "/Home/a", "/HOME/A/b/a")
    local dots = kfs.rename(sb, "/Home/a/b", "..")

    return ("%s %s %s %s %s"):format(tostring(ok), tostring(other), tostring(dots),
                                     table.concat(kfs.list(sb, "/Home") or { "gone" }, ","),
                                     table.concat(kfs.list(sb, "/Home/a") or { "gone" }, ","))
  end)

  check(same and lua_said == "nil nil nil a b" and c_said == lua_said,
        ("a directory is not moved into itself however the path is spelled, "
         .. "and nothing is renamed to ..: %s, %s"):format(lua_said, c_said))
end

if failed > 0 then
  print(("\nFAIL: %d of %d checks on kfs.c against kfs.lua.")
        :format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on kfs.c against kfs.lua: %d operations four ways, "
       .. "the same disk block for block, and the same files read off it.")
      :format(passed, select(2, first.said:gsub("\n", "")) + 1))
