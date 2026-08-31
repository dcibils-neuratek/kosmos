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
local ROLE_DEVICES   = 9  -- serves /dev: what hardware was found
local ROLE_BINFS     = 11 -- serves /bin: the programs carried in the image
local ROLE_RUNNER    = 12 -- runs one program, in an address space of its own
local ROLE_LIBFS     = 13 -- serves /lib: the libraries carried in the image
local ROLE_APPFS     = 14 -- serves /app: what each running program exposes
local ROLE_DISKFS    = 15 -- serves /disk: the block device, and only it

local SPAWN_CONSOLE = 1
local SPAWN_SCREEN  = 2

-- The disk. The strongest grant there is - raw sectors are every file on the
-- machine whatever any namespace says - so exactly one process gets it and
-- everything else asks that process. Same shape as the console and the
-- screen, and the reason is stronger.
local SPAWN_DISK    = 4

local function line(s) sys.write(s .. "\n") end

--------------------------------------------------------------------------
-- Text on its way to the console, in pieces if it has to be.
--
-- A message is 2048 bytes. `ns.read` already assembles a value that spans
-- several; this is the other direction, and it was missing - `cat` on a
-- four-kilobyte program read it back perfectly and then died trying to
-- print it.
--
-- Splitting belongs here and not in `ns.write`, because a console is a
-- stream and a file is not: two writes to /dev/console are one line after
-- another, and two writes to /data/notes are the second replacing the
-- first. Only the caller knows which it meant.
--------------------------------------------------------------------------
local CONSOLE_CHUNK = 1400

local function write_text(ns, path, text)
  if #text <= CONSOLE_CHUNK then
    return ns.write(path, text)
  end

  local at = 1

  while at <= #text do
    local piece = text:sub(at, at + CONSOLE_CHUNK - 1)
    local ok, err = ns.write(path, piece)

    if not ok then return nil, err end
    at = at + #piece
  end

  return true
end

--------------------------------------------------------------------------
-- What the machine is.
--
-- `sys.info()` hands back raw ID registers and pool counts and decodes
-- nothing, deliberately: decoding a MIDR is a table lookup, and tables
-- belong up here. A processor the kernel has never heard of gets described
-- properly without the kernel changing, which is the same division of
-- labour design.md 1 draws everywhere else.
--
-- The numbers below are from arch/arm64/include/asm/cputype.h and
-- arch/arm64/tools/sysreg, the same sources the kernel's own decode used.
--------------------------------------------------------------------------

local IMPLEMENTERS = {
  [0x41] = "ARM",      [0x42] = "Broadcom", [0x43] = "Cavium",
  [0x46] = "Fujitsu",  [0x48] = "HiSilicon", [0x4e] = "NVIDIA",
  [0x50] = "APM",      [0x51] = "Qualcomm", [0x61] = "Apple",
  [0x6d] = "Microsoft", [0xc0] = "Ampere",
}

local ARM_PARTS = {
  [0xb76] = "ARM1176JZF-S", [0xc07] = "Cortex-A7",  [0xc08] = "Cortex-A8",
  [0xc09] = "Cortex-A9",    [0xd03] = "Cortex-A53", [0xd05] = "Cortex-A55",
  [0xd07] = "Cortex-A57",   [0xd08] = "Cortex-A72", [0xd09] = "Cortex-A73",
  [0xd0a] = "Cortex-A75",   [0xd0b] = "Cortex-A76", [0xd0c] = "Neoverse-N1",
  [0xd0d] = "Cortex-A77",   [0xd41] = "Cortex-A78",
}

-- ID_AA64MMFR0_EL1.PARANGE [3:0] is a table, not a formula.
local PA_BITS = { [0]=32, 36, 40, 42, 44, 48, 52, 56 }

local function describe_machine()
  local i = sys.info()
  if not i then return nil end

  local impl = (i.midr >> 24) & 0xff
  local part = (i.midr >> 4) & 0xfff

  local m = { raw = i }

  m.cpu = {
    implementer = IMPLEMENTERS[impl] or string.format("0x%02x", impl),
    part        = (impl == 0x41 and ARM_PARTS[part])
                  or string.format("part 0x%03x", part),
    revision    = string.format("r%dp%d", (i.midr >> 20) & 0xf, i.midr & 0xf),
    midr        = i.midr,
    cores       = i.cpus,
    -- CTR_EL0 DminLine [19:16] is log2 of the line in *words*, not bytes.
    cache_line  = 4 << ((i.ctr >> 16) & 0xf),
    pa_bits     = PA_BITS[i.mmfr0 & 0xf] or 0,
    counter_hz  = i.counter_hz,
    -- ID_AA64ISAR0_EL1: AES [7:4], SHA1 [11:8], SHA2 [15:12], CRC32 [19:16],
    -- atomics [23:20]. Non-zero means present.
    aes         = ((i.isar0 >> 4) & 0xf) ~= 0,
    sha1        = ((i.isar0 >> 8) & 0xf) ~= 0,
    sha2        = ((i.isar0 >> 12) & 0xf) ~= 0,
    crc32       = ((i.isar0 >> 16) & 0xf) ~= 0,
    atomics     = ((i.isar0 >> 20) & 0xf) ~= 0,
    -- ID_AA64PFR0_EL1: FP [19:16], AdvSIMD [23:20]. 0xf means absent.
    fp          = ((i.pfr0 >> 16) & 0xf) ~= 0xf,
    simd        = ((i.pfr0 >> 20) & 0xf) ~= 0xf,
    el          = i.current_el,
  }

  m.memory = {
    total_mb    = i.pages_total * i.page_size // (1024 * 1024),
    free_mb     = i.pages_free  * i.page_size // (1024 * 1024),
    pages_total = i.pages_total,
    pages_free  = i.pages_free,
    page_size   = i.page_size,
    base        = i.ram_base,
  }

  m.kernel = {
    idle_ticks = i.idle_ticks,    busy_ticks    = i.busy_ticks,
    threads   = i.threads_used,   threads_max   = i.threads_total,
    processes = i.processes_used, processes_max = i.processes_total,
    endpoints = i.endpoints_used, endpoints_max = i.endpoints_total,
    spaces    = i.spaces_used,    spaces_max    = i.spaces_total,
    tick_hz   = i.tick_hz,
  }

  if i.screen_width > 0 then
    m.screen = { width = i.screen_width, height = i.screen_height,
                 pitch = i.screen_pitch }
  end

  if i.has_keyboard ~= 0 then
    m.keyboard = { transport = "virtio-input over virtio-mmio" }
  end

  m.console = { transport = "PL011 UART, polled" }
  m.timer   = { hz = i.tick_hz, counter_hz = i.counter_hz }

  return m
end

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

--
-- A handler returns this when it is not answering yet.
--
-- Live queries need it: a watcher's call is parked until something it is
-- watching changes, which may be minutes later or never. The client is
-- blocked in that call the whole time and that is the point - it is waiting
-- without asking, which is the difference between a live query and a poll.
--
-- A sentinel and not `nil`, because `nil` is what a handler returns when
-- somebody forgot a `return`, and the failure that produces - a client
-- blocked for ever on a reply nobody is going to send - is far too quiet.
--
local DEFER = { "deferred" }

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

  --
  -- One request, answered. Pulled out of the loop below because a handler
  -- that has to wait needs to be able to do this too - see `state.pump`.
  --
  -- `cap` is a capability that came *with* the request, at whatever index
  -- the kernel put it in this process's table. Most handlers ignore it; the
  -- registry below is the one that needs it, because registering is exactly
  -- handing over an endpoint.
  local function answer(request, sender, cap)
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
        local ok, result = coroutine.resume(co, request, sender, cap)

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

    --
    -- A reply that will not fit is still a reply.
    --
    -- `sys.reply` raises when the value does not serialise, and this call is
    -- outside the coroutine that isolates a handler - so a handler returning
    -- something too large took the whole server down. That is exactly what
    -- happened the first time /bin was asked for a program bigger than a
    -- message: the program store died, and the client saw only that its
    -- request never came back.
    --
    -- Now the failure reaches whoever asked, which is the one place that can
    -- do anything about it.
    -- The handler said it would answer later, and holds `sender` to do it
    -- with. Nothing to send now.
    if reply == DEFER then
      return
    end

    -- A reply may carry a capability. `send_cap` is taken out rather than
    -- serialised: the number in it is an index in *this* process's table
    -- and would mean something else entirely in the caller's.
    local passing = nil

    if type(reply) == "table" and reply.send_cap then
      passing = reply.send_cap
      reply.send_cap = nil
    end

    local sent = pcall(sys.reply, sender, reply, passing)

    if not sent then
      pcall(sys.reply, sender,
            { ok = false, error = "the answer does not fit in a message" })
    end
  end

  --------------------------------------------------------------------------
  -- For a handler that has to wait.
  --
  -- A server is one thread, so a handler that blocks blocks the server, and
  -- everyone else waits for something that has nothing to do with them. The
  -- console is where this stopped being theoretical: it blocks inside `read`
  -- until somebody types a line, and while it is blocked it answers nobody -
  -- so a status bar asking once a second whether Control-C was pressed was
  -- waiting on a keystroke that would only arrive if you stopped waiting for
  -- the status bar.
  --
  -- So a waiting handler calls this instead of only yielding: whatever has
  -- arrived gets answered, and then the wait continues. Non-blocking, so an
  -- empty queue costs one syscall and returns.
  --
  -- The alternative was a second thread inside the server, and there are no
  -- threads inside a process - which is not a limitation to work around
  -- here. One thread is why a handler never races another handler, and that
  -- is worth more than what it costs.
  --------------------------------------------------------------------------
  function state.pump()
    while true do
      local request, sender, cap = sys.receive(endpoint, true)
      if not request then return end
      answer(request, sender, cap)
    end
  end

  while true do
    local request, sender, cap = sys.receive(endpoint)
    if not request then return end          -- the endpoint went away
    answer(request, sender, cap)
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

--------------------------------------------------------------------------
-- Attributes, the index over them, and live queries.
--
-- `beos.md` 17.2 and roadmap M7. The BeOS idea: a file is a named thing
-- with typed attributes, the filesystem indexes those attributes, and a
-- query over them is as cheap as opening one file. Entity files - nodes
-- with attributes and no content at all - fall straight out of it.
--
-- The index is the whole claim. Without it a query is a walk, and a walk
-- costs more the more files there are, which is exactly the thing BeOS was
-- built to avoid and the thing the benchmark for this milestone measures.
-- `bench/` has it, and what it has to show is a flat line.
--
-- Structure: index[attribute][value] is the set of paths that have it.
-- Values are keyed by `tostring`, so 3 and "3" share a bucket - which is
-- wrong in general and right here, where an attribute has one type and the
-- alternative is a typed key nobody would ever look up.
--------------------------------------------------------------------------

local function ramfs_handlers(state)
  local function unindex(path, attrs)
    for name, value in pairs(attrs or {}) do
      local bucket = state.index[name]

      if bucket then
        local by_value = bucket[tostring(value)]
        if by_value then by_value[path] = nil end
      end
    end
  end

  local function reindex(path, attrs)
    for name, value in pairs(attrs or {}) do
      local bucket = state.index[name]

      if not bucket then
        bucket = {}
        state.index[name] = bucket
      end

      local key = tostring(value)
      local by_value = bucket[key]

      if not by_value then
        by_value = {}
        bucket[key] = by_value
      end

      by_value[path] = true
    end
  end

  --
  -- Everything matching `where`, as a sorted list of paths.
  --
  -- The first term is looked up in the index and the rest filter what came
  -- back, so the cost is the size of the *answer* and not the size of the
  -- filesystem. Which term goes first is whichever `pairs` hands over
  -- first, and that is a real limitation rather than a subtlety: with two
  -- terms of very different selectivity the wrong one first costs more.
  -- Choosing needs per-value counts, and counting is M8's problem.
  --
  local function evaluate(where)
    local first_name, first_value = next(where or {})

    if first_name == nil then
      return {}
    end

    local bucket = state.index[first_name]
    local candidates = bucket and bucket[tostring(first_value)]

    if not candidates then
      return {}
    end

    local out = {}

    for path in pairs(candidates) do
      local node = state.nodes[path]
      local keep = node ~= nil

      if keep then
        for name, value in pairs(where) do
          if tostring(node.attrs[name]) ~= tostring(value) then
            keep = false
            break
          end
        end
      end

      if keep then out[#out + 1] = path end
    end

    table.sort(out)
    return out
  end

  local function same(a, b)
    if #a ~= #b then return false end
    for i = 1, #a do
      if a[i] ~= b[i] then return false end
    end
    return true
  end

  --
  -- Something changed. Every watcher whose answer is now different gets the
  -- reply it has been parked on since it asked.
  --
  -- Re-evaluated rather than worked out incrementally, because a watcher's
  -- predicate can involve attributes the write never touched. Correct
  -- first; the number of watchers is small and the cost of each is the size
  -- of its own answer.
  --
  local function notify()
    local still = {}

    for _, w in ipairs(state.watchers) do
      local now = evaluate(w.where)

      if same(now, w.last) then
        still[#still + 1] = w
      else
        pcall(sys.reply, w.who, { ok = true, paths = now })
      end
    end

    state.watchers = still
  end

  local function find(path, create)
    local node = state.root

    for _, name in ipairs(split(path)) do
      --
      -- A node with no children is either a file or a directory nobody has
      -- put anything in yet, and on the way to creating something the two
      -- are the same case: make the table and carry on. Without this, any
      -- path more than one deep failed to be created - `find` returned nil
      -- and the caller indexed it - which is to say `/data/a/b` was not a
      -- writable path and `/data/b` was.
      --
      if not node.children then
        if not create then return nil end
        node.children = {}
      end

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
      state.nodes[req.path] = node
      -- A size, for the things that have one. A table has a shape rather
      -- than a length, and reporting 1 for it - which this used to - makes
      -- `ls` say "1 bytes" about a record with four fields.
      unindex(req.path, node.attrs)
      node.attrs.size = (type(req.value) == "string") and #req.value or nil
      reindex(req.path, node.attrs)
      state.writes = (state.writes or 0) + 1

      -- After the node is what it is going to be, so a watcher woken here
      -- sees the write it was waiting for and not the state before it.
      notify()
      return { ok = true }
    end,

    getattr = function(req)
      local node = find(req.path)
      if not node then return { ok = false, error = "no such path" } end
      return { ok = true, attrs = node.attrs }
    end,

    setattr = function(req)
      local node = find(req.path, true)
      state.nodes[req.path] = node

      unindex(req.path, node.attrs)
      for k, v in pairs(req.attrs) do node.attrs[k] = v end
      reindex(req.path, node.attrs)

      notify()
      return { ok = true }
    end,

    --
    -- Everything matching, now.
    --
    query = function(req)
      return { ok = true, paths = evaluate(req.where) }
    end,

    --
    -- Everything matching, when it changes.
    --
    -- The caller is blocked in this call until the answer is different from
    -- what it sent as `known`. That is the whole of "live": one outstanding
    -- call, no timer, no repeated question, and the process asking is not
    -- running while it waits.
    --
    -- It is answered immediately if the answer is *already* different, which
    -- is what stops a watcher missing a change that happened between its
    -- last reply and its next call.
    --
    watch = function(req, who)
      local known = req.known or {}
      local now = evaluate(req.where)

      if not same(now, known) then
        return { ok = true, paths = now }
      end

      state.watchers[#state.watchers + 1] = {
        who = who, where = req.where, last = now,
      }

      return DEFER
    end,

    --
    -- How many watchers are parked. For the tests, which otherwise have to
    -- prove a negative about a process that is doing nothing.
    --
    watchers = function(req)
      return { ok = true, value = #state.watchers }
    end,
  }
end

local function ramfs_main(endpoint)
  local state = {
    root = { children = {}, attrs = { kind = "directory" } },
    writes = 0,

    -- Every node that has ever been written or given an attribute, by path.
    -- The tree answers "what is under here"; this answers "what is at this
    -- path" without walking, which is what a query result needs.
    nodes = {},

    -- index[attribute][value] -> set of paths.
    index = {},

    -- Calls that have not been answered yet.
    watchers = {},
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
    --
    -- Mounting over a prefix replaces what was there.
    --
    -- Without this, two mounts at the same path both sit in the table and
    -- which one answers depends on how `table.sort` happens to order two
    -- equal keys - and Lua's sort is not stable, so it can differ between
    -- runs of the same program.
    --
    -- The case that needs it is a terminal: it hands its child a
    -- `/dev/console` of its own, and the child's runner has already mounted
    -- the real one there. The child must get exactly one console and it
    -- must be the terminal's.
    --
    for i, m in ipairs(mounts) do
      if m.prefix == prefix then
        table.remove(mounts, i)
        break
      end
    end

    mounts[#mounts + 1] = { prefix = prefix, cap = capability }

    -- Longest prefix first, so /a/b wins over /a regardless of mount order.
    table.sort(mounts, function(x, y) return #x.prefix > #y.prefix end)
  end

  --
  -- Directories whose children are looked up when they are first used.
  --
  -- `/app` is one: an application registers itself while it runs, so what is
  -- under there changes as programs come and go and cannot be mounted ahead
  -- of time. Asking the registry for a name and mounting what comes back is
  -- how that is done without a global tree - the answer is a capability, and
  -- what a capability is for is being held.
  --
  -- The registry is not asked to *forward*. It hands over the endpoint and
  -- steps out of the way, so a hung application blocks whoever chose to talk
  -- to it and nobody else. A registry that forwarded would be a single
  -- process every application could stop.
  --
  local autos = {}

  local function lookup_into(prefix, cap, path)
    local name = path:sub(#prefix + 2):match("^([^/]+)")

    if not name then return nil end

    local reply, got = sys.call(cap, { type = "lookup", name = name })

    if not reply or not reply.ok or not got or got < 0 then
      return nil
    end

    ns.mount(prefix .. "/" .. name, got)
    return true
  end

  local function match(path)
    for _, m in ipairs(mounts) do
      if path == m.prefix or path:sub(1, #m.prefix + 1) == m.prefix .. "/" then
        local rest = path:sub(#m.prefix + 1)
        return m.cap, (rest == "" and "/" or rest), m.prefix
      end
    end
    return nil
  end

  local function resolve(path)
    --
    -- A registry is asked *before* the answer is taken, not after it fails.
    --
    -- The first version only looked a name up when nothing matched, and
    -- nothing ever failed to match: `/app` is mounted, so `/app/gallery` hit
    -- the registry itself with `/gallery` left over, and the registry was
    -- asked to read a property it has never heard of. The symptom was a
    -- clean, wrong answer - "no such operation: write" from a directory.
    --
    -- So: if the deepest thing matching is a registry and there is more path
    -- after it, look the child up. The mount that produces is longer, so the
    -- ordinary longest-prefix rule picks it from then on and this costs one
    -- exchange the first time and nothing afterwards.
    --
    local cap, rest, prefix = match(path)

    for _, a in ipairs(autos) do
      if prefix == a.prefix and rest ~= "/" then
        if lookup_into(a.prefix, a.cap, path) then
          return match(path)
        end
      end
    end

    if cap then return cap, rest, prefix end

    -- Nothing matched at all. Still worth asking, for a registry mounted
    -- somewhere this path only partly overlaps.
    for _, a in ipairs(autos) do
      if path:sub(1, #a.prefix + 1) == a.prefix .. "/" then
        if lookup_into(a.prefix, a.cap, path) then
          return match(path)
        end
      end
    end

    return nil
  end

  --
  -- Mounts `capability` at `prefix`, and says that names under it are to be
  -- looked up rather than known in advance.
  --
  function ns.mount_registry(prefix, capability)
    ns.mount(prefix, capability)
    autos[#autos + 1] = { prefix = prefix, cap = capability }
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

  --
  -- The names that exist under `path` because something is *mounted* there.
  --
  -- This is the one question no server can answer. A server knows what it
  -- holds; only the namespace knows what has been attached to it and where,
  -- and the mount table lives in this process and nowhere else. Without it
  -- `/` is not a directory at all - there is no server for it, so listing it
  -- returns "no such path" while `/data` and `/dev` both plainly exist.
  --
  -- Only the immediate child: with `/dev/console` mounted, `/` contains
  -- `dev` and not `dev/console`, which is what a directory means.
  local function mounted_under(path)
    local prefix = (path == "/") and "/" or (path .. "/")
    local seen, names = {}, {}

    for _, m in ipairs(mounts) do
      if m.prefix ~= path and m.prefix:sub(1, #prefix) == prefix then
        local child = m.prefix:sub(#prefix + 1):match("^([^/]+)")

        if child and not seen[child] then
          seen[child] = true
          names[#names + 1] = child
        end
      end
    end

    return names
  end

  -- Everything mounted, as a list of prefixes.
  --
  -- The honest answer to "what can this process reach", and a process
  -- asking that is asking about itself: the table is in here and nowhere
  -- else, so nothing outside can answer it.
  function ns.mounts()
    local out = {}
    for _, m in ipairs(mounts) do out[#out + 1] = m.prefix end
    table.sort(out)
    return out
  end

  function ns.list(path)
    local r, e = request("list", path)
    local entries = r and r.entries
    local attached = mounted_under(path)

    -- Whatever the server said, plus whatever is mounted below it. Both are
    -- true: `/dev` holds cpu and memory because the device server says so,
    -- and it holds `console` because something else was attached there.
    if entries then
      local seen = {}
      for _, n in ipairs(entries) do seen[n] = true end
      for _, n in ipairs(attached) do
        if not seen[n] then entries[#entries + 1] = n end
      end
      table.sort(entries)
      return entries
    end

    -- No server for this path. If something is mounted below it, it is a
    -- directory made entirely of mount points - which is exactly what `/`
    -- is - and if nothing is, the error the server gave stands.
    if #attached > 0 then
      table.sort(attached)
      return attached
    end

    return nil, e
  end

  --
  -- A value larger than a message, in pieces.
  --
  -- MSG_BYTES is 2048 and a program is several kilobytes of Lua, so a read
  -- has to be able to span messages. Raising the message size was the other
  -- option and is the wrong one: `struct thread` embeds one, so every thread
  -- would pay for it, and `sys_call` keeps one on a 16 KB exception stack.
  --
  -- A server holding something large answers with `more = true` and honours
  -- `offset`. One that does not ignores the field and returns everything,
  -- which is what every server here did before this existed and still does.
  function ns.read(path)
    local r, e = request("read", path)
    if not r then return nil, e end
    if not r.more then return r.value end

    local parts = { r.value }
    local offset = #r.value

    while true do
      local n, err = request("read", path, { offset = offset })
      if not n then return nil, err end

      parts[#parts + 1] = n.value
      offset = offset + #n.value

      if not n.more then break end
    end

    return table.concat(parts)
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
  --
  -- Was the interrupt key pressed? Only the console answers this, and only
  -- because it is the one process allowed to read the keyboard.
  --
  --
  -- Everything under `path` whose attributes match, now.
  --
  --
  -- Answers come back in the server's own names and go out in this
  -- process's. A server has no idea where it is mounted - it cannot, that
  -- is the point of a namespace - so putting the prefix back on is the
  -- namespace's job, here, and not something every caller repeats.
  --
  local function localise(paths, prefix)
    local out = {}

    for i, p in ipairs(paths or {}) do
      out[i] = (p == "/") and prefix or (prefix .. p)
    end

    return out
  end

  function ns.query(path, where)
    local _, _, prefix = resolve(path)
    local r, e = request("query", path, { where = where })
    if not r then return nil, e end
    return localise(r.paths, prefix)
  end

  --
  -- The same question, answered when the answer changes.
  --
  -- This blocks. That is the feature: the process asking is not running, not
  -- polling and not on a timer, and it wakes when something it cares about
  -- happened. Pass the answer you already have as `known`.
  --
  function ns.watch(path, where, known)
    local capability, rest, prefix = resolve(path)

    if not capability then
      return nil, "no such path: " .. path
    end

    -- `known` goes back in the server's names, or it never matches what the
    -- server computed and every watch returns at once.
    local server_known = {}

    for i, p in ipairs(known or {}) do
      server_known[i] = (p:sub(1, #prefix) == prefix)
                        and (p:sub(#prefix + 1)) or p
      if server_known[i] == "" then server_known[i] = "/" end
    end

    local r, e = request("watch", path, { where = where, known = server_known })
    if not r then return nil, e end
    return localise(r.paths, prefix)
  end

  --
  -- Attributes: what a node is, as opposed to what is in it.
  --
  function ns.setattr(path, attrs)
    local r, e = request("setattr", path, { attrs = attrs })
    if not r then return nil, e end
    return true
  end

  --
  -- A message to whatever is mounted at `path`, and its answer.
  --
  -- Every operation above is this with a fixed verb. This is the one for a
  -- server whose vocabulary the namespace has never heard of - the window
  -- manager, say, which speaks of windows and damage and not of files.
  --
  -- It is not a hole in anything. A namespace maps names onto endpoints,
  -- and sending to an endpoint is what an endpoint is for; the authority is
  -- still exactly the mount table, and a path that is not in it is still a
  -- path that does not exist.
  --
  -- `pass` is a capability of this process's to hand over with the message,
  -- which is what registering with a directory is: giving it a way to reach
  -- you. It goes as a third argument rather than inside the table, because
  -- an index means something different on each side and only the kernel can
  -- translate it.
  function ns.send(path, message, pass)
    local capability, rest = resolve(path)

    if not capability then
      return nil, "no such path: " .. path
    end

    local req = { path = rest }
    for k, v in pairs(message) do req[k] = v end

    local reply, err = sys.call(capability, req, pass)
    if not reply then return nil, err end
    if not reply.ok then return nil, reply.error end
    return reply
  end

  --
  -- Every key typed since the last call. Only the console can answer this,
  -- and only a program that has taken over the screen should be asking.
  --
  function ns.keys(path)
    local r, e = request("keys", path)
    if not r then return nil, e end
    return r.value or {}
  end

  --
  -- Where the pointer is, if the machine has one.
  --
  --
  -- Keys and the pointer in one exchange, sleeping if there is nothing.
  --
  function ns.wait_input(path, ticks)
    local r, e = request("wait", path, { ticks = ticks })
    if not r then return nil, e end
    return r.value
  end

  function ns.pointer(path)
    local r, e = request("pointer", path)
    if not r then return nil, e end
    return r.value
  end

  function ns.interrupted(path)
    local r, e = request("poll", path)
    if not r then return nil, e end
    return r.value and true or false
  end

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
-- It serves three operations at one path. `write` puts a string; `read`
-- waits for a line, echoing as it goes, which is where the line editing
-- lives. A client that wants a line asks for one and blocks until there is
-- one, and that blocking is free: synchronous IPC already parks the caller.
--
-- `poll` is the odd one, and it is here because this is the only process
-- that may call `sys.getchar`. A program that runs for a while - a status
-- bar, a benchmark - has no other way to find out that Control-C was
-- pressed, because the keyboard is not its to read. So it asks.
--
-- Anything `poll` takes off the keyboard that is not the interrupt is kept,
-- not dropped: typing while a program runs and losing the characters when
-- it ends would be worse than not polling at all.
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
      --
      -- Only one reader at a time. `pump` below can deliver another `read`
      -- while this one is waiting, and two handlers both consuming keys
      -- would split the line between them. Saying no is right rather than
      -- merely safe: there is one keyboard, and a second client asking for
      -- a line is a bug in that client.
      --
      if state.reading then
        return { ok = false, error = "the console already has a reader" }
      end

      state.reading = true
      local buf = {}

      -- Whatever `poll` took off the keyboard while a program was running,
      -- in the order it arrived, before anything new.
      local function next_byte()
        if #state.typed > 0 then
          return table.remove(state.typed, 1)
        end
        return sys.getchar()
      end

      while true do
        local c = next_byte()

        if c == nil then
          -- Nothing waiting. Serve whoever else has asked for something -
          -- this is the handler that would otherwise make the whole server
          -- unavailable for as long as nobody types - and then yield.
          -- Yielding rather than spinning costs a scheduling slot instead of
          -- the machine; there is no UART interrupt to park on yet.
          state.pump()
          sys.yield()
        elseif c == 10 or c == 13 then
          sys.write("\n")
          state.lines = state.lines + 1
          state.reading = false
          return { ok = true, value = table.concat(buf) }
        elseif c == 3 then
          -- Control-C at the prompt abandons the line, as it does in every
          -- other shell. An empty line back, which the shell already knows
          -- how to do nothing with.
          sys.write("^C\n")
          state.interrupts = state.interrupts + 1
          state.reading = false
          return { ok = true, value = "" }
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

    keys = function(req)
      --
      -- Every byte typed since the last time somebody asked, as an array.
      --
      -- `poll` above answers a yes-or-no question and keeps what it saw for
      -- the line editor. This one is for a program that *is* the line
      -- editor for as long as it runs - the window manager - and wants the
      -- keys themselves. The stash goes with them, or a character typed
      -- just before it started would be delivered to the shell afterwards.
      --
      local out = state.typed
      state.typed = {}

      while true do
        local c = sys.getchar()
        if c == nil then break end
        if c == 3 then state.interrupts = state.interrupts + 1 end
        out[#out + 1] = c
      end

      return { ok = true, value = out }
    end,

    wait = function(req)
      --
      -- Everything the desktop needs, and a sleep if there is nothing.
      --
      -- One call rather than three, and blocking rather than polling. The
      -- window manager used to ask for keys, ask for the pointer, get
      -- nothing, yield, and go round again - which is a thread that is
      -- always runnable, which is a core at a hundred per cent for ever on
      -- an idle desktop. It sleeps in here now, and an input interrupt cuts
      -- the sleep short, so a key is noticed at interrupt speed and an idle
      -- machine is idle.
      --
      -- This process is the only one the kernel will let sleep on input,
      -- for the same reason it is the only one that may read it: input has
      -- one reader.
      --
      -- The cost is that the console is not answering anyone else while it
      -- is asleep. That is bounded by whatever the caller asked for - one
      -- tick, in practice - and it is why this is a separate operation
      -- rather than something `keys` started doing.
      --
      local keys = state.typed
      state.typed = {}

      while true do
        local c = sys.getchar()
        if c == nil then break end
        if c == 3 then state.interrupts = state.interrupts + 1 end
        keys[#keys + 1] = c
      end

      local where = sys.pointer()

      if #keys == 0 then
        sys.wait_input(tonumber(req.ticks) or 0)
      end

      return { ok = true, value = { keys = keys, pointer = where } }
    end,

    pointer = function(req)
      --
      -- Where the pointer is. Here for the same reason `keys` is: this is
      -- the only process the kernel will answer about input at all, because
      -- input has one reader and two pollers would each take events the
      -- other never sees.
      --
      -- Passed through untouched, range and all. The console has no more
      -- idea how big the screen is than the kernel does; the window manager
      -- does, and scaling is its business.
      --
      local where, err = sys.pointer()

      if not where then
        return { ok = false, error = tostring(err) }
      end

      return { ok = true, value = where }
    end,

    poll = function(req)
      --
      -- Was Control-C pressed? Everything else typed is kept for `read`.
      --
      -- This drains rather than reading one byte, because the answer has to
      -- be about everything typed since the last question. Reading one byte
      -- a second would find the interrupt a second late for every character
      -- typed ahead of it.
      --
      local seen = false

      while true do
        local c = sys.getchar()

        if c == nil then
          break
        elseif c == 3 then
          seen = true
          state.interrupts = state.interrupts + 1
        else
          state.typed[#state.typed + 1] = c
        end
      end

      return { ok = true, value = seen }
    end,

    stat = function(req)
      -- What it has done, out of the state table. It survives a reload,
      -- which is how you can see that the state and the code are separate
      -- things.
      return { ok = true, value = {
        bytes = state.bytes, lines = state.lines,
        interrupts = state.interrupts,
        reloads = state.reloads or 0,
      } }
    end,
  }
end

local function console_main(endpoint)
  serve(endpoint, { bytes = 0, lines = 0, interrupts = 0, typed = {},
          reading = false },
        console_handlers)
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

--------------------------------------------------------------------------
-- The devices server: /dev.
--
-- Every device the machine was found to have, reachable the way everything
-- else is - by name, through a namespace, over the same list/read protocol
-- the filesystem uses. `fs.list("/dev")` is not a special command; it is the
-- same request the ramfs answers, sent somewhere else.
--
-- It is the only thing that calls `sys.info()`, exactly as the console
-- server is the only thing that calls `sys.write`. The difference is that
-- nothing is lost if another process calls it: an inventory is not
-- authority.
--
-- Read fresh on every request rather than cached at startup, because half of
-- it is live: threads and processes and free pages change, and a /dev that
-- answered with the numbers from boot would be worse than useless.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- /bin: the programs this image carries.
--
-- Read-only, and that is not a limitation being apologised for. The
-- programs are compiled into the image because there is no disk until M8,
-- so a write that appeared to work would vanish at the next boot - which is
-- worse than being told no.
--
-- It is an ordinary server answering the ordinary protocol. `ls /bin` and
-- `cat /bin/htop.lua` are the same requests the filesystem answers, sent
-- somewhere else, and nothing in the shell knows /bin is special.
--------------------------------------------------------------------------

local function binfs_handlers(state)
  return {
    list = function(req)
      local names = {}
      for name in pairs(state.programs) do names[#names + 1] = name end
      table.sort(names)
      return { ok = true, entries = names }
    end,

    read = function(req)
      local name = req.path:match("([^/]+)$")
      local source = name and state.programs[name]

      if not source then
        return { ok = false, error = "no such program" }
      end

      -- A thousand bytes at a time, which leaves room in a 2048-byte
      -- message for the tag, the keys and the serialiser's framing. A
      -- program is a few kilobytes, so this is a handful of round trips
      -- once, when it is launched.
      local CHUNK = 1024
      local offset = req.offset or 0
      local piece = source:sub(offset + 1, offset + CHUNK)

      return { ok = true, value = piece,
               more = (offset + #piece) < #source }
    end,

    getattr = function(req)
      local name = req.path:match("([^/]+)$")
      local source = name and state.programs[name]

      if not source then return { ok = false, error = "no such program" } end

      --
      -- Whether it draws.
      --
      -- A program declares it, on its first line, with `-- kosmos:
      -- application`. Guessing instead - looking for `ui.window` in the
      -- source, say - would be a store deciding what a program is by
      -- reading it, and would be wrong the first time somebody wrote the
      -- name in a comment.
      --
      -- Worked out when the store loads, not per request: it is a property
      -- of source that cannot change while the system runs, because /bin is
      -- in the image.
      --
      return { ok = true, attrs = {
        size = #source,
        kind = state.windowed[name] and "application" or "program",
      } }
    end,

    write = function(req)
      return { ok = false,
               error = "this is in the image and cannot be written" }
    end,
  }
end

--
-- One server, two stores. `/bin` and `/lib` differ only in which table of
-- source they were handed - the protocol, the chunking and the refusal to
-- be written are identical, because "read-only source carried in the image"
-- is the same thing in both cases.
--
local function binfs_main(endpoint, source, what)
  -- Loaded once, here, rather than on every request. A syntax error in a
  -- program is a syntax error in this chunk and shows up at boot, which is
  -- when somebody can do something about it.
  local chunk, err = load(source, "=" .. what, "t")
  if not chunk then error(what .. ": " .. tostring(err)) end

  local programs = chunk()
  local windowed = {}

  for name, source in pairs(programs) do
    if source:match("^[^\n]*kosmos:%s*application") then
      windowed[name] = true
    end
  end

  serve(endpoint, { programs = programs, windowed = windowed },
        binfs_handlers)
end

--------------------------------------------------------------------------
-- /app: the registry of what is running and what it exposes.
--
-- `beos.md` 17.2 and roadmap M7's scripting architecture. Every application
-- publishes its own properties as nodes in its own namespace, and this is
-- the directory that says which name belongs to which endpoint. So from the
-- shell:
--
--   apps                            what is running
--   cat /app/gallery/title          read a property
--   write /app/gallery/title hi     change one
--
-- and the application in question contains no scripting code at all. It
-- called `ui.window`, and `ui.window` publishes the window's properties the
-- way it publishes anything else. That is the whole point: in BeOS an
-- application was scriptable because its author used the framework, not
-- because they wrote support for it.
--
-- **This registry hands out capabilities; it does not forward.** `lookup`
-- returns the application's endpoint and the caller mounts it, so talking to
-- a slow application is between the caller and that application. A registry
-- that forwarded would be one process that any application could stop, and
-- it would be holding every other application's door.
--------------------------------------------------------------------------

local function appfs_handlers(state)
  return {
    register = function(req, who, cap)
      if not cap or cap < 0 then
        return { ok = false, error = "register: no endpoint came with that" }
      end

      local name = tostring(req.name or "?")

      -- A second application of the same name gets a number, rather than
      -- replacing the first: two clocks are two clocks.
      if state.apps[name] then
        local n = 2
        while state.apps[name .. n] do n = n + 1 end
        name = name .. n
      end

      state.apps[name] = cap
      state.order[#state.order + 1] = name

      return { ok = true, name = name }
    end,

    lookup = function(req)
      local cap = state.apps[tostring(req.name or "")]

      if not cap then
        return { ok = false, error = "no such application" }
      end

      -- The endpoint itself, for the caller to mount and hold.
      return { ok = true, send_cap = cap }
    end,

    list = function(req)
      local names = {}

      for _, name in ipairs(state.order) do
        if state.apps[name] then names[#names + 1] = name end
      end

      table.sort(names)
      return { ok = true, entries = names }
    end,

    unregister = function(req)
      local name = tostring(req.name or "")

      if state.apps[name] then
        sys.destroy(state.apps[name])
        state.apps[name] = nil
      end

      return { ok = true }
    end,
  }
end

local function appfs_main(endpoint)
  serve(endpoint, { apps = {}, order = {} }, appfs_handlers)
end

local function devices_handlers(state)
  local function inventory()
    local m = describe_machine()
    if not m then return nil end

    -- The order is the order `list` returns, so a listing is reproducible.
    --
    -- **`console` is not in it, and that is the point.** The machine has one
    -- and this server knows about it, but `/dev/console` is mounted to the
    -- console *server* - longest prefix wins - so a read of that path goes
    -- somewhere else entirely and means "give me a line of input". Listing a
    -- name this server does not answer for would be a lie, and an expensive
    -- one: the first version listed it, the `devices` command dutifully read
    -- every name it was given, and the console server answered by swallowing
    -- the next thing typed at the prompt.
    return m, { "cpu", "memory", "kernel", "timer", "screen", "keyboard" }
  end

  return {
    list = function(req)
      local m, order = inventory()
      if not m then return { ok = false, error = "the kernel would not say" } end

      local names = {}
      for _, name in ipairs(order) do
        if m[name] then names[#names + 1] = name end
      end

      state.lists = (state.lists or 0) + 1
      return { ok = true, entries = names }
    end,

    read = function(req)
      local m = inventory()
      if not m then return { ok = false, error = "the kernel would not say" } end

      -- "/cpu" and "cpu" and "/dev/cpu" all mean the same node. The mount
      -- prefix is stripped before it gets here; the leading slash is not.
      local name = req.path:match("([^/]+)$")
      local node = name and m[name]

      if not node then
        return { ok = false, error = "no such device" }
      end

      return { ok = true, value = node }
    end,

    getattr = function(req)
      return { ok = true, attrs = { kind = "device" } }
    end,
  }
end

--------------------------------------------------------------------------
-- The disk server.
--
-- The only process holding SPAWN_DISK. Everything that wants the block
-- device comes through here, which is what makes "raw sectors" an authority
-- somebody was given rather than something any program can reach.
--
-- It serves two names and no more, because at this milestone there are only
-- two questions:
--
--   /disk/super    read: what the superblock says, or why there is none
--   /disk/format   write: format it, erasing everything
--
-- Both are the ordinary protocol - `read` and `write` over typed records -
-- rather than operations invented for the occasion. When the real
-- filesystem lands it answers `list`, `read` and `write` for actual paths
-- and these two stay as what they are: a way to look at, and to lay down,
-- the structure underneath.
--
-- Block *contents* deliberately do not travel through here. A message is
-- 2048 bytes and a block is 4096, and the answer to "how do I move a
-- megabyte" is never "in smaller messages" - design.md 8.4 has that as
-- mapped pages. What crosses this boundary is small tables.
--------------------------------------------------------------------------

local function diskfs_handlers(state)
  local kfs = state.kfs

  return {
    list = function(req)
      return { ok = true, entries = { "super", "format" } }
    end,

    read = function(req)
      local name = req.path:match("([^/]+)$")

      if name == "super" then
        local disk, why = sys.disk()

        if not disk then
          return { ok = false, error = tostring(why) }
        end

        local sb, err = kfs.mount()

        if not sb then
          -- Not an error. A disk with no filesystem on it is a normal
          -- state and the thing `mkfs` exists to change; reporting it as a
          -- failure would make "there is nothing here yet" look like a
          -- fault.
          return { ok = true, value = { sectors = disk.sectors,
                                        sector_size = disk.sector_size,
                                        bytes = disk.bytes,
                                        formatted = false,
                                        why = tostring(err) } }
        end

        sb.sectors     = disk.sectors
        sb.sector_size = disk.sector_size
        sb.bytes       = disk.bytes
        sb.formatted   = true
        sb.free_blocks = sb.blocks - sb.data_at

        return { ok = true, value = sb }
      end

      return { ok = false, error = "no such name" }
    end,

    write = function(req)
      local name = req.path:match("([^/]+)$")

      if name ~= "format" then
        return { ok = false, error = "no such name" }
      end

      -- The confirmation is checked here rather than only in `mkfs`,
      -- because this is the boundary. A program that reaches this path is
      -- asking to erase the disk, and "it asked nicely" has to be part of
      -- the request rather than a habit of one caller.
      if req.value ~= "yes, erase it" then
        return { ok = false,
                 error = "a format must say `yes, erase it`" }
      end

      local disk, why = sys.disk()

      if not disk then
        return { ok = false, error = tostring(why) }
      end

      local sb, err = kfs.mkfs(disk.sectors, sys.ticks())

      if not sb then
        return { ok = false, error = tostring(err) }
      end

      state.formats = (state.formats or 0) + 1
      return { ok = true, value = sb }
    end,

    getattr = function(req)
      return { ok = true, attrs = { kind = "device" } }
    end,
  }
end

local function diskfs_main(endpoint)
  local kfs = assert(load(sys.libraries(), "libraries"))()["kfs.lua"]

  serve(endpoint, { kfs = assert(load(kfs, "kfs.lua"))() }, diskfs_handlers)
end

local function devices_main(endpoint)
  serve(endpoint, {}, devices_handlers)
end

local RUNNER_ROLE = ROLE_RUNNER

local function shell_main(console_cap, ramfs_cap, devices_cap, bin_cap,
                          lib_cap, app_cap, disk_cap)
  local ns = new_namespace()
  ns.mount("/dev/console", console_cap)
  ns.mount("/data", ramfs_cap)

  -- Longest prefix wins, so /dev/console keeps going to the console server
  -- while everything else under /dev goes to the device server. Two servers
  -- under one directory, and neither knows about the other - which is what a
  -- per-process mount table buys.
  ns.mount("/dev", devices_cap)

  -- The programs this image carries. Read-only, and served by a process of
  -- its own like everything else.
  ns.mount("/bin", bin_cap)

  -- And what programs load rather than run. Separate from /bin so that `ls
  -- /bin` lists things you can type and nothing else.
  ns.mount("/lib", lib_cap)

  -- What is running, and what each one exposes. A registry rather than a
  -- mount: the names under it appear and disappear with the programs.
  ns.mount_registry("/app", app_cap)

  -- The block device, through the one process that holds it. What is under
  -- here is structure - a superblock, and the way to lay one down - not
  -- files; files arrive when there is a filesystem to serve them.
  ns.mount("/disk", disk_cap)

  local function out(s) write_text(ns, "/dev/console", s) end
  local function readline() return ns.read("/dev/console") end

  --------------------------------------------------------------------------
  -- help
  --
  -- A table rather than a function, with __tostring and __call, so that both
  -- `help` and `help("gfx")` do something sensible. The shell prints the
  -- result of what you type through tostring, so a bare `help` renders the
  -- overview without the parentheses that every newcomer forgets.
  --------------------------------------------------------------------------
  local topics = {}

  topics.overview = [=[
Kosmos - a microkernel with a Lua userland.

You are typing at a *process*. What you type is read by the console
server, sent to the shell over IPC, evaluated in the shell's own
lua_State, and the answer comes back the same way. Three processes,
two address spaces, to print one number.

  2 + 2
  ("hello"):upper()

What the shell can reach is exactly what it was handed - there is no
global anything. Try `sys.write("direct")`: it returns -102, because
the shell does not own the console and has to ask.

The prompt takes commands as well as Lua. `/commands` lists them.

  devices              a command
  /devices             the same command, said explicitly
  fs.list("/dev")      the same thing, as a program

**A leading slash always means a command.** Without one, a bare word is
only treated as a command when it does not also name something in Lua -
so `devices` works, and if you ever alias `print` or `type` you will have
to say `/print`. A shell where `type` sometimes means a command and
sometimes means the function is a shell you cannot write anything in.

  ls /bin        the programs this image carries
  run <name>     run one, in a process of its own (a bare name works too)

  help fs        files, through this process's namespace
  help gfx       surfaces, the screen, and text
  help sys       what a process can ask the kernel for
  help dev       what hardware was found, and the status bar
  help demos     things worth typing
]=]

  topics.fs = [=[
fs - this process's namespace, not a global filesystem.

At the prompt:

  ls [path]      a program in /bin; pwd and cd are the shell's own
  cat <path>     a program in /bin: reads one thing and prints it
  /commands      everything the shell answers to

The working directory lives in the shell and nowhere else. A server is
always told a whole path and knows nothing about where you think you
are - which is what keeps `fs.read` the same operation for everybody.

`fs` is a mount table living in the shell. A path that matches no
mount does not exist; that is not a permission check, there is simply
nothing there to deny.

  fs.list("/data")                     -> a table of names
  fs.read("/data/sensor")              -> whatever was written
  fs.write("/data/x", { n = 1 })       -> true
  fs.getattr("/data/x")                -> { size = ... }
  fs.read("/nowhere")                  -> nil, "no such path: /nowhere"

Values are Lua values, not bytes. A read gives you back the table you
wrote, integers still integers and floats still floats.

  fs.reload(path, source)   replaces a running server's code

See help("demos") for a hot reload you can watch happen.
]=]

  topics.gfx = [=[
gfx - surfaces, and the only place a pixel offset is computed.

  gfx.screen()             the framebuffer, as a surface
  gfx.surface{ w=, h= }    an offscreen one, from this process's heap
  gfx.font                 { w = 8, h = 16 }, the bitmap font's cell

On a surface:

  s:size()                        -> width, height
  s:fill(x, y, w, h, colour)
  s:span(x, y, len, colour)
  s:text(x, y, string, fg [, bg]) -> the x the next character starts at
  s:blit(src, sx, sy, w, h, dx, dy)
  s:blend(src, sx, sy, w, h, dx, dy [, alpha])
  s:get(x, y) / s:set(x, y, colour)
  s:free()

Colours are 0xAARRGGBB. Everything clips rather than complaining, so
drawing off the edge is fine. Every pixel loop runs in C - Lua decides
what to draw and where, and never computes an address.

Surfaces come from the kernel, not from this process's 2 MB heap: a
full-screen one is 3.2 MB and the heap is small on purpose, so that
collections stay short. `ps` counts them under `mapped`.
]=]

  topics.sys = [=[
sys - the syscalls, all twelve of them.

  sys.ticks()              the monotonic counter; time things with it
  sys.write(s)             refused here: the shell does not own the console
  sys.pack(v)/unpack(s)    a Lua value as bytes, and back
  sys.spawn(role, caps)    another process from this same image
  sys.wait()               -> id, exit code
  sys.endpoint()           a new IPC endpoint
  sys.call(cap, table)     send and wait for the reply
  sys.receive/reply        the other side of it
  sys.yield(), sys.exit(n)

A capability is an index into this process's own table. There are no
global names: you cannot reach what you were not handed, and you
cannot guess a number to get it.

  sys.call(99, {})         -> nil, "no such capability"
]=]

  topics.dev = [=[
What this machine is, and what was found on it.

  devices        every device, one line each
  cpu            the processor, decoded from its own ID registers
  mem            RAM, and how much of it the kernel has
  ps             threads, processes and endpoints, used of total

All four read /dev, which is a *server* reached through the namespace -
the same list/read protocol the filesystem answers, sent somewhere else.
Nothing here is a special case in the shell:

  fs.list("/dev")
  fs.read("/dev/cpu").part
  fs.read("/dev/memory").free_mb

The kernel decodes none of it. `sys.info()` hands back raw ID registers
and pool counts, and the tables that turn 0x410fd083 into "Cortex-A72"
live up here in Lua - so a processor the kernel has never heard of gets
described properly without the kernel changing.

The status bar:

  monitor        draw it once, along the bottom of the screen
  monitor on     redraw it after every command
  monitor watch  keep redrawing for a while (blocks the prompt)
  monitor off

It draws in the rows the kernel console reserves for its boot progress
bar and never scrolls text through. Two writers on one framebuffer with
no compositor, which works only because the regions cannot overlap - and
is exactly the arrangement a compositor exists to stop needing.

Aliases:

  alias                list every alias
  aliases              the same thing, under the name people try first
  alias ll devices     make one
  alias m=monitor      either spelling works

**And a command can be a Lua program.** `alias` points one word at
another; `def` compiles a line of Lua and gives it a name, so anything
you can type here can become a command:

  def hot = local d = fs.read("/data/sensor")
            return d.celsius > 40 and "hot" or "cold"
  /hot

The argument string arrives as `...`, so a program can take one:

  def count = local n = 0
              for _ in ipairs(fs.list(...)) do n = n + 1 end
              return n .. " under " .. ...
  /count /dev

It is compiled when you define it, so a syntax error is reported then
rather than the first time somebody runs it, and it is compiled into the
same environment as the prompt - so it reaches exactly what you reach.
Definitions live in the shell's memory and go when it does.
]=]

  topics.demos = [=[
Things worth typing.

Draw on the screen:

  local s = gfx.screen() s:fill(80, 300, 400, 200, 0xff1f6feb)
  local s = gfx.screen() s:text(80, 520, "hello", 0xff7ee787)

A gradient, 256 fills, each a C pixel loop:

  local s = gfx.screen() for i=0,255 do s:fill(80+i*3, 560, 3, 80, 0xff000000 + i*0x010101) end

Make the machine busy and watch it:

  monitor on         a status bar along the bottom of the screen
  benchmark 4        four processes spinning for ten seconds (or `spin 4`)
  monitor watch      redraw it live while they run

`benchmark` spawns processes that deliberately do not yield, so the scheduler
has to preempt them - which is what makes the meter read what a real
workload would rather than what a polite one does.

Time something, in counter ticks:

  local a = sys.ticks() for i=1,200000 do end return sys.ticks() - a

The serialiser, which is how every message travels:

  #sys.pack({ hello = "world", n = 7 })
  sys.unpack(sys.pack({ deep = { "a", "b" } })).deep[2]
  sys.pack(print)            -> refused: a function cannot cross

Replace a running server's code, while it is holding your files:

  fs.write("/data/a", "one")
  fs.reload("/data", "return function(s) return { read = function(r) return { ok = true, value = 'REPLACED, writes so far: ' .. tostring(s.writes) } end } end")
  fs.read("/data/a")

The counter in that answer was incremented by the code you just
deleted: the behaviour changed and the state survived. Reboot to get
your filesystem back.
]=]

  --------------------------------------------------------------------------
  -- The status bar.
  --
  -- Drawn into the rows at the bottom of the screen that the kernel console
  -- reserves for its progress bar and never scrolls text through - see
  -- RESERVED_ROWS in kernel/console.c. Two writers on one framebuffer with
  -- no compositor works here only because the regions are disjoint by
  -- construction, and that is exactly the arrangement a compositor exists to
  -- stop needing. It is honest for a status line and would not be for
  -- anything that moved.
  --------------------------------------------------------------------------
  -- Matches RESERVED_ROWS in kernel/console.c: the rows at the bottom that
  -- text never scrolls through. Two, so the bar is a single line of text
  -- with a rule above it rather than a band.
  local RESERVED_ROWS = 2

  --
  -- Usage is the difference between two readings, never one.
  --
  -- The kernel counts ticks charged to the idle thread and ticks charged to
  -- everything else, both only rising. A single reading says what fraction
  -- of *all time since boot* was busy, which after a minute of sitting at a
  -- prompt is a number that never moves again. Two readings say what has
  -- happened since the last look, which is the question actually being
  -- asked.
  local last_idle, last_busy = nil, nil

  local function cpu_usage(k)
    local idle, busy = k.idle_ticks, k.busy_ticks
    local pct

    if last_idle then
      local di, db = idle - last_idle, busy - last_busy
      if di + db > 0 then pct = (db * 100) // (di + db) end
    end

    last_idle, last_busy = idle, busy
    return pct
  end

  local function bar(pct, width)
    -- A meter that is readable at a glance and needs no glyphs the font
    -- might not have.
    local filled = (pct * width) // 100
    return "[" .. string.rep("#", filled) .. string.rep(".", width - filled) .. "]"
  end

  --
  -- Collects the processes this shell spawned and has finished with.
  --
  -- A process that exits keeps its slot until somebody waits for it: that is
  -- what makes an exit code readable afterwards. init reaps its own
  -- children; nobody was reaping the shell's, so spawning from the prompt
  -- filled the pool with slots that `ps` did not even count.
  --
  -- Non-blocking, which is why SYS_WAIT grew a flag: a blocking drain at the
  -- prompt would sit there for as long as a detached program takes.
  local function reap()
    for _ = 1, 32 do
      local id = sys.wait(true)
      if not id then return end
    end
  end

  --------------------------------------------------------------------------
  -- Commands.
  --
  -- The prompt is a Lua REPL and stays one; this is a layer in front of it so
  -- that the common things are words rather than programs. A line is treated
  -- as a command when its first word names one **and the rest contains no
  -- Lua punctuation** - so `help` and `help gfx` are commands while
  -- `help("gfx")` is an expression, and both work.
  --
  -- Aliases are a table from word to word, which is all an alias needs to be.
  --------------------------------------------------------------------------
  local commands = {}
  local aliases = {}

  -- The words Lua will not let you use as a name, plus everything already
  -- in scope. A command whose name collides with either is reachable only
  -- as `/name`; see the dispatcher below for why.
  local KEYWORDS = {
    ["and"]=true, ["break"]=true, ["do"]=true, ["else"]=true, ["elseif"]=true,
    ["end"]=true, ["false"]=true, ["for"]=true, ["function"]=true,
    ["goto"]=true, ["if"]=true, ["in"]=true, ["local"]=true, ["nil"]=true,
    ["not"]=true, ["or"]=true, ["repeat"]=true, ["return"]=true,
    ["then"]=true, ["true"]=true, ["until"]=true, ["while"]=true,
  }

  local env

  local function shadows_lua(word)
    return KEYWORDS[word] or (env ~= nil and env[word] ~= nil)
  end

  local function fmt_bytes(pages, size)
    return string.format("%d MB", pages * size // (1024 * 1024))
  end

  --------------------------------------------------------------------------
  -- A working directory, and the commands that use one.
  --
  -- It lives in the shell, not in the kernel and not in a server, because it
  -- is the shell's idea: a convenience for a person typing. A server is told
  -- a whole path, always, and knows nothing about where anybody thinks they
  -- are. That is what keeps `fs.read` the same operation whoever calls it.
  --------------------------------------------------------------------------
  local cwd = "/"

  local function resolve(path)
    if path == nil or path == "" then return cwd end
    if path:sub(1, 1) == "/" then return path end
    if cwd == "/" then return "/" .. path end
    return cwd .. "/" .. path
  end

  -- A value, printed so a person can read it. Tables are what servers
  -- return, so this has to handle them rather than saying "table: 0x...".
  local function show(value, indent)
    indent = indent or ""

    if type(value) ~= "table" then
      return tostring(value)
    end

    local keys = {}
    for k in pairs(value) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)

    local parts = {}
    for _, k in ipairs(keys) do
      parts[#parts + 1] = string.format("%s  %s = %s", indent, tostring(k),
                                        show(value[k], indent .. "  "))
    end

    return "{\n" .. table.concat(parts, "\n") .. "\n" .. indent .. "}"
  end

  commands.pwd = function()
    out(cwd .. "\n")
  end

  commands.cd = function(arg)
    local target = resolve(arg ~= "" and arg or "/")

    -- Checked by asking. There is no directory object to look up: a path is
    -- a directory exactly when whoever serves it will list it, which is the
    -- only definition that means anything across three different servers.
    local entries, err = ns.list(target)

    if not entries then
      out("cd: " .. target .. ": " .. tostring(err) .. "\n")
      return
    end

    cwd = target
    out(cwd .. "\n")
  end

  --------------------------------------------------------------------------
  -- Commands that are Lua programs.
  --
  -- `alias` points one word at another. `def` is the more useful half: it
  -- compiles a line of Lua into a command, so anything you can write at this
  -- prompt can be given a name and a place in `/commands`.
  --
  --   def ls2 = for _, n in ipairs(fs.list(...)) do print(n) end
  --   /ls2 /data
  --
  -- The argument string arrives as `...`, so a program can take one. It is
  -- compiled once, when defined, so a syntax error is reported then rather
  -- than the first time somebody runs it - and it is compiled into the same
  -- environment the prompt uses, so it can reach exactly what you can.
  --------------------------------------------------------------------------
  commands.def = function(arg)
    local name, source = arg:match("^([%w_%-]+)%s*=%s*(.+)$")

    if not name then
      name, source = arg:match("^([%w_%-]+)%s+(.+)$")
    end

    if not name or not source then
      out("usage: def <name> <lua>   or   def <name> = <lua>\n")
      out("the argument string arrives as ...\n")
      return
    end

    -- No preamble. A chunk loaded by `load` is already a vararg function, so
    -- `...` inside the source is the argument string with nothing added -
    -- the first version wrote `local ... = ...` in front, which is not legal
    -- Lua at all and failed at definition time for every program.
    local chunk, err = load(source, "=" .. name, "t", env)

    if not chunk then
      out("def: " .. tostring(err) .. "\n")
      return
    end

    commands[name] = function(rest)
      local results = table.pack(pcall(chunk, rest))

      if not results[1] then
        out("error: " .. tostring(results[2]) .. "\n")
        return
      end

      for i = 2, results.n do
        out(tostring(results[i]) .. (i < results.n and "\t" or "\n"))
      end
    end

    out(name .. " defined; run it as /" .. name .. "\n")
  end

  -- A trailing `&` detaches, as it does in every other shell: the program
  -- is started rather than run, and the prompt comes straight back. That is
  -- what a status bar wants and what a `cat` never does.
  local function split_detach(argument)
    local without = argument:match("^(.-)%s*&%s*$")
    if without then return without, true end
    return argument, false
  end

  --------------------------------------------------------------------------
  -- Running a program.
  --
  -- The shell reads nothing: it asks whether the program exists, spawns a
  -- process, and tells it the path. That process has /bin too, and fetching
  -- the source is its business - so the bytes cross the boundary once.
  --
  -- This is what exec looks like with no ambient authority. No path search,
  -- no inherited environment, no global tree: the program gets exactly the
  -- capabilities named here and can pass on no more than it holds.
  --------------------------------------------------------------------------
  local function run_program(name, argument, detach)
    local path = name:sub(1, 1) == "/" and name or ("/bin/" .. name .. ".lua")

    -- Asked about rather than read. The shell does not need the program;
    -- the process that will run it does.
    local attrs, err = ns.getattr(path)

    if not attrs then
      return false, path .. ": " .. tostring(err)
    end

    local ep = sys.endpoint()
    if not ep then return false, "no endpoint for the program" end

    local id = sys.spawn(RUNNER_ROLE, { ep, console_cap, ramfs_cap,
                                        bin_cap, devices_cap, lib_cap,
                                        app_cap, disk_cap },
                         SPAWN_SCREEN)

    if not id then
      sys.destroy(ep)
      return false, "could not start a process for it"
    end

    local reply = sys.call(ep, {
      path = path, args = argument or "", cwd = cwd,
      detach = detach and true or false,
      console = 1, data = 2, bin = 3, devices = 4, lib = 5, app = 6,
      disk = 7,
    })

    -- A private channel for one message. There are ninety-six of them, and
    -- leaving them behind is how a pool runs out for reasons nobody sees.
    sys.destroy(ep)

    -- A detached program is still running; waiting for it would be exactly
    -- what detaching was for. The next command's reap collects it.
    if not detach then sys.wait() end

    if not reply then return false, "the program did not answer" end
    if not reply.ok then return false, reply.error end

    return true
  end

  commands.run = function(arg)
    if arg == "" then
      out("usage: run <program> [arguments]\n")
      out("`ls /bin` lists them. A bare program name works too.\n")
      return
    end

    local name, rest = arg:match("^(%S+)%s*(.*)$")
    local argument, detach = split_detach(rest)
    local ok, err = run_program(name, argument, detach)

    if not ok then out("run: " .. tostring(err) .. "\n") end
  end

  commands.clear = function()
    -- Fifty newlines, because neither sink understands an escape sequence:
    -- the serial side is whatever terminal you are in, and the screen side
    -- is forty lines of glyph blitting with no notion of a cursor address.
    out(string.rep("\n", 50))
  end

  commands.devices = function()
    local names = ns.list("/dev")
    if not names then return end

    out("Devices found on this machine. Each is a node in /dev, read the\n")
    out("same way a file is - fs.read(\"/dev/cpu\") is the same request the\n")
    out("filesystem answers, sent to a different server.\n\n")

    -- Named here rather than listed by the device server, because it is not
    -- the device server that answers for it.
    out("  /dev/console    PL011 UART, polled - served by the console\n")
    out("                  server, not by /dev: a read of it is a line of\n")
    out("                  input, not a description\n")

    for _, name in ipairs(names) do
      local d = ns.read("/dev/" .. name)
      local summary = ""

      if name == "cpu" then
        summary = string.format("%s %s %s, %d core%s",
          d.implementer, d.part, d.revision, d.cores, d.cores == 1 and "" or "s")
      elseif name == "memory" then
        summary = string.format("%d MB, %d MB free", d.total_mb, d.free_mb)
      elseif name == "kernel" then
        summary = string.format("%d threads, %d processes, %d spaces",
          d.threads, d.processes, d.spaces)
      elseif name == "screen" then
        summary = string.format("%dx%d, %d bytes a row", d.width, d.height, d.pitch)
      elseif name == "keyboard" then
        summary = d.transport
      elseif name == "timer" then
        summary = string.format("%d Hz tick, %d MHz counter",
          d.hz, d.counter_hz // 1000000)
      end

      out(string.format("  /dev/%-10s %s\n", name, summary))
    end
  end

  commands.cpu = function()
    local c = ns.read("/dev/cpu")
    if not c then return end

    out(string.format("%s %s %s\n", c.implementer, c.part, c.revision))
    out(string.format("  MIDR_EL1      0x%08x\n", c.midr))
    out(string.format("  cores         %d  (SMP is not on yet)\n", c.cores))
    out(string.format("  running at    EL%d\n", c.el))
    out(string.format("  addresses     %d-bit physical\n", c.pa_bits))
    out(string.format("  cache line    %d bytes\n", c.cache_line))
    out(string.format("  counter       %d MHz  (not the core clock: AArch64\n",
                      c.counter_hz // 1000000))
    out( "                has no architectural way to read that)\n")

    local has = {}
    for _, f in ipairs({ "fp", "simd", "aes", "sha1", "sha2", "crc32", "atomics" }) do
      if c[f] then has[#has + 1] = f end
    end
    out("  features      " .. table.concat(has, " ") .. "\n")
  end

  commands.mem = function()
    local m = ns.read("/dev/memory")
    if not m then return end
    out(string.format("%d MB of RAM at 0x%x, in %d pages of %d KB\n",
        m.total_mb, m.base, m.pages_total, m.page_size // 1024))
    out(string.format("  %s used, %s free\n",
        fmt_bytes(m.pages_total - m.pages_free, m.page_size),
        fmt_bytes(m.pages_free, m.page_size)))
  end

  commands.ps = function()
    local k = ns.read("/dev/kernel")
    if not k then return end
    out(string.format("threads    %d of %d\n", k.threads, k.threads_max))
    out(string.format("processes  %d of %d\n", k.processes, k.processes_max))
    out(string.format("endpoints  %d of %d\n", k.endpoints, k.endpoints_max))
    out(string.format("spaces     %d of %d\n", k.spaces, k.spaces_max))

    local pct = cpu_usage(k)
    if pct then
      out(string.format("\ncpu        %s %d%% busy since the last look\n",
                        bar(pct, 20), pct))
    else
      out("\ncpu        no reading yet: usage is the difference between two,\n")
      out("           so the first `ps` only starts the clock\n")
    end

    out("\nFixed pools, because the kernel has no allocator: running out\n")
    out("is an error at a known limit rather than a failure at an unknown one.\n")
  end

  commands.alias = function(arg)
    if arg == "" then
      local names = {}
      for k in pairs(aliases) do names[#names + 1] = k end
      table.sort(names)
      if #names == 0 then
        out("No aliases yet. Make one:\n\n")
        out("  alias ll devices\n")
        out("  alias m=monitor\n")
        return
      end

      for _, k in ipairs(names) do
        out(string.format("  %-12s -> %s\n", k, aliases[k]))
      end
      return
    end

    -- Either spelling. `%S+` cannot be used for the name: it is greedy, so
    -- on `m=monitor` it swallows the whole thing and leaves no target - which
    -- is precisely the spelling this command's own usage line promises.
    local name, target = arg:match("^([%w_%-]+)%s*=%s*(%S+)$")

    if not name then
      name, target = arg:match("^([%w_%-]+)%s+(%S+)$")
    end

    if not name or not target then
      out("usage: alias <name> <command>   or   alias <name>=<command>\n")
      return
    end

    if not commands[target] and not aliases[target] then
      out("there is no command called " .. target .. "\n")
      return
    end

    aliases[name] = target
    out(name .. " -> " .. target .. "\n")
  end

  commands.commands = function()
    local names = {}
    for k in pairs(commands) do names[#names + 1] = k end
    table.sort(names)
    out("  " .. table.concat(names, "  ") .. "\n")
    out("\nAnything that is not one of these is evaluated as Lua. A leading\n")
    out("slash always means a command: /ps runs the command even if `ps`\n")
    out("has been given a meaning in Lua.\n")
    out("`alias` on its own lists the aliases; `alias <name> <command>`\n")
    out("makes one.\n")
  end

  -- Shipped so that the listing is reachable under the word most people try.
  aliases.aliases = "alias"



  local help = setmetatable({}, {
    __tostring = function() return topics.overview end,
    __call = function(_, what)
      return topics[what or "overview"]
          or ("no help for " .. tostring(what) ..
              "; try fs, gfx, sys or demos")
    end,
  })

  -- What a chunk typed at the prompt can see. `fs` is this process's own
  -- namespace, so what the shell can reach is what the shell was given -
  -- there is no privileged view to hand out.
  commands.help = function(arg)
    out((topics[arg ~= "" and arg or "overview"]
         or ("no help for " .. arg .. "; try fs, gfx, sys, dev or demos\n")))
  end

  env = {
    fs = ns,
    help = help,
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
  out("Type `help` for what there is, `commands` for what you can type,\n")
  out("or `devices` for what this machine turned out to be.\n\n")

  while true do
    out("kosmos> ")

    local input = readline()
    if input == nil then return end          -- the console went away

    if input ~= "" then
      --------------------------------------------------------------------
      -- A command, or a program?
      --
      -- The first word names a command, and what follows it does not *start*
      -- with something that would make the line a Lua expression: that is a
      -- command. Anything else is Lua.
      --
      -- Looking only at the first character is the whole trick, and the
      -- first version got it wrong by testing the entire rest for
      -- punctuation. `help("gfx")` and `help = 3` are Lua because the rest
      -- begins with `(` and `=`; `help gfx` is a command. But
      -- `alias m=monitor` is *also* a command, and rejecting it because an
      -- equals sign appears somewhere in the middle broke a spelling this
      -- shell's own help had already promised.
      --------------------------------------------------------------------
      local slashed = input:match("^/(.*)$")
      local word, rest = (slashed or input):match("^([%a][%w_%-]*)%s*(.*)$")
      local name = word and (aliases[word] or word)

      local dispatch = false

      if name and commands[name] then
        if slashed then
          -- The explicit form. Always a command, whatever the name collides
          -- with, which is the point of having it.
          dispatch = true
        elseif rest:match("^[=%(%.%:%[%,]") then
          dispatch = false            -- `help("gfx")`, `help = 3`
        elseif shadows_lua(word) then
          -- The bare word also names something in Lua, so it stays Lua and
          -- the slash is how you mean the command. Refusing to guess is the
          -- whole reason `/` exists: a shell where `type` sometimes means a
          -- command and sometimes means the function is a shell you cannot
          -- write anything in.
          dispatch = false
        else
          dispatch = true
        end
      end

      if dispatch then
        local ok, err = pcall(commands[name], rest)
        if not ok then out("error: " .. tostring(err) .. "\n") end
        reap()
        goto next_line
      end

      if slashed then
        out("no command called " .. tostring(word) ..
            "; `/commands` lists them\n")
        goto next_line
      end

      --------------------------------------------------------------------
      -- Not a command. Is it a program?
      --
      -- The classic shell behaviour, and the reason it is safe here is the
      -- same rule as before: only a word that does not already name
      -- something in Lua is looked up in /bin. So `htop` runs the program
      -- and `print` stays the function, and a program can never shadow the
      -- language by being installed.
      --------------------------------------------------------------------
      if word and not shadows_lua(word) and not rest:match("^[=%(%.%:%[]") then
        local exists = ns.getattr("/bin/" .. word .. ".lua")

        if exists then
          -- pcall, like the command path above. Without it a program that
          -- fails to *start* - as opposed to one that fails while running,
          -- which is already isolated in its own process - took the shell
          -- down with it, and the shell cannot print its own last words
          -- because it prints by asking the console server.
          local argument, detach = split_detach(rest)
          local started, ok, err = pcall(run_program, word, argument, detach)

          if not started then
            out("run: " .. tostring(ok) .. "\n")
          elseif not ok then
            out("run: " .. tostring(err) .. "\n")
          end
          reap()
            goto next_line
        end
      end

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

      reap()
    end

    ::next_line::
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
  sys.name("init")
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
  local DEVICES_EP = 2
  local BINFS_EP   = 3

  --
  -- The kernel hands init four endpoints and no more, so the fifth is made
  -- here. That is the right place for it: the kernel's four are the ones it
  -- needs in order to hand the system over, and every server invented after
  -- that is init's business and not the kernel's.
  --
  local LIBFS_EP = sys.endpoint()
  local APPFS_EP = sys.endpoint()
  local DISKFS_EP = sys.endpoint()

  if not LIBFS_EP or not APPFS_EP then
    line("init: no endpoint for the library store or the app registry")
    sys.exit(1)
  end

  -- **A failed spawn says which one and why.**
  --
  -- These used to be `if not x then sys.exit(1) end`, and the system would
  -- die at boot in complete silence: no banner, no prompt, no message, with
  -- the kernel's own output looking perfectly healthy above it. That cost a
  -- debugging session the first time a spawn started being refused. init
  -- holds the console at this point precisely so it can say things, and the
  -- one moment it most needs to is when it cannot build the system.
  -- Which child is which, so a death can be named rather than numbered.
  local names = {}

  local function start(what, role, caps, flags)
    local id, err = sys.spawn(role, caps, flags)

    if not id then
      line("init: could not start " .. what .. ": " .. tostring(err))
      sys.exit(1)
    end

    names[id] = what
    return id
  end

  local console = start("the console server", ROLE_CONSOLE,
                        { CONSOLE_EP }, SPAWN_CONSOLE)
  local ramfs   = start("the ramfs", ROLE_RAMFS, { RAMFS_EP })
  local devices = start("the device server", ROLE_DEVICES, { DEVICES_EP })
  local binfs   = start("the program store", ROLE_BINFS, { BINFS_EP })
  local libfs   = start("the library store", ROLE_LIBFS, { LIBFS_EP })
  local appfs   = start("the app registry", ROLE_APPFS, { APPFS_EP })

  -- The disk, to one process and no other.
  --
  -- Started even when there is no block device: it answers "there is no
  -- disk" perfectly well, and a machine that boots differently depending on
  -- whether a drive is attached is a machine with two boot paths to test.
  local diskfs  = start("the disk server", ROLE_DISKFS, { DISKFS_EP },
                        SPAWN_DISK)

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
                      { CONSOLE_EP, RAMFS_EP, DEVICES_EP, BINFS_EP, LIBFS_EP,
                        APPFS_EP, DISKFS_EP },
                      SPAWN_SCREEN)

  -- And now it does what an init does, which is outlive everything and
  -- notice when something ends.
  while true do
    local id, code = sys.wait()
    if not id then
      -- Nothing left. On a real system this is the moment to panic or to
      -- restart something; here there is nobody left to tell.
      sys.exit(0)
    end

    -- **Said out loud.**
    --
    -- This used to be recorded into a local and dropped, on the reasoning
    -- that the console server might be the thing that just died. That is
    -- true and it is still no reason to say nothing: a process that dies
    -- takes its own error message with it, because it prints by asking the
    -- console server and a dead process asks nothing. init is the only one
    -- left who knows, and a system where a server can vanish in silence is
    -- a system that lies to you.
    --
    -- It found this the first time it mattered: the shell died on a bad
    -- edit and the only symptom was a prompt that never answered.
    local what = names[id] or ("process " .. tostring(id))

    if code == 0 then
      line("init: " .. what .. " exited cleanly")
    else
      line("init: " .. what .. " died with code " .. tostring(code))
    end
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
  sys.name("console")
  console_main(CAP)
  return
end

if role == ROLE_SHELL then
  sys.name("shell")
  -- The capabilities init granted, in the order it granted them.
  shell_main(0, 1, 2, 3, 4, 5, 6)
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
  sys.name("ramfs")
  ramfs_main(CAP)
  return
end

if role == ROLE_DEVICES then
  sys.name("devices")
  devices_main(CAP)
  return
end

if role == ROLE_APPFS then
  sys.name("appfs")
  appfs_main(CAP)
end

if role == ROLE_DISKFS then
  sys.name("diskfs")
  diskfs_main(CAP)
  return
end

if role == ROLE_LIBFS then
  sys.name("libfs")
  binfs_main(CAP, sys.libraries(), "libraries")
end

if role == ROLE_BINFS then
  sys.name("binfs")
  binfs_main(CAP, sys.programs(), "programs")
  return
end

if role == ROLE_RUNNER then
  --------------------------------------------------------------------------
  -- A program, in a process of its own.
  --
  -- This is what `exec` looks like when there is no ambient authority. The
  -- shell spawns this, hands it capabilities it chose, and sends it the
  -- source over IPC; the program runs with a namespace built from exactly
  -- those capabilities and can reach nothing else. There is no path search,
  -- no inherited environment and no global tree - a program that was not
  -- given the screen simply cannot draw.
  --
  -- What arrives in the message is the *path*, and this process fetches the
  -- source itself through the namespace built below. Why it is that way
  -- rather than the other is spelt out where the namespace is built.
  --------------------------------------------------------------------------
  sys.name("run")

  local SOURCE_EP = 0

  local req, who = sys.receive(SOURCE_EP)
  if not req then return end

  local ns = new_namespace()
  -- The namespace is built before the program is fetched, because fetching
  -- it goes through the namespace: the shell sends a *name*, not the source.
  --
  -- The first version sent the source in the message and could not: a
  -- program is several kilobytes and a message is 2048 bytes. Sending the
  -- name is better than making it fit, and not only because it works - the
  -- shell no longer has to read a program in order to start one, and the
  -- bytes cross the boundary once instead of twice.

  -- Whatever the shell handed over, in the order it promised. A missing one
  -- is simply not mounted, and the program finds that path does not exist.
  if req.console then ns.mount("/dev/console", req.console) end
  if req.data    then ns.mount("/data",        req.data)    end
  if req.bin     then ns.mount("/bin",         req.bin)     end
  if req.devices then ns.mount("/dev",         req.devices) end
  if req.lib     then ns.mount("/lib",         req.lib)     end
  if req.app     then ns.mount_registry("/app", req.app)    end
  if req.disk    then ns.mount("/disk",        req.disk)    end

  -- Whatever the parent shared, at the indices it said, and *after* the
  -- defaults so that a parent can replace one. A program that was started
  -- by another program can be handed things the shell never had - and can
  -- be handed a different version of something it would have had anyway,
  -- which is how a terminal gives its child a console that draws into a
  -- window.
  if req.mounts then
    for _, m in ipairs(req.mounts) do
      ns.mount(m.path, m.index)
    end
  end

  local function out(s) write_text(ns, "/dev/console", s) end

  --
  -- A program can start a program.
  --
  -- The shell does it by spawning one of these and naming a path; a program
  -- has no way to, because the runner's role number is init's business and
  -- not a program's. So the runner - which *is* one, and knows the number -
  -- offers it, along with the capabilities it was given. A program can pass
  -- on no more than it holds, which is the same rule as everywhere else.
  --
  -- `detach` is the difference between `run` and `benchmark`: with it the
  -- child answers as soon as it has been told what to run, and gets on with
  -- it, so the launcher is not held for the ten seconds the work takes.
  -- Without it the answer comes back when the program is finished, which is
  -- what a command line wants.
  --
  -- `shares` hands the child capabilities this program holds, each under a
  -- name in the child's namespace: run(path, args, detach, { ["/dev/wm"] = c }).
  --
  -- This is how a program becomes a server for its own children. The window
  -- manager needs it: it makes an endpoint, starts applications, and each
  -- one finds it at a path - without any of them being able to name it any
  -- other way, and without the shell that started the manager knowing that
  -- endpoint exists at all.
  --
  -- The rule is the same one as everywhere: a program can pass on no more
  -- than it holds. `shares` names capabilities out of this process's own
  -- table, and the kernel refuses an index this process does not have.
  --
  local function launch(path, argument, detach, shares)
    local ep = sys.endpoint()
    if not ep then return false, "no endpoint" end

    -- The four the runner always passes, then whatever is being shared.
    -- Order is the contract: the child is told which index each landed at,
    -- because a capability table is indexed and never named.
    local caps = { ep, req.console, req.data, req.bin, req.devices,
                   req.lib, req.app, req.disk }
    local mounts = {}

    if shares then
      for path_, cap in pairs(shares) do
        caps[#caps + 1] = cap
        mounts[#mounts + 1] = { path = path_, index = #caps - 1 }
      end
    end

    local id = sys.spawn(RUNNER_ROLE, caps, SPAWN_SCREEN)

    if not id then
      sys.destroy(ep)
      return false, "no process"
    end

    local reply = sys.call(ep, {
      path = path, args = argument or "", cwd = req.cwd or "/",
      detach = detach and true or false,
      console = 1, data = 2, bin = 3, devices = 4, lib = 5, app = 6,
      disk = 7,
      mounts = (#mounts > 0) and mounts or nil,
    })

    -- Destroyed either way. It was a private channel for one message and
    -- there are only ninety-six of them; leaving it is how the pool runs
    -- out for reasons nobody can see.
    sys.destroy(ep)

    if not reply then return false, "no answer" end

    -- The child's id as well as whether it started. Whoever launched
    -- something is the only one who may end it, and cannot without this.
    return reply.ok, reply.error, id
  end

  local env = {
    fs = ns,
    args = req.args or "",

    -- Where the caller thought it was. The working directory is the
    -- shell's idea and servers know nothing about it, so it travels with
    -- the request rather than being asked for: a program that wants to
    -- resolve a relative path needs it, and nothing else does.
    cwd = req.cwd or "/",
    run = launch,

    --
    -- Control-C, for a program that runs long enough to need interrupting.
    --
    -- Cooperative, and that is not a shortcut being papered over: there is
    -- no way to stop a process from outside yet. `process_exit` is suicide
    -- by construction - it panics if it is not the running process - and a
    -- kill would have to unlink the target from three IPC queues and settle
    -- what happens to whoever is holding a reply handle for it. That is its
    -- own piece of work, written up in roadmap.md, not something to bolt on
    -- here.
    --
    -- So this is what it says it is: a program that asks can be stopped, and
    -- a program that never asks cannot. A program given no console always
    -- gets false, which is right - it cannot be typed at either.
    --
    interrupted = function()
      return ns.interrupted("/dev/console") == true
    end,
    print = function(...)
      local parts = {}
      for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring((select(i, ...)))
      end
      out(table.concat(parts, "\t") .. "\n")
    end,
  }
  --------------------------------------------------------------------------
  -- `use("/lib/ui.lua")` - a library, loaded into this program's world.
  --
  -- Not `require`. There is no package path, no search, no C loader and no
  -- global module table: a library is a file in this process's namespace,
  -- and a program that was not given /lib does not have one. That is the
  -- same sentence as everywhere else in this system, applied to code.
  --
  -- The library is loaded with *this program's* environment, so it sees the
  -- same `fs`, `gfx` and `sys` the program does and cannot reach anything
  -- the program could not. A library is not more privileged than its
  -- caller; it is the caller, spelled in another file.
  --
  -- Cached per process, so `use` twice is one read and one compile, and two
  -- callers in the same program share one instance of whatever it returns.
  --------------------------------------------------------------------------
  local loaded = {}

  env.use = function(path)
    if loaded[path] ~= nil then
      return loaded[path]
    end

    local source, err = ns.read(path)

    if not source then
      error(("use: %s: %s"):format(path, tostring(err)), 2)
    end

    local chunk, why = load(source, "=" .. path, "t", env)

    if not chunk then
      error(("use: %s: %s"):format(path, tostring(why)), 2)
    end

    local value = chunk()

    -- A library that returns nothing still counts as loaded, or every call
    -- after the first would run it again.
    if value == nil then value = true end

    loaded[path] = value
    return value
  end

  setmetatable(env, { __index = _G })

  local path = req.path

  --
  -- Named after what it is running.
  --
  -- Every one of these called itself "run", so `ps` and the process app
  -- showed a column of identical names and the only way to tell two
  -- applications apart was their id. The name is what a process table is
  -- for.
  --
  sys.name((path:match("([^/]+)%.lua$") or path:match("([^/]+)$") or "run"))

  local source, read_err = ns.read(path)

  if not source then
    sys.reply(who, { ok = false, error = tostring(read_err) })
    return
  end

  local chunk, err = load(source, "=" .. path, "t", env)

  if not chunk then
    sys.reply(who, { ok = false, error = tostring(err) })
    return
  end

  -- Detached: answer now, work afterwards. The caller wanted it started,
  -- not finished, and holding it until the program ends would make
  -- "start four of these" mean "run four of these one at a time".
  if req.detach then
    sys.reply(who, { ok = true })
    pcall(chunk)
    return
  end

  -- pcall, so a program that raises reports it instead of taking this
  -- process down without a word. It is its own process either way; this
  -- just means the shell hears why.
  local ok, e = pcall(chunk)

  sys.reply(who, { ok = ok, error = not ok and tostring(e) or nil })
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
