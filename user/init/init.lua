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
-- 6 was ROLE_RELOAD, which checked hot reload against a live server.
-- Hot reload went with ramfs; the number is left unused rather than
-- reassigned, because a role number that changes meaning is how a spawn
-- ends up starting the wrong thing.
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

--
-- No `describe_machine` here: `user/servers/devices.c` decodes the ID
-- registers now, and this was the same three tables and the same MIDR
-- arithmetic left behind when the devices server moved to C.
--
-- The division it was written to demonstrate is unchanged and is worth
-- restating, because it is the reason none of this is in the kernel:
-- `sys.info()` hands back raw ID registers and decodes nothing, so a
-- processor the kernel has never heard of gets described properly without
-- the kernel changing. What moved is which side of the syscall the lookup
-- table lives on, not whether the kernel holds one.
--

--------------------------------------------------------------------------
-- The protocol, which is now two protocols with one vocabulary.
--
-- design.md 4.4: `list`, `read`, `write`, `getattr`, `setattr`, over typed
-- records rather than byte streams. `read` returns a value and not a string,
-- which is the whole point - `fs.read("/dev/temp")` gives `{ celsius = 47.2 }`
-- rather than "47200\n" for whoever asked to parse.
--
-- **What carries those verbs depends on who answers**, and that is the
-- change this file has been through. Six servers are C and take a *declared
-- struct*: `/dev`, `/bin`, `/lib`, `/app`, `/dev/console` and `/data`, each
-- with a header in `user/include/` that both sides compile against. A mount
-- names which, and `request` below branches on it.
--
-- Everything else still sends a table with a `type`, and design.md 14 makes
-- that field mandatory: with no static types, a message that does not say
-- what it is becomes a silent nil three layers down. `diskfs` is the last
-- server that speaks this way; the window manager and application scripting
-- also do, and always will - their vocabularies are open, which is exactly
-- when a table is right.
--
-- The two are not a compromise between them. A struct is for a boundary
-- where the shape is agreed and a caller being wrong should be impossible to
-- express; a table is for one where the shape is the caller's to choose.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- A server: receive, dispatch, reply, repeat.
--
-- Each request runs in a coroutine. That buys error isolation - a handler
-- that raises kills its own request and not the server - and it is the
-- shape design.md 4.5 wants: every `receive` is a yield, so server code is
-- written sequentially over synchronous IPC instead of as a state machine.
--
-- **This used to reload too, and no longer does.** `serve` took a factory
-- rather than a table of handlers so that behaviour could be replaced while
-- state survived, and that worked: M5's definition of done was a server's
-- code being swapped mid-conversation with a client. It went when ramfs
-- became C, because ramfs was the last server this ran and there is nothing
-- left to reload. `design.md` records the decision.
--
-- The factory is kept, even with one caller and nothing to replace. It costs
-- a line and it is the shape that makes the state and the behaviour separate
-- things, which is worth having whether or not anything swaps them.
--------------------------------------------------------------------------

--
-- Three things this used to have, and no longer needs. All of them were
-- built for servers that are C now, and each solves its problem natively
-- there rather than needing the loop's help:
--
--   `DEFER`, a sentinel a handler returned when it was not answering yet.
--   ramfs's `watch` was the only thing that returned it; `ramfs.c` keeps the
--   sender in a slot and replies from `notify`.
--
--   `state.pump`, which let a blocked handler answer other callers while it
--   waited. The console's `read` was the only caller: it blocked inside the
--   handler until somebody typed a line, and while blocked it answered
--   nobody. `console.c` does not block at all - it records who asked and
--   replies from its own loop.
--
--   `manual`, which handed the loop back so the audio server could drive
--   its own: the device wants a period every 5.8 ms and no message says so.
--   `audio.c` has its own loop by construction.
--
-- Removed in the review before 0.8 rather than kept in case. Each was six
-- lines and is in the history; what none of them had any more was a caller.
--
local function serve(endpoint, state, make_handlers)
  local handlers = make_handlers(state)

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

    do
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

  while true do
    local request, sender, cap = sys.receive(endpoint)
    if not request then return end          -- the endpoint went away
    answer(request, sender, cap)
  end
end

--------------------------------------------------------------------------
-- /data is `user/servers/ramfs.c`, and `main.c` dispatches role 1 to it
-- before the interpreter is opened.
--
-- The seventh and last to move, and the only one whose conversion cost a
-- feature rather than only buying one. ramfs was what `ROLE_RELOAD`
-- reloaded and what `help("demos")` let you watch being reloaded, so with
-- it in C there is no server left whose code can be replaced while it runs.
-- Hot reload is gone from this system, deliberately - `design.md` records
-- the decision, and the honest word is *removed* rather than *outranked*.
--
-- What the C one does differently, beyond having no collector: it keeps a
-- flat table of paths instead of a tree plus a path map. The Lua version
-- held both and kept them in step by hand, which meant `write` had to
-- remember to touch two representations of one fact. A directory is now a
-- path with no value, and listing is a scan for children.
--

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
  function ns.mount(prefix, capability, root, proto)
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

    mounts[#mounts + 1] = { prefix = prefix, cap = capability, root = root , proto = proto }

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

    --
    -- Raw, because `/app` is C. The capability comes back beside the bytes
    -- as it always did: `sys.call_raw` returns the reply's payload and the
    -- endpoint travels in the message rather than in it.
    --
    local reply, got = sys.call_raw(cap, string.pack("<I4c24", 2, name))

    if not reply or #reply < 4 or string.unpack("<I4", reply) ~= 0
       or not got or got < 0 then
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

        return m.cap, rest, m.prefix, m.proto
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
    local cap, rest, prefix, proto = match(path)

    for _, a in ipairs(autos) do
      if prefix == a.prefix and rest ~= "/" then
        if lookup_into(a.prefix, a.cap, path) then
          return match(path)
        end

        --
        -- It is not registered, and the registry is not a substitute for it.
        --
        -- The paragraph above fixed the case where the lookup *succeeds*.
        -- This is the same bug on the other branch, and it survived because
        -- it only shows when a name is absent: falling through here returns
        -- the registry's own mount, so asking a desktop that is not running
        -- to start profiling came back "no such operation: profile" - a
        -- clean, wrong answer from a directory, which is the exact sentence
        -- the comment above was written about.
        --
        -- `design.md` 2 says what the answer is. Nothing was denied; there
        -- is no such path in this process's world, and saying so lets a
        -- caller tell "you have no window manager" from "your window
        -- manager said no".
        --
        return nil
      end
    end

    if cap then return cap, rest, prefix, proto end

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
  function ns.mount_registry(prefix, capability, proto)
    ns.mount(prefix, capability, nil, proto)
    autos[#autos + 1] = { prefix = prefix, cap = capability }
  end

  -- `pass` is a capability travelling with the request, which the kernel
  -- translates into the server's own index for the same object. It is how
  -- a buffer is handed over for a large read: the pages, not the bytes.
  --------------------------------------------------------------------------
  -- Servers that speak a struct rather than a table.
  --
  -- **This is the migration showing through, and it is meant to be visible
  -- rather than hidden.** A mount says which protocol the server on the
  -- other side speaks, and this kit packs accordingly. Every mount without
  -- one speaks tables, which is all of them but `/dev` today.
  --
  -- It lives here because this is the client half of the boundary: the
  -- namespace is the kit that knows how to talk to servers, so knowing that
  -- one of them wants 24 bytes of struct is exactly its business. When the
  -- last server has moved, the branch and the flag both go and `request`
  -- becomes one path again.
  --
  -- The layouts mirror `user/include/devproto.h`, which is the second and
  -- last place they are written. The asserts below are what stands in for
  -- the single implementation `serialize.h` argues for - a size that has
  -- drifted fails here, at load, rather than in a device tree of plausible
  -- nonsense.
  --------------------------------------------------------------------------

  local DEV_REQUEST = "<I4c20"          -- op, name[20]
  local DEV_FIELD   = "<I8I4c20c32"     -- number, kind, name[20], text[32]
  local DEV_HEAD    = "<I4I4"           -- error, count

  assert(#string.pack(DEV_REQUEST, 0, "") == 24,
         "namespace: the /dev request layout does not match devproto.h")
  assert(#string.pack(DEV_FIELD, 0, 0, "", "") == 64,
         "namespace: the /dev field layout does not match devproto.h")

  local DEV_OPS = { list = 1, read = 2, getattr = 3 }
  local DEV_ERRORS = {
    [1] = "the kernel would not say",
    [2] = "no such device",
    [3] = "the devices server did not understand that",
  }

  local function trim(s) return (s:gsub("%z.*$", "")) end

  local function dev_request(capability, op, rest)
    local code = DEV_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    local reply, why = sys.call_raw(capability,
                                    string.pack(DEV_REQUEST, code, rest or ""))

    if not reply then return nil, tostring(why) end

    if #reply < 8 then return nil, "a /dev reply of the wrong size" end

    local err, count = string.unpack(DEV_HEAD, reply)

    if err ~= 0 then
      return nil, DEV_ERRORS[err] or ("device error " .. tostring(err))
    end

    local names, value = {}, {}

    for i = 1, count do
      local at = 8 + (i - 1) * 64 + 1
      local number, kind, name, text = string.unpack(DEV_FIELD, reply, at)

      name = trim(name)
      names[i] = name
      value[name] = (kind == 1) and trim(text) or number
    end

    if op == "list" then return { ok = true, entries = names } end
    if op == "getattr" then return { ok = true, attrs = value } end

    return { ok = true, value = value }
  end

  --------------------------------------------------------------------------
  -- /bin, which is C and speaks `binproto.h`.
  --------------------------------------------------------------------------

  local BIN_REQUEST = "<I4I4c24"                    -- op, offset, name[24]
  local BIN_HEAD    = "<I4I4I4I4I4I4c16c16c16c16c16c16"

  assert(#string.pack(BIN_REQUEST, 0, 0, "") == 32,
         "namespace: the /bin request layout does not match binproto.h")

  local BIN_DATA = 24 + 96 + 1        -- past the header, 1-based
  local BIN_OPS = { list = 1, read = 2, getattr = 3 }
  local BIN_ERRORS = {
    [1] = "no such program",
    [2] = "this is in the image and cannot be written",
    [3] = "the /bin server did not understand that",
  }

  local function bin_request(capability, op, rest, extra)
    local code = BIN_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    local reply, why = sys.call_raw(capability,
        string.pack(BIN_REQUEST, code, (extra and extra.offset) or 0,
                    rest or ""))

    if not reply then return nil, tostring(why) end
    if #reply < BIN_DATA then return nil, "a /bin reply of the wrong size" end

    local err, count, size, length, more, windowed,
          kind, section, n1, n2, n3, n4 = string.unpack(BIN_HEAD, reply)

    if err ~= 0 then
      return nil, BIN_ERRORS[err] or ("bin error " .. tostring(err))
    end

    if op == "list" then
      local names = {}

      for i = 1, count do
        local at = BIN_DATA + (i - 1) * 24
        names[i] = trim(reply:sub(at, at + 23))
      end

      return { ok = true, entries = names }
    end

    if op == "getattr" then
      local needs = nil

      for _, w in ipairs({ n1, n2, n3, n4 }) do
        w = trim(w)

        if w ~= "" then
          needs = needs or {}
          needs[#needs + 1] = w
        end
      end

      return { ok = true, attrs = {
        size = size,
        kind = trim(kind),
        -- Applications only; a program is not in the menu at all.
        section = (windowed ~= 0) and trim(section) or nil,
        needs = needs,
      } }
    end

    return { ok = true, size = size, more = (more ~= 0),
             value = reply:sub(BIN_DATA, BIN_DATA + length - 1) }
  end

  --------------------------------------------------------------------------
  -- /app, which is C and speaks `appproto.h`.
  --
  -- The registry is not mounted like the others - `mount_registry` looks a
  -- child up on demand and mounts what comes back - so this is spoken to
  -- from two places: `request`, for `list`, and `lookup_into` above, which
  -- needs the endpoint itself rather than any bytes.
  --------------------------------------------------------------------------

  local APP_REQUEST = "<I4c24"                   -- op, name[24]
  local APP_HEAD    = "<I4I4c24"                 -- error, count, name[24]
  local APP_NAMES   = 32 + 1                     -- past the header, 1-based

  assert(#string.pack(APP_REQUEST, 0, "") == 28,
         "namespace: the /app request layout does not match appproto.h")

  local APP_OPS = { register = 1, lookup = 2, list = 3, unregister = 4 }
  local APP_ERRORS = {
    [1] = "register: no endpoint came with that",
    [2] = "no such application",
    [3] = "too many applications registered",
    [4] = "the /app registry did not understand that",
  }

  local function app_request(capability, op, name, pass)
    local code = APP_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    local reply, got = sys.call_raw(capability,
                                    string.pack(APP_REQUEST, code, name or ""),
                                    pass)

    if not reply then return nil, tostring(got) end
    if #reply < APP_NAMES - 1 then return nil, "an /app reply of the wrong size" end

    local err, count, settled = string.unpack(APP_HEAD, reply)

    if err ~= 0 then
      return nil, APP_ERRORS[err] or ("app error " .. tostring(err))
    end

    if op == "list" then
      local names = {}

      for i = 1, count do
        local at = APP_NAMES + (i - 1) * 24
        names[i] = trim(reply:sub(at, at + 23))
      end

      return { ok = true, entries = names }
    end

    return { ok = true, name = trim(settled) }
  end

  --------------------------------------------------------------------------
  -- /dev/console, which is C and speaks `conproto.h`.
  --
  -- Through the Console Kit rather than `string.pack`, and it is the only
  -- one of these four that does. The difference is that the console has two
  -- implementations: a terminal window mounts *itself* as its child's
  -- console, so an application answers this protocol as well as the server
  -- does. A format string here and a second copy in `terminal.lua` would be
  -- one layout described in two places, and only a size assertion between
  -- them. The kit compiles it once against the header.
  --------------------------------------------------------------------------

  local CON, CON_OPS

  local function console_kit()
    if CON == nil then
      CON = sys.kit("console") or false

      if CON then
        CON_OPS = { write = CON.WRITE, read = CON.READ, keys = CON.KEYS,
                    wait = CON.WAIT, pointer = CON.POINTER,
                    poll = CON.POLL, stat = CON.STAT }
      end
    end

    return CON
  end

  -- One exchange, with the reply decoded and the error turned back into a
  -- sentence. Every operation below is this plus a shape.
  local function con_call(con, capability, code, text, ticks)
    local raw, why = sys.call_raw(capability,
                                  con.encode_request{ op = code, text = text,
                                                      ticks = ticks or 0 })

    if not raw then return nil, tostring(why) end

    local rep, bad = con.decode_reply(raw)

    if not rep then return nil, bad end

    if rep.error ~= con.OK then
      return nil, con.message(rep.error)
    end

    return rep
  end

  local function con_request(capability, op, extra)
    local con = console_kit()

    if not con then
      return nil, "this image has no console kit"
    end

    local code = CON_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    --
    -- A write is a stream and the field is 1024 bytes, so a long one goes as
    -- several messages rather than being truncated.
    --
    -- The Lua console had the same limit and never said so: the text was
    -- serialised into a 2048-byte message, and a listing that did not fit
    -- failed at the boundary with an error about the message rather than
    -- about the text. This splits instead, and the limit is a number the
    -- kit publishes.
    --
    if op == "write" then
      local text = tostring(extra and extra.value or "")
      local at = 1

      repeat
        local piece = text:sub(at, at + con.TEXT_MAX - 1)
        local _, err = con_call(con, capability, code, piece)

        if err then return nil, err end

        at = at + #piece
      until at > #text

      return { ok = true }
    end

    local rep, err = con_call(con, capability, code, nil,
                              extra and tonumber(extra.ticks) or 0)

    if not rep then return nil, err end

    -- The shapes each caller already expects, unchanged from when a Lua
    -- table came back with them in it.
    if op == "read"    then return { ok = true, value = rep.line } end
    if op == "keys"    then return { ok = true, value = rep.keys } end
    if op == "poll"    then return { ok = true, value = rep.seen } end
    if op == "pointer" then return { ok = true, value = rep.pointer } end
    if op == "stat"    then return { ok = true, value = rep.stat } end

    if op == "wait" then
      return { ok = true, value = { keys = rep.keys, events = rep.events,
                                    pointer = rep.pointer } }
    end

    return { ok = true }
  end

  --------------------------------------------------------------------------
  -- /data, which is C and speaks `ramproto.h`.
  --
  -- `string.pack` rather than a kit, and the difference from the console is
  -- the whole reason that one needed a kit: ramfs has exactly one
  -- implementation, so the layout has one reader here and one in the server,
  -- and an assertion on the size is enough to catch a drift. The console has
  -- two, and a terminal is not a place to keep a copy of a struct.
  --------------------------------------------------------------------------

  local RAM_ATTR    = "I4c32c48"                     -- kind, name, value
  local RAM_REQUEST = "<I4I4I4I4I4c128" .. string.rep(RAM_ATTR, 8) .. "c1024"
  local RAM_REPLY   = "<I4I4I4I4I4c1024"

  local RAM_PATH_MAX, RAM_ATTRS_MAX = 128, 8
  local RAM_ENTRIES_MAX, RAM_DATA_MAX = 8, 1024

  assert(#string.pack(RAM_REPLY, 0, 0, 0, 0, 0, "") == 1044,
         "namespace: the /data reply layout does not match ramproto.h")

  local RAM_OPS = { list = 1, read = 2, write = 3, getattr = 4,
                    setattr = 5, query = 6, watch = 7, watchers = 8 }

  local RAM_ERRORS = {
    [1] = "no such path",
    [2] = "not a directory",
    [3] = "not readable",
    [4] = "/data did not understand that",
    [5] = "/data is full",
    [6] = "too many attributes on one node",
  }

  -- A string cut to exactly what a fixed field holds. `c128` pads a short
  -- one and refuses a long one, so the cut has to happen first.
  local function fixed(text, n)
    text = tostring(text or "")
    return (#text > n) and text:sub(1, n) or text
  end

  --
  -- A table of attributes, as the eight slots the struct has.
  --
  -- Numbers travel as text with `kind` saying they were numbers, which is
  -- what `/dev` settled: the wire carries characters either way, and the far
  -- side hands back the type that went in.
  --
  local function pack_attrs(attrs)
    local out, count = {}, 0

    for name, value in pairs(attrs or {}) do
      if count < RAM_ATTRS_MAX then
        count = count + 1
        out[#out + 1] = (type(value) == "number") and 1 or 0
        out[#out + 1] = fixed(name, 32)
        out[#out + 1] = fixed(tostring(value), 48)
      end
    end

    for _ = count + 1, RAM_ATTRS_MAX do
      out[#out + 1] = 0
      out[#out + 1] = ""
      out[#out + 1] = ""
    end

    return out, count
  end

  local function ram_pack(op, path, offset, length, count, attrs, blob, packed)
    local a = pack_attrs(attrs)

    a[#a + 1] = fixed(blob, RAM_DATA_MAX)   -- the union, zero-padded by `c`

    return string.pack(RAM_REQUEST, op, offset or 0, length or 0, count or 0,
                       packed or 0, fixed(path, RAM_PATH_MAX),
                       table.unpack(a, 1, RAM_ATTRS_MAX * 3 + 1))
  end

  -- The payload of a reply, read as whichever of the three things it is.
  local function ram_entries(blob, count)
    local out = {}

    for i = 1, math.min(count, RAM_ENTRIES_MAX) do
      local at = (i - 1) * RAM_PATH_MAX + 1
      out[i] = trim(blob:sub(at, at + RAM_PATH_MAX - 1))
    end

    return out
  end

  local function ram_attrs(blob, count)
    local out = {}

    for i = 1, math.min(count, RAM_ATTRS_MAX) do
      local at = (i - 1) * 84 + 1
      local kind, name, value = string.unpack(RAM_ATTR, blob, at)

      name = trim(name)

      if name ~= "" then
        out[name] = (kind == 1) and (tonumber(trim(value)) or trim(value))
                                or trim(value)
      end
    end

    return out
  end

  local function ram_call(capability, bytes)
    local raw, why = sys.call_raw(capability, bytes)

    if not raw then return nil, tostring(why) end
    if #raw < 1044 then return nil, "a /data reply of the wrong size" end

    local err, more, count, length, packed, blob = string.unpack(RAM_REPLY, raw)

    if err ~= 0 then
      return nil, RAM_ERRORS[err] or ("/data error " .. tostring(err))
    end

    return { more = more ~= 0, count = count, length = length,
             packed = packed ~= 0, blob = blob }
  end

  local function ram_request(capability, op, rest, extra)
    local code = RAM_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    extra = extra or {}

    if op == "write" then
      --
      -- /data holds Lua values, and this is where that survives the move to
      -- C. A string goes as itself; anything else - a table, a float, a
      -- boolean - goes as `sys.pack` and comes back through `sys.unpack`, so
      -- `help("fs")`'s promise still holds: you get back the table you wrote.
      --
      -- A write is also a stream, and `offset` is what makes it one. The Lua
      -- ramfs replaced the whole value every time, so a file larger than one
      -- message could not be written at all: the namespace split long text
      -- and each piece overwrote the last. Writing at 0 truncates, which is
      -- what `fs.write` has always meant; the rest appends.
      --
      local value = extra.value
      local text, packed

      if type(value) == "string" then
        text, packed = value, 0
      else
        text, packed = sys.pack(value), 1
      end

      local at = 0

      repeat
        local piece = text:sub(at + 1, at + RAM_DATA_MAX)
        local _, err = ram_call(capability,
                                ram_pack(code, rest, at, #piece, 0, nil,
                                         piece, packed))

        if err then return nil, err end

        at = at + #piece
      until at >= #text

      return { ok = true }
    end

    if op == "setattr" then
      local _, n = pack_attrs(extra.attrs)
      local _, err = ram_call(capability,
                              ram_pack(code, rest, 0, 0, n, extra.attrs))

      if err then return nil, err end

      return { ok = true }
    end

    if op == "watch" then
      --
      -- `known` goes in the union where a write's bytes go, as eight fixed
      -- slots, and the number of query terms rides in `offset` because a
      -- watch never pages and `count` is spoken for.
      --
      local known = {}

      for i = 1, RAM_ENTRIES_MAX do
        local p = fixed((extra.known or {})[i] or "", RAM_PATH_MAX)

        -- Each slot padded to its full width by hand, because these are
        -- concatenated into one field and `c` pads only the whole of it.
        known[i] = p .. string.rep("\0", RAM_PATH_MAX - #p)
      end

      local _, nwhere = pack_attrs(extra.where)
      local r, err = ram_call(capability,
                              ram_pack(code, rest, nwhere, 0,
                                       math.min(#(extra.known or {}),
                                                RAM_ENTRIES_MAX),
                                       extra.where, table.concat(known)))

      if not r then return nil, err end

      return { ok = true, paths = ram_entries(r.blob, r.count) }
    end

    local offset = tonumber(extra.offset) or 0
    local nterms = 0

    if op == "query" then
      nterms = select(2, pack_attrs(extra.where))
    end

    local r, err = ram_call(capability,
                            ram_pack(code, rest, offset, 0, nterms,
                                     (op == "query") and extra.where or nil))

    if not r then return nil, err end

    if op == "list" then
      return { ok = true, entries = ram_entries(r.blob, r.count),
               more = r.more, offset = offset + r.count }
    end

    if op == "read" then
      local bytes = r.blob:sub(1, r.length)

      if not r.packed then
        -- Text pages the way every other server's does, and `ns.read` above
        -- is what puts the pieces together.
        return { ok = true, value = bytes, more = r.more }
      end

      --
      -- A serialised value is reassembled *here*, not by `ns.read`.
      --
      -- The pieces are only a Lua value once all of them are present, so the
      -- generic paging loop - which concatenates strings and hands back
      -- whatever it has - cannot be the thing that finishes this. The server
      -- stores bytes and remembers what they were; the encoding is between
      -- this function and `sys.pack`.
      --
      -- The replicant is what needs it: a clock publishes a table holding
      -- its own source, which is well over one message.
      --
      local parts, more, at = { bytes }, r.more, #bytes

      while more do
        local nxt, err = ram_call(capability,
                                  ram_pack(code, rest, at, 0, 0, nil, nil, 0))

        if not nxt then return nil, err end

        parts[#parts + 1] = nxt.blob:sub(1, nxt.length)
        at = at + nxt.length
        more = nxt.more
      end

      return { ok = true, value = sys.unpack(table.concat(parts)) }
    end

    if op == "getattr" then
      return { ok = true, attrs = ram_attrs(r.blob, r.count) }
    end

    if op == "query" then
      return { ok = true, paths = ram_entries(r.blob, r.count),
               more = r.more }
    end

    if op == "watchers" then
      return { ok = true, value = r.count }
    end

    return { ok = true }
  end

  local function request(op, path, extra, pass)
    local capability, rest, _, proto = resolve(path)
    if not capability then
      -- The sentence design.md 2 asks for. Nothing was denied; there is
      -- simply no such path in this process's world.
      return nil, "no such path: " .. path
    end

    if proto == "console" then
      return con_request(capability, op, extra)
    end

    if proto == "ram" then
      return ram_request(capability, op, rest, extra)
    end

    if proto == "dev" then
      return dev_request(capability, op, rest)
    end

    if proto == "bin" then
      return bin_request(capability, op, rest, extra)
    end

    if proto == "app" then
      -- The name is whatever is left of the path after the mount prefix,
      -- with the slash `resolve` leaves on the front taken off.
      return app_request(capability, op,
                         (rest or ""):match("([^/]+)$") or "",
                         extra and extra.pass)
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
    local capability, rest, _, proto = resolve(path)

    if not capability then
      return nil, "no such path: " .. path
    end

    --
    -- A server with a declared protocol does not take arbitrary tables.
    --
    -- `send` is the generic escape hatch - whatever you put in the table
    -- reaches the server - and that is exactly what a struct server must
    -- not be given. Refused here, with a sentence, because the alternative
    -- is what happened the first time: a `send` to a path under `/dev` that
    -- named nothing was answered by the C devices server, its struct reply
    -- was unpacked as a Lua value, and the caller crashed indexing a
    -- number. Before the conversion the same mistake produced "no such
    -- device", which is the behaviour to keep.
    --
    --
    -- A server with a declared protocol does not take arbitrary tables.
    --
    -- `send` is the generic escape hatch - whatever is in the table reaches
    -- the server - and that is exactly what a struct server must not be
    -- handed. Where the protocol has an operation for what was asked, this
    -- routes to it; where it does not, it refuses with a sentence.
    --
    -- The alternative is what happened before the check existed: a `send`
    -- to a path under `/dev` that named nothing was answered by the C
    -- devices server, its struct reply was unpacked as a Lua value, and the
    -- caller crashed indexing a number. "No such device" is the behaviour
    -- worth keeping.
    --
    if proto == "app" then
      return app_request(capability, tostring(message.type or ""),
                         tostring(message.name or ""), pass)
    end

    if proto then
      return nil, ("/" .. tostring(proto) .. " speaks a fixed protocol; "
                   .. "there is no `send` to it")
    end

    local req = { path = rest }
    for k, v in pairs(message) do req[k] = v end

    local reply, err = sys.call(capability, req, pass)
    if not reply then return nil, err end
    if not reply.ok then return nil, reply.error end
    return reply
  end

  --
  -- A call whose payload is bytes rather than a table.
  --
  -- The namespace still resolves the path - that is what a namespace is for
  -- and it is unchanged - but what travels afterwards is opaque to this
  -- server. A C server reads a struct; only the client library and that
  -- server know its shape, and they share a header that says so.
  --
  function ns.raw(path, bytes, pass)
    local capability = resolve(path)

    if not capability then
      return nil, "no such path: " .. path
    end

    return sys.call_raw(capability, bytes, pass)
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
  --
  -- The one call on the frame path, and the only one that gets its own door.
  --
  -- The window manager makes this every pass, sixty times a second, whether
  -- or not anything happened - and going the ordinary way meant a 1036-byte
  -- Lua string for the request, a 1400-byte one for the reply, and five
  -- tables, all dropped immediately. `frames` measured it at 3.63 KB a pass,
  -- eighty-six per cent of everything the desktop allocated, against 0.02 KB
  -- for composing.
  --
  -- `con.wait` does the whole exchange in C and fills a table this keeps, so
  -- the steady state allocates nothing at all. The table is reused, which is
  -- the price: what reads it must be done before the next call. The window
  -- manager is - it uses the answer inside the pass that asked for it.
  --
  -- Nothing else needs this. It is here because a *measurement* said so, and
  -- if a second call ever shows up on a frame path the answer is another
  -- door rather than a general mechanism nobody needed yet.
  --
  local wait_out = {}

  function ns.wait_input(path, ticks)
    local con = console_kit()
    local capability, _, _, proto = resolve(path)

    if con and capability and proto == "console" then
      local got, why = con.wait(capability, tonumber(ticks) or 0, wait_out)

      if got then return got end

      return nil, why and con.message(why) or "the console did not answer"
    end

    -- Whatever is mounted there does not speak the console protocol - a
    -- terminal window, say. The ordinary path still works and still costs
    -- what it costs, which nothing on a frame path pays.
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

--
-- No handlers here: the console is `user/servers/console.c`, and `main.c`
-- dispatches role 4 to it before the interpreter is opened.
--
-- It is the fifth server to move and the first whose protocol something
-- other than a server implements. A terminal window mounts itself as its
-- child's `/dev/console`, so `terminal.lua` answers `conproto.h` too -
-- through `use("/kits/console")`, which is the same header compiled once
-- rather than a format string copied into an application.
--
-- What the move bought, beyond a server with no collector on the path every
-- `print` in the system takes: `read` no longer blocks inside a handler
-- pumping its own mailbox. It records who asked and answers from the loop,
-- so nothing re-enters and a half-typed line costs a receive with a
-- deadline instead of a `sys.yield` spin.
--

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

--
-- No `binfs` here: /bin is served by `user/servers/binfs.c`, and `main.c`
-- dispatches role 11 before the interpreter is opened. The program store it
-- reads is the array `tools/progs2c.py` now emits beside the Lua chunk -
-- which also means /bin no longer costs a Lua `load` of four hundred
-- kilobytes at boot to get a table of sources.
--

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

--
-- No `appfs` here: /app is served by `user/servers/appfs.c`, and `main.c`
-- dispatches role 14 before the interpreter is opened.
--


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

    --
    -- A new name for the same file, in the same directory.
    --
    -- Also not one of the five, and here for the same reason as `mkdir`: it
    -- is a real filesystem operation, and this protocol is ours. What makes
    -- it worth having rather than leaving to the caller is that it edits a
    -- directory entry and copies nothing - renaming a four-megabyte song by
    -- copy-and-delete would read and write four megabytes to change eleven
    -- characters, and would leave two files or none if it failed halfway.
    --
    rename = function(req)
      local sb = mounted()

      if not sb then
        return { ok = false, error = "there is no filesystem here" }
      end

      local name = req.path:match("([^/]+)$")

      if not name or RESERVED[name] then
        return { ok = false, error = "that name is reserved" }
      end

      if type(req.to) ~= "string" or RESERVED[req.to] then
        return { ok = false, error = "that name is reserved" }
      end

      local ok, err = atomic(sb, kfs.rename, req.path, req.to)

      if not ok then
        return { ok = false, error = tostring(err) }
      end

      -- Both names changed: the old one is gone and the new one is here,
      -- and a watcher on either has to hear about it.
      touched(sb, req.path)
      -- The new path is the old one with its last component replaced. The
      -- extra parentheses drop `gsub`'s count, which would otherwise arrive
      -- as a second argument.
      touched(sb, (req.path:gsub("[^/]+$", req.to)))

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

local function diskfs_main(endpoint)
  local kfs = assert(load(sys.libraries(), "libraries"))()["kfs.lua"]

  serve(endpoint, { kfs = assert(load(kfs, "kfs.lua"))() }, diskfs_handlers)
end

--
-- No `devices_main`: the devices server is C. `user/servers/devices.c` is
-- the whole of it and `main.c` dispatches role 9 before Lua is opened.
--

local RUNNER_ROLE = ROLE_RUNNER

local function shell_main(console_cap, ramfs_cap, devices_cap, bin_cap,
                          lib_cap, app_cap, disk_cap, audio_cap)
  local ns = new_namespace()
  ns.mount("/dev/console", console_cap, nil, "console")
  ns.mount("/data", ramfs_cap, nil, "ram")

  -- Longest prefix wins, so /dev/console keeps going to the console server
  -- while everything else under /dev goes to the device server. Two servers
  -- under one directory, and neither knows about the other - which is what a
  -- per-process mount table buys.
  ns.mount("/dev", devices_cap, nil, "dev")

  --
  -- Over the top of `/dev`, because longest prefix wins.
  --
  -- `/dev/audio` is a different server from the one that answers the rest
  -- of `/dev`, exactly as `/dev/console` is - the devices server describes
  -- hardware and this one *is* a piece of it. Mounted for everybody rather
  -- than passed to children the way `/app/wm` is, because any program may
  -- ask to make a noise and the answer is a stream with a volume on it
  -- rather than a refusal.
  --
  if audio_cap then ns.mount("/dev/audio", audio_cap) end

  -- The programs this image carries. Read-only, and served by a process of
  -- its own like everything else.
  ns.mount("/bin", bin_cap, nil, "bin")

  -- And what programs load rather than run. Separate from /bin so that `ls
  -- /bin` lists things you can type and nothing else.
  ns.mount("/lib", lib_cap, nil, "bin")

  -- What is running, and what each one exposes. A registry rather than a
  -- mount: the names under it appear and disappear with the programs.
  ns.mount_registry("/app", app_cap, "app")

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

Attributes, and a query that finds by them rather than by name:

  fs.write("/data/a", "one")
  fs.setattr("/data/a", { kind = "note" })
  fs.query("/data", { kind = "note" })

BeOS's idea: the filesystem is a database, and a folder is a saved
query. `find` and `watch` are built on exactly these two calls.
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

--
-- No branch for ROLE_DEVICES: the devices server is C, and
-- `user/init/main.c` dispatches it before the interpreter is opened.
--


if role == ROLE_DISKFS then
  sys.name("diskfs")
  diskfs_main(CAP)
  return
end

--
-- No branch for ROLE_AUDIO here, and its absence is the point.
--
-- The audio server is C and `user/init/main.c` dispatches it before the
-- interpreter is opened, so a process serving /dev/audio never has a
-- `lua_State` at all. `CLAUDE.md` says a server runs on behalf of another
-- process and therefore does not get a collector; the way to mean that is
-- for there to be no collector in the process, rather than a promise not to
-- allocate that somebody adds a `print` to next month.
--

--
-- No branch for ROLE_LIBFS or ROLE_BINFS: both are served by
-- `user/servers/binfs.c`, which is the same code over a different array.
--


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
  -- `"console"` whoever is behind it. A terminal window mounts itself here
  -- for its child, and a terminal speaks the same protocol the server does -
  -- through the same kit, which is the whole reason that kit exists. The
  -- runner cannot tell the two apart and must not need to.
  if req.console then ns.mount("/dev/console", req.console, nil, "console") end
  if req.data    then ns.mount("/data",        req.data, nil, "ram") end
  if req.bin     then ns.mount("/bin",         req.bin, nil, "bin") end
  if req.devices then ns.mount("/dev",         req.devices, nil, "dev") end
  if req.lib     then ns.mount("/lib",         req.lib, nil, "bin") end
  if req.app     then ns.mount_registry("/app", req.app, "app") end
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
      ns.mount(m.path, m.index, nil, m.proto)
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
  -- name in the child's namespace: run(path, args, detach, { ["/app/wm"] = c }).
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

    --
    -- A share carries its protocol, because a capability is not enough to
    -- say what is behind it.
    --
    -- This passed the index alone, and the child mounted it speaking Lua
    -- tables - which was right while every server did. It stopped being
    -- right when `/dev/console` became a struct: a terminal shares its own
    -- endpoint there, the child mounted it with no protocol, and a `write`
    -- arrived at the terminal as 83 bytes of serialised table where 1036
    -- bytes of `con_request` were expected.
    --
    -- The parent is the one that knows. It is asserting "I am a console" by
    -- mounting itself at that path, and the protocol is the other half of
    -- that sentence. A bare capability still works and still means tables,
    -- which is what every share before this one meant.
    --
    if shares then
      for path_, share in pairs(shares) do
        local cap, proto = share, nil

        if type(share) == "table" then
          cap, proto = share.cap, share.proto
        end

        caps[#caps + 1] = cap
        mounts[#mounts + 1] = { path = path_, index = #caps - 1,
                                proto = proto }
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
local mount_point = (role == ROLE_CLIENT_B) and "/files" or "/data"

local fs = new_namespace()

-- `"ram"` because the server on the other end is `user/servers/ramfs.c` and
-- speaks `ramproto.h`. A mount with no protocol means Lua tables, which is
-- what this said while the ramfs was Lua and what made both of these checks
-- fail the moment it was not.
fs.mount(mount_point, CAP, nil, "ram")

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
