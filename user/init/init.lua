-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
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
local ROLE_DEVICES   = 9  -- serves /Devices: what hardware was found
local ROLE_BINFS     = 11 -- serves /bin: the programs carried in the image
local ROLE_RUNNER    = 12 -- runs one program, in an address space of its own
local ROLE_LIBFS     = 13 -- serves /Kosmos/Libraries: the libraries carried in the image
local ROLE_APPFS     = 14 -- serves /Running: what each running program exposes
local ROLE_DISKFS    = 15 -- serves /Home: the block device, and only it
local ROLE_AUDIO     = 16 -- serves /Devices/audio: the one process that may play
local ROLE_NET       = 17 -- serves /Network: the one process that holds the card
local ROLE_POWERBUTTON = 18 -- drives the power key, where there is one
local ROLE_XHCI       = 19 -- drives the USB host controllers, where there are any
local ROLE_DRIVES     = 20 -- serves /Drives: every volume on every drive, read only
local ROLE_BACKLIGHT  = 21 -- Intel's backlight PWMs: /Devices/backlight
local ROLE_E1000      = 22 -- an Intel Ethernet controller, where there is one
local ROLE_NOTIFY     = 23 -- serves /Notifications: what applications have said
local ROLE_SMBFS      = 24 -- shares over the network: SMB 2 and 3 (`sharing.md`)

--
-- **A program's own image** (`docs/elf.md` step 4). A program whose header
-- says `-- kosmos: image doom.elf` runs in that image - the file of that name
-- beside it - rather than in this process's own: the way a program with C in
-- it is installed rather than compiled in. The shell's `run_program` and the
-- runner's `launch` both start a program through `IMAGES.spawn`, so the two
-- ways of starting one cannot disagree about it.
--
-- The file may be tens of megabytes and a process's heap is two, so no byte
-- of it passes through Lua: its first four kilobytes are read and planned
-- (`sys.elf_plan`), and each segment is read a window at a time into one
-- scratch region and copied from there to where it belongs in the image
-- (`sys.region_copy`). A region mapped here stays mapped - a share cannot be
-- unmapped - so the image is made once a file and kept, and the next start
-- of the same file begins from it.
--
local IMAGES = { WINDOW = 256 * 1024, made = {}, window = nil,
                 -- Where it says so when it makes one: Lua's `print`, which
                 -- reaches the log from any program; the shell writes to its
                 -- console instead, and says so where it is typed.
                 say = print }

-- The name in a program's header, or nil. The header is the comment its file
-- opens with, blank lines and all.
function IMAGES.named(ns, path)
  local source = ns.read(path)

  if type(source) ~= "string" then return nil end

  for line in (source .. "\n"):gmatch("(.-)\n") do
    if line ~= "" and not line:match("^%-%-") then break end

    local name = line:match("^%-%-%s*kosmos:%s*image%s+(%S+)")

    if name then return name end
  end

  return nil
end

-- The image `file` holds, made the first time and kept: the region and its
-- length, or nil and why - the file named, and the rule it broke.
--
-- **`pace`, when given, is called between windows** - after each one has been
-- copied into the image, so the scratch window is free again. The window
-- manager passes `coroutine.yield`: it starts an application while it keeps
-- drawing, and an installed one is eighteen megabytes read off a disk, which
-- on the M700's stick held the whole desktop still for a second (Diego, 29
-- September). Everyone else passes nothing and waits, as a command line does.
function IMAGES.load(ns, file, pace)
  local attrs = ns.getattr(file)
  local size = attrs and attrs.size

  if math.type(size) ~= "integer" or size <= 0 then
    return nil, file .. ": not there"
  end

  local key = ("%s:%d:%s"):format(file, size, tostring(attrs.mtime or ""))
  local made = IMAGES.made[key]

  if made then return made.region, made.length end

  IMAGES.window = IMAGES.window or sys.memory(IMAGES.WINDOW // 4096)

  if not IMAGES.window then return nil, "no room to read " .. file end

  local function bytes_at(offset, n)
    local got = ns.read_into(file, IMAGES.window, offset, n)

    if got ~= n then return nil end

    return sys.region_read(IMAGES.window, 0, n)
  end

  local first = bytes_at(0, math.min(size, 4096))

  if not first then return nil, file .. ": could not be read" end

  local plan, why = sys.elf_plan(first, size)

  if plan then
    plan, why = sys.elf_plan(first, size, bytes_at(plan.head, 16) or "")
  end

  if not plan then return nil, file .. ": " .. tostring(why) end

  local region = sys.memory((plan.length + 4095) // 4096)

  if not region then return nil, "no room for " .. file .. "'s image" end

  for _, seg in ipairs(plan.segments) do
    local done = 0

    while done < seg.size do
      local want = math.min(IMAGES.WINDOW, seg.size - done)

      if ns.read_into(file, IMAGES.window, seg.offset + done, want) ~= want then
        return nil, ("%s: stopped after %d bytes"):format(file, seg.offset + done)
      end

      sys.region_copy(region, seg.at + done, IMAGES.window, 0, want)
      done = done + want

      if pace then pace() end
    end
  end

  IMAGES.made[key] = { region = region, length = plan.length }

  -- Said once an image, when it is made: the next start of the same file
  -- says nothing, which is how a start from the kept one can be told apart.
  IMAGES.say(("image: made %s, %d bytes"):format(file, plan.length))
  return region, plan.length
end

-- A runner for `path`, in its own image if it names one and in this
-- process's otherwise: the child's id, or nil and why. `pace` is
-- `IMAGES.load`'s.
function IMAGES.spawn(ns, path, role, caps, flags, pace)
  local image = IMAGES.named(ns, path)

  if not image then
    local id, why = sys.spawn(role, caps, flags)

    return id, id == nil and ("could not start a process for it: " .. tostring(why)) or nil
  end

  if image:find("/", 1, true) or image == "." or image == ".." then
    return nil, image .. ": an image is a file beside its program"
  end

  local region, length = IMAGES.load(ns, (path:match("^(.*)/[^/]*$") or "") .. "/" .. image,
                                     pace)

  if not region then return nil, length end

  local id, why = sys.spawn_image(region, length, role, caps, flags)

  if not id then return nil, image .. ": " .. tostring(why) end

  return id
end

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

--
-- And the same question about the network card, for the fourth time.
--
-- Through `sys.info` rather than `sys.net`, and that is the whole lesson of
-- this one: `sys.net` is owner-only and has to be, because a MAC is an
-- identity and whoever can read frames can read everybody's. But *whether
-- there is a card* is a fact about the machine, and the shell has to be able
-- to ask it without holding one. Asking the wrong way made every launch say
-- "this process does not hold the network card".
--
-- Cached like the other two because it cannot change while the machine runs.
--
local net_seen = nil

local function may_pass_net()
  if net_seen == nil then
    local i = sys.info()

    net_seen = (i ~= nil) and (i.net_mtu or 0) > 0
  end

  return net_seen
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

-- The audio band (`kernel/sched.h`): a thread of this process's may put
-- itself above every program, under the scheduler's budget. What `needs
-- audio` grants - a program that makes sound - and not the device, which is
-- SPAWN_AUDIO and the audio server's alone. Not hardware, so every machine
-- has it to give.
local SPAWN_AUDIO_BAND = 128

-- Where every processor is, a tick at a time (`kernel/profile.c`): what
-- `needs profile` grants, to `profile` and to whatever starts it. Not
-- hardware, so every machine has it to give.
local SPAWN_PROFILE = 256

-- The network card. The disk's grant, pointed outwards: a process that can
-- put a raw frame on the wire can claim any address on the network and read
-- every frame that reaches the machine, whatever any namespace says. So one
-- process gets it - the stack - and everything else asks that one.
local SPAWN_NET     = 32

-- Hardware itself: the right to be told where a device is, map its
-- registers and claim its interrupt. The kernel gives it to init alone, and
-- init passes it to a driver - never to anything that merely wants to talk
-- to one, which asks the driver instead.
local SPAWN_DEVICES = 64

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
-- stream and a file is not: two writes to /Devices/console are one line after
-- another, and two writes to /Temporary/notes are the second replacing the
-- first. Only the caller knows which it meant.
--------------------------------------------------------------------------
local CONSOLE_CHUNK = 1400

--------------------------------------------------------------------------
-- The colours a console program may ask for, by name.
--
-- The wire carries 0xAARRGGBB and nothing else, so this is resolved on the
-- way out rather than travelling as a name. That is a decision and not a
-- shortcut: **a console program has no theme.** It never loads `ui.lua`,
-- and the console it is writing to may be the kernel's, which exists before
-- the desktop and has no palette to consult. Sending a name would mean
-- every reader of the protocol carrying a table like this one, and the
-- kernel is not going to.
--
-- The names and values are the theme's dark palette, because a console is
-- dark in both themes - `console` is 0xff0b0b0b either way.
--
-- What this gives up, and it is worth naming: output does not follow the
-- theme when it changes. If that turns out to matter, the field is already
-- the right width to carry a name instead, and the decision moves rather
-- than the protocol.
--------------------------------------------------------------------------
local CONSOLE_COLOURS = {
  text   = 0xffd8d8d8,          -- what the console draws in anyway
  dim    = 0xff8b949e,
  good   = 0xff3fb950,
  bad    = 0xffda3633,
  accent = 0xff1f6feb,
  tab    = 0xffffc700,          -- the BeOS yellow
  ring   = 0xff58a6ff,
}

--
-- A colour argument, however it was spelled. `nil` and an unknown name both
-- come back as 0, which every reader takes to mean "the console's own" - so
-- a typo is plain text rather than an error, which is the right failure for
-- something whose whole job is decoration.
--
local function colour_of(want)
  if type(want) == "number" then return want end
  return CONSOLE_COLOURS[want] or 0
end

local function write_text(ns, path, text, colour)
  if #text <= CONSOLE_CHUNK then
    return ns.write(path, text, colour)
  end

  local at = 1

  while at <= #text do
    local piece = text:sub(at, at + CONSOLE_CHUNK - 1)
    local ok, err = ns.write(path, piece, colour)

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
-- which is the whole point - `fs.read("/Devices/temp")` gives `{ celsius = 47.2 }`
-- rather than "47200\n" for whoever asked to parse.
--
-- **What carries those verbs depends on who answers**, and that is the
-- change this file has been through. Every system server is C and takes a
-- *declared struct* - `/Devices`, `/Kosmos`, `/Running`, `/Temporary`,
-- `/Home` and the rest - each with a header in `user/include/` that both
-- sides compile against. A mount names which, and `request` below branches
-- on it.
--
-- Only applications' own names in `/Running` still take a table with a
-- `type`, and design.md 14 makes that field mandatory: with no static
-- types, a message that does not say what it is becomes a silent nil three
-- layers down. The window manager and application scripting speak this way
-- and always will - their vocabularies are open, which is exactly when a
-- table is right.
--
-- The two are not a compromise between them. A struct is for a boundary
-- where the shape is agreed and a caller being wrong should be impossible to
-- express; a table is for one where the shape is the caller's to choose.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- /Temporary is `user/servers/ramfs.c`, and `main.c` dispatches role 1 to it
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
-- /Devices, which is C and speaks `devproto.h`.
--
-- **Out here, not inside the namespace**, since 27 September: the disk
-- server has no namespace and is handed the devices endpoint to read the
-- clock from (`roadmap.md` 6za step b), and a second copy of these layouts
-- would be a third place `devproto.h` is written. The namespace and the
-- disk server both ask through this one.
--------------------------------------------------------------------------

local DEV_REQUEST = "<I4c20"          -- op, name[20]
local DEV_FIELD   = "<I8I4c20c32"     -- number, kind, name[20], text[32]
local DEV_HEAD    = "<I4I4"           -- error, count

assert(#string.pack(DEV_REQUEST, 0, "") == 24,
       "namespace: the /Devices request layout does not match devproto.h")
assert(#string.pack(DEV_FIELD, 0, 0, "", "") == 64,
       "namespace: the /Devices field layout does not match devproto.h")

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

  --
  -- A name longer than the field is no device the server holds, and is
  -- answered as one. `string.pack` would raise instead - ending whoever
  -- asked with a line about packing - and cutting the name could find a
  -- different device that shares its first twenty bytes.
  --
  if #(rest or "") > 20 then return nil, DEV_ERRORS[2] end

  local reply, why = sys.call_raw(capability,
                                  string.pack(DEV_REQUEST, code, rest or ""))

  if not reply then return nil, tostring(why) end

  if #reply < 8 then return nil, "a /Devices reply of the wrong size" end

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
  -- **A name is found whatever its case, and keeps the case it was given**
  -- (`roadmap.md` 6s): `/HOME/notes.txt` reaches the mount at `/Home`, and
  -- what is left of the path goes to the server as it was typed, for the
  -- server to fold in its own way. `key` is a mount's prefix folded once,
  -- when it is mounted, so a lookup folds only the path it is asked about.
  --
  local function fold(path) return path:lower() end

  -- `path` is `prefix`, or inside it, whatever the case of either.
  local function within(path, key)
    local p = fold(path)

    return p == key or p:sub(1, #key + 1) == key .. "/"
  end

  --
  -- `root` is which part of the server appears here. Left out, the whole
  -- of it does, which is what every mount did before subtrees existed.
  --
  -- `proto` is which protocol the server speaks, and **a server in C always
  -- has one**: left out means a server that takes tables, and the namespace
  -- will send it tables. `request` refuses a table to any protocol it does
  -- not route.
  --
  --
  -- `also` is a second server behind the same name, asked about what is
  -- under it rather than about the name itself: smbfs beside the stack at
  -- `/Network` (`docs/sharing.md`, *`/Network` is already somebody's*).
  -- Only the `share_*` calls below take it.
  --
  function ns.mount(prefix, capability, root, proto, also)
    --
    -- Mounting over a prefix replaces what was there.
    --
    -- Without this, two mounts at the same path both sit in the table and
    -- which one answers depends on how `table.sort` happens to order two
    -- equal keys - and Lua's sort is not stable, so it can differ between
    -- runs of the same program.
    --
    -- The case that needs it is a terminal: it hands its child a
    -- `/Devices/console` of its own, and the child's runner has already mounted
    -- the real one there. The child must get exactly one console and it
    -- must be the terminal's.
    --
    for i, m in ipairs(mounts) do
      if m.key == fold(prefix) then
        table.remove(mounts, i)
        break
      end
    end

    mounts[#mounts + 1] = { prefix = prefix, key = fold(prefix), cap = capability,
                            root = root, proto = proto, also = also }

    -- Longest prefix first, so /a/b wins over /a regardless of mount order.
    table.sort(mounts, function(x, y) return #x.prefix > #y.prefix end)
  end

  --
  -- Directories whose children are looked up when they are first used.
  --
  -- `/Running` is one: an application registers itself while it runs, so what is
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

  -- The names `lookup_into` mounted, by their folded prefix: what a registry
  -- said once, which `forget_if_gone` drops when it stops being true.
  local looked_up = {}

  local function lookup_into(prefix, cap, path)
    local name = path:sub(#prefix + 2):match("^([^/]+)")

    if not name then return nil end

    --
    -- Raw, because `/Running` is C. The capability comes back beside the bytes
    -- as it always did: `sys.call_raw` returns the reply's payload and the
    -- endpoint travels in the message rather than in it.
    --
    --
    -- Cut to the 23 bytes a registered name keeps, as `app_request` cuts it,
    -- so a window registered under a long title is found by that title -
    -- and `string.pack` never sees a string longer than its field.
    --
    local reply, got = sys.call_raw(cap, string.pack("<I4c24", 2, name:sub(1, 23)))

    if not reply or #reply < 4 or string.unpack("<I4", reply) ~= 0
       or not got or got < 0 then
      return nil
    end

    ns.mount(prefix .. "/" .. name, got)
    looked_up[fold(prefix .. "/" .. name)] = true
    return true
  end


  local function match(path)
    for _, m in ipairs(mounts) do
      if within(path, m.key) then
        local rest = path:sub(#m.prefix + 1)

        if rest == "" then rest = "/" end

        -- A mount may name a *subtree* of what the server holds: `/Home`
        -- is the disk's `/Home` folder rather than the whole of it, and
        -- one disk could appear at several places without several
        -- servers - which is what made the layout in `layout.md` possible
        -- with one disk and one server.
        --
        -- Prepended here rather than by the server, because the server has
        -- no idea what anybody mounted it as and should not: it answers
        -- about paths in its own space, and this is the one place that
        -- knows how this process's names map onto them.
        if m.root then
          rest = (rest == "/") and m.root or (m.root .. rest)
        end

        return m.cap, rest, m.prefix, m.proto, m.root
      end
    end
    return nil
  end

  --
  -- **A name looked up is forgotten when what it named has gone.**
  --
  -- `resolve` below looks a registry's name up once and mounts it, so a
  -- program that keeps running keeps the capability it was given - for the
  -- process that was there then. When that process ends and another takes
  -- the name, every call from here went to the dead endpoint and failed:
  -- Preferences, open the whole time, moved the Deskbar to the bottom once
  -- and never again, because the Deskbar it moved had started itself again
  -- (Diego, 3 October: "it changes only one time and then does not change
  -- any more times"). `setprop`, new each time, looked the name up fresh.
  --
  -- So a call that finds the endpoint gone drops the mount and gives the
  -- capability back, and the caller asks once more - which looks the name
  -- up again. Only for names a registry gave: a mount somebody made by hand
  -- is theirs to replace.
  --
  local function forget_if_gone(path, err)
    if err ~= "no such capability" and err ~= "the endpoint was destroyed" then
      return false
    end

    local _, _, prefix = match(path)
    local key = prefix and fold(prefix)

    if not key or not looked_up[key] then return false end

    looked_up[key] = nil

    for i, m in ipairs(mounts) do
      if m.key == key then
        table.remove(mounts, i)
        pcall(sys.release, m.cap)
        break
      end
    end

    return true
  end

  local function resolve(path)
    --
    -- A registry is asked *before* the answer is taken, not after it fails.
    --
    -- The first version only looked a name up when nothing matched, and
    -- nothing ever failed to match: `/Running` is mounted, so `/Running/gallery` hit
    -- the registry itself with `/gallery` left over, and the registry was
    -- asked to read a property it has never heard of. The symptom was a
    -- clean, wrong answer - "no such operation: write" from a directory.
    --
    -- So: if the deepest thing matching is a registry and there is more path
    -- after it, look the child up. The mount that produces is longer, so the
    -- ordinary longest-prefix rule picks it from then on and this costs one
    -- exchange the first time and nothing afterwards.
    --
    local cap, rest, prefix, proto, root = match(path)

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

    if cap then return cap, rest, prefix, proto, root end

    -- Nothing matched at all. Still worth asking, for a registry mounted
    -- somewhere this path only partly overlaps.
    for _, a in ipairs(autos) do
      if within(path, fold(a.prefix)) and fold(path) ~= fold(a.prefix) then
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
  --
  -- **A path in its mount's own spelling** (`roadmap.md` 6s): `/home/x` is
  -- `/Home/x`, answered from the mount table without asking a server. What
  -- follows the mount keeps the case it was typed in - that part is the
  -- server's, and spelling it would be a walk. A path a person types comes
  -- through here (`files.abs`, `cd`), so one compared with the system's own
  -- - the Trash, the Desktop - is written the way they are.
  --
  function ns.canonical(path)
    path = tostring(path or "")

    for _, m in ipairs(mounts) do
      if within(path, m.key) then
        return m.prefix .. path:sub(#m.prefix + 1)
      end
    end

    return path
  end

  --
  -- **A program's file by its name** (`roadmap.md` 6s c2): in `/Kosmos/Apps`
  -- or `/Kosmos/Programs`, whichever holds it - a name is in one of the two,
  -- as its header decides, so the order asked in never changes the answer.
  -- A name with a slash is a path already. Here rather than in a library
  -- because every program has its namespace as `fs` and the shell has it as
  -- `ns`, so the prompt, the window manager and a launcher ask the same
  -- question the same way, with nothing to load; `/Kosmos/Programs/<name>`
  -- when neither has it, for the refusal to name.
  --
  function ns.program(name)
    name = tostring(name or "")

    -- A launcher or a startup list written before `/bin` was split, on 27
    -- September, says `/bin/clock.lua`: the program of that name, wherever
    -- it is now. A person's file is not rewritten to say it.
    name = name:match("^/[Bb][Ii][Nn]/([^/]+)%.lua$") or name

    -- **And Editor, which is Text Editor** since 28 September (`roadmap.md`
    -- 6zs): a launcher or a startup list that says `editor` starts it, the
    -- same way, rather than being rewritten.
    if name == "editor" then name = "texteditor" end

    if name:sub(1, 1) == "/" then return name end

    for _, dir in ipairs({ "/Kosmos/Apps", "/Kosmos/Programs" }) do
      local path = dir .. "/" .. name .. ".lua"

      if ns.getattr(path) then return path end
    end

    --
    -- **And an installed application** (`docs/elf.md` step 5): one folder
    -- in `/Home/Apps`, its program the Lua file named after it -
    -- `/Home/Apps/Doom/doom.lua` for `doom`, whatever the folder's case.
    --
    if name:match("^[%w_%-]+$") then
      local path = "/Home/Apps/" .. name .. "/" .. name .. ".lua"

      if ns.getattr(path) then return path end
    end

    return "/Kosmos/Programs/" .. name .. ".lua"
  end

  function ns.mount_registry(prefix, capability, proto)
    ns.mount(prefix, capability, nil, proto)
    autos[#autos + 1] = { prefix = prefix, cap = capability }
  end

  --
  -- **A name of this process's own, answered in this process** (`roadmap.md`
  -- 6zb). A window registers its title under `/Running` with an endpoint it
  -- drains between frames, and `/Running/Tracker` is reached by calling that
  -- endpoint - so Tracker opening its own name called itself, and waited
  -- for an answer only it could give. The kernel cannot tell a call to
  -- yourself from any other; the process can. So the window says which name
  -- is its own and how it answers, and a request for anything under that
  -- name is handed to it here, as `/Kosmos/Kits` is answered here. `nil`
  -- takes the name back.
  --
  local own = {}

  function ns.answer_here(path, answer)
    own[fold(tostring(path))] = answer
  end

  -- The reply, when `prefix` is a name of this process's own; nothing
  -- otherwise, and the request goes out as it always did.
  local function answered_here(prefix, req)
    local answer = prefix and own[fold(prefix)]

    if not answer then return nil end

    local ok, reply = pcall(answer, req)

    if not ok then return { ok = false, error = tostring(reply) } end

    return reply or { ok = false, error = "no answer" }
  end

  -- `pass` is a capability travelling with the request, which the kernel
  -- translates into the server's own index for the same object. It is how
  -- a buffer is handed over for a large read: the pages, not the bytes.
  --------------------------------------------------------------------------
  -- Servers that speak a struct rather than a table.
  --
  -- **Every system server does, since `/Home` moved on 29 September.** A
  -- mount says which protocol the server on the other side speaks, and this
  -- kit packs accordingly. The only mounts without one are applications'
  -- own names in `/Running`, which take tables and always will: what a
  -- window answers is the application's to choose, which is exactly when a
  -- table is right.
  --
  -- It lives here because this is the client half of the boundary: the
  -- namespace is the kit that knows how to talk to servers, so knowing that
  -- one of them wants 24 bytes of struct is exactly its business.
  --
  -- The layouts mirror `user/include/devproto.h`, which is the second and
  -- last place they are written. The asserts below are what stands in for
  -- the single implementation `serialize.h` argues for - a size that has
  -- drifted fails here, at load, rather than in a device tree of plausible
  -- nonsense.
  --------------------------------------------------------------------------

  --------------------------------------------------------------------------
  -- /Drives: every volume on every drive, read only (USB step 6b).
  --
  -- `user/servers/drives.c` is the server and `drivesproto.h` the shapes.
  -- Its own protocol rather than /Temporary's, because a read-only drive server
  -- would implement three of that one's eleven operations and refuse eight.
  --
  -- **The sizes are the compiler's, not arithmetic done here.** Every field
  -- below was measured with `offsetof` against `drivesproto.h` before these
  -- formats were written: 280, 1056, 104 and 80 bytes. Lua packs without
  -- alignment and C pads, so the two agree only by luck unless somebody
  -- checks - and the asserts are what turn luck into a load-time failure.
  --------------------------------------------------------------------------

  local DRIVES_REQUEST = "<I4I4I8I4I4c256"    -- op, offset, at, length, _, path
  local DRIVES_REPLY   = "<I4I4I4I4I8I4I4c1024"
  local DRIVES_VOLUME  = "<c64I4I4I8I8I4I4I4I4c16"   -- ..., readable, id_kind, id
  local DRIVES_VOLUME_BYTES = 120
  local DRIVES_ENTRY   = "<c64I8I4I4"

  assert(#string.pack(DRIVES_REQUEST, 0, 0, 0, 0, 0, "") == 280,
         "namespace: the /Drives request layout does not match drivesproto.h")
  assert(#string.pack(DRIVES_REPLY, 0, 0, 0, 0, 0, 0, 0, "") == 1056,
         "namespace: the /Drives reply layout does not match drivesproto.h")
  assert(#string.pack(DRIVES_VOLUME, "", 0, 0, 0, 0, 0, 0, 0, 0, "")
         == DRIVES_VOLUME_BYTES,
         "namespace: the /Drives volume layout does not match drivesproto.h")
  assert(#string.pack(DRIVES_ENTRY, "", 0, 0, 0) == 80,
         "namespace: the /Drives entry layout does not match drivesproto.h")

  -- Cut to the field and zero-padded to it. `string.pack`'s `c` raises on a
  -- string longer than the field, and the namespace already has a `fixed`
  -- for this - but that one is declared six hundred lines further down, so
  -- at this point the name is a nil global rather than the function. Its
  -- own, beside the constants it pads to.
  local function drives_fixed(text, n)
    text = tostring(text or "")

    if #text >= n then return text:sub(1, n) end

    return text .. string.rep("\0", n - #text)
  end

  local DRIVES_OPS = { volumes = 1, list = 2, read = 3, getattr = 4 }
  local DRIVES_PATH_MAX, DRIVES_DATA_MAX = 256, 1024
  local DRIVES_ENTRIES_MAX, DRIVES_VOLUMES_MAX = 12, 8

  local DRIVES_FS_NAMES = { [0] = "none", "FAT16", "FAT32", "kfs", "unknown" }

  local DRIVES_ERRORS = {
    [1] = "no such path",
    [2] = "not a directory",
    [3] = "the drive server did not understand that",
    [4] = "the drive would not read",
    [5] = "a filesystem this cannot read",
    [6] = "the volume is damaged",
  }


  --
  -- **A volume's identity, as text that says what it is.** Sixteen bytes
  -- and a kind arrive; what a shortcut stores has to read as itself without
  -- the reader knowing `drivesproto.h`, so the kind is written into it:
  -- `fat:1A2B-3C4D`, the serial as Windows' `vol` prints it, or
  -- `gpt:BA231D95-9576-4349-A359-1D3FD2B045D8`, the same form a stick's own
  -- command line already names its `/Home` partition by. A GUID's first
  -- three fields are little-endian on disk and its last two are not.
  --
  local function hex(bytes)
    return (bytes:gsub(".", function(c) return ("%02X"):format(c:byte()) end))
  end

  local function drives_id(kind, raw)
    if kind == 1 then
      local serial = string.unpack("<I4", raw)

      return ("fat:%04X-%04X"):format(serial >> 16, serial & 0xffff)
    elseif kind == 2 then
      local a, b, c = string.unpack("<I4I2I2", raw)

      return ("gpt:%08X-%04X-%04X-%s-%s"):format(a, b, c,
                                                 hex(raw:sub(9, 10)),
                                                 hex(raw:sub(11, 16)))
    end

    return nil
  end

  --
  -- A request to the drive server, and its answer.
  --
  -- Three operations and no fourth: `list`, `read` and `getattr`, plus
  -- `volumes`, which is what the Drives list asks and no other mount has.
  -- There is deliberately no write of any kind - `drivesproto.h` has no
  -- operation for one and this process was never given the endpoint that
  -- would carry it.
  --
  local function drives_request(capability, op, rest, extra)
    local code = DRIVES_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    extra = extra or {}

    -- A path longer than the field is no path the server holds, and is
    -- answered as one. `string.pack` would raise instead, ending whoever
    -- asked with a line about packing.
    if #(rest or "") > DRIVES_PATH_MAX - 1 then
      return nil, DRIVES_ERRORS[1]
    end

    local want = math.min(tonumber(extra.length) or DRIVES_DATA_MAX,
                          DRIVES_DATA_MAX)

    local reply, why = sys.call_raw(capability,
                                    string.pack(DRIVES_REQUEST, code,
                                                -- A listing's page, and a
                                                -- read's byte. `ns.read`
                                                -- pages with `offset`, so a
                                                -- read takes its byte from
                                                -- there; reading `at` left
                                                -- every page after the first
                                                -- starting at zero, and a
                                                -- 3000-byte file came back
                                                -- as nothing at all.
                                                (code == DRIVES_OPS.list)
                                                  and (tonumber(extra.offset) or 0)
                                                  or 0,
                                                (code == DRIVES_OPS.read)
                                                  and (tonumber(extra.offset)
                                                       or tonumber(extra.at) or 0)
                                                  or 0,
                                                want, 0,
                                                drives_fixed(rest or "",
                                                     DRIVES_PATH_MAX)))

    if not reply then return nil, tostring(why) end

    if #reply < 1056 then return nil, "a /Drives reply of the wrong size" end

    local err, more, count, length, size, directory, _, blob =
        string.unpack(DRIVES_REPLY, reply)

    if err ~= 0 then
      return nil, DRIVES_ERRORS[err]
                  or ("the drive server refused it, error " .. tostring(err))
    end

    -- In `attrs`, as every other protocol answers: `ns.getattr` reads that
    -- field, and this answered beside it - so every folder on a USB drive
    -- came back as nothing, and Tracker drew it as a file of 0 B it would
    -- not open (Diego, on the M700, 27 September).
    if op == "getattr" then
      return { ok = true, attrs = { size = size,
                                    kind = (directory ~= 0) and "directory" or "file" } }
    end

    if op == "read" then
      return { ok = true, value = blob:sub(1, length), more = more ~= 0 }
    end

    if op == "volumes" then
      local out = {}

      for i = 1, math.min(count, DRIVES_VOLUMES_MAX) do
        --
        -- **By the record's size, named and asserted, never a literal.** This
        -- was `(i - 1) * 104 + 1`, and a bare stride that disagrees with the
        -- record is what `testing.md` 18.87 is about.
        --
        local at = (i - 1) * DRIVES_VOLUME_BYTES + 1
        local name, fs, exact, bytes, free, unit, part, readable, id_kind, raw =
            string.unpack(DRIVES_VOLUME, blob, at)

        out[i] = { name = trim(name), filesystem = DRIVES_FS_NAMES[fs] or "unknown",
                   bytes = bytes, free = free, free_exact = exact ~= 0,
                   unit = unit, partition = part, readable = readable ~= 0,
                   id = drives_id(id_kind, raw) }
      end

      return { ok = true, volumes = out, more = more ~= 0 }
    end

    -- `list`: names, with what each one is, so a column does not cost a
    -- second request per row.
    local names, entries = {}, {}

    for i = 1, math.min(count, DRIVES_ENTRIES_MAX) do
      local at = (i - 1) * 80 + 1
      local name, bytes, directory_flag = string.unpack(DRIVES_ENTRY, blob, at)

      name = trim(name)
      names[#names + 1] = name
      entries[#entries + 1] = { name = name, size = bytes,
                                kind = (directory_flag ~= 0) and "directory"
                                                             or "file" }
    end

    -- **`entries` is names, because that is what every other mount puts
    -- there** and `ns.list` hands it straight back to the caller. This
    -- returned the rich rows instead, and the first program to list a drive
    -- died in `table.concat` on a table of tables - which Tracker would have
    -- done too. The sizes and kinds keep their own key, for a column that
    -- wants them without a second request per row.
    return { ok = true, value = names, entries = names, rows = entries,
             more = more ~= 0 }
  end


  --------------------------------------------------------------------------
  -- /bin, which is C and speaks `binproto.h`.
  --------------------------------------------------------------------------

  --
  -- **One constant, three uses.** It was written out three times - the
  -- request's format string, the assertion on its size, and the stride a
  -- listing is cut into - and raising `BIN_NAME_MAX` in `binproto.h` from
  -- 24 to 48 left all three saying 24.
  --
  -- What that looked like was not a truncated name. `string.pack` refused
  -- the field outright, so a read of a perfectly ordinary file failed with
  -- "bad argument #4 to 'pack'" from a line about packing, and the two
  -- files whose names still fitted kept working - which is the worst
  -- version of this, because it looks like a problem with those files.
  --
  local BIN_NAME_MAX = 64                 -- has to match binproto.h
  local BIN_REQUEST  = "<I4I4c" .. BIN_NAME_MAX   -- op, offset, name
  local BIN_HEAD     = "<I4I4I4I4I4I4c16c16c16c16c16c16c16c16c16c16c32c40c32"

  assert(#string.pack(BIN_REQUEST, 0, 0, "") == 8 + BIN_NAME_MAX,
         "namespace: the /bin request layout does not match binproto.h")

  -- Past the header, the icon, what it opens and its name, 1-based.
  local BIN_DATA = 24 + 160 + 32 + 40 + 32 + 1
  local BIN_OPS = { list = 1, read = 2, getattr = 3 }
  local BIN_ERRORS = {
    [1] = "no such program",
    [3] = "the /bin server did not understand that",
  }

  local function bin_request(capability, op, rest, extra)
    local code = BIN_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    -- Longer than the field is no program in the image, for `dev_request`'s
    -- reason: an answer, not a raise from `pack` and not a cut that could
    -- name some other program.
    if #(rest or "") > BIN_NAME_MAX then return nil, BIN_ERRORS[1] end

    local reply, why = sys.call_raw(capability,
        string.pack(BIN_REQUEST, code, (extra and extra.offset) or 0,
                    rest or ""))

    if not reply then return nil, tostring(why) end
    if #reply < BIN_DATA then return nil, "a /bin reply of the wrong size" end

    local err, count, size, length, more, windowed,
          kind, section, n1, n2, n3, n4, n5, n6, n7, n8, icon, opens, title =
      string.unpack(BIN_HEAD, reply)

    if err ~= 0 then
      return nil, BIN_ERRORS[err] or ("bin error " .. tostring(err))
    end

    if op == "list" then
      local names = {}
      local from = (extra and extra.offset) or 0

      for i = 1, count do
        local at = BIN_DATA + (i - 1) * BIN_NAME_MAX
        names[i] = trim(reply:sub(at, at + BIN_NAME_MAX - 1))
      end

      --
      -- `more` and where to continue, which `ns.list` has looped on since
      -- the disk grew directories too big for one message - and which this
      -- server answered for `read` and not for `list`. So `/bin` reported
      -- its first 74 programs of 82 and said nothing, and the Deskbar was
      -- four applications short with no error anywhere.
      --
      -- The offset is counted here rather than sent back, because the
      -- reply has no field for it and the caller knows where it started.
      --
      return { ok = true, entries = names,
               more = (more == 1) or nil, offset = from + count }
    end

    if op == "getattr" then
      local needs = nil

      for _, w in ipairs({ n1, n2, n3, n4, n5, n6, n7, n8 }) do
        w = trim(w)

        if w ~= "" then
          needs = needs or {}
          needs[#needs + 1] = w
        end
      end

      --
      -- **A launcher in the shipped menu** (`/Kosmos/Deskbar`, `roadmap.md`
      -- 6zd) names the application it starts in its data, as the store
      -- knows it; its place here is `/Kosmos/Apps`, where `ns.program`
      -- finds an application. The whole path, as every launcher records
      -- what it starts.
      --
      local launcher = (trim(kind) == "launcher")
      local starts = launcher and reply:sub(BIN_DATA, BIN_DATA + length - 1)
      local page = nil

      -- A page's launcher (`user/pages`): the browser, then its address
      -- after a NUL.
      if starts and starts:find("\0", 1, true) then
        starts, page = starts:match("^([^\0]*)\0(.*)$")
      end

      return { ok = true, attrs = {
        size = size,
        kind = trim(kind),
        -- Applications only; a program is not in the menu at all.
        section = (windowed ~= 0) and trim(section) or nil,
        needs = needs,
        -- What the Deskbar draws beside it, and nil rather than "" when the
        -- program declares nothing, so `or` picks the default.
        icon = (trim(icon) ~= "") and trim(icon) or nil,
        -- Its name for a person (`kosmos: name`), nil when it declares none.
        title = (trim(title) ~= "") and trim(title) or nil,
        type = launcher and "launcher" or nil,
        program = launcher and ("/Kosmos/Apps/" .. starts) or nil,
        args = launcher and (page or "") or nil,
        -- The types it opens (`kosmos: opens`), each word lowercased, or
        -- nil when it declares none.
        opens = (function()
          local out = nil

          for word in trim(opens):lower():gmatch("[%w_]+") do
            out = out or {}
            out[#out + 1] = word
          end

          return out
        end)(),
      } }
    end

    return { ok = true, size = size, more = (more ~= 0),
             value = reply:sub(BIN_DATA, BIN_DATA + length - 1) }
  end

  --------------------------------------------------------------------------
  -- /Running, which is C and speaks `appproto.h`.
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
         "namespace: the /Running request layout does not match appproto.h")

  local APP_OPS = { register = 1, lookup = 2, list = 3, unregister = 4 }
  local APP_ERRORS = {
    [1] = "register: no endpoint came with that",
    [2] = "no such application",
    [3] = "too many applications registered",
    [4] = "the /Running registry did not understand that",
  }

  local function app_request(capability, op, name, pass)
    local code = APP_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    --
    -- Cut to what the field keeps, which is what the server keeps.
    --
    -- `name[24]` holds 23 bytes and a terminator: `appfs.c` ends whatever
    -- arrives at 23. But `string.pack` does not cut a string longer than
    -- `c24`, it raises - so a window whose title was longer than that killed
    -- its own process while opening, before the server could do what it
    -- already does with a long name. A Super Nintendo ROM named the way
    -- No-Intro names them was the first title that long. The reply carries
    -- the name that was settled on, so the caller still learns what it got.
    --
    local field = tostring(name or ""):sub(1, 23)

    local reply, got = sys.call_raw(capability,
                                    string.pack(APP_REQUEST, code, field),
                                    pass)

    if not reply then return nil, tostring(got) end
    if #reply < APP_NAMES - 1 then return nil, "a /Running reply of the wrong size" end

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
  -- /Devices/console, which is C and speaks `conproto.h`.
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
                    poll = CON.POLL }
      end
    end

    return CON
  end

  --
  -- The network kit, loaded the same lazy way and for the same reason: a
  -- machine with no card still runs, and a program that never touches the
  -- network should not pay for the kit being there.
  --
  local NET = nil

  local function network_kit()
    if NET == nil then
      NET = sys.kit("network") or false
    end

    return NET
  end

  --
  -- `/Network`, through the kit, exactly as the console goes through its own.
  --
  -- **A program never sees the capability.** It says `fs.ping("/Network", ...)`
  -- and the namespace resolves the path, checks that what is mounted there
  -- really is a network stack, and hands the kit the capability. That is
  -- what keeps the rule the whole system runs on - what you were not handed,
  -- you cannot reach - and it is why there is no `fs.capability`.
  --
  local function net_at(path)
    local net = network_kit()
    local capability, _, _, proto = resolve(path)

    if not net then return nil, nil, "there is no network kit" end

    if not capability or proto ~= "net" then
      return nil, nil, "there is no network stack at " .. tostring(path)
    end

    return net, capability
  end

  function ns.net_info(path)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.info(capability)
  end

  function ns.net_configure(path, address, netmask, gateway, dns)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.configure(capability, address, netmask, gateway, dns)
  end

  -- An address from the network, by DHCP: asked for here and given later,
  -- which `net_info`'s `addressed_by` says.
  function ns.net_dhcp(path)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.dhcp(capability)
  end

  --
  -- A name, as four bytes.
  --
  -- Blocks, which is what a lookup is, and the stack does not: it parks
  -- this caller and goes on serving. `ticks` bounds the wait - every park
  -- in that server has a deadline, because UDP gives no indication that
  -- anything was lost and a caller waiting on a datagram that will never
  -- arrive is a process stopped for ever.
  --
  function ns.resolve(name, ticks)
    local net, capability, why = net_at("/Network")

    if not net then return nil, why end

    return net.resolve(capability, name, ticks)
  end

  --
  -- One echo, and the answer.
  --
  -- This blocks, which is what a ping is. The *stack* does not: it parks
  -- this caller and goes on serving, so one program waiting on a host that
  -- is not there does not stop another from reading a file.
  --
  function ns.ping(path, to, seq, payload)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.ping(capability, to, seq, payload)
  end

  --
  -- A connection, as an object with methods.
  --
  -- The handle that comes back is a userdata the kit owns, holding the
  -- region the bytes live in. A program can only have one by being given
  -- one, which is the same rule as everything else here - there is no
  -- number to guess and no table to index.
  --
  -- `at_once`: back while it is still opening, for a program opening
  -- several (`netproto.h`, `NET_CONNECT_AT_ONCE`).
  function ns.connect(path, to, port, at_once)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.connect(capability, to, port, at_once)
  end

  -- Answering on a port, and taking whoever arrives. The listener is a
  -- number because it holds nothing; what `accept` hands back is a
  -- connection, which is a region somebody was given.
  function ns.listen(path, port)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.listen(capability, port)
  end

  -- `ticks` is how long to wait for somebody; without one it waits for
  -- ever, which is right for a program that has nothing else to do and
  -- wrong for an event loop.
  function ns.accept(path, listener, ticks)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.accept(capability, listener, ticks)
  end

  --
  -- Wait on several connections at once, and on a listener.
  --
  -- The `select` this system has wanted six times, and the reason a server
  -- here can serve more than one request at a time: with it a program runs a
  -- coroutine per connection and resumes whichever is ready.
  --
  -- Two lists, because waiting to read and waiting to write are different
  -- questions with different answers - `netproto.h` records what the single
  -- list cost. A connection may be in both.
  --
  function ns.poll(path, reading, writing, listener, ticks)
    local net, capability, why = net_at(path or "/Network")

    if not net then return nil, why end

    return net.poll(capability, reading, writing, listener, ticks)
  end

  -- One exchange, with the reply decoded and the error turned back into a
  -- sentence. Every operation below is this plus a shape.
  local function con_call(con, capability, code, text, ticks, colour)
    --
    -- **Bound to a local, and that is not a style preference.**
    --
    -- A call in final argument position expands to all its return values, so
    -- passing `con.encode_request{...}` straight in made its *second* return
    -- the third argument of `call_raw` - which is `pass`, the capability to
    -- send with the message. See the note in `con_kosmos.c`: it gave one of
    -- this program's capabilities away on every short write.
    --
    local bytes = con.encode_request{ op = code, text = text,
                                      ticks = ticks or 0,
                                      colour = colour or 0 }

    local raw, why = sys.call_raw(capability, bytes)

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
      local colour = extra and extra.colour or 0
      local at = 1

      repeat
        local piece = text:sub(at, at + con.TEXT_MAX - 1)
        local _, err = con_call(con, capability, code, piece, 0, colour)

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

    if op == "wait" then
      return { ok = true, value = { keys = rep.keys, events = rep.events,
                                    pointer = rep.pointer } }
    end

    return { ok = true }
  end

  --------------------------------------------------------------------------
  -- /Temporary, which is C and speaks `ramproto.h`.
  --
  -- `string.pack` rather than a kit, and the difference from the console is
  -- the whole reason that one needed a kit: ramfs has exactly one
  -- implementation, so the layout has one reader here and one in the server,
  -- and an assertion on the size is enough to catch a drift. The console has
  -- two, and a terminal is not a place to keep a copy of a struct.
  --------------------------------------------------------------------------

  local RAM_ATTR    = "I4c32c48"                     -- kind, name, value
  local RAM_REQUEST = "<I4I4I4I4I4c256" .. string.rep(RAM_ATTR, 8) .. "c1024"
  local RAM_REPLY   = "<I4I4I4I4I4c1024"

  local RAM_PATH_MAX, RAM_ATTRS_MAX = 256, 8
  local RAM_ENTRIES_MAX, RAM_DATA_MAX = 4, 1024

  assert(#string.pack(RAM_REPLY, 0, 0, 0, 0, 0, "") == 1044,
         "namespace: the /Temporary reply layout does not match ramproto.h")

  local RAM_OPS = { list = 1, read = 2, write = 3, getattr = 4,
                    setattr = 5, query = 6, watch = 7,
                    delete = 9, rename = 10, mkdir = 11 }

  local RAM_ERRORS = {
    [1] = "no such path",
    [2] = "not a directory",
    [3] = "not readable",
    [4] = "/Temporary did not understand that",
    -- [5], full, names the path it was given: see `ram_call`.
    [6] = "too many attributes on one node",
    [7] = "the directory is not empty",
    [8] = "it is already there",
  }

  -- A string cut to exactly what a fixed field holds. `c256` pads a short
  -- one and refuses a long one, so the cut has to happen first.
  local function fixed(text, n)
    text = tostring(text or "")
    return (#text > n) and text:sub(1, n) or text
  end

  --
  -- A table of attributes, as the eight slots the struct has.
  --
  -- Numbers travel as text with `kind` saying they were numbers, which is
  -- what `/Devices` settled: the wire carries characters either way, and the far
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

  --
  -- `shown` is the path as the caller gave it, for a refusal to name. **A
  -- full store says which file did not fit**: it said "/Temporary is full"
  -- for a file in `/Home`, which on a machine with no disk is this store
  -- too - true of the server and wrong for the person reading it.
  --
  local function ram_call(capability, shown, bytes)
    local raw, why = sys.call_raw(capability, bytes)

    if not raw then return nil, tostring(why) end
    if #raw < 1044 then return nil, "a /Temporary reply of the wrong size" end

    local err, more, count, length, packed, blob = string.unpack(RAM_REPLY, raw)

    if err == 5 then
      return nil, ("no room for %s in memory"):format(tostring(shown))
    end

    if err ~= 0 then
      return nil, RAM_ERRORS[err] or ("/Temporary error " .. tostring(err))
    end

    return { more = more ~= 0, count = count, length = length,
             packed = packed ~= 0, blob = blob }
  end

  local function ram_request(capability, op, rest, extra, pass, shown)
    local code = RAM_OPS[op]

    shown = shown or rest

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    extra = extra or {}

    --
    -- The three that only move names around: nothing to pack but a path, and
    -- nothing to read back but whether it worked.
    --
    -- `rename` carries its destination in the union rather than in a field
    -- of its own, and it arrives here already translated into the server's
    -- own path space - `ns.send` does that, because it is the only place
    -- that knows what this process mounted where.
    --
    if op == "delete" or op == "mkdir" or op == "rename" then
      local to = (op == "rename") and tostring(extra.to or "") or ""
      local _, err = ram_call(capability, shown,
                              ram_pack(code, rest, 0, #to, 0, nil, to))

      if err then return nil, err end

      return { ok = true }
    end

    if op == "write" then
      --
      -- /Temporary holds Lua values, and this is where that survives the move to
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

      --
      -- **Bytes in the caller's pages** (`write_from`), which this server
      -- never took: it wrote `nil` - packed - as the file and answered with
      -- no count, and `diagnose` on a `/Home` in memory said
      -- "/Home/diagnose.txt: nil" (Diego, 2 October; `testing.md` 18.350).
      -- They are read out of the region here and written as the string they
      -- are; `/Temporary` holds values a message at a time either way.
      --
      if extra.from then
        local why

        value, why = sys.region_read(pass, 0, tonumber(extra.bytes) or 0)

        if not value then return nil, why end
      end

      if type(value) == "string" then
        text, packed = value, 0
      elseif type(value) == "table" and rest:sub(1, 6) == "/Home/" then
        --
        -- **A table under `/Home` is text**, when `/Home` is in memory as
        -- when it is on a disk (`tabletext`, and `DISK`'s write below): a
        -- setting is a file a person can read, wherever it is kept.
        -- `/Temporary` keeps its values packed - what programs hand each
        -- other there is not a setting, and its packing is the C one.
        --
        local why

        if not tabletext then return nil, "cannot store a table: no table-as-text reader" end

        text, why = tabletext.encode(value)

        if not text then return nil, "cannot store that: " .. tostring(why) end

        packed = 0
      else
        -- Refused is an answer, and the caller's to have: this crashed on
        -- the nil instead, and took the Clock with it.
        local why

        text, why = sys.pack(value)

        if not text then return nil, why end

        packed = 1
      end

      local at = 0

      repeat
        local piece = text:sub(at + 1, at + RAM_DATA_MAX)
        local _, err = ram_call(capability, shown,
                                ram_pack(code, rest, at, #piece, 0, nil,
                                         piece, packed))

        if err then return nil, err end

        at = at + #piece
      until at >= #text

      return { ok = true, bytes = #text }
    end

    if op == "setattr" then
      local _, n = pack_attrs(extra.attrs)
      local _, err = ram_call(capability, shown,
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
      local r, err = ram_call(capability, shown,
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

    local r, err = ram_call(capability, shown,
                            ram_pack(code, rest, offset, 0, nterms,
                                     (op == "query") and extra.where or nil))

    if not r then return nil, err end

    if op == "list" then
      return { ok = true, entries = ram_entries(r.blob, r.count),
               more = r.more, offset = offset + r.count }
    end

    if op == "read" then
      local bytes = r.blob:sub(1, r.length)

      --
      -- A table stored as text (`tabletext`): every piece, then read as
      -- values - here rather than by `ns.read`, for the reason the packed
      -- one is, below.
      --
      if not r.packed and offset == 0 and tabletext and tabletext.is(bytes) then
        local parts, more, at = { bytes }, r.more, #bytes

        while more do
          local nxt, err = ram_call(capability, shown,
                                    ram_pack(code, rest, at, 0, 0, nil, nil, 0))

          if not nxt then return nil, err end

          parts[#parts + 1] = nxt.blob:sub(1, nxt.length)
          at = at + nxt.length
          more = nxt.more
        end

        local value, why = tabletext.decode(table.concat(parts))

        if value == nil then return nil, rest .. " is not a table Kosmos can read: " .. why end

        return { ok = true, value = value }
      end

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
        local nxt, err = ram_call(capability, shown,
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

    return { ok = true }
  end

  --------------------------------------------------------------------------
  -- /Home, which is C and speaks `diskproto.h` (`docs/diskfs.md` step 3).
  --
  -- `/Temporary`'s way: `string.pack` here, one reader in the server, and
  -- the sizes asserted. What the disk server in Lua did for a caller and the
  -- C one does not is done here instead, so nothing above this changes:
  --
  --   - **a value is packed here** - `fs.write(path, table)` stores it with
  --     a mark in front, and a read that finds the mark gives the table back;
  --   - **`.super`, `.device` and `.format`** are its three operations of
  --     their own, and the tables they answer are made here from structs;
  --   - **a read, a listing and a query are gathered** from as many pages as
  --     they took, a large write goes through a region of its own, and a
  --     query's answer is sorted.
  --------------------------------------------------------------------------

  local DISK_REQUEST = "<I4I4I8I8I4I4c512c1024"
  local DISK_REPLY   = "<I4I4I4I4I8I8I8I4I4I8I8I8I4I4c1024"
  local DISK_SUPER   = "<I8I8I4I4I4I4I8" .. string.rep("I4", 10) .. "I8I4I4"
                       .. string.rep("I4", 11) .. "I4I8I8c128c128c96c96"
  local DISK_DEVICE  = "<" .. string.rep("I8", 8)

  local DISK_DATA_MAX, DISK_PATH_MAX, DISK_REGION = 1024, 512, 1

  assert(string.packsize(DISK_REQUEST) == 1568,
         "namespace: the /Home request layout does not match diskproto.h")
  assert(string.packsize(DISK_REPLY) == 1104,
         "namespace: the /Home reply layout does not match diskproto.h")
  assert(string.packsize(DISK_SUPER) == 608,
         "namespace: the /Home superblock layout does not match diskproto.h")

  local DISK_OPS = { list = 1, read = 2, write = 3, delete = 4, rename = 5,
                     mkdir = 6, getattr = 7, setattr = 8, query = 9 }
  local DISK_OP_SUPER, DISK_OP_DEVICE, DISK_OP_FORMAT = 10, 11, 12

  local DISK_ERRORS = {
    [1]  = "/Home did not understand that",
    [2]  = "there is no filesystem here",
    [3]  = "that name is reserved",
    [4]  = "that is the directory itself",
    [6]  = "attributes are a table of names and values",
    [7]  = "that is not a region the disk server can use",
    [8]  = "a format must say `yes, erase it`",
    [10] = "more attributes than fit in a block",
    [11] = "more folders than one search holds",
    [12] = "more answers than one query keeps",
  }

  -- The filesystem's own refusals, 32 on: `kfs_why` (`user/servers/kfs.c`).
  local KFS_WHY = {
    "the disk refused", "not a kosmos filesystem",
    "a version of the format this does not understand",
    "blocks of a size this does not understand",
    "the superblock's layout does not make sense",
    "the disk is too small to hold a filesystem", "no such file",
    "not a directory", "that is a directory", "that name is taken",
    "the directory is not empty", "the disk is full", "no inodes left",
    "the file is too fragmented for 12 extents",
    "a transaction is already open", "no transaction is open",
    "more blocks changed than the journal can hold",
    "a path may not contain . or ..", "the root has no parent",
    "a name longer than 255 bytes", "a name cannot be empty",
    "a directory cannot be moved into itself", "no such inode",
    "an inode claims more extents than fit in one",
    "a directory entry is malformed", "more attributes than fit in a block",
    "this is not an attribute block", "a directory larger than this can edit",
    "a block write longer than a block",
  }

  -- How finding a stick's `/Home` stopped, a look at a time (`stick` in
  -- `diskfs.c`, which counts them in this order).
  local STICK_STEPS = {
    "no memory for its buffer", "the USB driver refusing its buffer",
    "no stick named yet", "a unit named but not ready",
    "a stick whose blocks are not 512 bytes",
    "a stick whose block 1 would not read", "a stick with no GPT",
    "a GPT whose partitions would not read",
    "a stick with no Kosmos partition",
    "a Kosmos partition, not the one asked for",
    "a Kosmos partition past the stick's end",
  }

  -- What marked a file as holding a value rather than bytes, before a table
  -- was stored as text (`tabletext`): a NUL first, so anything reading it as
  -- text stopped at once. Still read, never written.
  local DISK_VALUE_MARK = "\0KTV"

  local function disk_error(err, blob, length)
    if err == 5 then
      return ("`%s` is what this is, not something you can set; nothing was "
              .. "written"):format(blob:sub(1, length))
    end

    if err == 9 then return blob:sub(1, length) end

    if err >= 32 then
      return KFS_WHY[err - 32] or ("the filesystem refused it, error " .. (err - 32))
    end

    return DISK_ERRORS[err] or ("/Home error " .. tostring(err))
  end

  local function disk_call(capability, op, path, offset, bytes, flags, data, pass)
    path, data = tostring(path or "/"), data or ""

    if #path >= DISK_PATH_MAX then
      return nil, "a path longer than the disk takes"
    end

    if #data > DISK_DATA_MAX then
      return nil, "more than one request to the disk carries"
    end

    local raw, why = sys.call_raw(capability,
                                  string.pack(DISK_REQUEST, op, flags or 0,
                                              offset or 0, bytes or 0, #data, 0,
                                              path, data), pass)

    if not raw then return nil, tostring(why) end
    if #raw < 1104 then return nil, "a /Home reply of the wrong size" end

    local err, more, count, length, next_at, moved, size, kind, extents, nsize,
          mtime, modified, dated, _, blob = string.unpack(DISK_REPLY, raw)

    if err ~= 0 then return nil, disk_error(err, blob, length) end

    return { more = more ~= 0, count = count, length = length,
             offset = next_at, bytes = moved, size = size, blob = blob,
             node = { kind = kind, extents = extents, size = nsize,
                      mtime = mtime, modified = modified, dated = dated ~= 0 } }
  end

  --------------------------------------------------------------------------
  -- Shares over the network: `shareproto.h`, to smbfs (`docs/sharing.md`,
  -- step N2).
  --
  -- The same request and reply as a disk's, with what each operation
  -- carries packed into `u.data`; smbfs is reached through the mount of
  -- `/Network`, whose second capability it is. **Every operation is
  -- answered at once**: `share_connect` says the asking has begun, and
  -- `share_status` - asked on the caller's own clock - says how it went.
  --------------------------------------------------------------------------

  local SHARE_OP = { probe = 64, connect = 65, status = 66, disconnect = 68 }
  local SHARE_ASK = "<c48c40c32c256"
  local SHARE_SERVER = "<c48c40c40c32c152I4I2BBI4I4I8"
  local SHARE_SERVER_BYTES = 336
  local SHARE_STATES = { "asking", "answered", "connected", "refused", "away" }
  local SHARE_DIALECTS = { [0x0202] = "2.0.2", [0x0210] = "2.1", [0x0300] = "3.0",
                           [0x0302] = "3.0.2", [0x0311] = "3.1.1" }

  local function share_at(path)
    path = path or "/Network"

    for _, m in ipairs(mounts) do
      if within(path, m.key) then
        if m.proto == "net" and m.also then return m.also end
        break
      end
    end

    return nil, "there is no SMB client at " .. tostring(path)
  end

  -- A field cut to fit, with room for the zero that ends it.
  local function share_field(text, room)
    return tostring(text or ""):sub(1, room - 1)
  end

  local function share_call(op, ask, offset)
    local capability, why = share_at("/Network")

    if not capability then return nil, why end

    local data = ""

    if ask then
      data = string.pack(SHARE_ASK, share_field(ask.address, 48),
                         share_field(ask.share, 40), share_field(ask.account, 32),
                         share_field(ask.password, 256))
    end

    local raw, err = sys.call_raw(capability,
                                  string.pack(DISK_REQUEST, SHARE_OP[op], 0,
                                              offset or 0, 0, #data, 0, "", data))
    data = nil

    if not raw then return nil, tostring(err) end
    if #raw < 1104 then return nil, "a reply from smbfs of the wrong size" end

    local e, more, count, length, next_at, _, _, _, _, _, _, _, _, _, blob =
      string.unpack(DISK_REPLY, raw)

    if e ~= 0 then
      if e >= 64 then return nil, blob:sub(1, length) end
      return nil, "smbfs did not understand that"
    end

    return { more = more ~= 0, count = count, offset = next_at, blob = blob }
  end

  -- Does a server answer? Asked, and answered at once; `share_status`
  -- says how it went.
  function ns.share_probe(address)
    local r, why = share_call("probe", { address = address })

    if not r then return nil, why end
    return true
  end

  -- Sign in and connect to a share: begun, and answered at once.
  function ns.share_connect(address, share, account, password)
    local r, why = share_call("connect", { address = address, share = share,
                                           account = account,
                                           password = password })

    if not r then return nil, why end
    return true
  end

  function ns.share_disconnect(address)
    local r, why = share_call("disconnect", { address = address })

    if not r then return nil, why end
    return true
  end

  -- What each server asked for is doing: a list, each a table.
  function ns.share_status()
    local list, offset = {}, 0

    repeat
      local r, why = share_call("status", nil, offset)

      if not r then return nil, why end

      for i = 1, r.count do
        local at = (i - 1) * SHARE_SERVER_BYTES + 1
        local address, name, share, account, why_, state, dialect, signing,
              sealing, probe, _, ms = string.unpack(SHARE_SERVER, r.blob, at)

        list[#list + 1] = {
          address = trim(address), name = trim(name), share = trim(share),
          account = trim(account), why = trim(why_),
          state = SHARE_STATES[state] or "unknown",
          dialect = SHARE_DIALECTS[dialect], signing = signing ~= 0,
          sealing = sealing ~= 0, probe = probe ~= 0, in_state_ms = ms,
        }
      end

      offset = r.offset
    until not r.more or r.count == 0

    return list
  end

  -- A page's names or paths: `count` of them, each ending in a zero byte.
  local function disk_names(r, into)
    local at = 1

    for _ = 1, r.count do
      local stop = r.blob:find("\0", at, true) or (r.length + 1)

      into[#into + 1] = r.blob:sub(at, stop - 1)
      at = stop + 1
    end

    return into
  end

  -- `.super`, as the table the disk server in Lua made of it.
  local function disk_super(blob)
    local v = { string.unpack(DISK_SUPER, blob) }
    local s = {}
    local sectors, bytes, sector_size, present, formatted, free_known,
          free_blocks = table.unpack(v, 1, 7)
    local sb_fields = { "magic", "version", "block_size", "blocks", "bitmap_at",
                        "bitmap_blocks", "inodes_at", "inode_count",
                        "journal_at", "data_at" }
    local searched, looks = v[19], v[20]
    local first, found = v[33], v[34]
    local why, where, flush_why, free_why = trim(v[35]), trim(v[36]),
                                            trim(v[37]), trim(v[38])

    if present == 0 then
      return { sectors = 0, sector_size = 0, bytes = 0, formatted = false,
               present = false, why = why }
    end

    s.sectors, s.sector_size, s.bytes = sectors, sector_size, bytes
    s.present = true
    s.formatted = formatted ~= 0
    s.where = (where ~= "") and where or nil
    s.flush_why = (flush_why ~= "") and flush_why or nil

    if searched ~= 0 then
      local stops = {}

      for i, name in ipairs(STICK_STEPS) do
        if v[20 + i] > 0 then stops[name] = v[20 + i] end
      end

      s.search = { looks = looks, stops = stops,
                   first = (first ~= 0) and first or nil,
                   found = (found ~= 0) and found or nil }
    end

    if not s.formatted then
      s.why = "not a kosmos filesystem"
      return s
    end

    for i, name in ipairs(sb_fields) do s[name] = v[7 + i] end

    s.created = v[18]

    if free_known ~= 0 then
      s.free_blocks = free_blocks
    else
      s.free_why = free_why
    end

    return s
  end

  local function disk_request(capability, op, rest, extra, pass)
    extra = extra or {}

    local name = (tostring(rest or "")):match("([^/]+)$")
    local special = name and name:lower()

    if op == "read" and (special == ".super" or special == ".device") then
      local r, e = disk_call(capability, (special == ".super") and DISK_OP_SUPER
                                         or DISK_OP_DEVICE, rest)

      if not r then return nil, e end

      if special == ".super" then return { ok = true, value = disk_super(r.blob) } end

      local d = { string.unpack(DISK_DEVICE, r.blob) }

      return { ok = true, value = {
        reads = d[1], writes = d[2], read_bytes = d[3], write_bytes = d[4],
        read_counter_ticks = d[5], write_counter_ticks = d[6],
        cache_hits = d[7], cache_misses = d[8] } }
    end

    if op == "write" and special == ".format" then
      local r, e = disk_call(capability, DISK_OP_FORMAT, rest, 0, 0, 0,
                             tostring(extra.value or ""))

      if not r then return nil, e end

      return { ok = true, value = disk_super(r.blob) }
    end

    local code = DISK_OPS[op]

    if not code then
      return nil, "no such operation: " .. tostring(op)
    end

    if op == "list" then
      local entries, at = {}, tonumber(extra.offset) or 0

      repeat
        local r, e = disk_call(capability, code, rest, at)

        if not r then return nil, e end

        disk_names(r, entries)
        at = r.offset
      until not r.more

      return { ok = true, entries = entries }
    end

    if op == "read" then
      local offset = tonumber(extra.offset) or 0

      if extra.into then
        local r, e = disk_call(capability, code, rest, offset,
                               tonumber(extra.bytes) or math.maxinteger,
                               DISK_REGION, nil, pass)

        if not r then return nil, e end

        return { ok = true, bytes = r.bytes, size = r.size }
      end

      local r, e = disk_call(capability, code, rest, offset)

      if not r then return nil, e end

      local bytes = r.blob:sub(1, r.length)

      --
      -- A table stored as text comes back as one, whole, read as values -
      -- and a file broken by hand is refused with its line.
      --
      if offset == 0 and tabletext and tabletext.is(bytes) then
        local parts, more, at = { bytes }, r.more, r.offset

        while more do
          local n, ne = disk_call(capability, code, rest, at)

          if not n then return nil, ne end

          parts[#parts + 1] = n.blob:sub(1, n.length)
          more, at = n.more, n.offset
        end

        local value, why = tabletext.decode(table.concat(parts))

        if value == nil then return nil, rest .. " is not a table Kosmos can read: " .. why end

        return { ok = true, value = value }
      end

      -- A value stored as one by an older build comes back as one, whole.
      if offset == 0 and bytes:sub(1, #DISK_VALUE_MARK) == DISK_VALUE_MARK then
        local parts, more, at = { bytes }, r.more, r.offset

        while more do
          local n, ne = disk_call(capability, code, rest, at)

          if not n then return nil, ne end

          parts[#parts + 1] = n.blob:sub(1, n.length)
          more, at = n.more, n.offset
        end

        local value, perr = sys.unpack(table.concat(parts):sub(#DISK_VALUE_MARK + 1))

        if value == nil then
          return nil, "stored value is damaged: " .. tostring(perr)
        end

        return { ok = true, value = value }
      end

      return { ok = true, value = bytes, more = r.more }
    end

    if op == "write" then
      if extra.from then
        local r, e = disk_call(capability, code, rest, 0,
                               tonumber(extra.bytes) or 0, DISK_REGION, nil, pass)

        if not r then return nil, e end

        return { ok = true, bytes = r.bytes }
      end

      local body = extra.value or ""

      --
      -- **A table is written as text** (`tabletext`; Diego, 4 October: "I
      -- don't like binary files for settings for anything in the system"):
      -- Lua's table syntax under a first line that says so, read back as
      -- values only. It was `DISK_VALUE_MARK` and the serialiser's bytes,
      -- which is still read, below, and becomes text the next time it is
      -- written.
      --
      if type(body) == "table" then
        if not tabletext then return nil, "cannot store a table: no table-as-text reader" end

        local text, terr = tabletext.encode(body)

        if not text then return nil, "cannot store that: " .. tostring(terr) end

        body = text
      elseif type(body) ~= "string" then
        body = tostring(body)
      end

      if #body <= DISK_DATA_MAX then
        local r, e = disk_call(capability, code, rest, 0, #body, 0, body)

        if not r then return nil, e end

        return { ok = true }
      end

      -- Larger than a request: through a region sized to it, given back after.
      local region = sys.memory((#body + 4095) // 4096)

      if not region then return nil, "no memory for a write of " .. #body .. " bytes" end

      sys.region_write(region, 0, body)

      local r, e = disk_call(capability, code, rest, 0, #body, DISK_REGION, nil, region)

      sys.release(region)

      if not r then return nil, e end

      return { ok = true }
    end

    if op == "getattr" then
      local r, e = disk_call(capability, code, rest)

      if not r then return nil, e end

      if r.node.kind == 3 then return { ok = true, attrs = { kind = "device" } } end

      local parts, more, at = { r.blob:sub(1, r.length) }, r.more, r.offset

      while more do
        local n, ne = disk_call(capability, code, rest, at)

        if not n then return nil, ne end

        parts[#parts + 1] = n.blob:sub(1, n.length)
        more, at = n.more, n.offset
      end

      local stored, attrs = table.concat(parts), {}

      if #stored > 0 then
        attrs = sys.unpack(stored)

        if type(attrs) ~= "table" then
          return nil, "the attributes did not unpack"
        end
      end

      -- What was said about it, then what it is, which overwrites.
      local node = r.node

      attrs.kind = (node.kind == 2) and "directory" or attrs.kind or "file"
      attrs.size = node.size
      attrs.mtime = node.mtime
      attrs.modified = node.dated and node.modified or nil
      attrs.extents = node.extents

      return { ok = true, attrs = attrs }
    end

    if op == "setattr" or op == "query" then
      local given = (op == "setattr") and extra.attrs or extra.where

      if type(given) ~= "table" then
        return nil, (op == "setattr") and "setattr wants a table of attributes"
                    or "a query wants a table of attributes"
      end

      local packed, perr = sys.pack(given)

      if not packed then return nil, tostring(perr) end

      if op == "setattr" then
        local r, e = disk_call(capability, code, rest, 0, 0, 0, packed)

        if not r then return nil, e end

        return { ok = true }
      end

      local paths, at = {}, 0

      repeat
        local r, e = disk_call(capability, code, rest, at, 0, 0, packed)

        if not r then return nil, e end

        disk_names(r, paths)
        at = r.offset
      until not r.more

      table.sort(paths)
      return { ok = true, paths = paths }
    end

    -- `delete`, `mkdir`, and `rename`, whose destination arrives here
    -- already in the server's own path space (`ns.send`).
    local r, e = disk_call(capability, code, rest, 0, 0, 0,
                           (op == "rename") and tostring(extra.to or "") or nil)

    if not r then return nil, e end

    return { ok = true }
  end

  --
  -- **`/Kosmos/Kits`, answered here.** A kit is C in this process's own
  -- image, so no server holds it and nothing needs asking: the folder lists
  -- the kits this image has, each one a thing to `use` and never a file to
  -- read (`roadmap.md` 6s c).
  --
  local function kits_request(op, rest)
    local name = (rest or ""):match("^/?([^/]+)$")
    local names = sys.kit_names and sys.kit_names() or {}

    if op == "list" then
      if name then return nil, "not a directory" end

      return { ok = true, entries = names }
    end

    if op == "getattr" then
      if not name then return { ok = true, attrs = { kind = "directory" } } end

      for _, k in ipairs(names) do
        if k:lower() == name:lower() then
          return { ok = true, attrs = { kind = "kit" } }
        end
      end

      return nil, "no such path: " .. tostring(rest)
    end

    return nil, ('a kit is C, and is used rather than read: use("/Kosmos/Kits/%s")')
                :format(tostring(name))
  end

  local function request(op, path, extra, pass, again)
    local capability, rest, prefix, proto = resolve(path)
    if not capability then
      -- The sentence design.md 2 asks for. Nothing was denied; there is
      -- simply no such path in this process's world.
      return nil, "no such path: " .. path
    end

    if proto == "kits" then
      return kits_request(op, rest)
    end

    if proto == "console" then
      return con_request(capability, op, extra)
    end

    if proto == "ram" then
      return ram_request(capability, op, rest, extra, pass, path)
    end

    if proto == "disk" then
      return disk_request(capability, op, rest, extra, pass)
    end

    if proto == "dev" then
      return dev_request(capability, op, rest)
    end

    if proto == "drives" then
      return drives_request(capability, op, rest, extra)
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

    --
    -- **Every other protocol is a declared shape, and a table is not one.**
    --
    -- A mount with no protocol is an application's own name in `/Running`,
    -- which takes tables, and nothing else does: every system server speaks a
    -- declared shape. A mount that names one is a C server with a
    -- struct of its own, reached through its kit (`fs.raw`, the network
    -- kit), and a table sent to it is answered as though it were that
    -- struct. That is what happened on 19 September: `find` asks every
    -- mount, `/Devices/backlight` answered its table with BACKLIGHT_ERR_BAD_OP,
    -- and the reply's first byte, 2, unpacked as the Lua value `true` - so
    -- init indexed a boolean. `/Devices/audio` and `/Devices/blocks` had been sent
    -- the same tables for months and survived only because their BAD_OP
    -- numbers unpack as `false` and as a string, which read as a failure. Refused here, with a sentence,
    -- which is what `ns.send` already does for the same reason.
    --
    if proto then
      return nil, path .. " speaks a fixed protocol; there is no `"
                  .. tostring(op) .. "` on it"
    end


    local req = { type = op, path = rest }
    if extra then for k, v in pairs(extra) do req[k] = v end end

    local mine = answered_here(prefix, req)

    if mine then
      if not mine.ok then return nil, mine.error end
      return mine
    end

    local reply, err = sys.call(capability, req, pass)

    if not reply and not again and forget_if_gone(path, err) then
      return request(op, path, extra, pass, true)
    end

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
  -- returns "no such path" while `/Temporary` and `/Devices` both plainly exist.
  --
  -- Only the immediate child: with `/Devices/console` mounted, `/` contains
  -- `dev` and not `dev/console`, which is what a directory means.
  local function mounted_under(path)
    local prefix = (path == "/") and "/" or (path .. "/")
    local seen, names = {}, {}

    for _, m in ipairs(mounts) do
      if m.key ~= fold(path) and m.key:sub(1, #prefix) == fold(prefix) then
        local child = m.prefix:sub(#prefix + 1):match("^([^/]+)")

        if child and not seen[fold(child)] then
          seen[fold(child)] = true
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
  --
  -- Every volume on every drive, with what a sidebar needs to draw one.
  --
  -- **Its own verb, because nothing else could reach the operation.**
  -- `/Drives` speaks a declared shape, so `ns.send` refuses it - a struct
  -- server must not be handed an arbitrary table - and `ns.raw` would make
  -- the caller pack `drivesproto.h` itself, which is exactly the knowledge a
  -- namespace exists to hold. `fs.list("/Drives")` gives the names; this
  -- gives the filesystem, the size, how much is free, whether that number
  -- was counted or is FAT32's hint, and the unit and partition - which are
  -- the stable handle, since a *name* can renumber when a drive is replugged.
  --
  function ns.volumes(path)
    local r, e = request("volumes", path or "/Drives")

    if not r then return nil, e end

    return r.volumes or {}
  end

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
    -- true: `/Devices` holds cpu and memory because the device server says so,
    -- and it holds `console` because something else was attached there.
    if entries then
      local seen = {}
      for _, n in ipairs(entries) do seen[fold(n)] = true end
      for _, n in ipairs(attached) do
        if not seen[fold(n)] then entries[#entries + 1] = n end
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
  --   for piece in fs.chunks("/Home/song.mp3") do decode(piece) end
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
  --   local n = fs.read_into("/Home/big.img", buf, 0, 65536)
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

    -- An answer with no count is not a write this can vouch for: said, not
    -- passed on as a `nil` with no reason beside it.
    if r.bytes == nil then
      return nil, "the server answered without saying what it wrote"
    end

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

  --
  -- `colour` is for a console and is ignored by everything else. It is put
  -- in the request only when there is one, so a file server never receives
  -- a field it has no opinion about - the same reason `getattr` does not
  -- carry one either.
  --
  function ns.write(path, value, colour)
    local extra = { value = value }

    if colour ~= nil then extra.colour = colour_of(colour) end

    local r, e = request("write", path, extra)
    return r ~= nil, e
  end

  --
  -- **A name in a registry is the registry's to describe** (`roadmap.md`
  -- 6zb). `/Running/wm` is the window manager's own endpoint, which is what
  -- lets `fs.send("/Running/wm", ...)` reach it - so asking what the name
  -- *is* went to the program too, and a program is not a server: it answers
  -- when its loop comes round, or never if it is the asker. Tracker opening
  -- `/Running` asked every name there, its own among them, and hung. The
  -- name is in the registry's list or it is not, and that is the whole
  -- answer; it is a folder, because what is under it - a window's
  -- properties, a server's operations - is the program's. `nil` for a path
  -- that is not directly under a registry.
  --
  local function registered(path)
    local key = fold(path)

    for _, a in ipairs(autos) do
      local pre = fold(a.prefix)

      if key:sub(1, #pre + 1) == pre .. "/" then
        local name = key:sub(#pre + 2)

        if name ~= "" and not name:find("/", 1, true) then
          local r = app_request(a.cap, "list")

          for _, n in ipairs(r and r.entries or {}) do
            if fold(n) == name then return { kind = "directory" } end
          end

          return false
        end
      end
    end

    return nil
  end

  function ns.getattr(path)
    local listed = registered(path)

    if listed then return listed end
    if listed == false then return nil, "no such path: " .. path end

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
      if m.key == fold(path) then
        local r = request("getattr", path)
        local attrs = r and r.attrs or {}

        attrs.kind = "directory"
        return attrs
      end
    end

    local r, e = request("getattr", path)

    -- A place made only of mounts - `/Kosmos`, whose parts are each mounted
    -- - is a folder, which only this table knows; no server holds it, so
    -- asking one answered nothing and a listing drew it as a file.
    if not r and #mounted_under(path) > 0 then return { kind = "directory" } end

    return r and r.attrs, e
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
  --
  -- **A mount may name a subtree, and then the prefix is not the whole of
  -- it.** `match` maps `/Home/doc.pdf` onto `/Home/doc.pdf` in the server -
  -- prefix `/Home`, root `/Home` - because the disk is mounted by its
  -- `/Home` folder rather than whole. Coming back, the root has to come off
  -- before the prefix goes on, or the answer is `/Home/home/doc.pdf`.
  --
  -- Which is exactly what `find /Home kind=book` returned, for as long as
  -- the disk has been able to answer a query. It went unnoticed because
  -- every test of queries used `/Temporary`, and `/Temporary` is mounted with no root
  -- - so the two paths through this function had never both been walked.
  --
  local function to_local(p, prefix, root)
    if root and fold(p:sub(1, #root)) == fold(root) then
      p = p:sub(#root + 1)

      if p == "" then p = "/" end
    end

    return (p == "/") and prefix or (prefix .. p)
  end

  local function to_server(p, prefix, root)
    if fold(p:sub(1, #prefix)) == fold(prefix) then
      p = p:sub(#prefix + 1)

      if p == "" then p = "/" end
    end

    if root then
      p = (p == "/") and root or (root .. p)
    end

    return p
  end

  local function localise(paths, prefix, root)
    local out = {}

    for i, p in ipairs(paths or {}) do
      out[i] = to_local(p, prefix, root)
    end

    return out
  end

  function ns.query(path, where)
    local _, _, prefix, _, root = resolve(path)
    local r, e = request("query", path, { where = where })
    if not r then return nil, e end
    return localise(r.paths, prefix, root)
  end

  --
  -- The same question, answered when the answer changes.
  --
  -- This blocks. That is the feature: the process asking is not running, not
  -- polling and not on a timer, and it wakes when something it cares about
  -- happened. Pass the answer you already have as `known`.
  --
  function ns.watch(path, where, known)
    local capability, _, prefix, _, root = resolve(path)

    if not capability then
      return nil, "no such path: " .. path
    end

    -- `known` goes back in the server's names, or it never matches what the
    -- server computed and every watch returns at once.
    local server_known = {}

    for i, p in ipairs(known or {}) do
      server_known[i] = to_server(p, prefix, root)
    end

    local r, e = request("watch", path, { where = where, known = server_known })
    if not r then return nil, e end
    return localise(r.paths, prefix, root)
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
  function ns.send(path, message, pass, again)
    local capability, rest, prefix, proto = resolve(path)

    if not capability then
      return nil, "no such path: " .. path
    end

    --
    -- A server with a declared protocol does not take arbitrary tables.
    --
    -- `send` is the generic escape hatch - whatever is in the table reaches
    -- the server - and that is exactly what a struct server must not be
    -- handed. Where the protocol has an operation for what was asked, this
    -- routes to it; where it does not, it refuses with a sentence.
    --
    -- The alternative is what happened before the check existed: a `send`
    -- to a path under `/Devices` that named nothing was answered by the C
    -- devices server, its struct reply was unpacked as a Lua value, and the
    -- caller crashed indexing a number. "No such device" is the behaviour
    -- worth keeping.
    --
    if proto == "app" then
      return app_request(capability, tostring(message.type or ""),
                         tostring(message.name or ""), pass)
    end

    --
    -- `mkdir`, `delete` and `rename` on /Temporary.
    --
    -- The refusal below is right for everything else this protocol speaks -
    -- a struct server must not be handed an arbitrary table - and wrong for
    -- these three, which are the filesystem verbs every mount is supposed to
    -- have. Routed rather than passed through: what crosses is still a
    -- declared shape, and the table never reaches the server.
    --
    -- **A rename's destination is resolved here**, because `rest` is a path
    -- in the *server's* space and only this process knows what it mounted
    -- where. Both ends have to land on the same capability: a rename that
    -- crossed mounts would be a copy and a delete, which is a different
    -- operation with a different failure.
    --
    if proto == "ram" then
      local op = tostring(message.type or "")

      if op == "rename" then
        local other, elsewhere = match(tostring(message.to or ""))

        if other ~= capability then
          return nil, "a rename cannot cross a mount"
        end

        return ram_request(capability, op, rest, { to = elsewhere }, nil, path)
      end

      if op == "mkdir" or op == "delete" then
        return ram_request(capability, op, rest, nil, nil, path)
      end
    end

    -- The same three on the disk, and for the same reason.
    if proto == "disk" then
      local op = tostring(message.type or "")

      if op == "rename" then
        local to = tostring(message.to or "")

        -- A bare name renames within the folder; a path has to land on
        -- this same disk, in the server's own spelling of it.
        if to:find("/", 1, true) then
          local other, elsewhere = match(to)

          if other ~= capability then
            return nil, "a rename cannot cross a mount"
          end

          to = elsewhere
        end

        return disk_request(capability, op, rest, { to = to })
      end

      if op == "mkdir" or op == "delete" then
        return disk_request(capability, op, rest)
      end
    end

    if proto then
      return nil, ("/" .. tostring(proto) .. " speaks a fixed protocol; "
                   .. "there is no `send` to it")
    end

    local req = { path = rest }
    for k, v in pairs(message) do req[k] = v end

    local mine = answered_here(prefix, req)

    if mine then
      if not mine.ok then return nil, mine.error end
      return mine
    end

    local reply, err = sys.call(capability, req, pass)

    -- The name's process gone and another in its place: once more, looked
    -- up again (`forget_if_gone`).
    if not reply and not again and forget_if_gone(path, err) then
      return ns.send(path, message, pass, true)
    end

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
  --
  -- **And the protocol the caller expects, when it names one.** A path is
  -- served by the longest mount that holds it, so a program that was not
  -- given `/Devices/midi` still resolves the path - to `/Devices`, the
  -- devices server - which read a MIDI request and answered it: `midi.lua`
  -- took the reply for seven MIDI devices (`roadmap.md` 6zg). A struct means
  -- something only to the server whose header it is, so a caller that says
  -- which it speaks is told the path is not there when it resolves to any
  -- other - which, for this process, is the truth.
  --
  function ns.raw(path, bytes, pass, proto)
    local capability = resolve(path)

    if proto and capability then
      local _, _, _, speaks = match(path)

      if speaks ~= proto then capability = nil end
    end

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

  --
  -- **And whose callers should end that wait**: `endpoint`, a capability in
  -- this process's table, which the console keeps a copy of. For the window
  -- manager, which sleeps in `wait_input` and serves its applications after -
  -- so without this an application's request waited for the sleep to end.
  -- Only the console speaks it; anything else mounted at `path` is told so.
  --
  function ns.watch_input(path, endpoint)
    local con = console_kit()
    local capability, _, _, proto = resolve(path)

    if not (con and capability and proto == "console") then
      return nil, "only the console can watch an endpoint"
    end

    return con.watch(capability, endpoint)
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
-- It owns the serial port, and it is the only process that does: writing
-- to it and reading the keyboard are refused to everything else. That is
-- what makes this a server rather than a convention - a client cannot
-- decide to print directly, because the machine will not let it.
--
-- It serves three operations at one path. `write` puts a string; `read`
-- waits for a line, echoing as it goes, which is where the line editing
-- lives. A client that wants a line asks for one and blocks until there is
-- one, and that blocking is free: synchronous IPC already parks the caller.
--
-- `poll` is the odd one, and it is here because this is the only process
-- that may read the keyboard. A program that runs for a while - a status
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
-- child's `/Devices/console`, so `terminal.lua` answers `conproto.h` too -
-- through `use("/Kosmos/Kits/console")`, which is the same header compiled once
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
-- The devices server: /Devices.
--
-- Every device the machine was found to have, reachable the way everything
-- else is - by name, through a namespace, over the same list/read protocol
-- the filesystem uses. `fs.list("/Devices")` is not a special command; it is the
-- same request the ramfs answers, sent somewhere else.
--
-- It is the only thing that calls `sys.info()`, exactly as the console
-- server is the only thing that calls `sys.write`. The difference is that
-- nothing is lost if another process calls it: an inventory is not
-- authority.
--
-- Read fresh on every request rather than cached at startup, because half of
-- it is live: threads and processes and free pages change, and a /Devices that
-- answered with the numbers from boot would be worse than useless.
--------------------------------------------------------------------------

--------------------------------------------------------------------------
-- /bin: the programs this image carries.
--
-- Read-only, and that is not a limitation being apologised for. The
-- programs are compiled into the image, so a write that appeared to work
-- would vanish at the next boot - which is worse than being told no.
--
-- **It said "because there is no disk until M8", and that stopped being
-- the reason.** There has been a disk for a long time and `/bin` is still
-- in the image, because carrying the whole system in one file turns out to
-- be the better arrangement rather than the temporary one: the loader
-- reads a single file and jumps, there is no root filesystem to mount, and
-- starting a program never touches storage at all. `design.md` 8.3a is
-- where `/Home` picks up the other half of that - the one place that is
-- writable, on a disk when there is one and in memory when there is not.
--
-- It is an ordinary server answering the ordinary protocol. `ls /bin` and
-- `cat /Kosmos/Programs/htop.lua` are the same requests the filesystem answers, sent
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
-- /Running: the registry of what is running and what it exposes.
--
-- `beos.md` 17.2 and roadmap M7's scripting architecture. Every application
-- publishes its own properties as nodes in its own namespace, and this is
-- the directory that says which name belongs to which endpoint. So from the
-- shell:
--
--   apps                              what is running
--   cat /Running/gallery/title        read a property
--   write /Running/gallery/title hi   change one
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
-- No `appfs` here: /Running is served by `user/servers/appfs.c`, and `main.c`
-- dispatches role 14 before the interpreter is opened.
--

--
-- No `diskfs` here: /Home is served by `user/servers/diskfs.c`
-- (`docs/diskfs.md` step 3), and `main.c` dispatches role 15 before the
-- interpreter is opened. Its Lua - `diskfs_handlers`, `/Home` on a stick's
-- partition, the exit codes it died with, and `kfs.lua` and `blockcache.lua`
-- under them - went in step 4, once nothing ran it.
--

--
-- No `devices_main`: the devices server is C. `user/servers/devices.c` is
-- the whole of it and `main.c` dispatches role 9 before Lua is opened.
--

local RUNNER_ROLE = ROLE_RUNNER

local function shell_main(console_cap, ramfs_cap, devices_cap, bin_cap,
                          lib_cap, app_cap, disk_cap, audio_cap, net_cap,
                          blocks_cap, drives_cap, backlight_cap, camera_cap,
                          midi_cap, notify_cap, share_cap)
  local ns = new_namespace()
  ns.mount("/Devices/console", console_cap, nil, "console")
  ns.mount("/Temporary", ramfs_cap, nil, "ram")

  -- Longest prefix wins, so /Devices/console keeps going to the console server
  -- while everything else under /Devices goes to the device server. Two servers
  -- under one directory, and neither knows about the other - which is what a
  -- per-process mount table buys.
  ns.mount("/Devices", devices_cap, nil, "dev")

  --
  -- Every volume on every drive (USB step 6b, `docs/drives.html`).
  --
  -- **One server owns the whole prefix** rather than a mount per volume:
  -- drives appear and disappear while programs run, and a mount is made when
  -- a process is built, so a volume plugged in later could never be given a
  -- mount of its own in a namespace that already exists.
  --
  if drives_cap then ns.mount("/Drives", drives_cap, nil, "drives") end

  --
  -- Over the top of `/Devices`, because longest prefix wins.
  --
  -- `/Devices/audio` is a different server from the one that answers the rest
  -- of `/Devices`, exactly as `/Devices/console` is - the devices server describes
  -- hardware and this one *is* a piece of it. Mounted for everybody rather
  -- than passed to children the way `/Running/wm` is, because any program may
  -- ask to make a noise and the answer is a stream with a volume on it
  -- rather than a refusal.
  --
  if audio_cap then ns.mount("/Devices/audio", audio_cap, nil, "audio") end

  --
  -- `/Devices/blocks`: the USB sticks' blocks, served by the USB driver (USB step
  -- 5d, `usb.md` §7). Read only - the driver refuses a write - and mounted
  -- for every program, as `/Devices/audio` is; writing will be given to one
  -- process, the disk server, and not mounted like this.
  --
  if blocks_cap then ns.mount("/Devices/blocks", blocks_cap, nil, "blocks") end

  --
  -- `/Devices/backlight`: the screen's brightness, from the backlight driver.
  -- Mounted for everybody, as `/Devices/audio` is and for its reason - a level a
  -- program may change - and the driver keeps a floor no caller can go
  -- under (`backlightproto.h`).
  --
  if backlight_cap then
    ns.mount("/Devices/backlight", backlight_cap, nil, "backlight")
  end

  --
  -- `/Notifications`: what applications have said (`notifyproto.h`). Not
  -- under `/Devices`, for `/Network`'s reason - it is somebody you ask, not
  -- a piece of the machine.
  --
  if notify_cap then ns.mount("/Notifications", notify_cap, nil, "notify") end

  --
  -- `/Network`, not `/Devices/net`, and the distinction is the one the window
  -- manager settled: a *card* is a device and the stack is someone you ask.
  -- The card is behind `SPAWN_NET` and has no name in the namespace at all,
  -- because nothing but the stack may reach it.
  --
  -- Plan 9 put the whole of networking under `/Network` as files - `/Network/tcp/
  -- clone`, then a `ctl` and a `data` - and `roadmap.md` M12 keeps that as
  -- the target. What is here is the same name with a declared protocol
  -- behind it, because there are no connections yet to be directories of.
  --
  --
  -- **And smbfs's capability beside the stack's** (`docs/sharing.md`,
  -- *`/Network` is already somebody's*): the stack answers `netproto.h`
  -- about `/Network` itself, smbfs answers `shareproto.h` - and from step
  -- N3 a share's files - about what is under it, and the two never overlap.
  --
  if net_cap then ns.mount("/Network", net_cap, nil, "net", share_cap) end

  -- The programs this image carries. Read-only, and served by a process of
  -- its own like everything else.
  -- The applications and the programs: one store in the image, shown as two
  -- folders by the root each mount gives (`binfs.c`, `view_of`).
  ns.mount("/Kosmos/Apps", bin_cap, "/apps", "bin")
  ns.mount("/Kosmos/Programs", bin_cap, "/programs", "bin")

  -- And the looks that ship, a folder of the same store.
  ns.mount("/Kosmos/Themes", bin_cap, "/themes", "bin")

  -- And the Deskbar's menu as it ships, laid out from each application's
  -- header (`binfs.c`, `menu_path`); a person's own is `/Home/Deskbar`.
  ns.mount("/Kosmos/Deskbar", bin_cap, "/deskbar", "bin")

  -- And what programs load rather than run. Separate from the programs so
  -- that `ls /Kosmos/Programs` lists things you can type and nothing else.
  ns.mount("/Kosmos/Libraries", lib_cap, nil, "bin")
  ns.mount("/Kosmos/Kits", true, nil, "kits")          -- answered in-process

  -- What is running, and what each one exposes. A registry rather than a
  -- mount: the names under it appear and disappear with the programs.
  ns.mount_registry("/Running", app_cap, "app")

  -- Files, on the disk, surviving the power going off. design.md 8.1 names
  -- this as where user data lives, and the two reserved names at its root -
  -- `.super` and `.format` - are how the disk underneath it is asked about
  -- and laid down.
  --
  -- **One name now, which is `layout.html`'s root.** The disk was mounted
  -- three times - `/system` for what the operating system ships, `/user`
  -- for what somebody installed, `/Home` for what somebody made - and the
  -- first two held almost nothing: what the system ships is in the image,
  -- `/Kosmos`, and what somebody installs goes in `/Home/Apps` (`roadmap.md`
  -- 6s c3). A disk made before keeps its two folders, and nothing mounts
  -- them. The mount names a part of the disk still: before subtree mounts
  -- it had to be the whole disk at one name, and a file written by
  -- `mkimage` at `/Home/notes` arrived as `/Home/home/notes`.
  ns.mount("/Home", disk_cap, "/Home", "disk")

  --
  -- ...and `/Home` moves into memory when there is no disk under it.
  --
  -- **Three programs told you it already worked this way.** `neofetch`,
  -- `machine` and `df` all printed "/Home is in memory and will not
  -- survive" on a machine with no disk, and it was an intention written in
  -- the present tense: `/Home` was mounted on the disk server whatever
  -- happened, and that server answers every request with "there is no
  -- filesystem here". The sentence was true about what somebody meant and
  -- false about what the machine did.
  --
  -- What found it was the machine it matters on. `make x86-uefi` is a
  -- ThinkPad-shaped QEMU - firmware, a loader, an i8042, no virtio
  -- anything - and on that machine the desktop does not come up at all:
  -- Tracker makes `/Home/Desktop` if it is missing, the disk refuses, and
  -- there is no backdrop. A laptop with no NVMe driver is exactly that
  -- machine, so this was the first real boot arriving without a desktop.
  --
  -- The same server that serves `/Temporary`, at a different root - so a file
  -- written to `/Home/notes` is `/Temporary/home/notes` as well, which is
  -- honest rather than a coincidence: it *is* the same memory, and it goes
  -- away for the same reason.
  --
  local home_in_memory = false

  do
    local sb = ns.read("/Home/.super")

    if type(sb) ~= "table" or not sb.formatted then
      ns.mount("/Home", ramfs_cap, "/Home", "ram")
      home_in_memory = true

      --
      -- And the directory itself, which is the part that looked like the
      -- mount not working.
      --
      -- `/Temporary` is mounted with no root, so its prefix names the server's
      -- own root and that always exists. This one is a *subtree*: `/Home`
      -- resolves to the path `/Home` inside the same server, and a path
      -- inside `ramfs` exists only once something has made it. So the mount
      -- was there and correct, and `ls /Home` answered `no such path`
      -- because there was no such node - which reads exactly like a mount
      -- that did not happen.
      --
      ns.send("/Home", { type = "mkdir" })
    end
  end

  --
  -- **A person's preferences, in `/Home/Preferences`** (`roadmap.md` 6s d,
  -- `layout.html`): the folder made when it is not there, and the files
  -- that used to be dotfiles at the top of `/Home` moved into it once - on
  -- a home that has one where the new place has none, so a person's look,
  -- keyboard and Tracker survive the move. Here because the shell is the
  -- first to know where `/Home` is, and everything else is started after.
  -- The filesystem's own dotfiles, the IDE's working copies and a
  -- benchmark's results are not preferences and stay where they are.
  --
  do
    local PREFS = "/Home/Preferences"

    if not ns.getattr(PREFS) then ns.send(PREFS, { type = "mkdir" }) end

    for _, name in ipairs({ "appearance", "clock", "keyboard", "power",
                            "startup", "terminal", "tracker", "logview",
                            "music", "network", "ide" }) do
      local old, new = "/Home/." .. name, PREFS .. "/" .. name

      if ns.getattr(old) and not ns.getattr(new) then
        local moved, why = ns.send(old, { type = "rename", to = new })

        if not moved then
          print(("shell: %s stayed where it was: %s"):format(old, tostring(why)))
        end
      end
    end
  end

  local function out(s) write_text(ns, "/Devices/console", s) end

  -- An installed program's image made at the prompt is said where it was
  -- typed: this process's own `print` reaches nothing (`IMAGES`).
  IMAGES.say = function(text) out(text .. "\n") end
  local function readline() return ns.read("/Devices/console") end

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
  fs.list("/Devices")  the same thing, as a program

**A leading slash means a command**, unless the word is a file ending in
`.lua`, which runs: `/Home/hello.lua`. Without one, a bare word is
only treated as a command when it does not also name something in Lua -
so `devices` works, and if you ever alias `print` or `type` you will have
to say `/print`. A shell where `type` sometimes means a command and
sometimes means the function is a shell you cannot write anything in.

  ls /bin        the programs this image carries
  run <name>     run one, in a process of its own (a bare name works too)
  ./hello.lua    run a file from where you are - so does hello.lua

  help "shell"   every command worth typing, in one page
  help "fs"      files, through this process's namespace
  help "gfx"     surfaces, the screen, and text
  help "sys"     what a process can ask the kernel for
  help "dev"     what hardware was found, and the status bar
  help "demos"   things worth typing

**The quotes are not decoration.** `help` names a value in this shell, so
a bare `help fs` is Lua and Lua has no such expression. `help "fs"` is the
call, and `/help fs` is the command; the overview said `help fs` for months
and it has never once worked.
]=]

  topics.shell = [=[
The prompt, as a place to work.

THE LINE YOU ARE TYPING
  up / down            the lines typed before, thirty-two of them
  backspace            the only editing there is - there is no cursor
  Control-C            abandon the line and start a fresh one

MOVING AROUND
  pwd                  where you are
  cd <path>            somewhere else; `cd` alone goes to /
  ls [path]            one level
  tree [path]          all of them, from here down

READING
  cat <path>           the whole thing - a value, so a table prints as one
  head [-n 5] <path>   the first lines, ten by default
  tail [-n 5] <path>   the last ones
  wc <path>            lines, words and bytes
  grep <pat> <path>    the lines that match, with their numbers
  du [path]            how much is under each directory in here

CHANGING
  touch <path>         an empty file
  mkdir <path>         one directory, no -p
  cp <from> <to>       a directory as the destination means "into it"
  mv <from> <to>       rename and move are the same operation
  rm [-r] <path>...    -r is this program walking, not a flag a server has
  save <path> <text>   write a line and read it back
  edit <path>          the editor, in a window

WHAT A FILE IS, RATHER THAN WHAT IS IN IT
  stat <path>          kind, size, and where the bytes are
  which <name>         where a program is, if it is anywhere
  attr <path>          its attributes
  attr <path> k=v      set one
  find kind=note       everything that matches, anywhere that can answer

THE MACHINE
  ps                   what is running, with an id and a band each
  kill <id or name>    end one; two of a name is a refusal, not a guess
  df                   what each mount holds, and what the disk has left
  neofetch             the short answer
  machine              the long one, including what it cannot be asked
  ps / mem / cpu       processes, memory, processors
  devices              every device, and whether a driver claimed it
  diskinfo             what is on the disk

WHERE THINGS LIVE
  /bin                 the programs, in the image
  /Kosmos/Libraries    the libraries, in the image
  /Kosmos/Kits         the kits: C, in every program's own image
  /Home                what you have, on the disk; it survives a reboot
  /Temporary           memory; this does not
  /Devices             the hardware
  /Drives              other drives, by their names
  /Network             the network
  /Running             what is running now, by name

`/` is not a filesystem. It is a list of mounts, each answered by a
different server, and `ls /` shows exactly the ones this process was
handed.

TWO RULES THAT WILL CATCH YOU
  A line with Lua punctuation in it is Lua, not a command. `grep [ f`
  is evaluated and fails; `/grep [ f` runs the program.

  There are no pipes and no redirection. `|` and `>` are not spelled
  differently here, they are absent - a shell that composed programs
  that way would need a stream between two processes, and what this
  system has between processes is messages and shared memory.

WHAT IS DELIBERATELY MISSING
  chmod, chown, sudo, useradd - there are no users and no permission
  bits. What you may reach is decided by the capabilities you were
  handed, which is not something a command can change.
  man - it is `help`.
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

  fs.list("/Temporary")                 -> a table of names
  fs.read("/Temporary/sensor")          -> whatever was written
  fs.write("/Temporary/x", { n = 1 })   -> true
  fs.getattr("/Temporary/x")            -> { size = ... }
  fs.read("/nowhere")                   -> nil, "no such path: /nowhere"

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
  s:stretch(src, sx, sy, sw, sh, dx, dy, dw, dh [, alpha])
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

All four read /Devices, which is a *server* reached through the namespace -
the same list/read protocol the filesystem answers, sent somewhere else.
Nothing here is a special case in the shell:

  fs.list("/Devices")
  fs.read("/Devices/cpu").part
  fs.read("/Devices/memory").free_mb

The kernel decodes none of it. `sys.info()` hands back raw ID registers
and pool counts, and the tables that turn 0x410fd083 into "Cortex-A72"
live up here in Lua - so a processor the kernel has never heard of gets
described properly without the kernel changing.

The status bar:

  monitor        along the bottom of the screen, for ten minutes
  monitor 30     for thirty seconds; Control-C stops either sooner
  monitor &      the same, drawing on while you use the prompt

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

  def hot = local d = fs.read("/Temporary/sensor")
            return d.celsius > 40 and "hot" or "cold"
  /hot

The argument string arrives as `...`, so a program can take one:

  def count = local n = 0
              for _ in ipairs(fs.list(...)) do n = n + 1 end
              return n .. " under " .. ...
  /count /Devices

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

  monitor &          a status bar along the bottom of the screen
  benchmark 4        four processes spinning for ten seconds (or `spin 4`)

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

  fs.write("/Temporary/a", "one")
  fs.setattr("/Temporary/a", { kind = "note" })
  fs.query("/Temporary", { kind = "note" })

BeOS's idea: the filesystem is a database, and a folder is a saved
query. `find` and `watch` are built on exactly these two calls.
]=]

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
  -- Lua punctuation** - so `devices` and `devices all` are commands while
  -- `devices("x")` is an expression.
  --
  -- `help` is *not* one of them, and this comment used it as the example
  -- for months while being wrong about it: `help` also names a value in
  -- the environment, so `shadows_lua` sends it to Lua and `help gfx` is a
  -- syntax error. `help "gfx"` and `/help gfx` are the spellings.
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

  -- A path with its `.` and `..` taken out, so `../hello.lua` names the file a
  -- person means rather than a directory called `..` that no server has.
  local function tidy(path)
    local parts = {}

    for part in path:gmatch("[^/]+") do
      if part == ".." then
        parts[#parts] = nil
      elseif part ~= "." then
        parts[#parts + 1] = part
      end
    end

    return "/" .. table.concat(parts, "/")
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

    -- `cd /home` is `/Home`, as its mount spells it.
    cwd = ns.canonical(target)
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
  --   /ls2 /Temporary
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
    local path = ns.program(name)

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
    local camera, midi = nil, nil

    for _, want in ipairs(attrs and attrs.needs or {}) do
      if want == "processes" then flags = flags | SPAWN_PROCCTL end
      if want == "audio" then flags = flags | SPAWN_AUDIO_BAND end
      if want == "profile" then flags = flags | SPAWN_PROFILE end
      if want == "network" and may_pass_net() then
        flags = flags | SPAWN_NET
      end
      if want == "camera" then camera = camera_cap end
      if want == "midi" then midi = midi_cap end
    end

    -- The camera and MIDI last, and only when declared: everything before
    -- them keeps its number, and a program that did not ask never holds
    -- either. Their places are said in the request, as the rest are.
    local caps = { ep, console_cap, ramfs_cap, bin_cap, devices_cap,
                   lib_cap, app_cap, disk_cap, audio_cap, net_cap,
                   blocks_cap, drives_cap, backlight_cap, notify_cap,
                   share_cap }
    local camera_at, midi_at = nil, nil

    if camera then caps[#caps + 1] = camera; camera_at = #caps - 1 end
    if midi then caps[#caps + 1] = midi; midi_at = #caps - 1 end

    -- In the program's own image when it names one (`IMAGES.spawn`).
    local id, why = IMAGES.spawn(ns, path, RUNNER_ROLE, caps, flags)

    if not id then
      sys.destroy(ep)
      return false, why or "could not start a process for it"
    end

    local reply = sys.call(ep, {
      path = path, args = argument or "", cwd = cwd,
      detach = detach and true or false,
      console = 1, data = 2, bin = 3, devices = 4, lib = 5, app = 6,
      disk = 7, audio = 8, net = 9, blocks = 10, drives = 11,
      backlight = 12, notify = 13, share = share_cap and 14 or nil,
      camera = camera_at, midi = midi_at,
      home_in_memory = home_in_memory or nil,
      protostamp = sys.protostamp,
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
      out("usage: run <program or file> [arguments]\n")
      out("`ls /bin` lists the programs. A bare program name works too,\n")
      out("and so does a file from where you are: ./hello.lua\n")
      return
    end

    local name, rest = arg:match("^(%S+)%s*(.*)$")
    local argument, detach = split_detach(rest)

    -- A file, from where you are, as the prompt takes one.
    if name:match("%.lua$") or name:find("/", 1, true) then
      name = tidy(resolve(name))
    end

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
    local names = ns.list("/Devices")
    if not names then return end

    out("Devices found on this machine. Each is a node in /Devices, read the\n")
    out("same way a file is - fs.read(\"/Devices/cpu\") is the same request the\n")
    out("filesystem answers, sent to a different server.\n\n")

    -- Named here rather than listed by the device server, because it is not
    -- the device server that answers for it.
    --
    -- And it says what the node *is* rather than what the hardware is,
    -- which it used to: "PL011 UART, polled", printed on a PC that has a
    -- 16550 at port 0x3f8 and no PL011 anywhere. Nothing here knows which
    -- - the console server does, and it answers with lines of input rather
    -- than with descriptions - so the honest thing is not to claim.
    out("  /Devices/console    served by the console server, not by /Devices: a\n")
    out("                      read of it is a line of input, not a\n")
    out("                      description\n")

    for _, name in ipairs(names) do
      local d = ns.read("/Devices/" .. name)
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
        summary = "present"
      elseif name == "timer" then
        summary = string.format("%d Hz tick, %d MHz counter",
          d.hz, d.counter_hz // 1000000)
      end

      out(string.format("  /Devices/%-10s %s\n", name, summary))
    end
  end

  commands.cpu = function()
    local c = ns.read("/Devices/cpu")
    if not c then return end

    -- What every machine answers, then what only this one does. /Devices/cpu
    -- carries the architecture precisely so a reader can tell the
    -- difference rather than printing "nil" for a field that was never
    -- going to be there.
    if c.implementer then
      out(string.format("%s %s %s\n", c.implementer, c.part, c.revision))
    else
      out(string.format("%s\n", c.arch))
    end

    if c.midr then
      out(string.format("  MIDR_EL1      0x%08x\n", c.midr))
    end

    out(string.format("  architecture  %s\n", c.arch))
    out(string.format("  cores         %d  (SMP is not on yet)\n", c.cores))
    out(string.format("  running at    EL%d\n", c.el))

    if c.pa_bits then
      out(string.format("  addresses     %d-bit physical\n", c.pa_bits))
      out(string.format("  cache line    %d bytes\n", c.cache_line))
    end

    -- Not the core clock on either machine, and the reason differs, so the
    -- sentence does. AArch64 simply has no architectural way to read the
    -- core clock; x86 has one that is not architecturally the core's speed
    -- either, and whose rate the processor often declines to state at all -
    -- which is why the board measures it against the PIT at boot.
    if c.counter_hz and c.counter_hz > 0 then
      out(string.format("  counter       %d MHz  (not the core clock: %s)\n",
                        c.counter_hz // 1000000,
                        c.arch == "x86-64" and "a calibrated TSC"
                                            or "AArch64 cannot read that"))
    end

    local has = {}
    for _, f in ipairs({ "fp", "simd", "aes", "sha1", "sha2", "crc32", "atomics" }) do
      if c[f] then has[#has + 1] = f end
    end

    if #has > 0 then
      out("  features      " .. table.concat(has, " ") .. "\n")
    end
  end

  commands.mem = function()
    local m = ns.read("/Devices/memory")
    if not m then return end
    out(string.format("%d MB of RAM at 0x%x, in %d pages of %d KB\n",
        m.total_mb, m.base, m.pages_total, m.page_size // 1024))
    out(string.format("  %s used, %s free\n",
        fmt_bytes(m.pages_total - m.pages_free, m.page_size),
        fmt_bytes(m.pages_free, m.page_size)))
  end

  commands.ps = function()
    local k = ns.read("/Devices/kernel")
    if not k then return end

    --
    -- **What is running, before how much of the machine is left.**
    --
    -- This printed the pool counts and nothing else, which made it a
    -- summary wearing the name of the command that lists processes. The
    -- numbers were never the hard part to get: `sys.processes` answers
    -- from this shell and always has, and the Processes application has
    -- listed them since it was written. So the prompt had a `kill` with
    -- nowhere to read an id from, and a `ps` that could not tell you what
    -- to kill.
    --
    local rows = sys.processes()

    if rows then
      -- The bands by name. A number here would be five values nobody can
      -- read; `help "sys"` is where the scheduler is explained.
      local BAND = { [0] = "idle", "low", "normal", "display", "audio", "input" }

      out(("%-4s %-16s %-8s %6s %8s\n")
          :format("id", "name", "band", "cpu", "caps"))

      for _, r in ipairs(rows) do
        out(("%-4d %-16s %-8s %5d%% %8d%s\n")
            :format(r.id, tostring(r.name or "?"),
                    BAND[r.priority] or tostring(r.priority),
                    tonumber(r.cpu) or 0, tonumber(r.caps) or 0,
                    r.exited and "  (exited)" or ""))
      end

      out("\n")
    end

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

    out("\nPools, because the kernel keeps its objects in no heap. They grow a\n")
    out("slab at a time up to a ceiling set from this machine's memory, and\n")
    out("running out is an error at a known limit rather than a failure at an\n")
    out("unknown one.\n")
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
    out("slash means a command: /ps runs the command even if `ps`\n")
    out("has been given a meaning in Lua. A word ending in .lua is a file,\n")
    out("and runs: ./hello.lua, /Home/hello.lua.\n")
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
              "; try shell, fs, gfx, sys or demos")
    end,
  })

  -- What a chunk typed at the prompt can see. `fs` is this process's own
  -- namespace, so what the shell can reach is what the shell was given -
  -- there is no privileged view to hand out.
  commands.help = function(arg)
    out((topics[arg ~= "" and arg or "overview"]
         or ("no help for " .. arg .. "; try shell, fs, gfx, sys, dev or demos\n")))
  end

  env = {
    fs = ns,
    help = help,
    --
    -- One run of text, in a colour. No newline is added, because a run is
    -- not a line: a line in several colours is several of these, and the
    -- console joins them because it appends until a newline arrives.
    --
    --   write("KOSMOS", 0xffcc2222)
    --   write(" ok\n", "good")
    --
    -- A number is 0xAARRGGBB; a name is one of text, dim, good, bad,
    -- accent, tab, ring. Anything else is plain text rather than an error -
    -- a misspelled colour should cost you a colour, not your output.
    --
    write = function(text, colour)
      return write_text(ns, "/Devices/console", tostring(text), colour)
    end,
    print = function(...)
      local parts = {}
      for i = 1, select("#", ...) do
        parts[#parts + 1] = tostring((select(i, ...)))
      end
      out(table.concat(parts, "\t") .. "\n")
    end,
  }
  setmetatable(env, { __index = _G })

  --------------------------------------------------------------------------
  -- What machine this turned out to be, before what to type at it.
  --
  -- A *program*, run through the same path a typed name goes through,
  -- rather than a block of printing inside the shell. That is the same
  -- argument the autostart below makes and it is worth making twice: there
  -- is one way a program starts, so `neofetch` at the prompt and `neofetch`
  -- at boot are the same thing running with the same authority, and a
  -- banner that wanted a fact the shell does not hold would be refused
  -- exactly as any other program would.
  --
  -- **Wrapped, because a banner may not be able to stop the machine
  -- reaching a prompt.** That is the argument the autostart already makes
  -- about `-fw_cfg opt/kosmos/boot` - a machine you cannot get a prompt on
  -- is a machine you cannot fix from the prompt - and it applies harder
  -- here, since nobody asked for this and it runs on every boot. A failure
  -- is silence and a prompt, never a stop.
  --------------------------------------------------------------------------
  pcall(run_program, "neofetch", "", false)

  out("Kosmos shell. A process, talking to servers.\n")
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
  --
  -- **And this command line on the network**, when the machine was told to:
  -- `opt/kosmos/telnetd=23`, which a development stick carries (`roadmap.md`,
  -- remote). In the background and before the desktop, so the Mac can reach
  -- a machine whose screen is the window manager's.
  --
  local telnet_port = sys.boot("opt/kosmos/telnetd")

  if telnet_port and telnet_port ~= "" then
    out("starting telnetd on port " .. telnet_port .. "\n")

    local ok, why = run_program("telnetd", telnet_port, true)

    if not ok then
      out("boot: telnetd: " .. tostring(why) .. "\n")
    end
  end

  --
  -- **And the servers the Servers window marked to start with the machine**
  -- (`/Home/Preferences/servers`, `user/bin/apps/servers.lua`): each by its
  -- program, in the background, before the desktop. The command line is not
  -- started twice when a development stick's option started it above.
  --
  local servers = ns.read("/Home/Preferences/servers")

  if type(servers) == "table" then
    local STARTS = {
      web = function(c)
        return "httpd", ("%d %s"):format(tonumber(c.port) or 80, tostring(c.folder or "/Home/www"))
      end,
      telnet = function(c) return "telnetd", tostring(tonumber(c.port) or 23) end,
    }

    -- Not the screen's: the window manager starts `vncd` when it starts,
    -- since it is what lends it the desktop (`wm.lua`, `remote`).
    for _, id in ipairs({ "web", "telnet" }) do
      local c = servers[id]

      if type(c) == "table" and c.at_start == true
         and not (id == "telnet" and telnet_port and telnet_port ~= "") then
        local program, argument = STARTS[id](c)

        out("starting " .. program .. " " .. argument .. "\n")

        local ok, why = run_program(program, argument, true)

        if not ok then
          out("boot: " .. program .. ": " .. tostring(why) .. "\n")
        end
      end
    end
  end

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
      -- begins with `(` and `=`; `devices all` is a command. But
      -- `alias m=monitor` is *also* a command, and rejecting it because an
      -- equals sign appears somewhere in the middle broke a spelling this
      -- shell's own help had already promised.
      --------------------------------------------------------------------
      --------------------------------------------------------------------
      -- A program by its file: `./hello.lua`, `notes/hello.lua`,
      -- `/Home/hello.lua`, or `hello.lua` - found from where you are.
      --
      -- A first word ending in `.lua` is none of the other things a line can
      -- be: not a command's name, and not Lua unless its stem already names
      -- something in Lua, in which case `hello.lua` is a field and stays one.
      -- It was: `hello.lua` went to Lua and failed on a table called `hello`,
      -- and `/Home/hello.lua` was taken for a command called `home`.
      --
      -- **A bare name still means `/bin` and nothing else.** The current
      -- directory is never searched for a word, so a file that happens to be
      -- where you are cannot stand in for a program you meant.
      --------------------------------------------------------------------
      do
        local file, after = input:match("^(%S+)%s*(.*)$")
        local stem = file and file:match("^([%a_][%w_]*)%.lua$")

        if file and file:match("%.lua$")
           and not after:match("^[=%(%:%[%,]")
           and not (stem and shadows_lua(stem)) then
          local path = tidy(resolve(file))
          local argument, detach = split_detach(after)

          if not ns.getattr(path) then
            out("run: " .. path .. ": no such program\n")
          else
            local started, ok, err = pcall(run_program, path, argument,
                                           detach)

            if not started then
              out("run: " .. tostring(ok) .. "\n")
            elseif not ok then
              out("run: " .. tostring(err) .. "\n")
            end
          end

          reap()
          goto next_line
        end
      end

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
        local exists = ns.getattr(ns.program(word))

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
  local NET_EP = sys.endpoint()
  local BLOCKS_EP = sys.endpoint()
  local BLOCKS_WRITE_EP = sys.endpoint()

  --
  -- **Frames, between the USB driver and the network stack** (`usb.md` 7d).
  --
  -- Both ends are given it: the stack calls, the driver answers. What
  -- crosses on it is control only - attach, send, info - and the frames
  -- themselves live in a region the stack makes and hands over
  -- (`ethring.h`), which is `CLAUDE.md`'s rule about a stream.
  --
  -- The driver is given it even on a machine with no USB controller,
  -- because the stack asks whether there is an adapter either way and a
  -- question nobody can answer is a stack that never starts.
  --
  local FRAMES_EP = sys.endpoint()

  --
  -- And a second, for the Intel Ethernet driver (`roadmap.md` 5zd-f). One
  -- endpoint each rather than one between them: two servers receiving on one
  -- endpoint is a race about which of them answers, and the stack asks each
  -- in turn and takes the first with a card.
  --
  local PCI_FRAMES_EP = sys.endpoint()
  local DRIVES_EP = sys.endpoint()
  local BACKLIGHT_EP = sys.endpoint()

  --
  -- **`/Devices/camera`**, answered by the USB driver (`usb.md` §11 8d). Handed
  -- down only to a program that declares `kosmos: needs camera` - and to
  -- the desktop, which declares it so it can pass it on - because a camera
  -- is a thing a program should have to say it wants: what a program was
  -- not handed it cannot reach.
  --
  local CAMERA_EP = sys.endpoint()

  --
  -- **`/Devices/midi`**, answered by the same driver (`usb.md` §12), and
  -- handed down the same way: only to a program that declares `kosmos: needs
  -- midi`, and to the desktop so it can pass it on.
  --
  local MIDI_EP = sys.endpoint()

  --
  -- **`/Notifications`** (`roadmap.md`, *Notifications*): what applications
  -- have said, kept in order with who said it. Mounted for everybody, as
  -- `/Devices/audio` is and for its reason - any program may have something
  -- to say, and what the person allows is decided where it is shown.
  --
  local NOTIFY_EP = sys.endpoint()

  --
  -- **smbfs**, the SMB client (`docs/sharing.md`, step N2): asked through
  -- `/Network`, whose mount carries its capability beside the stack's -
  -- what the stack is asked and what a share is asked never overlap.
  --
  local SMBFS_EP = sys.endpoint()

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
  --
  -- And the USB driver's two block endpoints (USB step 5e): `/Devices/blocks`, to
  -- find a stick's Kosmos partition, and the write endpoint, which nothing
  -- else is given - so `/Home` on a stick is this process's to write and
  -- nobody else's, as the kernel's disk is.
  --
  --
  -- And the devices endpoint, for the clock and nothing else it uses: a
  -- file's time is a date now (`roadmap.md` 6za step b), and a server
  -- reaches what it is handed.
  --
  -- The console's endpoint too, since the disk server is C (`diskfs.c`): a
  -- journal replayed and a blank disk formatted are said in the log, where
  -- the Lua one had no console and its `print` went nowhere.
  local diskfs  = start("the disk server", ROLE_DISKFS,
                        { DISKFS_EP, BLOCKS_EP, BLOCKS_WRITE_EP, DEVICES_EP,
                          CONSOLE_EP },
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
                      SPAWN_AUDIO_BAND | (may_pass_audio() and SPAWN_AUDIO or 0))

  --
  -- And the network stack, on exactly the same terms.
  --
  -- One owner, for a stronger reason than sound's: a process that can put a
  -- raw frame on the wire can claim any address on the network and read
  -- every frame that reaches the machine. So this is the disk's grant
  -- pointed outwards, and it goes to one process.
  --
  -- Guarded like the disk and the screen, because the kernel refuses a flag
  -- this process does not hold and a machine with no card is a supported way
  -- to run. The server starts either way and answers "there is no card",
  -- which is what makes `ping` say something useful rather than not start.
  --
  --
  -- The USB host controllers: a driver with device authority and the
  -- console's endpoint to report through. On a machine with none it asks, is
  -- told so, and exits without a word.
  --
  -- **Before the network stack**, since 22 September, because the stack asks
  -- this driver whether there is an Ethernet adapter and a call waits for
  -- whoever receives it (`usb.md` 7d). `usb_driver` is whether it started at
  -- all, and the stack is given `FRAMES_EP` only when it did.
  --
  -- It is given the block endpoint it serves (USB step 5d) - a stick's
  -- blocks, to whoever is given `/Devices/blocks` - and the write endpoint, which
  -- only the disk server is given as well (USB step 5e): the right to write
  -- to a stick is holding it.
  --
  --
  -- **The Intel Ethernet controller**, where the machine has one: a driver
  -- with device authority and the console's endpoint to report through, the
  -- same shape the USB driver has and for the same reasons (`drivers.md`).
  -- On a machine with no such card it asks, is told so, and answers the
  -- stack that there is none.
  --
  local ether_driver = false

  do
    local _, err = sys.spawn(ROLE_E1000, { CONSOLE_EP, PCI_FRAMES_EP },
                             SPAWN_DEVICES)

    if err then
      line("init: no Intel Ethernet driver: " .. tostring(err))
    else
      ether_driver = true
    end
  end

  local usb_driver = false

  do
    local _, err = sys.spawn(ROLE_XHCI,
                             { CONSOLE_EP, BLOCKS_EP, BLOCKS_WRITE_EP,
                               FRAMES_EP, CAMERA_EP, MIDI_EP, NOTIFY_EP },
                             SPAWN_DEVICES)

    if err then
      line("init: no USB driver: " .. tostring(err))
    else
      usb_driver = true
    end
  end

  --
  -- **`FRAMES_EP` only when a USB driver was started**, which is what makes
  -- the stack's attach safe: a call waits for whoever receives it, so a
  -- capability to an endpoint nobody will ever receive on is a stack that
  -- hangs on its first attempt. The driver is spawned above this line for
  -- the same reason.
  --
  --
  -- **`SPAWN_NET` whether or not the kernel found a card**, since 22
  -- September. The grant says *who holds the network*, and the kernel's part
  -- in it is that there is one such process; whether this machine has a card
  -- the kernel can see is a different question that `sys.net` answers for
  -- itself. A machine whose network arrives on a USB Ethernet adapter has no
  -- card here and one stack all the same - and it needs the flag, because
  -- that is how the kernel finds the stack to wake when a frame arrives
  -- (`process_wake_net`, `usb.md` 7d). Without it a ping over the adapter
  -- came back in the stack's own receive deadline, 104 ms, rather than the
  -- network's.
  --
  -- **`may_pass_net` still gates an application's**, further down: a program
  -- that declares `needs network` on a machine with no card gets nothing,
  -- which keeps the stack the only holder exactly where it matters.
  --
  --
  -- **The card on the bus before the adapter in a socket.** A machine may
  -- have both, and a socket is the one somebody may want for something else.
  --
  --
  -- The console second, whatever wires follow: the stack says in the log
  -- what DHCP gave it (`net_server`'s second argument).
  --
  local wires = { NET_EP, CONSOLE_EP }

  if ether_driver then wires[#wires + 1] = PCI_FRAMES_EP end
  if usb_driver then wires[#wires + 1] = FRAMES_EP end

  local net = start("the network stack", ROLE_NET, wires, SPAWN_NET)

  --
  -- **The SMB client** (`docs/sharing.md` step N2), after the stack it
  -- connects through: its own endpoint, the stack's as a client of it, and
  -- the console's. Idle until somebody connects to a share; a machine with
  -- no network has it all the same and it answers "not answering", which
  -- keeps one boot path.
  --
  start("the SMB client", ROLE_SMBFS, { SMBFS_EP, NET_EP, CONSOLE_EP })

  --
  -- **The power button**, the first driver outside the kernel.
  --
  -- Handed the console server's endpoint and nothing else, so it can report
  -- without owning the console - which would let it read every key on the
  -- machine. On a board with no such button it asks, is told there is none,
  -- and exits.
  --
  -- **Not through `start`**, because `start` ends init when a spawn fails,
  -- which is right for the console and the filesystem and wrong for a
  -- button. An optional driver that cannot start is a line in the log.
  --
  do
    local _, err = sys.spawn(ROLE_POWERBUTTON, { CONSOLE_EP }, SPAWN_DEVICES)

    if err then
      line("init: no power button driver: " .. tostring(err))
    end
  end

  --
  -- **The backlight**, the same way: device authority, the console's
  -- endpoint, and the one it serves `/Devices/backlight` on. It reads the Intel
  -- display engine's two PWM controllers, says what they hold, raises a dim
  -- one to a comfortable level, and then answers for the brightness keys; on
  -- a machine without Intel graphics it says so and answers "no backlight",
  -- because a driver that exited would leave its endpoint with nobody on the
  -- other end and every caller waiting.
  --
  do
    local _, err = sys.spawn(ROLE_BACKLIGHT, { CONSOLE_EP, BACKLIGHT_EP },
                             SPAWN_DEVICES)

    if err then
      line("init: no backlight driver: " .. tostring(err))
    end
  end

  --
  -- And the drive server, which reads what is on those sticks (USB step 6b).
  --
  -- **It is given `BLOCKS_EP` and never `BLOCKS_WRITE_EP`**, so `/Drives` is
  -- read-only by what this process holds rather than by what its code agrees
  -- to. Writing to another machine's filesystem comes later and deliberately.
  --
  -- Started whether or not there is a USB driver, for the disk server's
  -- reason: a machine with no stick is a supported way to run - it is how
  -- every display test runs - and a server that answers "no volumes" keeps
  -- one boot path where a spawn that is skipped would leave `/Drives`
  -- unmounted and every program asking about it getting a different error.
  --
  start("the drive server", ROLE_DRIVES,
        { DRIVES_EP, BLOCKS_EP, CONSOLE_EP })

  -- And the notification server, which needs nothing but the console to say
  -- what it was told: who sent a post is the kernel's to say (`SYS_SENDER`).
  start("the notification server", ROLE_NOTIFY, { NOTIFY_EP, CONSOLE_EP })

  --
  -- And its address, which init asks for or gives because the stack has no
  -- namespace to read a setting from.
  --
  -- **From the network by DHCP, since 29 September**; it was written in -
  -- QEMU's 10.0.2.15, with the router and the resolver at 10.0.2.2 and
  -- 10.0.2.3 - for as long as the stack had no UDP, and the M700 sat on a
  -- network of 192.168.0 under that address. `/Home/Preferences/network`
  -- still names one by hand, so a machine that must have a fixed address is
  -- a file rather than a rebuild - the same arrangement `.appearance` has.
  --
  -- **Given whether or not the kernel found a card**, which it was not until
  -- 22 September. An address is the *stack's*, not a card's: a machine whose
  -- network arrives on a USB Ethernet adapter has no card the kernel knows
  -- about and a wire all the same (`usb.md` 7d), and `may_pass_net` guarding
  -- this meant it came up with no address at all - `ping` said "this machine
  -- has no address yet" with the adapter attached and frames moving. That
  -- flag is about the *capability* the stack is spawned with, which the
  -- kernel does refuse without a card, and it still guards that.
  --
  do
    --
    -- init has no namespace of its own - it hands them out - so this makes
    -- one holding only what the address needs: `/Network` to configure, and the
    -- disk to read the settings from if there is one.
    --
    local mine = new_namespace()

    mine.mount("/Network", NET_EP, nil, "net")

    if DISKFS_EP then mine.mount("/Home", DISKFS_EP, "/Home", "disk") end

    --
    -- Nothing by default: an address, a router and a resolver come from
    -- DHCP unless `.network` names them.
    --
    local address, netmask, gateway = nil, "255.255.255.0", nil
    local dns = nil
    --
    -- In `/Home/Preferences` since 28 September (`roadmap.md` 6s d), and at
    -- the top of `/Home` before: this runs before the shell has moved it,
    -- on the one boot that does, so the old place is asked when the new
    -- one has nothing.
    --
    local ok_read, saved = pcall(mine.read, "/Home/Preferences/network")

    if not (ok_read and type(saved) == "table") then
      ok_read, saved = pcall(mine.read, "/Home/.network")
    end

    if ok_read and type(saved) == "table" then
      address = saved.address or address
      netmask = saved.netmask or netmask
      gateway = saved.gateway or gateway
      dns     = saved.dns or dns
    end

    local function bytes(text)
      local a, b, c, d =
        tostring(text):match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")

      if not a then return "\0\0\0\0" end

      return string.char(tonumber(a) % 256, tonumber(b) % 256,
                         tonumber(c) % 256, tonumber(d) % 256)
    end

    --
    -- **From the network, unless the file says otherwise** (`roadmap.md`,
    -- remote; Diego, 29 September). An address in `.network` is used as it
    -- always was; without one the stack asks by DHCP, and says in the log
    -- what it was given. Under QEMU that is 10.0.2.15 with the router at
    -- 10.0.2.2 and the resolver at 10.0.2.3 - what was written here before -
    -- from QEMU's own DHCP server; on the M700, what its router gives.
    --
    local ok, why

    if address then
      ok, why = mine.net_configure("/Network", bytes(address), bytes(netmask),
                                   bytes(gateway or "0.0.0.0"),
                                   bytes(dns or "0.0.0.0"))
    else
      ok, why = mine.net_dhcp("/Network")
    end

    if not ok then
      line("init: the network stack would not take its address: "
           .. tostring(why))
    end
  end

  local _ = audio
  local _net = net

  -- The shell gets both endpoints, in the order it expects them, and the
  -- screen.
  --
  -- The screen belongs to whichever process composes, which is the
  -- window manager - and the shell is what starts it, by the ordinary
  -- command path (`opt/kosmos/boot`, `string=wm`), so the shell holds the
  -- screen in order to pass it on: the kernel refuses a flag the parent
  -- does not hold. It also means `gfx.screen()` works at the prompt before
  -- there is a desktop, and a person can draw. init decides, the same way
  -- it already decides who gets the console.
  --
  -- It does *not* get the console: it prints by asking the console server,
  -- like everything else, and `sys.write` from the prompt returning -102 is
  -- the demonstration.
  local shell = start("the shell", ROLE_SHELL,
                      -- `DRIVES_EP` last, so no index already given out
                      -- moves: every one of these is positional and the
                      -- runner names them by number further down.
                      { CONSOLE_EP, RAMFS_EP, DEVICES_EP, BINFS_EP, LIBFS_EP,
                        APPFS_EP, DISKFS_EP, AUDIO_EP, NET_EP, BLOCKS_EP,
                        DRIVES_EP, BACKLIGHT_EP, CAMERA_EP, MIDI_EP,
                        NOTIFY_EP, SMBFS_EP },
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
                      --
                      -- And the network card, on exactly the terms above:
                      -- the shell holds it in order to *pass it on*, because
                      -- the kernel refuses a flag the parent does not hold
                      -- and the stack is started from a prompt. Guarded like
                      -- the screen and the disk, because a machine with no
                      -- card is a supported way to run and asking anyway
                      -- would kill the shell at boot on one - which is the
                      -- same mistake this function has now made four times.
                      --
                      (may_pass_screen() and SPAWN_SCREEN or 0)
                      | SPAWN_AUDIO_BAND
                      | (may_pass_net() and SPAWN_NET or 0)
                      | SPAWN_PROCCTL
                      | SPAWN_PROFILE)

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
  shell_main(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15)
  return
end

if role == ROLE_SELFTEST then
  -- Lua itself, at user level, on a heap it cannot grow. No capabilities and no
  -- server: this checks that the language works out here, which everything
  -- above quietly assumes.
  local function check(c, what) if not c then error("selftest: " .. what) end end

  line("selftest: Lua " .. _VERSION .. " at user level")
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


--
-- No branch for ROLE_DISKFS: the disk server is C, and `main.c` dispatches
-- it before the interpreter is opened (`docs/diskfs.md` steps 3 and 4).
--

--
-- No branch for ROLE_AUDIO here, and its absence is the point.
--
-- The audio server is C and `user/init/main.c` dispatches it before the
-- interpreter is opened, so a process serving /Devices/audio never has a
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

  --
  -- **An image built for other protocols says so, and runs nothing**
  -- (`tools/protostamp.py`; Diego, 4 October, "launching doom raises an
  -- error and does not work"). The launcher said which protocols the system
  -- speaks; this runtime knows which it was built for. An installed
  -- application linked before a protocol changed reads the system wrong in
  -- ways that look like anything but that - Doom's "ui.lua:1: unexpected
  -- symbol" - so the refusal is a sentence, and its button says it.
  --
  if req.protostamp and req.protostamp ~= sys.protostamp then
    local name = tostring(req.path or "it"):match("([^/]+)%.lua$") or tostring(req.path)

    sys.reply(who, { ok = false,
                     error = ("%s was built for another Kosmos (protocols %s, and this "
                              .. "system's %s): build it again - make install-apps")
                             :format(name, sys.protostamp, req.protostamp) })
    return
  end

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
  if req.console then ns.mount("/Devices/console",   req.console, nil, "console") end
  if req.data    then ns.mount("/Temporary",         req.data, nil, "ram") end
  if req.bin     then ns.mount("/Kosmos/Apps",       req.bin, "/apps", "bin") end
  if req.bin     then ns.mount("/Kosmos/Programs",   req.bin, "/programs", "bin") end
  if req.bin     then ns.mount("/Kosmos/Themes",     req.bin, "/themes", "bin") end
  if req.bin     then ns.mount("/Kosmos/Deskbar",    req.bin, "/deskbar", "bin") end
  if req.devices then ns.mount("/Devices",           req.devices, nil, "dev") end
  if req.lib     then ns.mount("/Kosmos/Libraries",  req.lib, nil, "bin") end

  -- A kit is in this process's own image, so every program has the folder
  -- that lists them, answered in-process (`kits_request`).
  ns.mount("/Kosmos/Kits", true, nil, "kits")
  if req.app     then ns.mount_registry("/Running", req.app, "app") end
  if req.disk    then ns.mount("/Home", req.disk, "/Home", "disk") end

  -- After the disk, because this replaces what that mounted. The shell
  -- decided once, at boot, whether there is a filesystem to put `/Home` on;
  -- a program that decided for itself could disagree with the shell that
  -- started it, and then `ls /Home` would depend on who was asking.
  if req.home_in_memory and req.data then
    ns.mount("/Home", req.data, "/Home", "ram")
  end

  -- After `/Devices`, because longest prefix wins and this is a different
  -- server from the one that answers the rest of it.
  if req.audio   then ns.mount("/Devices/audio",   req.audio, nil, "audio") end
  if req.net     then ns.mount("/Network",         req.net, nil, "net", req.share) end
  if req.blocks  then ns.mount("/Devices/blocks",  req.blocks, nil, "blocks") end
  if req.drives  then ns.mount("/Drives",          req.drives, nil, "drives") end
  if req.backlight then
    ns.mount("/Devices/backlight", req.backlight, nil, "backlight")
  end
  if req.camera  then ns.mount("/Devices/camera",  req.camera, nil, "camera") end
  if req.midi    then ns.mount("/Devices/midi",    req.midi, nil, "midi") end
  if req.notify  then ns.mount("/Notifications",   req.notify, nil, "notify") end

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

  local function out(s) write_text(ns, "/Devices/console", s) end

  --
  -- **Faces off the disk** (`roadmap.md` 6zz j5): what `gfx` reads when a
  -- character needs a face the image does not carry - Japanese, Korean,
  -- Chinese - from `/Home/Fonts`, into a region of its own that the face
  -- keeps for the life of the process. Asked for only when a character
  -- needs it, so a process that never draws one reads nothing; and through
  -- this program's namespace, so one with no `/Home` has none and draws `?`.
  --
  gfx.font_loader(function(file)
    local path = "/Home/Fonts/" .. file
    local attrs = ns.getattr(path)
    local size = attrs and tonumber(attrs.size)

    if not size or size < 12 then return false end

    local region = sys.memory((size + 4095) // 4096)
    local at = region and sys.memory_map(region)

    if not at or ns.read_into(path, region, 0, size) ~= size then
      if region then sys.release(region) end
      return false
    end

    local ok, why = gfx.font_fallback(file, at, size)

    out(("fonts: %s, %d bytes, %s\n"):format(file, size,
                                             ok and "loaded" or tostring(why)))
    return ok == true
  end)

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
  -- name in the child's namespace: run(path, args, detach, { ["/Running/wm"] = c }).
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
  --
  -- `pace`, when given, is called while an installed program's image is read
  -- off the disk, a window at a time (`IMAGES.load`): the window manager
  -- passes `coroutine.yield`, so it draws while an application starts.
  --
  local function launch(path, argument, detach, shares, where, pace)
    local ep = sys.endpoint()
    if not ep then return false, "no endpoint" end

    -- The four the runner always passes, then whatever is being shared.
    -- Order is the contract: the child is told which index each landed at,
    -- because a capability table is indexed and never named.
    -- The audio server comes last, and has to be here: a program launched
    -- by another program - which is every application, because the window
    -- manager launches them - gets its namespace from this list, and
    -- without it `/Devices/audio` is a path that does not exist. The Mixer said
    -- "nothing is playing" while two tones were running, because they were
    -- not able to reach the server to say otherwise.
    -- `req.backlight` last, matching `backlight = 12` in the request below
    -- and the order init hands them to the shell. Every entry here is named by
    -- number on the other side, so a new one goes on the end or every index
    -- after it means something different.
    local caps = { ep, req.console, req.data, req.bin, req.devices,
                   req.lib, req.app, req.disk, req.audio, req.net,
                   req.blocks, req.drives, req.backlight, req.notify,
                   req.share }
    local mounts = {}

    --
    -- A share carries its protocol, because a capability is not enough to
    -- say what is behind it.
    --
    -- This passed the index alone, and the child mounted it speaking Lua
    -- tables - which was right while every server did. It stopped being
    -- right when `/Devices/console` became a struct: a terminal shares its own
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
    local camera_at, midi_at = nil, nil

    for _, want in ipairs(attrs and attrs.needs or {}) do
      if want == "processes" then flags = flags | SPAWN_PROCCTL end

      -- A program that makes sound: its threads may take the audio band.
      if want == "audio" then flags = flags | SPAWN_AUDIO_BAND end

      -- A program that sees where every processor is: `profile`, and what
      -- starts it - the window manager and a Terminal.
      if want == "profile" then flags = flags | SPAWN_PROFILE end

      if want == "network" and may_pass_net() then
        flags = flags | SPAWN_NET
      end

      -- The camera, only to a program that declares it, and only from a
      -- program that holds it: on the end, after the shares, with its
      -- index named in the request the way theirs are.
      if want == "camera" and req.camera then
        caps[#caps + 1] = req.camera
        camera_at = #caps - 1
      end

      if want == "midi" and req.midi then
        caps[#caps + 1] = req.midi
        midi_at = #caps - 1
      end
    end

    -- In the program's own image when it names one (`IMAGES.spawn`).
    local id, why = IMAGES.spawn(ns, path, RUNNER_ROLE, caps, flags, pace)

    if not id then
      sys.destroy(ep)
      return false, why or "no process"
    end

    local reply = sys.call(ep, {
      path = path, args = argument or "", cwd = where or req.cwd or "/",
      detach = detach and true or false,
      console = 1, data = 2, bin = 3, devices = 4, lib = 5, app = 6,
      disk = 7, audio = 8, net = 9, blocks = 10, drives = 11,
      backlight = 12, notify = 13, share = req.share and 14 or nil,
      camera = camera_at, midi = midi_at,
      mounts = (#mounts > 0) and mounts or nil,

      -- Which protocols the system speaks, for an image built for others
      -- to say so (`tools/protostamp.py`).
      protostamp = sys.protostamp,

      -- Inherited rather than decided again. This is a program starting a
      -- program - the window manager starting Tracker is the case that
      -- matters - and a child that worked out for itself where `/Home` is
      -- could disagree with its parent, which would mean `ls /Home`
      -- answering differently depending on who asked.
      home_in_memory = req.home_in_memory or nil,
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
      return ns.interrupted("/Devices/console") == true
    end,
    --
    -- One run of text, in a colour. No newline is added, because a run is
    -- not a line: a line in several colours is several of these, and the
    -- console joins them because it appends until a newline arrives.
    --
    --   write("KOSMOS", 0xffcc2222)
    --   write(" ok\n", "good")
    --
    -- A number is 0xAARRGGBB; a name is one of text, dim, good, bad,
    -- accent, tab, ring. Anything else is plain text rather than an error -
    -- a misspelled colour should cost you a colour, not your output.
    --
    write = function(text, colour)
      return write_text(ns, "/Devices/console", tostring(text), colour)
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
  -- `use("/Kosmos/Libraries/ui.lua")` - a library, loaded into this program's world.
  --
  -- Not `require`. There is no package path, no search, no C loader and no
  -- global module table: a library is a file in this process's namespace,
  -- and a program that was not given /Kosmos/Libraries does not have one. That is the
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
    --
    -- Cached by the path folded, as a name is found (`roadmap.md` 6s): a
    -- library asked for as `/Kosmos/Libraries/ui.lua` and as `/LIB/UI.lua` is one file,
    -- and two instances of it would be two sets of its state.
    --
    local key = tostring(path):lower()

    if loaded[key] ~= nil then
      return loaded[key]
    end

    --
    -- A kit is a library that happens to be C.
    --
    -- `use("/Kosmos/Libraries/ui.lua")` reads Lua out of the namespace and runs it;
    -- `use("/Kosmos/Kits/pdf")` gets a table the runtime built. The caller writes
    -- the same line either way, which is the point: where a library's speed
    -- comes from is not something the program using it should have to know,
    -- and a kit that later grows a Lua half - or a Lua library that has its
    -- hot loop moved into C - should not change a single call site.
    --
    -- Kits come through the namespace rather than as globals so that the
    -- rule the rest of the system runs on still holds: what you were not
    -- given, you do not have. A program with no `use` has no kits.
    --
    local kit = key:match("^/kosmos/kits/([%w_]+)$")

    if kit then
      local value, why = sys.kit(kit)

      if not value then
        error(("use: %s: %s"):format(path, tostring(why)), 2)
      end

      loaded[key] = value
      return value
    end

    --
    -- **An application's own C, by its file** (`docs/elf.md` step 5):
    -- `use("doom.elf")` is the table the engine in that image builds, from
    -- a program running in it - which is what `-- kosmos: image doom.elf`
    -- in its header made it. Not a kit: `/Kosmos/Kits` is what Kosmos
    -- ships, and Doom's engine is Doom's.
    --
    local image = key:match("^([%w_%-]+)%.elf$")

    if image then
      local value, why = sys.kit(image, true)

      if not value then
        error(("use: %s: %s"):format(path, tostring(why)), 2)
      end

      loaded[key] = value
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

    loaded[key] = value
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
  -- for. And the whole path beside it, which Processes shows: two of the
  -- same name are told apart by where they came from (`roadmap.md` 6m).
  --
  sys.name((path:match("([^/]+)%.lua$") or path:match("([^/]+)$") or "run"),
           path)

  -- And kept, for what this process says of itself: a window it opens
  -- names the file it runs, which is how the Deskbar finds its picture
  -- whoever started it (`roadmap.md` 6r).
  sys.program = path

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
  --
  -- **The error is said out loud, and it used to be thrown away.**
  --
  -- `pcall(chunk)` with the result unused, which is a detached program
  -- dying in complete silence: nothing on the screen, nothing on the serial
  -- line, and - because the window manager does not reap a window whose
  -- process has gone - a window still sitting there looking perfectly
  -- normal. Every graphical application is launched this way, so this was
  -- the silence behind every application crash this desktop has ever had.
  --
  -- It cost an afternoon: a window that had died on its first pass was read
  -- as a window that was hung, then as a lost mouse event, then as a
  -- message-size limit, and the one thing that would have said otherwise in
  -- ten seconds was this line.
  --
  -- The attached path four lines below has always reported - it hands the
  -- error back in the reply, which is how a program run from the shell
  -- prints its own traceback. Detaching is not a reason to stop saying why
  -- something failed; it is only a reason not to *wait* for it.
  if req.detach then
    sys.reply(who, { ok = true })

    local ok, e = pcall(chunk)

    if not ok then
      --
      -- `out`, not `print`.
      --
      -- The child's `print` is `env.print`, which writes to the console
      -- through the namespace; this scope is the *runner's* and its `print`
      -- is Lua's own, which in a freestanding build goes nowhere at all.
      -- The first version of this fix used it and was as silent as the bug
      -- it was fixing - and it took a probe that printed successfully from
      -- inside the program, three lines away, to see the difference.
      --
      out(path .. ": " .. tostring(e) .. "\n")

      --
      -- **And said where it is seen** (`roadmap.md`, *Notifications*): an
      -- application started from the desktop that stops on an error is an
      -- alert, under its own name - this process is still it - with the
      -- error's first line. The log has the whole of it.
      --
      pcall(function()
        local attrs = ns.getattr(path)
        local stem = path:match("([^/]+)%.lua$") or path
        local name = (attrs and attrs.title and attrs.title ~= "") and attrs.title
                     or (stem:sub(1, 1):upper() .. stem:sub(2))

        env.use("/Kosmos/Libraries/notify.lua").post{
          title = name .. " stopped",
          body = (tostring(e):match("^[^\n]*") or ""):sub(1, 200),
          alert = true }
      end)

      -- **And ended as a failure.** It ended with code 0 whatever
      -- happened, so whoever collects it - the shell, the IDE, `telnetd`
      -- saying "(exit code 1)" to the Mac - could not tell a program that
      -- died of an error from one that finished.
      sys.exit(1)
    end

    return
  end

  -- pcall, so a program that raises reports it instead of taking this
  -- process down without a word. It is its own process either way; this
  -- just means the shell hears why - and its code says it failed.
  local ok, e = pcall(chunk)

  sys.reply(who, { ok = ok, error = not ok and tostring(e) or nil })

  if not ok then sys.exit(1) end

  return
end


-- A client. The name it mounts the filesystem under is its own business,
-- and is the whole demonstration: the same server, two processes, two
-- different worlds.
local mount_point = (role == ROLE_CLIENT_B) and "/files" or "/Temporary"

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
local other = (mount_point == "/Temporary") and "/files" or "/Temporary"
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
