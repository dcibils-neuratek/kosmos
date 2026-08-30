-- The first Lua outside the kernel, and the first servers.
--
-- One image, several roles. The kernel starts a process per role and hands
-- each exactly the capabilities that role needs; nothing here can reach
-- anything it was not given, because there is no other way to name one.

-- A process is told one word: which role it is. Its capabilities are not
-- arguments, they are already in its table, and the first one is what it was
-- started for. A spawned *thread* gets its capabilities as arguments because
-- it is created by Lua; a process is created by the kernel, which grants
-- them before it can run.
local role = ...

local CAP = 0             -- by convention, a process's first capability

local ROLE_CLIENT   = 0
local ROLE_RAMFS    = 1
local ROLE_CLIENT_B = 2
local ROLE_SELFTEST = 3   -- needs no capability: checks the language itself

local function line(s) sys.write(s .. "\n") end

--------------------------------------------------------------------------
-- The protocol
--
-- design.md 4.4: `list`, `read`, `write`, `getattr`, `setattr`, over typed
-- records rather than byte streams. A request is a table with a `type`, and
-- design.md 14 makes that field mandatory: with no static types, a message
-- that does not say what it is becomes a silent nil three layers down.
--
-- `read` returns a table, not a string. That is the whole point of the
-- protocol: fs.read("/dev/temp") gives { celsius = 47.2 } rather than
-- "47200\n" to be parsed by whoever asked.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- A server: receive, dispatch, reply, repeat.
--
-- Each request runs in a coroutine. Today that buys error isolation - a
-- handler that raises kills its own request and not the server - and it is
-- also the shape design.md 4.5 wants: every `receive` is a yield, so server
-- code is written sequentially over synchronous IPC instead of as a state
-- machine.
--------------------------------------------------------------------------

local function serve(endpoint, handlers)
  while true do
    local request, sender = sys.receive(endpoint)
    if not request then return end          -- the endpoint went away

    local handler = handlers[request.type]
    local reply

    if not handler then
      reply = { ok = false, error = "no such operation: " ..
                tostring(request.type) }
    else
      local co = coroutine.create(handler)
      local ok, result = coroutine.resume(co, request)

      if not ok then
        -- A handler that raised. The client is told, and the server keeps
        -- serving, which is the entire reason each request gets its own
        -- coroutine rather than being called directly.
        reply = { ok = false, error = tostring(result) }
      else
        reply = result
      end
    end

    sys.reply(sender, reply)
  end
end

--------------------------------------------------------------------------
-- ramfs: a tree of nodes, in memory.
--
-- A node has attributes and either children or a value. Directories and
-- files are the same kind of thing with a different field filled in, which
-- is what lets `list` and `read` be the same protocol rather than two.
--------------------------------------------------------------------------

local function split(path)
  local parts = {}
  for part in path:gmatch("[^/]+") do parts[#parts + 1] = part end
  return parts
end

local function ramfs_main(endpoint)
  local root = { children = {}, attrs = { kind = "directory" } }

  local function find(path, create)
    local node = root
    for _, name in ipairs(split(path)) do
      if not node.children then return nil end
      local child = node.children[name]
      if not child then
        if not create then return nil end
        child = { attrs = {} }
        node.children[name] = child
      end
      node = child
    end
    return node
  end

  serve(endpoint, {
    list = function(req)
      local node = find(req.path)
      if not node then return { ok = false, error = "no such path" } end
      if not node.children then
        return { ok = false, error = "not a directory" }
      end
      local names = {}
      for name in pairs(node.children) do names[#names + 1] = name end
      table.sort(names)                     -- so a listing is reproducible
      return { ok = true, entries = names }
    end,

    read = function(req)
      local node = find(req.path)
      if not node then return { ok = false, error = "no such path" } end
      if node.value == nil then
        return { ok = false, error = "not readable" }
      end
      return { ok = true, value = node.value }
    end,

    write = function(req)
      local node = find(req.path, true)
      node.value = req.value
      node.children = nil                   -- it holds data now
      node.attrs.size = (type(req.value) == "string") and #req.value or 1
      return { ok = true }
    end,

    getattr = function(req)
      local node = find(req.path)
      if not node then return { ok = false, error = "no such path" } end
      return { ok = true, attrs = node.attrs }
    end,

    setattr = function(req)
      local node = find(req.path, true)
      for k, v in pairs(req.attrs) do node.attrs[k] = v end
      return { ok = true }
    end,
  })
end

--------------------------------------------------------------------------
-- The client side of the protocol: a namespace.
--
-- design.md 2: what a process has not mounted does not exist. Not permission
-- denied - no such path. That falls out of this being a lookup rather than a
-- check: an unmounted prefix matches nothing, so there is nothing to deny.
--
-- The mount table lives in the process, which is what makes the namespace
-- per process rather than global. Two processes can mount the same server at
-- different names, and neither can see the other's.
--------------------------------------------------------------------------

local function new_namespace()
  local mounts = {}

  local ns = {}

  function ns.mount(prefix, capability)
    mounts[#mounts + 1] = { prefix = prefix, cap = capability }
    -- Longest prefix first, so /a/b wins over /a regardless of mount order.
    table.sort(mounts, function(x, y) return #x.prefix > #y.prefix end)
  end

  local function resolve(path)
    for _, m in ipairs(mounts) do
      if path == m.prefix or path:sub(1, #m.prefix + 1) == m.prefix .. "/" then
        local rest = path:sub(#m.prefix + 1)
        return m.cap, (rest == "" and "/" or rest)
      end
    end
    return nil
  end

  local function request(op, path, extra)
    local capability, rest = resolve(path)
    if not capability then
      -- The sentence design.md 2 asks for. Nothing was denied; there is
      -- simply no such path in this process's world.
      return nil, "no such path: " .. path
    end

    local req = { type = op, path = rest }
    if extra then for k, v in pairs(extra) do req[k] = v end end

    local reply, err = sys.call(capability, req)
    if not reply then return nil, err end
    if not reply.ok then return nil, reply.error end
    return reply
  end

  function ns.list(path)
    local r, e = request("list", path)
    return r and r.entries, e
  end

  function ns.read(path)
    local r, e = request("read", path)
    if not r then return nil, e end
    return r.value
  end

  function ns.write(path, value)
    local r, e = request("write", path, { value = value })
    return r ~= nil, e
  end

  function ns.getattr(path)
    local r, e = request("getattr", path)
    return r and r.attrs, e
  end

  return ns
end

--------------------------------------------------------------------------

if role == ROLE_SELFTEST then
  -- Lua itself, at EL0, on a heap it cannot grow. No capabilities and no
  -- server: this checks that the language works out here, which everything
  -- above quietly assumes.
  local function check(c, what) if not c then error("selftest: " .. what) end end

  line("selftest: Lua " .. _VERSION .. " at EL0")
  check(2 + 2 == 4, "arithmetic")
  check(1 / 2 == 0.5 and math.type(1 / 2) == "float", "floats")
  check(math.sqrt(16.0) == 4.0, "the math library")
  check(("kosmos"):upper() == "KOSMOS", "strings")

  local co = coroutine.create(function(a)
    local b = coroutine.yield(a * 2)
    return b + 1
  end)
  local _, x = coroutine.resume(co, 21)
  local _, y = coroutine.resume(co, 100)
  check(x == 42 and y == 101, "coroutines")

  local ok, err = pcall(function() error("deliberate") end)
  check(ok == false and err:find("deliberate"), "pcall")

  check(io == nil and os == nil and debug == nil, "a forbidden library is present")

  collectgarbage()
  local before = collectgarbage("count")
  local t = {}
  for i = 1, 2000 do t[i] = { i } end
  local peak = collectgarbage("count")
  t = nil
  collectgarbage()
  local after = collectgarbage("count")
  check(peak > before * 2 and after < peak / 2, "the collector did not reclaim")
  line(string.format("selftest: gc %.0fK -> %.0fK -> %.0fK", before, peak, after))

  line("selftest: done")
  return
end

if role == ROLE_RAMFS then
  ramfs_main(CAP)
  return
end

-- A client. The name it mounts the filesystem under is its own business,
-- and is the whole demonstration: the same server, two processes, two
-- different worlds.
local mount_point = (role == ROLE_CLIENT) and "/data" or "/files"

local fs = new_namespace()
fs.mount(mount_point, CAP)

line("client: mounted the ramfs at " .. mount_point)

-- Everything below asserts as well as prints. A failure raises, the chunk
-- returns non-zero, and the test that runs these processes sees it; printing
-- alone would make a broken run look like a working one to anything that is
-- not a person reading the output.
local function check(condition, what)
  if not condition then error("client: " .. what) end
end

-- write and read back. `read` returns a table, not a string to be parsed,
-- which is the whole argument in design.md 4.4.
check(fs.write(mount_point .. "/sensor", { celsius = 47.2, unit = "C" }),
      "write failed")
check(fs.write(mount_point .. "/note", "hello"), "write of a string failed")

local sensor = fs.read(mount_point .. "/sensor")
check(type(sensor) == "table", "read did not return a table")
check(sensor.celsius == 47.2 and sensor.unit == "C", "read returned the wrong table")
line("client: " .. mount_point .. "/sensor -> " .. sensor.celsius
     .. " " .. sensor.unit .. "  (a table, not a string)")

local entries = fs.list(mount_point)
check(#entries == 2 and entries[1] == "note" and entries[2] == "sensor",
      "list returned the wrong entries")
line("client: " .. mount_point .. " contains " .. table.concat(entries, ", "))

local attrs = fs.getattr(mount_point .. "/note")
check(attrs ~= nil and attrs.size == 5, "getattr returned the wrong size")

-- And the property the milestone is about. The other client mounted the same
-- server somewhere else. That name does not exist here, and the answer is
-- "no such path" rather than "denied": nothing was refused, because there was
-- nothing to refuse.
local other = (mount_point == "/data") and "/files" or "/data"
local value, err = fs.read(other .. "/sensor")
check(value == nil, "the other client's mount point was visible")
check(err:find("no such path") ~= nil, "the wrong error for an unmounted path")
line("client: " .. other .. "/sensor -> " .. tostring(value) .. ", " .. err)

-- An operation the server does not implement is an error, not a crash, and
-- the server keeps serving afterwards.
local ok = fs.read(mount_point)               -- a directory is not readable
check(ok == nil, "reading a directory should have failed")
check(fs.read(mount_point .. "/sensor") ~= nil, "the server stopped serving")

line("client: done")
