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
local ROLE_CONSOLE  = 4
local ROLE_SHELL    = 5
local ROLE_RELOAD   = 6   -- checks hot reload against a live server
local ROLE_INIT     = 7   -- starts everything else, and outlives it

local ROLE_SPAWNTEST = 8  -- checks what a spawn may and may not pass on

local SPAWN_CONSOLE = 1
local SPAWN_SCREEN  = 2

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
-- A server: receive, dispatch, reply, repeat. And reload.
--
-- Each request runs in a coroutine. Today that buys error isolation - a
-- handler that raises kills its own request and not the server - and it is
-- also the shape design.md 4.5 wants: every `receive` is a yield, so server
-- code is written sequentially over synchronous IPC instead of as a state
-- machine.
--
-- **The state is a table and the behaviour is a function of it.** That
-- separation is the whole of hot reload, and it is why `serve` takes a
-- factory rather than a table of handlers. Anything captured in a closure
-- built at startup is lost when the code is replaced; anything in `state`
-- survives, because the new handlers are handed the same table.
--
-- design.md 10 calls this level 1: the process never died and the clients
-- never knew. Level 2, where a supervisor restarts a server that actually
-- died and clients reconnect through the namespace, is a different problem
-- and comes after there is something to supervise.
--------------------------------------------------------------------------

local function serve(endpoint, state, make_handlers)
  local handlers = make_handlers(state)

  local function reload(source)
    -- Loaded, then run, then installed, in that order and each checked. A
    -- server that half-reloads is worse than one that refuses to: the
    -- client is told no and the old code is still serving.
    local chunk, err = load(source, "=reload", "t")
    if not chunk then return { ok = false, error = "reload: " .. tostring(err) } end

    local ok, factory = pcall(chunk)
    if not ok then return { ok = false, error = "reload: " .. tostring(factory) } end
    if type(factory) ~= "function" then
      return { ok = false, error = "reload: the chunk did not return a factory" }
    end

    local built
    ok, built = pcall(factory, state)
    if not ok then return { ok = false, error = "reload: " .. tostring(built) } end
    if type(built) ~= "table" then
      return { ok = false, error = "reload: the factory did not return handlers" }
    end

    -- The same `state` table the old handlers were built around. Nothing is
    -- copied and nothing is migrated: it is the same table, and the new code
    -- simply keeps using it.
    handlers = built
    state.reloads = (state.reloads or 0) + 1
    return { ok = true, reloads = state.reloads }
  end

  while true do
    local request, sender = sys.receive(endpoint)
    if not request then return end          -- the endpoint went away

    local reply

    if request.type == "reload" then
      reply = reload(request.source)
    else
      local handler = handlers[request.type]

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

--
-- The state is the tree; the behaviour is everything below. Reloading
-- replaces the second and keeps the first, which is what makes a filesystem
-- server something you can fix while it is holding your files.

local function ramfs_handlers(state)
  local function find(path, create)
    local node = state.root
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

  return {
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
      state.writes = (state.writes or 0) + 1
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
  }
end

local function ramfs_main(endpoint)
  local state = {
    root = { children = {}, attrs = { kind = "directory" } },
    writes = 0,
  }

  serve(endpoint, state, ramfs_handlers)
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

  function ns.stat(path)
    local r, e = request("stat", path)
    return r and r.value, e
  end

  -- Replaces the code of whatever serves this path, keeping its state.
  --
  -- An operation like any other, reached by name like any other, which is
  -- the point: there is no separate management channel and no privileged
  -- back door. A process can reload a server exactly when it holds a
  -- capability for it, and not otherwise.
  function ns.reload(path, source)
    local r, e = request("reload", path, { source = source })
    return r and r.reloads or nil, e
  end

  return ns
end

--------------------------------------------------------------------------
-- The console server.
--
-- It owns the serial port, and it is the only process that does: `sys.write`
-- and `sys.getchar` are refused to everything else. That is what makes this
-- a server rather than a convention - a client cannot decide to print
-- directly, because the machine will not let it.
--
-- It serves two operations at one path. `write` puts a string; `read` waits
-- for a line, echoing as it goes, which is where the line editing lives. A
-- client that wants a line asks for one and blocks until there is one, and
-- that blocking is free: synchronous IPC already parks the caller.
--------------------------------------------------------------------------

local function console_handlers(state)
  return {
    write = function(req)
      local text = tostring(req.value)
      state.bytes = state.bytes + #text
      sys.write(text)
      return { ok = true }
    end,

    read = function(req)
      local buf = {}

      while true do
        local c = sys.getchar()

        if c == nil then
          -- Nothing waiting. Yielding rather than spinning costs a
          -- scheduling slot instead of the machine; there is no UART
          -- interrupt to park on until the terminal at M6.
          sys.yield()
        elseif c == 10 or c == 13 then
          sys.write("\n")
          state.lines = state.lines + 1
          return { ok = true, value = table.concat(buf) }
        elseif c == 8 or c == 127 then
          if #buf > 0 then
            table.remove(buf)
            -- Back up, overwrite, back up again: a serial terminal erases
            -- nothing just because the cursor moved over it.
            sys.write("\b \b")
          end
        elseif c >= 32 and c < 127 then
          buf[#buf + 1] = string.char(c)
          sys.write(string.char(c))
        end
        -- Anything else is dropped rather than echoed. An arrow key arrives
        -- as three bytes of escape sequence, and the thing that understands
        -- those is a terminal emulator, which is M6.
      end
    end,

    stat = function(req)
      -- What it has done, out of the state table. It survives a reload,
      -- which is how you can see that the state and the code are separate
      -- things.
      return { ok = true, value = {
        bytes = state.bytes, lines = state.lines,
        reloads = state.reloads or 0,
      } }
    end,
  }
end

local function console_main(endpoint)
  serve(endpoint, { bytes = 0, lines = 0 }, console_handlers)
end

--------------------------------------------------------------------------
-- The shell.
--
-- A process like any other. It holds two capabilities and can name nothing
-- else: the console, and a filesystem. It cannot print except by asking the
-- console server, and it cannot read a file except by asking the ramfs.
--
-- design.md 9.1's Lisp Machine property in its first form: the system is
-- modified from the same language it is written in, from a prompt, while it
-- is running.
--------------------------------------------------------------------------

local function shell_main(console_cap, ramfs_cap)
  local ns = new_namespace()
  ns.mount("/dev/console", console_cap)
  ns.mount("/data", ramfs_cap)

  local function out(s) ns.write("/dev/console", s) end
  local function readline() return ns.read("/dev/console") end

  -- What a chunk typed at the prompt can see. `fs` is this process's own
  -- namespace, so what the shell can reach is what the shell was given -
  -- there is no privileged view to hand out.
  local env = {
    fs = ns,
    print = function(...)
      local parts = {}
      for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring((select(i, ...)))
      end
      out(table.concat(parts, "\t") .. "\n")
    end,
  }
  setmetatable(env, { __index = _G })

  out("\nKosmos shell. A process, talking to servers.\n")
  out("Try: fs.list(\"/data\")   fs.read(\"/data/sensor\")   2+2\n\n")

  while true do
    out("kosmos> ")

    local input = readline()
    if input == nil then return end          -- the console went away

    if input ~= "" then
      -- `2+2` is not a chunk, it is an expression. Every Lua prompt wraps
      -- the line in `return` first and falls back to the line as written.
      local chunk, err = load("return " .. input, "=stdin", "t", env)
      if not chunk then
        chunk, err = load(input, "=stdin", "t", env)
      end

      if not chunk then
        out("error: " .. tostring(err) .. "\n")
      else
        local results = table.pack(pcall(chunk))
        if not results[1] then
          out("error: " .. tostring(results[2]) .. "\n")
        else
          for i = 2, results.n do
            out(tostring(results[i]) .. (i < results.n and "\t" or "\n"))
          end
        end
      end
    end
  end
end

--------------------------------------------------------------------------

if role == ROLE_SPAWNTEST then
  -- What a process may hand a child, and what it may not.
  --
  -- It reports by exit code rather than by writing, because it deliberately
  -- does not hold the console: the point of the last check is that it cannot
  -- get one. A fixture that needs the console to say why it failed cannot
  -- test not having the console.
  local function check(c, code) if not c then sys.exit(code) end end

  -- A child of its own image, with no capabilities at all. It runs the
  -- selftest, which needs none, and ends.
  local id = sys.spawn(ROLE_SELFTEST, {})
  check(id ~= nil, 10)                      -- spawn failed

  local waited, code = sys.wait()
  check(waited == id, 11)                   -- wait returned the wrong child
  check(code == 0, 12)                      -- the child itself failed

  -- Nothing left to wait for, and saying so beats blocking forever.
  local none = sys.wait()
  check(none == nil, 13)                    -- wait invented a child

  -- And the property worth having: this process does not own the console,
  -- so it cannot give one away. Otherwise any process could promote itself
  -- by spawning a child and asking it to print.
  local promoted = sys.spawn(ROLE_SELFTEST, {}, SPAWN_CONSOLE)
  check(promoted == nil, 14)                -- it handed out a console it lacked

  sys.exit(0)
end

if role == ROLE_INIT then
  --------------------------------------------------------------------------
  -- init.
  --
  -- The kernel used to do this: create the servers, wire up their
  -- capabilities, start them. It is a process now, and the kernel's job ends
  -- at starting this one.
  --
  -- What it can do is bounded by what it holds. It was given the console and
  -- an endpoint for each server it is expected to start, and it passes those
  -- on; it cannot promote a child beyond itself, because a spawn resolves
  -- every capability against the parent's own table and refuses to hand out
  -- a device the parent does not hold.
  --
  -- Supervision is design.md 10's criticality hierarchy in its first form:
  -- init waits, and when a server ends it says so. Restarting one is level 2
  -- and needs somewhere for its state to have lived, which is a decision
  -- design.md deliberately leaves until there is state worth recovering.
  --------------------------------------------------------------------------
  local CONSOLE_EP = 0
  local RAMFS_EP   = 1

  -- **A failed spawn says which one and why.**
  --
  -- These used to be `if not x then sys.exit(1) end`, and the system would
  -- die at boot in complete silence: no banner, no prompt, no message, with
  -- the kernel's own output looking perfectly healthy above it. That cost a
  -- debugging session the first time a spawn started being refused. init
  -- holds the console at this point precisely so it can say things, and the
  -- one moment it most needs to is when it cannot build the system.
  local function start(what, role, caps, flags)
    local id, err = sys.spawn(role, caps, flags)

    if not id then
      line("init: could not start " .. what .. ": " .. tostring(err))
      sys.exit(1)
    end

    return id
  end

  local console = start("the console server", ROLE_CONSOLE,
                        { CONSOLE_EP }, SPAWN_CONSOLE)
  local ramfs   = start("the ramfs", ROLE_RAMFS, { RAMFS_EP })

  -- The shell gets both endpoints, in the order it expects them, and the
  -- screen.
  --
  -- Temporary, and it is worth saying why rather than leaving it to look
  -- like the design. The screen belongs to whichever process composes, and
  -- that will be the app server. There is no app server yet, so it goes to
  -- the shell - which means `gfx.screen()` works at the prompt and a person
  -- can draw. When the app server arrives this line hands it there instead
  -- and nothing else about the mechanism changes: init decides, the same way
  -- it already decides who gets the console.
  --
  -- It does *not* get the console: it prints by asking the console server,
  -- like everything else, and `sys.write` from the prompt returning -102 is
  -- the demonstration.
  local shell = start("the shell", ROLE_SHELL,
                      { CONSOLE_EP, RAMFS_EP }, SPAWN_SCREEN)

  -- And now it does what an init does, which is outlive everything and
  -- notice when something ends.
  while true do
    local id, code = sys.wait()
    if not id then
      -- Nothing left. On a real system this is the moment to panic or to
      -- restart something; here there is nobody left to tell.
      sys.exit(0)
    end

    -- It cannot say so itself: it does not own the console and the console
    -- server may well be the thing that just died. Recorded rather than
    -- printed, which is the honest shape until there is a log server.
    local _ = { id = id, code = code }
  end
end

if role == ROLE_RELOAD then
  -- M5's other half: reload a server's code while a client is connected,
  -- without the client noticing.
  --
  -- "Without noticing" is precise. This process holds one capability and
  -- never reconnects: the same endpoint, the same server process, the same
  -- table of files. What changes underneath it is the code.
  local function check(c, what) if not c then error("reload: " .. what) end end

  local fs = new_namespace()
  fs.mount("/fs", CAP)

  check(fs.write("/fs/note", "before"), "the first write failed")
  check(fs.read("/fs/note") == "before", "the first read came back wrong")

  -- New behaviour, same state. `read` now shouts; everything else is as it
  -- was. The tree is not passed in or copied - the factory is handed the
  -- state table the old handlers were already using.
  local source = [[
    return function(state)
      local function find(path)
        local node = state.root
        for name in path:gmatch("[^/]+") do
          if not node.children then return nil end
          node = node.children[name]
          if not node then return nil end
        end
        return node
      end

      return {
        read = function(req)
          local node = find(req.path)
          if not node or node.value == nil then
            return { ok = false, error = "no such path" }
          end
          return { ok = true, value = tostring(node.value):upper() }
        end,

        stat = function(req)
          return { ok = true, value = {
            writes = state.writes, reloads = state.reloads or 0,
          } }
        end,
      }
    end
  ]]

  local reloads, err = fs.reload("/fs", source)
  check(reloads == 1, "reload failed: " .. tostring(err))

  -- The file is still there, which is the state surviving.
  -- It comes back shouting, which is the code having been replaced.
  check(fs.read("/fs/note") == "BEFORE", "state and code did not both survive")

  -- The counter the old code kept is the counter the new code reads.
  local stat = fs.stat("/fs")
  check(stat ~= nil and stat.writes == 1, "the write counter did not survive")
  check(stat.reloads == 1, "the reload was not counted")

  -- And an operation the new code dropped is gone, which is proof the
  -- handlers really were replaced rather than added to.
  local ok = fs.write("/fs/other", "x")
  check(ok == false, "an operation the new code does not have still worked")

  -- A reload that does not compile is refused, and the server keeps
  -- serving the code it already had. Half-reloading would be worse than
  -- not reloading.
  local bad, berr = fs.reload("/fs", "this is not lua")
  check(bad == nil and berr:find("reload") ~= nil, "a broken reload was accepted")
  check(fs.read("/fs/note") == "BEFORE", "a refused reload broke the server")

  line("reload: state survived, code replaced, bad reload refused")
  return
end

if role == ROLE_CONSOLE then
  console_main(CAP)
  return
end

if role == ROLE_SHELL then
  -- Two capabilities, in the order the kernel granted them.
  shell_main(0, 1)
  return
end

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
