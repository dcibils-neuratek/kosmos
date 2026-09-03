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
local ROLE_AUDIO     = 16 -- serves /dev/audio: the one process that may play

-- Whether this process can pass the screen on to a child.
--
-- Asking for the flag on a machine with no display is refused by the
-- kernel, and a refused spawn is a program that does not start - which is
-- the whole reason this exists.
--
-- Asked through `sys.screen()`, which is the only thing the kernel will
-- answer: it reports about *this* process, not about the machine. That is
-- the right answer to the question actually being asked - "may I pass this
-- on" - and it is a different question from "is there a display", which
-- nothing here can ask.
--
-- Remembered once true, and never re-asked after that. Ownership is set at
-- spawn and never taken away (`SYS_SCREEN_TAKE` suspends the console's
-- drawing, it does not move the grant), so a true answer cannot go back to
-- false. A false one is retried, because a process can be given the screen
-- later than it started.
local screen_seen = false

local function may_pass_screen()
  if not screen_seen then
    screen_seen = sys.screen() ~= nil
  end

  return screen_seen
end

--
-- The same question about sound, and asked for the same reason.
--
-- The kernel refuses a spawn that asks to pass on authority the parent does
-- not hold, so passing SPAWN_AUDIO on a machine with no sound device does
-- not silently do nothing - it fails the spawn, and the thing that fails to
-- start is the shell.
--
-- This is the *third* time in this function. The comment beside the shell's
-- spawn already records the first two, both about the screen, and the fix
-- was this exact shape both times. Adding audio without looking at it made
-- it three, and `make test` caught it the way it caught the others: a
-- machine with no display never reached a prompt.
--
local audio_seen = nil

local function may_pass_audio()
  if audio_seen == nil then
    local i = sys.info()

    audio_seen = (i ~= nil) and (i.audio_period or 0) > 0
  end

  return audio_seen
end

local SPAWN_CONSOLE = 1
local SPAWN_SCREEN  = 2

-- The disk. The strongest grant there is - raw sectors are every file on the
-- machine whatever any namespace says - so exactly one process gets it and
-- everything else asks that process. Same shape as the console and the
-- screen, and the reason is stronger.
local SPAWN_DISK    = 4

-- Authority over every process, for the task manager and nothing else.
-- Declared by the program, granted by whoever launches it, and refused by
-- the kernel when the launcher does not hold it itself.
local SPAWN_PROCCTL = 8
local SPAWN_AUDIO   = 16

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

  --
  -- What time it is, which is a different question from how long this
  -- machine has been running and could not be answered before.
  --
  -- `sys.ticks()` counts since boot, so a file written yesterday has an
  -- mtime that means nothing today - which is why Tracker has no Modified
  -- column and says so in its own header. This is the number that fixes
  -- that, read from the board's clock through the HAL.
  --
  -- **UTC, and only UTC.** There is no timezone database and no setting for
  -- one, so calling this local time would be calling it something it is
  -- not. A zone is a table of political decisions that changes several
  -- times a year; when this system wants one it will carry it and say
  -- where it came from, the way it does with fonts and icons.
  --
  if i.epoch and i.epoch > 0 then
    m.clock = { epoch = i.epoch, utc = true }

    local days = i.epoch // 86400
    local secs = i.epoch % 86400

    --
    -- Days since 1970 into a date, by Howard Hinnant's `civil_from_days`.
    --
    -- Written out rather than approximated, because the approximations are
    -- all wrong in the same interesting way: 365.2425 days a year is right
    -- on average and wrong on a specific Tuesday, and the error is a day,
    -- which is exactly the resolution anybody reading a date cares about.
    -- This one is exact for the proleptic Gregorian calendar, with no
    -- table and no loop over years, by shifting the era to start on 1 March
    -- so that the leap day is the last day of the year rather than a hole
    -- in the middle of one.
    --
    local z = days + 719468
    local era = (z >= 0 and z or z - 146096) // 146097
    local doe = z - era * 146097
    local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    local y = yoe + era * 400
    local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    local mp = (5 * doy + 2) // 153
    local d = doy - (153 * mp + 2) // 5 + 1
    local mo = mp + (mp < 10 and 3 or -9)

    if mo <= 2 then y = y + 1 end

    m.clock.year, m.clock.month, m.clock.day = y, mo, d
    m.clock.hour = secs // 3600
    m.clock.min  = (secs % 3600) // 60
    m.clock.sec  = secs % 60

    -- 1 January 1970 was a Thursday, which is why the 4.
    m.clock.weekday = (days + 4) % 7          -- 0 is Sunday
  end

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

--
-- `manual` hands the loop back to the caller.
--
-- Every server here blocks on receive, which is right when the only reason
-- to wake is a message. The audio server has another: the device wants
-- another period every 5.8 milliseconds and nobody sends a message to say
-- so, and a server that waits for one runs dry. So it drives its own loop,
-- pumping the mailbox between refills - and this returns the state with
-- `pump` and `answer` on it instead of looping.
--
local function serve(endpoint, state, make_handlers, manual)
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

  state.answer = answer

  if manual then
    return state
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

  --
  -- `root` is which part of the server appears here. Left out, the whole
  -- of it does, which is what every mount did before subtrees existed.
  --
  function ns.mount(prefix, capability, root)
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

    mounts[#mounts + 1] = { prefix = prefix, cap = capability, root = root }

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

        if rest == "" then rest = "/" end

        -- A mount may name a *subtree* of what the server holds, so one
        -- disk can appear at three places without three servers. `/system`
        -- and `/home` are both the same filesystem, at `/system` and
        -- `/home` inside it - which is what makes the layout in
        -- `layout.md` possible with one disk and one server.
        --
        -- Prepended here rather than by the server, because the server has
        -- no idea what anybody mounted it as and should not: it answers
        -- about paths in its own space, and this is the one place that
        -- knows how this process's names map onto them.
        if m.root then
          rest = (rest == "/") and m.root or (m.root .. rest)
        end

        return m.cap, rest, m.prefix
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

  -- `pass` is a capability travelling with the request, which the kernel
  -- translates into the server's own index for the same object. It is how
  -- a buffer is handed over for a large read: the pages, not the bytes.
  local function request(op, path, extra, pass)
    local capability, rest = resolve(path)
    if not capability then
      -- The sentence design.md 2 asks for. Nothing was denied; there is
      -- simply no such path in this process's world.
      return nil, "no such path: " .. path
    end

    local req = { type = op, path = rest }
    if extra then for k, v in pairs(extra) do req[k] = v end end

    local reply, err = sys.call(capability, req, pass)
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

    -- A server with more to say than fits in a message says so, exactly
    -- as `read` does. One that has never heard of `more` answers once and
    -- this loop does not run, which is what every server here did before
    -- the disk grew directories big enough to need it.
    while r and r.more and entries do
      local next_r = request("list", path, { offset = r.offset })

      if not next_r or not next_r.entries then break end

      for _, name in ipairs(next_r.entries) do
        entries[#entries + 1] = name
      end

      r = next_r
    end

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

  --
  -- The same read, without ever holding the whole thing.
  --
  --   for piece in fs.chunks("/home/song.mp3") do decode(piece) end
  --
  -- `ns.read` streams correctly and then undoes the benefit on its last
  -- line: it concatenates the pieces, so a four-megabyte file is fetched
  -- two kilobytes at a time and then fails on a two-megabyte heap. The
  -- protocol was never the limit. Accumulating was.
  --
  -- This is what anything long enough to matter should use - audio being
  -- the case that asked for it. A round trip is about 1.8 ms and carries
  -- roughly two kilobytes, so a stream runs at about a megabyte a second,
  -- which is seventy times what playing an MP3 needs. There is no reason
  -- to reach for shared pages until something needs to jump around inside
  -- a large file rather than read it through.
  --
  -- Returns an iterator, so the caller writes a `for` loop and the pieces
  -- are collected by the garbage collector as it goes.
  --
  --
  -- `read(fd, buf, n)`. The file may be any size; the buffer is yours.
  --
  --   local buf = sys.memory(16)                    -- 64 KB of pages
  --   local n = fs.read_into("/home/big.img", buf, 0, 65536)
  --
  -- Returns how many bytes arrived, and the file's total size, so a caller
  -- can walk a large file a window at a time without asking twice.
  --
  function ns.read_into(path, region, offset, bytes)
    local r, e = request("read", path,
                         { into = true, offset = offset or 0,
                           bytes = bytes }, region)

    if not r then return nil, e end

    return r.bytes, r.size
  end

  --
  -- `write(fd, buf, n)`, the mirror of `read_into`.
  --
  function ns.write_from(path, region, bytes)
    local r, e = request("write", path, { from = true, bytes = bytes },
                         region)

    if not r then return nil, e end

    return r.bytes
  end

  function ns.chunks(path)
    local offset = 0
    local done = false

    return function()
      if done then return nil end

      local r, e = request("read", path, { offset = offset })

      if not r then
        done = true
        return nil, e
      end

      local piece = r.value or ""

      offset = offset + #piece

      -- A server that has never heard of `more` answers everything at
      -- once, and this stops after that one piece rather than asking
      -- again for ever.
      if not r.more then done = true end

      if #piece == 0 then return nil end

      return piece
    end
  end

  function ns.write(path, value)
    local r, e = request("write", path, { value = value })
    return r ~= nil, e
  end

  function ns.getattr(path)
    -- A mount point is a directory, and only this table knows it.
    --
    -- `/bin` is a name in this process's mount table; the server behind it
    -- knows what it holds and nothing about where it was attached, so
    -- asking it about the empty path gets whatever that server thinks an
    -- empty path is - which is why `ls /` marked `data` a directory and not
    -- `bin`, `dev`, `lib` or `home`. Same reasoning as `ns.list` combining
    -- what the server said with what is mounted below: the shape of the
    -- tree is the namespace's answer to give.
    for _, m in ipairs(mounts) do
      if m.prefix == path then
        local r = request("getattr", path)
        local attrs = r and r.attrs or {}

        attrs.kind = "directory"
        return attrs
      end
    end

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

      --
      -- And the transitions, which are a different question.
      --
      -- `keys` is what the keys *meant* - shifted, mapped, an arrow spread
      -- over three bytes - and is what a terminal wants. This is what they
      -- *did*, and is the only thing that can say a key is still held: no
      -- stream of characters expresses that, which is why holding a
      -- direction in a game was a step per repeat rather than a walk.
      --
      -- Drained after the characters and from the same device pass, so the
      -- two never disagree about what happened.
      --
      local events = {}

      while true do
        local code, down = sys.key_event()

        if code == nil then break end

        events[#events + 1] = { code = code, down = down }
      end

      local where = sys.pointer()

      if #keys == 0 and #events == 0 then
        sys.wait_input(tonumber(req.ticks) or 0)
      end

      return { ok = true,
               value = { keys = keys, events = events, pointer = where } }
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
      -- A program declares it in its opening comment block, with
      -- `-- kosmos: application`. Anywhere in that block, so the copyright
      -- line every file carries can sit above it - it used to have to be
      -- line one, and the day the licence header arrived every
      -- application in here became a console program.
      --
      -- Guessing instead - looking for `ui.window` in the
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
        kind = state.windowed[name] and "application"
               or state.kinds[name] or "program",

        -- Applications only; a program is not in the menu at all.
        section = state.windowed[name]
                  and (state.sections[name] or "applications") or nil,

        -- What it declared it needs, so a launcher can decide what to
        -- grant without reading the source itself.
        needs = state.needs[name],
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
  local kinds = {}          -- for what is neither application nor program
  local sections = {}       -- which part of the Deskbar's menu it lives in
  local needs = {}

  -- The comment block a file opens with, and nothing after it.
  --
  -- Written as a loop rather than a pattern because a pattern that does
  -- this is a pattern nobody can read six months later, and this is the
  -- code that decides whether something is an application at all.
  local function header_of(source)
    local lines = {}

    for line in source:gmatch("([^\n]*)\n?") do
      local trimmed = line:match("^%s*(.-)%s*$")

      if trimmed ~= "" and not trimmed:match("^%-%-") then
        break
      end

      lines[#lines + 1] = line
    end

    return table.concat(lines, "\n")
  end

  for name, source in pairs(programs) do
    -- Anywhere in the file's opening comment block, not on the first line.
    --
    -- It used to have to be line one, and that broke the moment every file
    -- grew a copyright line above its description: every application in
    -- /bin quietly became a console program and the Deskbar emptied out.
    -- The manifest belongs to the header comment, and where in the header
    -- it sits is not something a program should have to get right.
    --
    -- The block ends at the first line that is not a comment and not
    -- blank, so nothing found in the body counts - a string in the middle
    -- of a program that happens to contain these words is not a
    -- declaration.
    local header = header_of(source)

    --
    -- What the file says it is. Three kinds, and the header decides - the
    -- same way it already decided what an application was:
    --
    --   -- kosmos: application     a window, listed in the Deskbar
    --   -- kosmos: server          owns something; others ask it
    --   (neither)                  a console program
    --
    -- Declaring `server` is a *description*, not a grant. What a process
    -- may actually do is still only what it was handed, and the kernel
    -- still refuses a flag the launcher does not hold - so a program that
    -- calls itself a server gets a word in a monitor and nothing more.
    --
    -- Which is why the declaration belongs here. The alternative was a
    -- list of known server names inside the process monitor, and `procs`
    -- says exactly what is wrong with that: it "would be wrong the first
    -- time somebody wrote another window manager".
    --
    if header:match("kosmos:%s*application") then
      windowed[name] = true
    elseif header:match("kosmos:%s*server") then
      kinds[name] = "server"
    end

    --
    -- Which part of the menu an application belongs in.
    --
    --   -- kosmos: section demos
    --
    -- BeOS sorted these by *directory* - /boot/apps, /boot/demos,
    -- /boot/preferences - and the Deskbar's menu was those three folders.
    -- Kosmos has one `/bin`, so the file says instead, which is where
    -- everything else about a file is already said. Anything that does not
    -- say is an application, because that is what most things are and a
    -- declaration everybody has to write is a declaration everybody forgets.
    --
    local said = header:match("kosmos:%s*section%s+(%a+)")

    if said then sections[name] = said:lower() end

    -- And a program may declare an authority it needs, which is the small
    -- beginning of `design.md` 9.2's manifest.
    --
    --   -- kosmos: needs processes
    --
    -- Read here rather than by whoever launches it, because /bin is what
    -- holds the source and reading several kilobytes to check one line
    -- would be a launcher paying for a fact this server already has. The
    -- launcher asks `getattr` and gets a list.
    -- The *header*, not the source. The block above explains that it ends
    -- at the first line of code so that "nothing found in the body counts"
    -- - and then this line read the whole file anyway, so a program with
    -- those words in a string was declaring an authority by accident.
    local declared = header:match("kosmos:%s*needs%s+([^\n]*)")

    if declared then
      local wanted = {}

      for word in declared:gmatch("%a+") do
        wanted[#wanted + 1] = word
      end

      needs[name] = wanted
    end
  end

  serve(endpoint,
        { programs = programs, windowed = windowed, kinds = kinds,
          sections = sections, needs = needs },
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
    local names = { "cpu", "memory", "kernel", "timer", "screen", "keyboard" }

    -- Listed only when there is one, like `screen` and `keyboard` above it:
    -- a board with no clock should not offer a node that answers nothing.
    if m.clock then names[#names + 1] = "clock" end

    return m, names
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

-- Reserved at the root of the filesystem, and the only two names that are.
--
-- A filesystem has to be able to say what it is and to be laid down, and
-- both are questions about the *disk* rather than about a file on it. They
-- live here as names because the protocol this system has is paths - adding
-- two operations to it so that one server could be asked two questions
-- would be a bigger change than reserving two names.
--
-- The dot is what keeps them from colliding with an ordinary file: nothing
-- creates a name starting with one, and `store` refuses to.
local RESERVED = { [".super"] = true, [".format"] = true }

-- The attributes that are read out of the inode rather than stored beside
-- it. `getattr` always reports these and `setattr` always refuses them.
--
-- `kind` is deliberately not in the list, and it took a failing test to
-- see why. A directory's kind is structural - it is what the inode says,
-- and nobody may set it. A *file's* kind is the BeOS idea: a People file
-- is a node whose kind is `person`, with an address and a phone number and
-- nothing inside it, and `attr notes.txt kind=note` is the documented way
-- to say so. One name, derived in one case and free in the other, which is
-- why it is handled where the node is in hand rather than in this table.
local DERIVED = { size = true, mtime = true, extents = true }

-- What marks a file as holding a serialised value rather than bytes.
--
-- Four bytes that no text file starts with: a NUL first, so anything that
-- reads this as text stops immediately rather than showing the rest.
local TABLE_MARK = "\0KTV"

local function diskfs_handlers(state)
  local kfs = state.kfs

  -- The superblock, read once at mount and kept.
  --
  -- Not re-read per request: it does not change except at format, and a
  -- filesystem that read block 0 before every operation would double the
  -- I/O of the whole system to learn something it already knew.
  local function mounted()
    if state.sb then return state.sb end

    local sb = kfs.mount()

    -- Before anything else looks at the disk.
    --
    -- A committed transaction that was not finished is not an error and not
    -- a repair: it is a write that was in progress when the power went, and
    -- replaying it is what makes that write have happened. Said out loud,
    -- because a silent replay would hide the only evidence that the machine
    -- did not shut down cleanly.
    if sb then
      local replayed = kfs.recover(sb)

      if replayed > 0 then
        print(("disk: the last write did not finish; %d block(s) replayed")
              :format(replayed))
      end
    end

    state.sb = sb
    return sb
  end

  --------------------------------------------------------------------------
  -- The index, and where it comes from.
  --
  -- `index[attribute][value]` is the set of paths that have it - the same
  -- shape the ramfs builds, because it answers the same question and the
  -- query code should not care which filesystem it is talking to.
  --
  -- **It is not on the disk.** BFS kept its indices as on-disk B+trees
  -- because it had hundreds of thousands of files; this has hundreds. A
  -- journaled B+tree with split and merge is the largest and most
  -- error-prone thing in this milestone, and dropping it costs one scan.
  --
  -- The deeper reason is the one design.md 8.3 gives: an index is derived
  -- from the attributes, and derived state that is *also* stored is state
  -- that can disagree with itself. On a filesystem that disagreement is a
  -- query returning a file that is not there, or missing one that is, and
  -- it survives reboots because both copies are on the disk. Rebuilding
  -- makes that failure impossible rather than unlikely.
  --
  -- When mount time hurts, that is the moment to persist it - with a
  -- measurement saying so, not before.
  --
  -- Built on first use rather than at mount. Same thing from the outside,
  -- and a machine that never runs a query never pays for one.
  --------------------------------------------------------------------------

  -- What a node is, as opposed to what is in it. Stored attributes first,
  -- then the four the inode already knows, which overwrite them.
  local function attrs_of(sb, path, node)
    local attrs = kfs.read_attrs(sb, node) or {}

    attrs.kind = (node.kind == kfs.KIND_DIR) and "directory"
                 or attrs.kind or "file"
    attrs.size    = node.size
    attrs.mtime   = node.mtime
    attrs.extents = #node.extents

    -- Always indexed, without anyone declaring it, which is the BFS rule
    -- and the reason a query by name is fast whatever else is going on.
    -- The name is not stored anywhere on the node - it lives in the
    -- directory entry - so it is put here where everything else can see it.
    attrs.name = path:match("([^/]+)$") or path

    return attrs
  end

  local function index_add(path, attrs)
    for name, value in pairs(attrs) do
      local bucket = state.index[name]

      if not bucket then
        bucket = {}
        state.index[name] = bucket
      end

      local key = tostring(value)

      bucket[key] = bucket[key] or {}
      bucket[key][path] = true
    end
  end

  local function index_remove(path, attrs)
    for name, value in pairs(attrs or {}) do
      local bucket = state.index[name]
      local by_value = bucket and bucket[tostring(value)]

      if by_value then by_value[path] = nil end
    end
  end

  -- The whole tree, depth first, into the index.
  --
  -- Recursive over directories, which is safe here for the reason the
  -- format is: a directory holding itself is not something this filesystem
  -- can express - there are no links - so the walk terminates.
  local function scan(sb, path, node, seen)
    local entries = kfs.read_dir(sb, node)

    for _, e in ipairs(entries or {}) do
      local child = (path == "") and ("/" .. e.name)
                                 or (path .. "/" .. e.name)
      local child_node = kfs.read_inode(sb, e.inode)

      if child_node and child_node.kind ~= kfs.KIND_FREE then
        local attrs = attrs_of(sb, child, child_node)

        seen[child] = attrs
        index_add(child, attrs)

        if child_node.kind == kfs.KIND_DIR then
          scan(sb, child, child_node, seen)
        end
      end
    end
  end

  local function indexed()
    if state.index then return state.index end

    local sb = mounted()

    state.index = {}
    state.attrs = {}

    if not sb then return state.index end

    local root = kfs.read_inode(sb, kfs.ROOT_INODE)

    if root then
      scan(sb, "", root, state.attrs)
    end

    return state.index
  end

  -- Something changed at this path. Cheaper than rescanning and, more to
  -- the point, correct: a rescan would also have to notice everything that
  -- did *not* change.
  local function touched(sb, path)
    if not state.index then return end          -- nothing built yet

    index_remove(path, state.attrs[path])
    state.attrs[path] = nil

    local number, node = kfs.find(sb, path)

    if number and node and node.kind ~= kfs.KIND_FREE then
      local attrs = attrs_of(sb, path, node)

      state.attrs[path] = attrs
      index_add(path, attrs)
    end
  end

  --
  -- One operation, all of it or none of it.
  --
  -- Everything that changes the disk goes through here. The alternative is
  -- `begin` and `commit` written out at each of the four call sites, and
  -- the failure that produces is one path that returns early between them,
  -- leaving a transaction open for the *next* operation to inherit.
  --
  local function atomic(sb, fn, ...)
    local ok, err = kfs.begin()

    if not ok then return nil, err end

    local result, why = fn(sb, ...)

    if not result then
      kfs.rollback()
      return nil, why
    end

    local done, cerr = kfs.commit(sb)

    if not done then return nil, cerr end

    return result
  end

  local function describe()
    local disk, why = sys.disk()

    if not disk then
      -- The same fields, zeroed. A reply whose *shape* depends on whether
      -- there is a disk makes every caller test for the absent case
      -- separately, and the one that forgets crashes on a machine that
      -- happens not to have a drive - which is exactly what happened.
      return { sectors = 0, sector_size = 0, bytes = 0,
               formatted = false, present = false, why = tostring(why) }
    end

    local sb = mounted()

    if not sb then
      return { sectors = disk.sectors, sector_size = disk.sector_size,
               bytes = disk.bytes, formatted = false, present = true,
               why = "not a kosmos filesystem" }
    end

    local out = {}
    for k, v in pairs(sb) do out[k] = v end

    out.sectors     = disk.sectors
    out.sector_size = disk.sector_size
    out.bytes       = disk.bytes
    out.formatted   = true
    out.present     = true
    out.free_blocks = sb.blocks - sb.data_at

    return out
  end

  return {
    list = function(req)
      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      local names, err = kfs.list(sb, req.path or "/")

      if not names then
        return { ok = false, error = tostring(err) }
      end

      -- In pieces, the same way `read` answers with something larger than
      -- a message. A directory of two hundred files does not fit in 2048
      -- bytes, and the first version of this simply failed - `ls` on a
      -- large directory said "the answer does not fit in a message" and
      -- showed nothing at all, which is a filesystem that works until you
      -- use it.
      --
      -- Chunked by the length of the names rather than by a count,
      -- because names are not a fixed size and a count that is safe for
      -- two hundred short ones is not safe for twenty long ones.
      --
      -- Twenty bytes of overhead an entry, which is measured rather than
      -- guessed: an array entry costs a tag and an eight-byte integer for
      -- its key, then a tag and a four-byte length for the string. The
      -- first version budgeted eight, packed a hundred and sixteen names
      -- into a reply that needed 2088 bytes, and failed with exactly the
      -- message it was written to avoid.
      local from = tonumber(req.offset) or 0
      local out, bytes = {}, 0

      for i = from + 1, #names do
        local name = names[i]

        if bytes + #name + 20 > 1600 and #out > 0 then
          return { ok = true, entries = out, more = true,
                   offset = from + #out }
        end

        out[#out + 1] = name
        bytes = bytes + #name + 20
      end

      return { ok = true, entries = out }
    end,

    read = function(req, who, cap)
      local name = req.path:match("([^/]+)$")

      if name == ".super" or req.path == "/.super" then
        return { ok = true, value = describe() }
      end

      --
      -- `read(fd, buf, n)`, with the buffer named by a capability.
      --
      -- Unix can copy into the caller's buffer because the kernel can
      -- reach into the caller's address space. This server is another
      -- process at EL0 and cannot, so the caller supplies shared pages and
      -- this writes into those. Same three questions: which file, where to
      -- put it, how much.
      --
      -- Without `into` the bytes come back inside the reply, which is
      -- right for a settings file and impossible for a large one - the
      -- message is 2048 bytes, and that was the whole reason a file bigger
      -- than a message could not be read.
      --
      -- Stepped rather than read in one piece, so this process's own heap
      -- holds sixty-four kilobytes however large the window is. A server
      -- that read a megabyte into a Lua string to copy it out again would
      -- have moved the limit rather than removed it.
      --
      if req.into then
        --
        -- The buffer is the caller's, and this holds a capability to it
        -- until it says otherwise. Sixteen is all a thread gets, so a
        -- server that keeps them answers sixteen requests and then refuses
        -- every one after - which is what a PDF read in 256-byte windows
        -- found on its fifteenth read.
        --
        -- Released on the way out of every path, not only the happy one:
        -- an error is still a request that was handed a buffer.
        --
        local function done_with(answer)
          if cap then sys.release(cap) end
          return answer
        end

        local sb = mounted()

        if not sb then
          return done_with({ ok = false,
                             error = "there is no filesystem here" })
        end

        local number, node = kfs.find(sb, req.path)

        if not number then
          return done_with({ ok = false, error = tostring(node) })
        end

        local from = tonumber(req.offset) or 0
        local want = tonumber(req.bytes) or node.size
        local STEP = 64 * 1024
        local done = 0

        while done < want do
          local piece = kfs.read_range(sb, node, from + done,
                                       math.min(STEP, want - done))

          if not piece or #piece == 0 then break end

          local ok, err = sys.region_write(cap, done, piece)

          if not ok then
            return done_with({ ok = false, error = tostring(err) })
          end

          done = done + #piece
        end

        return done_with({ ok = true, bytes = done, size = node.size })
      end

      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      if not name then
        return { ok = false, error = "that is the directory itself" }
      end

      local number, node = kfs.find(sb, req.path)

      if not number then
        return { ok = false, error = tostring(node) }
      end

      if node.kind == kfs.KIND_DIR then
        return { ok = false, error = "that is a directory" }
      end

      local data, err = kfs.read_file(sb, node)

      if not data then
        return { ok = false, error = tostring(err) }
      end

      -- A value that was stored as one comes back as one, and in a single
      -- reply: a table is small by construction here, because anything
      -- large is a file and files are bytes.
      if data:sub(1, #TABLE_MARK) == TABLE_MARK then
        local value, perr = sys.unpack(data:sub(#TABLE_MARK + 1))

        if value == nil then
          return { ok = false, error = "stored value is damaged: "
                                       .. tostring(perr) }
        end

        return { ok = true, value = value }
      end

      -- Chunked, because a message is 2048 bytes and a file is not. The
      -- namespace's `read` already knows how to ask for the rest - it sends
      -- an offset and looks at `more` - so a large file needs nothing new
      -- from the caller. design.md 8.4 replaces this with mapped pages when
      -- the file is large enough for the round trips to matter.
      local CHUNK = 1024
      local offset = req.offset or 0
      local piece = data:sub(offset + 1, offset + CHUNK)

      return { ok = true, value = piece,
               more = (offset + #piece) < #data }
    end,

    write = function(req, who, cap)
      local name = req.path:match("([^/]+)$")

      if name == ".format" then
        -- Checked here rather than only in `mkfs`, because this is the
        -- boundary. A program reaching this path is asking to erase the
        -- disk, and "it asked nicely" has to be part of the request rather
        -- than a habit of one caller.
        if req.value ~= "yes, erase it" then
          return { ok = false, error = "a format must say `yes, erase it`" }
        end

        local disk, why = sys.disk()

        if not disk then
          return { ok = false, error = tostring(why) }
        end

        local sb, err = kfs.mkfs(disk.sectors, sys.ticks())

        if not sb then
          return { ok = false, error = tostring(err) }
        end

        state.sb = sb           -- what is mounted is what was just written
        return { ok = true, value = sb }
      end

      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      if not name or RESERVED[name] then
        return { ok = false, error = "that name is reserved" }
      end

      --
      -- The other half of `read(fd, buf, n)`: the bytes come out of pages
      -- the caller owns rather than out of the message.
      --
      -- Without this nothing above about two kilobytes could be written at
      -- all - reads could stream and writes had a ceiling nothing could
      -- get past, which is a filesystem that works until you use it.
      --
      -- The ceiling that remains is this server's own heap: the bytes are
      -- assembled here and handed to `store`, which takes a whole file.
      -- Making `store` take a producer is what lifts that, and it is the
      -- next piece rather than this one.
      --
      if req.from then
        local want = tonumber(req.bytes) or 0

        if want > 1024 * 1024 then
          return { ok = false,
                   error = "more than a megabyte in one write, which this "
                           .. "server cannot assemble yet" }
        end

        local parts = {}
        local done = 0

        while done < want do
          local piece, rerr = sys.region_read(cap, done,
                                              math.min(64 * 1024,
                                                       want - done))

          if not piece then
            return { ok = false, error = tostring(rerr) }
          end

          parts[#parts + 1] = piece
          done = done + #piece
        end

        local number, serr = atomic(sb, kfs.store, req.path,
                                    table.concat(parts), sys.ticks())

        if not number then
          return { ok = false, error = tostring(serr) }
        end

        state.writes = (state.writes or 0) + 1
        touched(sb, req.path)

        return { ok = true, bytes = done }
      end

      -- A leading dot is ordinary. It was refused for a while, on the
      -- grounds that the two reserved names have one - and the first thing
      -- that broke was the desktop's own settings file, `.appearance`,
      -- which could then be written by nothing at all. Reserving two names
      -- reserves two names; it does not reserve a punctuation mark.

      -- A disk holds bytes, and the protocol carries values.
      --
      -- The ramfs stores whatever it was given, because it is memory and a
      -- table is a thing memory can hold. A disk cannot, and the first
      -- version handed the table straight to `store`, where `#data` on a
      -- table with no array part is zero - so `.appearance` was written as
      -- a nought-byte file, no error was raised anywhere, and the setting
      -- appeared to save and came back empty.
      --
      -- So a table is serialised, with a marker in front so that reading
      -- knows to undo it. `sys.pack` is the same serialiser a message uses,
      -- which is the point: there is one encoding for a value in this
      -- system and this is it.
      local body = req.value or ""

      if type(body) == "table" then
        local packed, perr = sys.pack(body)

        if not packed then
          return { ok = false, error = "cannot store that: " .. tostring(perr) }
        end

        body = TABLE_MARK .. packed
      elseif type(body) ~= "string" then
        body = tostring(body)
      end

      local number, err = atomic(sb, kfs.store, req.path, body, sys.ticks())

      if not number then
        return { ok = false, error = tostring(err) }
      end

      state.writes = (state.writes or 0) + 1

      -- The size and the modification time just changed, and both are
      -- indexed whether anybody asked for them or not.
      touched(sb, req.path)

      return { ok = true }
    end,

    delete = function(req)
      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      local name = req.path:match("([^/]+)$")

      if not name or RESERVED[name] then
        return { ok = false, error = "that name is reserved" }
      end

      local ok, err = atomic(sb, kfs.unlink, req.path)

      if not ok then
        return { ok = false, error = tostring(err) }
      end

      -- `touched` re-reads the path and finds nothing, which removes it.
      -- One function for "this changed" rather than a separate one for
      -- "this is gone": the second is the first, and two of them is two
      -- chances to forget one.
      touched(sb, req.path)

      return { ok = true }
    end,

    -- Not one of the five the protocol names, and deliberately so: making a
    -- directory is a real filesystem operation and this protocol is ours.
    -- It is reachable as `fs.send(path, { type = "mkdir" })` without the
    -- namespace client needing to know it exists.
    mkdir = function(req)
      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      local ok, err = atomic(sb, kfs.mkdir, req.path, sys.ticks())

      if not ok then
        return { ok = false, error = tostring(err) }
      end

      touched(sb, req.path)

      return { ok = true }
    end,

    getattr = function(req)
      local name = req.path:match("([^/]+)$")
      local sb = mounted()

      if not name or RESERVED[name] then
        return { ok = true, attrs = { kind = "device" } }
      end

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      local number, node = kfs.find(sb, req.path)

      if not number then
        return { ok = false, error = tostring(node) }
      end

      -- What somebody said about this file, and then what the file
      -- actually is. In that order deliberately: the derived four are
      -- facts read out of the inode a moment ago, and they overwrite
      -- anything stored under the same name rather than being overwritten
      -- by it. `setattr` refuses those names anyway, so this is a second
      -- lock on a door that is already locked - which is the right number
      -- of locks for the answer to "how big is this file".
      local attrs, aerr = kfs.read_attrs(sb, node)

      if not attrs then
        return { ok = false, error = tostring(aerr) }
      end

      -- A directory's kind is what it is. A file's is whatever was said
      -- about it, and `file` only when nothing was.
      attrs.kind  = (node.kind == kfs.KIND_DIR) and "directory"
                    or attrs.kind or "file"
      attrs.size  = node.size
      attrs.mtime = node.mtime
      -- How the file is laid out on the disk. Not something a filesystem
      -- usually tells you, and worth telling here: this is a machine for
      -- learning how one works, and "one extent" versus "nine" is the
      -- whole of what fragmentation means.
      attrs.extents = #node.extents

      return { ok = true, attrs = attrs }
    end,

    -- Attributes, which is the half of this filesystem that is not ext2.
    --
    -- BFS's semantics: they belong to the file, they are typed, and they
    -- are not its contents. `attr` and `find` already worked this way
    -- against the ramfs; this is the same protocol answered by something
    -- that survives the power going off.
    setattr = function(req)
      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      if type(req.attrs) ~= "table" then
        return { ok = false, error = "setattr wants a table of attributes" }
      end

      local name = req.path:match("([^/]+)$")

      if not name or RESERVED[name] then
        return { ok = false, error = "that name is the disk, not a file" }
      end

      local number, node = kfs.find(sb, req.path)

      if not number then
        return { ok = false, error = tostring(node) }
      end

      local attrs, aerr = kfs.read_attrs(sb, node)

      if not attrs then
        return { ok = false, error = tostring(aerr) }
      end

      for k, v in pairs(req.attrs) do
        -- Refused rather than quietly dropped. These four are read out of
        -- the inode on every `getattr`, so storing them would create a
        -- second copy of a fact - and the failure that produces is a
        -- `find` that returns a file whose size is wrong, months later,
        -- with nothing to point at. A caller that tries finds out now.
        -- Refused rather than quietly dropped, and refused before
        -- anything is written: a set that mentions one name it cannot
        -- have stores none of the others either. Half-applying it would
        -- leave the caller with no way to know which half.
        if DERIVED[k] or (k == "kind" and node.kind == kfs.KIND_DIR) then
          return { ok = false,
                   error = "`" .. k .. "` is what this is, not something "
                           .. "you can set; nothing was written" }
        end

        -- nil is how an attribute is removed, and `pairs` cannot carry
        -- one, so the empty string means the same thing. The alternative
        -- is a `delattr` in the protocol for a case this rare.
        if v == "" then
          attrs[k] = nil
        else
          attrs[k] = v
        end
      end

      local ok, werr = atomic(sb, kfs.write_attrs, number, node, attrs)

      if not ok then
        return { ok = false, error = tostring(werr) }
      end

      touched(sb, req.path)

      return { ok = true }
    end,

    --
    -- Everything matching, now.
    --
    -- The first term is looked up in the index and the rest filter what
    -- came back, so the cost is the size of the *answer* rather than the
    -- size of the filesystem. That is the entire claim of an indexed
    -- filesystem, and `qbench` is what checks it stays true.
    --
    -- Which term goes first is whichever `pairs` hands over first, which
    -- is the same real limitation the ramfs has: with two terms of very
    -- different selectivity, the wrong one first costs more. Choosing
    -- properly needs per-value counts.
    --
    query = function(req)
      local index = indexed()
      local where = req.where or {}
      local first_name, first_value = next(where)

      if first_name == nil then
        return { ok = true, paths = {} }
      end

      local bucket = index[first_name]
      local candidates = bucket and bucket[tostring(first_value)]

      if not candidates then
        return { ok = true, paths = {} }
      end

      local out = {}

      for path in pairs(candidates) do
        local attrs = state.attrs[path]
        local keep = attrs ~= nil

        if keep then
          for name, value in pairs(where) do
            if tostring(attrs[name]) ~= tostring(value) then
              keep = false
              break
            end
          end
        end

        if keep then out[#out + 1] = path end
      end

      table.sort(out)

      return { ok = true, paths = out }
    end,
  }
end

--------------------------------------------------------------------------
-- audio: the one process that makes a noise, so that several can.
--
-- `SPAWN_AUDIO` grants the device to exactly one process, the same way the
-- screen and the disk work. That is what makes per-application volume
-- possible rather than a coincidence: if every program could write to the
-- device, the last one to write would win and there would be nothing to
-- turn down.
--
-- So this holds it, and everything else asks. A client opens a stream, says
-- what it is called, and sends periods; this sums them with a gain each and
-- hands one period to the device.
--
-- **The summing is `sys.mix`, in C**, and the note beside it says why: 512
-- samples per stream per period against a 5.8 millisecond deadline, and a
-- Lua mixer would also allocate a kilobyte of garbage 172 times a second on
-- the one path in this system that has a deadline. What is in Lua is the
-- part that is *policy* - which streams exist, what each is called, how
-- loud each should be - which is a few dozen decisions a period.
--
-- The peak comes back from the same pass, which is what makes a level meter
-- free: the mixer has already touched every sample.
--------------------------------------------------------------------------

local function audio_main(endpoint)
  local streams = {}          -- id -> stream
  local order = {}            -- ids, oldest first, so the mixer list is stable
  local next_id = 1
  local master = 256          -- 256 is unity; see sys.mix

  local info = sys.info() or {}
  local PERIOD = info.audio_period or 0
  local DEPTH  = info.audio_periods or 0

  --
  -- How much a stream may hold before its writes start failing.
  --
  -- Four periods, the same as the device. A client that is further ahead
  -- than the hardware is a client buffering for a latency nobody asked for,
  -- and telling it to wait is what keeps the *whole* pipe short - which is
  -- the promise this is all in aid of.
  --
  local BACKLOG = 4

  -- Declared here and defined below, because `handlers.play` needs to call
  -- it: a client asking for room should be told whether there is room
  -- *now*, not whether there was room the last time this loop went round.
  local refill

  -- Periods actually mixed, reported with `streams` so a client can tell a
  -- silent server from a busy one.
  local mixes = 0

  local function pick(id)
    return streams[id]
  end

  local handlers = {}

  handlers.open = function(req, sender, cap)
    if PERIOD == 0 then
      return { ok = false, error = "this machine has no sound device" }
    end

    local id = next_id

    next_id = next_id + 1

    --
    -- The samples arrive in memory the client owns, not in messages.
    --
    -- `CLAUDE.md`: control by message, data by shared memory. The capability
    -- travels in this one message and nothing else ever does - after this,
    -- the client writes periods into the ring and this server reads them
    -- where they lie. A stream that sent its periods as message payloads
    -- minted two Lua strings per period, 172 times a second, inside a 5.8 ms
    -- deadline.
    --
    local at

    if cap and cap >= 0 then
      local why

      at, why = sys.ring_map(cap)

      if not at then
        return { ok = false, error = "that is not an audio ring: "
                                     .. tostring(why) }
      end
    else
      return { ok = false, error = "open wants a ring capability" }
    end

    streams[id] = {
      id = id,
      name = tostring(req.name or "sound"),
      gain = 256,
      balance = 0,              -- -100 left .. +100 right
      muted = false,
      ring = at,
      cap = cap,
      peak = 0,
      quiet = 0,                -- periods since it last sent anything
    }

    order[#order + 1] = id

    return { ok = true, stream = id, period = PERIOD, periods = DEPTH }
  end

  --
  -- There is no `play`, and its absence is the point.
  --
  -- It took a period as a message payload and this server put it in a Lua
  -- table. That is a stream travelling as a message, which `CLAUDE.md` now
  -- forbids, and the cost was not theoretical: two Lua strings minted per
  -- period, 172 times a second, 340 KB a second of garbage inside a 5.8 ms
  -- deadline. Throughput measured perfect and it clicked four times every
  -- two seconds.
  --
  -- A client writes into the ring it handed over at `open` and nothing is
  -- sent at all. What is left in this protocol is control: open, close,
  -- set a gain, ask what is playing.
  --

  handlers.close = function(req)
    local s = pick(req.stream)

    if s then
      --
      -- The ring was the client's memory and this process only borrowed a
      -- view of it.
      --
      -- `release` unmaps the pages *and* drops the capability, in that
      -- order, which is the order that matters: dropping first would leave
      -- a window where the pages could be freed underneath a mapping this
      -- process still holds. Letting go here rather than at exit is what
      -- stops a server that runs for weeks from accumulating a mapping per
      -- song played - which is exactly the shape of leak `make stress`
      -- exists to catch.
      --
      if s.cap then sys.release(s.cap) end

      streams[req.stream] = nil

      for i, id in ipairs(order) do
        if id == req.stream then table.remove(order, i) break end
      end
    end

    return { ok = true }
  end

  --
  -- What the mixer application draws.
  --
  -- `peak` is from the last mix and is measured *before* gain, so a muted
  -- stream that is still playing shows a moving meter - which is the
  -- question a meter answers: who is sending audio.
  --
  handlers.streams = function()
    local out = {}

    for _, id in ipairs(order) do
      local s = streams[id]

      if s then
        out[#out + 1] = {
          stream = id, name = s.name, gain = s.gain,
          balance = s.balance, muted = s.muted,
          peak = s.peak, queued = sys.ring_ready(s.ring) or 0,
          playing = s.quiet < 8,
        }
      end
    end

    return { ok = true, master = master, streams = out,
             period = PERIOD, periods = DEPTH, mixes = mixes }
  end

  handlers.set = function(req)
    if req.master then
      master = math.min(math.max(math.floor(req.master), 0), 256)
    end

    local s = pick(req.stream)

    if s then
      if req.gain then
        s.gain = math.min(math.max(math.floor(req.gain), 0), 256)
      end

      if req.balance then
        s.balance = math.min(math.max(math.floor(req.balance), -100), 100)
      end

      if req.muted ~= nil then s.muted = req.muted and true or false end
    end

    return { ok = true }
  end

  --
  -- One period out, if the device has room for one.
  --
  -- Every stream contributes what it has; a stream with nothing contributes
  -- nothing, which is not the same as contributing silence - a period with
  -- no streams in it at all is not sent, because sending silence would keep
  -- the device awake for nothing.
  --
  local mixlist = {}          -- reused, so the loop allocates nothing

  function refill()
    if PERIOD == 0 then return false end

    local queued = sys.sound_queued() or 0

    if queued >= DEPTH then return false end

    local n = 0

    for _, id in ipairs(order) do
      local s = streams[id]

      if s and s.ring then
        --
        -- Balance is a pan, applied to the gain rather than beside it.
        --
        -- Left of centre attenuates the right channel and vice versa, which
        -- is what a balance control does and is not what a *pan* law does -
        -- a real pan keeps the total power constant and needs a square
        -- root. This is the simple one, and saying so is better than
        -- implying the other.
        --
        local g = s.muted and 0 or ((s.gain * master) // 256)
        local l, r = g, g

        if s.balance > 0 then
          l = (g * (100 - s.balance)) // 100
        elseif s.balance < 0 then
          r = (g * (100 + s.balance)) // 100
        end

        --
        -- Every stream goes in the list whether or not it has a period
        -- ready, and `sys.mix` skips the empty ones.
        --
        -- Asking here as well would be a second look at an index the client
        -- may change in between, and two answers to one question is how
        -- this system keeps hurting itself. C looks once, mixes what it
        -- found, and advances only the rings it actually read.
        --
        n = n + 1
        mixlist[n] = mixlist[n] or {}
        mixlist[n].ring = s.ring
        mixlist[n].left = l
        mixlist[n].right = r
        mixlist[n].peak = 0
        mixlist[n].owner = s

        if (sys.ring_ready(s.ring) or 0) == 0 then
          s.quiet = s.quiet + 1
          s.idle_this_pass = true
        end
      end
    end

    for i = n + 1, #mixlist do mixlist[i] = nil end

    if n == 0 then return false end

    --
    -- **What `sys.mix` says, not what this hoped.**
    --
    -- It returns false when no ring had a period ready, and that answer is
    -- the loop's only way to know it has nothing to do. This used to be
    -- called and ignored, which was survivable while `n` counted only
    -- streams with data - `n == 0` meant the same thing. The moment every
    -- stream went into the list so that C could skip the empty ones, `n`
    -- stopped being zero, this always returned true, and the server never
    -- blocked again.
    --
    -- It span, at the top priority band, above the window manager. The
    -- desktop stopped being usable while anything played, which is what a
    -- spin at the top of a strict-priority scheduler looks like from the
    -- outside.
    --
    if not sys.mix(mixlist) then
      for _, id in ipairs(order) do
        local st = streams[id]

        if st then
          st.quiet = st.quiet + 1
          st.idle_this_pass = true
        end
      end

      return false
    end

    for i = 1, n do
      mixlist[i].owner.peak = mixlist[i].peak or 0
      mixlist[i].owner.idle_this_pass = nil
    end

    --
    -- Counted rather than printed.
    --
    -- This process is spawned with one capability - its own endpoint - and
    -- no console, so it *cannot* print: a `line` here does not go
    -- unnoticed, it raises, and the server dies mid-refill with the sound
    -- stopping and nothing said. Which is what happened while looking for
    -- the bug below.
    --
    -- So a diagnostic in a server like this has to be a *reply*. The count
    -- goes out with `streams` and the Mixer can show it.
    --
    mixes = mixes + 1

    --
    -- The meter falls, and it falls *per mixed period* rather than per turn
    -- round this loop.
    --
    -- That distinction was a bug and an instructive one. Decaying wherever
    -- a stream had nothing to contribute looks equivalent and is not: this
    -- loop spins - it has to, because the device wants a period every 5.8
    -- milliseconds and nothing sends a message to say so - so it goes round
    -- thousands of times a second while only 172 periods are played. A
    -- decay of three quarters reached zero between one period and the next,
    -- and every meter read empty while the sound was plainly playing.
    --
    -- Here it runs once per period actually mixed, which is the rate the
    -- meter is a picture of. About 40 milliseconds to halve, which is the
    -- decay every hardware meter has and for the same reason: an eye cannot
    -- follow a value that changes 172 times a second, so the instrument
    -- holds what it saw.
    --
    for _, id in ipairs(order) do
      local s = streams[id]

      if s and s.idle_this_pass then
        s.peak = (s.peak * 3) // 4
        s.idle_this_pass = nil
      end
    end

    return true
  end

  local state = serve(endpoint, {}, function() return handlers end, true)

  --
  -- The loop, and the one place in this system that deliberately spins.
  --
  -- `serve` blocks on receive, which is right for every other server here
  -- and wrong for this one: nobody sends a message when the device wants
  -- another period, so waiting for one means running dry. So this pumps the
  -- mailbox without blocking and refills between times.
  --
  -- It spins *only while something is playing*. With no streams it goes
  -- back to a blocking receive, because an idle desktop should be idle and
  -- a machine that burns a core to play silence is worse than one with no
  -- sound at all.
  --
  while true do
    state.pump()

    local worked = refill()

    if not worked then
      local busy = false

      for _, id in ipairs(order) do
        local s = streams[id]

        if s and s.ring and (sys.ring_ready(s.ring) or 0) > 0 then
          busy = true break
        end
      end

      if busy or (sys.sound_queued() or 0) > 0 then
        --
        -- The device is full and there is more to send. Wait - for a
        -- message, or for a tick, whichever arrives first.
        --
        -- This used to be `sys.yield()`, with a comment about coming back
        -- for the next period rather than sleeping through it, and it was
        -- wrong in a way worth recording: yielding does not wait, so this
        -- loop was runnable for ever and the meter read 26% for playing a
        -- tone. Doom, which actually renders, reads 8%.
        --
        -- A tick is 10 ms and the device holds four periods, 23.2 ms. So
        -- waking a tick from now finds 13 ms still in flight, which is not
        -- a near thing. Sleeping through a period would need two ticks to
        -- go by unserved, and if that is happening the machine has a
        -- problem this loop cannot fix by spinning.
        --
        -- The right answer is still the virtio-sound interrupt - the
        -- device saying it consumed a period, rather than this asking a
        -- hundred times a second whether it has. That is a driver change;
        -- this is the same latency for a fiftieth of the cost.
        --
        -- **It was `sys.sleep(1)` for an afternoon, and sleeping was the
        -- wrong verb.** A sleeping server answers nobody: a client sending
        -- a period waited for the timer rather than for this loop, so every
        -- send cost a tick. The Music window, which hands over twelve
        -- periods a pass, spent 45 ms doing twelve round trips that should
        -- be microseconds - and played at two thirds speed with the
        -- processor almost idle, which is a shape of bug worth recognising:
        -- slow *and* idle means waiting on the wrong thing.
        --
        -- A receive with a deadline is both. Somebody calls and this
        -- answers now; nobody calls and it is back for the next period
        -- anyway.
        --
        local request, sender, cap = sys.receive(endpoint, false, 1)

        if request then state.answer(request, sender, cap) end
      else
        -- Nothing playing. Wait for a message like an ordinary server.
        local request, sender, cap = sys.receive(endpoint)

        if not request then return end

        state.answer(request, sender, cap)
      end
    end
  end
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
                          lib_cap, app_cap, disk_cap, audio_cap)
  local ns = new_namespace()
  ns.mount("/dev/console", console_cap)
  ns.mount("/data", ramfs_cap)

  -- Longest prefix wins, so /dev/console keeps going to the console server
  -- while everything else under /dev goes to the device server. Two servers
  -- under one directory, and neither knows about the other - which is what a
  -- per-process mount table buys.
  ns.mount("/dev", devices_cap)

  --
  -- Over the top of `/dev`, because longest prefix wins.
  --
  -- `/dev/audio` is a different server from the one that answers the rest
  -- of `/dev`, exactly as `/dev/console` is - the devices server describes
  -- hardware and this one *is* a piece of it. Mounted for everybody rather
  -- than passed to children the way `/dev/wm` is, because any program may
  -- ask to make a noise and the answer is a stream with a volume on it
  -- rather than a refusal.
  --
  if audio_cap then ns.mount("/dev/audio", audio_cap) end

  -- The programs this image carries. Read-only, and served by a process of
  -- its own like everything else.
  ns.mount("/bin", bin_cap)

  -- And what programs load rather than run. Separate from /bin so that `ls
  -- /bin` lists things you can type and nothing else.
  ns.mount("/lib", lib_cap)

  -- What is running, and what each one exposes. A registry rather than a
  -- mount: the names under it appear and disappear with the programs.
  ns.mount_registry("/app", app_cap)

  -- Files, on the disk, surviving the power going off. design.md 8.1 names
  -- this as where user data lives, and the two reserved names at its root -
  -- `.super` and `.format` - are how the disk underneath it is asked about
  -- and laid down.
  --
  -- One disk, three names, which is what `layout.md` describes.
  --
  --   /system   what the operating system ships
  --   /user     what somebody installed
  --   /home     what somebody made
  --
  -- All three are the same filesystem and the same server; the mount says
  -- which part of it appears where. Before subtree mounts this had to be
  -- the whole disk at one name, and a file written by `mkimage` at
  -- `/home/notes` arrived as `/home/home/notes`.
  ns.mount("/system", disk_cap, "/system")
  ns.mount("/user",   disk_cap, "/user")
  ns.mount("/home",   disk_cap, "/home")

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

    -- What the program declared, and what this process may actually hand
    -- on. A declaration is a request, never a grant: the kernel refuses a
    -- flag the parent does not hold, so a program that asks for authority
    -- nobody gave this shell simply does not get it.
    --
    -- The screen to everything, which is wrong and is staying for now.
    --
    -- Two things follow from it and neither was meant. Every program has
    -- the framebuffer mapped into its address space and could draw over
    -- the desktop without going near the window manager - ambient
    -- authority, in a system whose first principle is that what you were
    -- not handed you cannot reach. And `process_grant_screen` promotes to
    -- SCHED_PRIO_DISPLAY, so every program runs in the compositor's band,
    -- which means nothing once everything is in it. The comment there says
    -- "whoever was handed the screen is the one drawing it", and that was
    -- true when only the desktop was handed it.
    --
    -- Granting it only to programs that declare `kosmos: needs screen` was
    -- tried, and it is the right change - but it uncovers something worse
    -- underneath, so it is not this change. With programs at NORMAL rather
    -- than DISPLAY, one that spins on `sys.yield()` instead of blocking is
    -- starved outright while the desktop runs: `say 3 hello` never reaches
    -- its own deadline, and the display harness caught it. A thread that
    -- *blocks* is woken and runs; a thread that only yields is not.
    --
    -- So the scheduler has to answer for that first. The declarations are
    -- already in the four programs that draw (`wm`, `deskbar`, `monitor`,
    -- `edit`), so the change is one line here once yielding at NORMAL is
    -- fair. See `docs/state.md`.
    --
    local flags = may_pass_screen() and SPAWN_SCREEN or 0
    local attrs = ns.getattr(path)

    for _, want in ipairs(attrs and attrs.needs or {}) do
      if want == "processes" then flags = flags | SPAWN_PROCCTL end
      if want == "audio" and may_pass_audio() then
        flags = flags | SPAWN_AUDIO
      end
    end

    local id = sys.spawn(RUNNER_ROLE, { ep, console_cap, ramfs_cap,
                                        bin_cap, devices_cap, lib_cap,
                                        app_cap, disk_cap, audio_cap },
                         flags)

    if not id then
      sys.destroy(ep)
      return false, "could not start a process for it"
    end

    local reply = sys.call(ep, {
      path = path, args = argument or "", cwd = cwd,
      detach = detach and true or false,
      console = 1, data = 2, bin = 3, devices = 4, lib = 5, app = 6,
      disk = 7, audio = 8,
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

  --------------------------------------------------------------------------
  -- What the machine was told to start with.
  --
  --   qemu ... -fw_cfg name=opt/kosmos/boot,string=wm
  --
  -- Run through the ordinary command path rather than by a special case,
  -- so `boot=wm` and typing `wm` are the same thing and there is one way a
  -- program starts. Anything in /bin works, with arguments:
  -- `string=wm blocks` opens the desktop with a game on it.
  --
  -- It is a *command line* option and not a setting on disk, because it is
  -- how you decide what this boot is for - and a machine that will not
  -- reach a prompt because of something written in a file is a machine you
  -- cannot fix from the prompt.
  --------------------------------------------------------------------------
  local autostart = sys.boot("opt/kosmos/boot")

  if autostart and autostart ~= "" then
    out("starting " .. autostart .. "\n")

    -- Through the same function a typed program name goes through, so
    -- `boot=wm` and typing `wm` are the same thing and there is one way a
    -- program starts.
    local word, rest = autostart:match("^(%S+)%s*(.*)$")
    local ok, why = run_program(word, rest, false)

    if not ok then
      out("boot: " .. tostring(word) .. ": " .. tostring(why) .. "\n")
    end
  end

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
  local AUDIO_EP = sys.endpoint()

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
  -- The grant is asked for only when there is something to grant. A machine
  -- with no drive is a supported way to run - it is how every display test
  -- runs - and the first version of this asked unconditionally, so the
  -- spawn was refused, `start` did what it is supposed to do about a server
  -- that will not start, and the whole system died at boot on any machine
  -- without a disk.
  --
  -- The server itself starts either way and answers "there is no disk",
  -- which is what keeps this from being two boot paths: what differs is one
  -- flag, not whether a process exists.
  local diskfs  = start("the disk server", ROLE_DISKFS, { DISKFS_EP },
                        sys.disk() and SPAWN_DISK or 0)

  --
  -- The sound device goes here and nowhere else.
  --
  -- Same shape as the disk, and the same conditional: the grant is asked
  -- for only when there is something to grant, because a machine with no
  -- sound card is a supported way to run and asking anyway would fail the
  -- spawn. The server starts either way and answers "this machine has no
  -- sound device", so there is one boot path rather than two.
  --
  -- One owner is the whole design and not a simplification. If every
  -- program could write to the device the last writer would win, and per-
  -- application volume would have nothing to be a volume *of*.
  --
  local audio = start("the audio server", ROLE_AUDIO, { AUDIO_EP },
                      may_pass_audio() and SPAWN_AUDIO or 0)

  local _ = audio

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
                        APPFS_EP, DISKFS_EP, AUDIO_EP },
                      -- The screen, and authority over processes.
                      --
                      -- The shell needs the second in order to *pass it
                      -- on*: the desktop declares it needs it, and the task
                      -- manager declares it to the desktop. The kernel
                      -- refuses a flag the parent does not hold, so without
                      -- this the chain breaks at the first link and the
                      -- desktop will not start at all - which is what
                      -- happened, and is the model saying no correctly.
                      --
                      -- It also makes a `kill` command in the shell
                      -- possible, which is where it belongs.
                      --
                      -- The screen is asked for only when there is one, for
                      -- the reason spelled out above the disk server: the
                      -- kernel refuses a flag this process does not hold,
                      -- and it does not hold the screen on a machine with
                      -- no display. Asking anyway made `make serial` - and
                      -- any real board without a framebuffer - die at boot
                      -- with the shell never starting. The same mistake,
                      -- twice, in the same function.
                      (may_pass_screen() and SPAWN_SCREEN or 0)
                      | (may_pass_audio() and SPAWN_AUDIO or 0)
                      | SPAWN_PROCCTL)

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
  shell_main(0, 1, 2, 3, 4, 5, 6, 7)
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

  --
  -- The collector reclaims what was allocated. Measured as a *difference*,
  -- not a ratio.
  --
  -- This used to assert `peak > before * 2 and after < peak / 2`, which is
  -- a statement about the size of the baseline heap rather than about the
  -- collector: it passes while this chunk is small and fails when it grows,
  -- because two thousand small tables are a fixed amount of memory and
  -- doubling a larger number takes more of them. Adding an audio server to
  -- this file broke it, and the collector was working perfectly.
  --
  -- What the test is for is that allocation costs memory and collection
  -- gives it back. So: the rise has to be real, and nearly all of it has to
  -- come back. Neither depends on what else happens to be on the heap.
  --
  collectgarbage()
  local before = collectgarbage("count")
  local t = {}
  for i = 1, 2000 do t[i] = { i } end
  local peak = collectgarbage("count")
  t = nil
  collectgarbage()
  local after = collectgarbage("count")

  local rose = peak - before
  local kept = after - before

  check(rose > 50, "allocating two thousand tables cost no memory")
  check(kept < rose / 4, "the collector did not reclaim")
  line(string.format("selftest: gc %.0fK, +%.0fK allocated, %.0fK kept",
                     before, rose, kept))

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

if role == ROLE_AUDIO then
  sys.name("audio")
  audio_main(CAP)
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
  if req.disk    then
    ns.mount("/system", req.disk, "/system")
    ns.mount("/user",   req.disk, "/user")
    ns.mount("/home",   req.disk, "/home")
  end

  -- After `/dev`, because longest prefix wins and this is a different
  -- server from the one that answers the rest of it.
  if req.audio   then ns.mount("/dev/audio",   req.audio)   end

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
  -- `where` is the directory the child starts in. Without it a program run
  -- from a Terminal always started at the Terminal's *parent's* cwd, so
  -- `cd` moved the prompt and nothing that ran from it.
  local function launch(path, argument, detach, shares, where)
    local ep = sys.endpoint()
    if not ep then return false, "no endpoint" end

    -- The four the runner always passes, then whatever is being shared.
    -- Order is the contract: the child is told which index each landed at,
    -- because a capability table is indexed and never named.
    -- The audio server comes last, and has to be here: a program launched
    -- by another program - which is every application, because the window
    -- manager launches them - gets its namespace from this list, and
    -- without it `/dev/audio` is a path that does not exist. The Mixer said
    -- "nothing is playing" while two tones were running, because they were
    -- not able to reach the server to say otherwise.
    local caps = { ep, req.console, req.data, req.bin, req.devices,
                   req.lib, req.app, req.disk, req.audio }
    local mounts = {}

    if shares then
      for path_, cap in pairs(shares) do
        caps[#caps + 1] = cap
        mounts[#mounts + 1] = { path = path_, index = #caps - 1 }
      end
    end

    -- What the child declared it needs. The kernel refuses any flag this
    -- process does not itself hold, so a program cannot ask its way to
    -- authority the desktop was never given.
    --
    -- The screen to everything, which is wrong and is staying for now.
    --
    -- Two things follow from it and neither was meant. Every program has
    -- the framebuffer mapped into its address space and could draw over
    -- the desktop without going near the window manager - ambient
    -- authority, in a system whose first principle is that what you were
    -- not handed you cannot reach. And `process_grant_screen` promotes to
    -- SCHED_PRIO_DISPLAY, so every program runs in the compositor's band,
    -- which means nothing once everything is in it. The comment there says
    -- "whoever was handed the screen is the one drawing it", and that was
    -- true when only the desktop was handed it.
    --
    -- Granting it only to programs that declare `kosmos: needs screen` was
    -- tried, and it is the right change - but it uncovers something worse
    -- underneath, so it is not this change. With programs at NORMAL rather
    -- than DISPLAY, one that spins on `sys.yield()` instead of blocking is
    -- starved outright while the desktop runs: `say 3 hello` never reaches
    -- its own deadline, and the display harness caught it. A thread that
    -- *blocks* is woken and runs; a thread that only yields is not.
    --
    -- So the scheduler has to answer for that first. The declarations are
    -- already in the four programs that draw (`wm`, `deskbar`, `monitor`,
    -- `edit`), so the change is one line here once yielding at NORMAL is
    -- fair. See `docs/state.md`.
    --
    local flags = may_pass_screen() and SPAWN_SCREEN or 0
    local attrs = ns.getattr(path)

    for _, want in ipairs(attrs and attrs.needs or {}) do
      if want == "processes" then flags = flags | SPAWN_PROCCTL end
      if want == "audio" and may_pass_audio() then
        flags = flags | SPAWN_AUDIO
      end
    end

    local id = sys.spawn(RUNNER_ROLE, caps, flags)

    if not id then
      sys.destroy(ep)
      return false, "no process"
    end

    local reply = sys.call(ep, {
      path = path, args = argument or "", cwd = where or req.cwd or "/",
      detach = detach and true or false,
      console = 1, data = 2, bin = 3, devices = 4, lib = 5, app = 6,
      disk = 7, audio = 8,
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

    --
    -- A kit is a library that happens to be C.
    --
    -- `use("/lib/ui.lua")` reads Lua out of the namespace and runs it;
    -- `use("/kits/pdf")` gets a table the runtime built. The caller writes
    -- the same line either way, which is the point: where a library's speed
    -- comes from is not something the program using it should have to know,
    -- and a kit that later grows a Lua half - or a Lua library that has its
    -- hot loop moved into C - should not change a single call site.
    --
    -- Kits come through the namespace rather than as globals so that the
    -- rule the rest of the system runs on still holds: what you were not
    -- given, you do not have. A program with no `use` has no kits.
    --
    local kit = path:match("^/kits/([%w_]+)$")

    if kit then
      local value, why = sys.kit(kit)

      if not value then
        error(("use: %s: %s"):format(path, tostring(why)), 2)
      end

      loaded[path] = value
      return value
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
