-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- sandbox: small operating systems, each in a virtual machine of its own.
--
--   sandbox run alpine -- uname -a      a task: run once, said, thrown away
--   sandbox run alpine --share /Home/Projects/site:/work -- make
--                                       ... given a folder of /Home (`:ro`: read only)
--   sandbox run alpine --network -- wget -O- http://example.com/
--                                       ... and the network, outgoing
--   sandbox run alpine --port 8080:80 -- httpd -f -p 80
--                                       ... Kosmos's port 8080 carried to its 80
--   sandbox run alpine                  a shell in a new one, until it ends
--   sandbox list                        every sandbox, and how it is doing
--   sandbox new web --image alpine [--memory 256] [--service] [--at-start] [--network]
--                   [--share FOLDER] [--port N:M] [-- COMMAND]
--   sandbox start web | stop web | console web | delete web
--   sandbox images                      the images there are to start from
--
-- `docs/sandboxes.md` S1 - Diego, 12 October 2026: "continue with the sandbox
-- app", "as designed in the mockup", whose last page is this program. It
-- shows and asks; `sandboxes.lua` does everything, and is what the Sandboxes
-- window uses too. In a console, Control-W then D leaves a sandbox running.

local files = use("/Kosmos/Libraries/files.lua")
local sandboxes = use("/Kosmos/Libraries/sandboxes.lua")

local words = files.words(args)
local verb = words[1]

-- The words after `--`, as one command; and the options before it.
local options, command = {}, nil

do
  local i = 2

  while i <= #words do
    local w = words[i]

    if w == "--" then
      command = table.concat(words, " ", i + 1)
      break
    elseif w == "--share" then
      -- More than one: each a folder of /Home, `path[:at][:ro]`.
      options.shares = options.shares or {}
      options.shares[#options.shares + 1] = words[i + 1]
      i = i + 2
    elseif w == "--port" then
      -- More than one: each Kosmos's port and the guest's, `N:M` or `N`.
      options.ports = options.ports or {}
      options.ports[#options.ports + 1] = words[i + 1]
      i = i + 2
    elseif w:sub(1, 2) == "--" then
      options[w:sub(3)] = (words[i + 1] and words[i + 1]:sub(1, 2) ~= "--") and words[i + 1] or true
      i = i + ((options[w:sub(3)] == true) and 1 or 2)
    else
      options[#options + 1] = w
      i = i + 1
    end
  end
end

-- Text as it comes, without a newline: a program has no `io`; its console
-- is written to as a file is.
local function say(text) fs.write("/Devices/console", text) end

local function megabytes(bytes) return ("%d MB"):format((bytes or 0) // (1024 * 1024)) end

--
-- **A console, attached**: what the guest says, as it says it, and what is
-- typed here, to it - until it ends, or Control-W then D, which leaves it
-- running. Answers its exit code when it ended.
--
local function attach(name)
  local at, armed = 0, false

  for _ = 1, 3 do                      -- its console from the start of what is kept
    local _, next_at, r = sandboxes.read(name, 0)

    if r then at = r.offset break end

    sys.sleep(5)
  end

  while true do
    local text, next_at, r = sandboxes.read(name, at)

    if not text then return nil end

    if #text > 0 then say(text) end

    at = next_at

    if not r.running and #text == 0 then
      sandboxes.stop(name)            -- ended: its monitor may go
      return r.exit_code
    end

    local typed = {}

    for _, code in ipairs(fs.keys("/Devices/console") or {}) do
      if armed and (code == 100 or code == 68) then
        print("\nsandbox: left " .. name .. " running - sandbox console " .. name .. " to come back")
        return nil
      end

      armed = code == 23            -- Control-W

      if not armed then typed[#typed + 1] = string.char(code) end
    end

    if #typed > 0 then sandboxes.type(name, table.concat(typed)) end

    if #text == 0 then sys.sleep(2) end
  end
end

if verb == "run" then
  local image = options[1] or "alpine"
  local memory = math.tointeger(tonumber(options.memory or "")) or 256

  if command then
    local began = sys.ticks()
    local code, up, took, last = sandboxes.run(image, command, memory, say, options.shares,
                                                     options.network, options.ports)

    -- Refused before it started: `sandboxes.run` answers nil and why - a
    -- folder outside /Home, a name in use - and the why is its second value.
    if code == nil and type(up) ~= "number" then
      print("sandbox: " .. tostring(up or took or "it would not start"))
      sys.exit(1)
    end

    -- And what its network carried, when it had one.
    local carried = ""

    if last and (options.network or options.ports) then
      carried = ("; it carried %s in and %s out"):format(files.size(last.net_in), files.size(last.net_out))
    end

    print(("sandbox: %s exited %s after %.1f s, up in %.1f s%s; the sandbox is gone")
          :format(command:match("^%S+") or command, tostring(code or "without saying"), took or 0, up or 0,
                  carried))
    sys.exit(code or 1)
  end

  -- No command: a shell in a sandbox of its own, until it ends.
  local s = sandboxes.parse("", ("shell-%d"):format(sys.ticks() % 100000))

  s.image, s.memory = image, memory
  s.folders = options.shares or {}
  s.network = options.network and "outgoing" or "none"
  s.ports = options.ports or {}

  local id, why = sandboxes.start(s)

  if not id then print("sandbox: " .. tostring(why)) sys.exit(1) end

  print(("sandbox: %s in %s, %d MB - Control-W then D leaves it running"):format(image, s.name, memory))
  attach(s.name)
  return
end

if verb == "list" or verb == nil then
  local all = sandboxes.all()

  if #all == 0 then
    print("sandbox: none yet - sandbox new <name> --image alpine, or sandbox run alpine")
    return
  end

  print(("%-14s %-10s %-9s %-16s %s"):format("NAME", "IMAGE", "STATE", "MEMORY", "COMMAND"))

  for _, s in ipairs(all) do
    local state = s.now and (s.now.running and "running" or "ended") or "stopped"

    -- Running, what it uses of what it was given: its RAM is given on
    -- touch, so the first is what it costs.
    print(("%-14s %-10s %-9s %-16s %s"):format(s.name, s.task and "-" or s.image, state,
          s.now and ("%d of %s"):format((s.now.ram_used or 0) // (1024 * 1024), megabytes(s.now.ram))
                 or ("%d MB"):format(s.memory),
          s.task and "(a task)" or (s.command ~= "" and s.command or "a shell")))
  end

  return
end

if verb == "images" then
  local images = sandboxes.images()

  if #images == 0 then print("sandbox: no images - pkg install alpine") return end

  for _, i in ipairs(images) do
    print(("%-10s %-20s %s  %s"):format(i.name, i.title, megabytes(i.bytes), i.about))
  end

  return
end

local name = options[1]

if not name then
  print("sandbox: run, list, new, start, stop, console, delete or images - sandbox.lua says how")
  sys.exit(1)
end

if verb == "new" then
  local s = sandboxes.parse("", name)

  s.image = options.image or "alpine"
  s.memory = math.tointeger(tonumber(options.memory or "")) or 256
  s.kind = options.service and "service" or (command and "task" or "shell")
  s.starts = options["at-start"] and "with Kosmos" or "by hand"
  s.network = options.network and "outgoing" or "none"
  s.command = command or ""

  for _, text in ipairs(options.shares or {}) do
    local f, why = sandboxes.folder(text)

    if not f then print("sandbox: " .. why) sys.exit(1) end

    s.folders[#s.folders + 1] = sandboxes.folder_text(f)
  end

  for _, text in ipairs(options.ports or {}) do
    local from, to = sandboxes.port(text)

    if not from then print("sandbox: " .. to) sys.exit(1) end

    s.ports[#s.ports + 1] = ("%d:%d"):format(from, to)
  end

  local ok, why = sandboxes.save(s)

  print(ok and ("sandbox: %s made - sandbox start %s"):format(name, name) or ("sandbox: " .. tostring(why)))
  return
end

if verb == "start" then
  local s = sandboxes.definition(name)
  local why

  -- `--report FILE`: the outcome written there too, for a window that
  -- started this in a process of its own and reads it on its clock.
  local function report(line)
    if type(options.report) == "string" then fs.write(options.report, line .. "\n") end
  end

  if not s then
    why = ("there is no sandbox called %s - sandbox list"):format(name)
  else
    local id

    id, why = sandboxes.start(s)

    if id then
      print(("sandbox: %s started - sandbox console %s"):format(name, name))
      report("started")
      return
    end
  end

  print("sandbox: " .. tostring(why))
  report("failed: " .. tostring(why))
  sys.exit(1)
end

if verb == "stop" then
  local r = sandboxes.stop(name)

  print(r and ("sandbox: %s stopping"):format(name) or ("sandbox: %s is not running"):format(name))
  return
end

if verb == "console" then
  if not sandboxes.state(name) then print(("sandbox: %s is not running"):format(name)) sys.exit(1) end

  print(("sandbox: the console of %s - Control-W then D to leave it running"):format(name))
  attach(name)
  return
end

if verb == "delete" then
  local ok, why = sandboxes.delete(name)

  print(ok and ("sandbox: %s deleted"):format(name) or ("sandbox: " .. tostring(why)))
  return
end

print("sandbox: no such thing as sandbox " .. tostring(verb))
sys.exit(1)
