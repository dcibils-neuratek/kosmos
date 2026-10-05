-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network profile processes audio
-- telnetd: this machine's command line, over the network.
--
--   telnetd            on port 23
--   telnetd 2323       on that one
--
-- From the Mac: `nc <address> 23`, Homebrew's `telnet`, or
-- `tools/kosmos_telnet.py` for what is run from a script.
--
-- **So the Mac can run commands on the M700** (`roadmap.md`, remote; Diego,
-- 29 September: "Can we just implement a Telnet server and client?") - a
-- log, a profile, `diagnose`, without a stick carried back and forth.
--
-- **A Terminal whose window is a TCP connection.** A program run here is
-- handed this process as its `/Devices/console`, exactly as the Terminal
-- hands itself: what it writes goes down the connection, and a line typed at
-- the other end is what its `read` gets. No program can tell, which is the
-- design rather than a trick - a name resolves to a capability, and "the
-- console" is whatever it was handed.
--
-- **Telnet as far as a line needs.** The client echoes and sends a line at a
-- time, which is what it does when a server negotiates nothing, and this
-- negotiates nothing: every option offered is refused, so the conversation
-- stays plain text, a line in and lines out. Control-C arrives as a byte or
-- as Telnet's interrupt, and is what `interrupted()` answers.
--
-- **No key and no password: Diego's choice**, for a machine on his own
-- network - "no key, local network only". A connection from outside this
-- machine's own subnet is closed unanswered, and nothing beyond the router
-- can make one. Started by itself on a development stick
-- (`opt/kosmos/telnetd`), and by hand anywhere else; ended with `kill`.
-- It does not ask the console for Control-C, which on the M700 would be
-- the keyboard controller asked ten times a second (`testing.md` 18.287).
--
-- **And a file back, as text**: `get <path>` prints it in base64 between a
-- `BEGIN` and an `END` line. Telnet is text - a byte of 255 means something
-- to it - and one port serving both is simpler than a second server beside
-- this one. `kosmos_telnet.py get` undoes it; `put` is the other way.

local con = use("/Kosmos/Kits/console")
local regions = use("/Kosmos/Libraries/regions.lua")
local compress = use("/Kosmos/Kits/compress")   -- base64, for `get` and `put`

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local port = tonumber(words[1]) or 23

local function dotted(bytes)
  if type(bytes) ~= "string" or #bytes ~= 4 then return "?" end

  return ("%d.%d.%d.%d"):format(bytes:byte(1, 4))
end

local info = fs.net_info("/Network")

if not info or not info.card then
  print("telnetd: this machine has no network card")
  return
end

--
-- **Waiting for an address, rather than leaving** (`testing.md` 18.352). A
-- stick and a network boot both start this before DHCP has answered, and
-- with no address the stack refuses to listen - so on the M700 it said
-- "could not listen on port 23: 3" and was gone a second after it started,
-- and the Mac had nothing to reach once the lease came. So a refusal while
-- the machine has no address is waited out, a second at a time, said once;
-- any other refusal is said and ends it as before.
--
local tick_hz = (sys.info() or {}).tick_hz or 250
local listener, why = fs.listen("/Network", port)
local waited = false

while not listener do
  info = fs.net_info("/Network") or info

  local addressed = type(info.address) == "string" and info.address ~= "\0\0\0\0"

  if addressed then
    print("telnetd: could not listen on port " .. port .. ": " .. tostring(why))
    return
  end

  if not waited then
    print(("telnetd: waiting for an address to listen on port %d"):format(port))
    waited = true
  end

  sys.sleep(tick_hz)
  listener, why = fs.listen("/Network", port)
end

info = fs.net_info("/Network") or info

-- The address is DHCP's, and may change; asked again whenever somebody
-- connects.
print(("telnetd: on port %d, at %s"):format(port, dotted(info.address)))

--
-- **A name in `/Running`, for the Servers window's Disconnect**: a request
-- `{ type = "disconnect", from = "<address>" }` on it ends that address's
-- sessions. Tables, since this is a program's own name rather than a
-- server's wire (`CLAUDE.md`, a declared shape).
--
local control = sys.endpoint()

if control then
  fs.send("/Running", { type = "register", name = "telnetd" }, control)
end

--------------------------------------------------------------------------
-- Who may connect: this machine's own subnet, and nobody else.
--------------------------------------------------------------------------

local function neighbour(from)
  local now = fs.net_info("/Network") or info
  local mine, mask = now.address, now.netmask

  if type(from) ~= "string" or #from ~= 4 or type(mine) ~= "string"
     or type(mask) ~= "string" or #mine ~= 4 or #mask ~= 4 then
    return false
  end

  for i = 1, 4 do
    if (from:byte(i) & mask:byte(i)) ~= (mine:byte(i) & mask:byte(i)) then
      return false
    end
  end

  return true
end

--------------------------------------------------------------------------
-- The sessions: one connection each, and each its own console.
--------------------------------------------------------------------------

local IAC, SB, SE = 255, 250, 240
local WILL, WONT, DO, DONT = 251, 252, 253, 254
local IP = 244                      -- Telnet's "interrupt process"

local PROMPT = "kosmos> "

local sessions = {}

--------------------------------------------------------------------------
-- What the Servers window reads (`user/bin/apps/servers.lua`): the state and
-- the sessions under `/Temporary/telnetd`, and its last lines - written when
-- they change, since this does not know a window is watching. The same
-- arrangement `httpd` has with it.
--------------------------------------------------------------------------

local STATUS = "/Temporary/telnetd/status"
local LOG = "/Temporary/telnetd/log"
local LOG_LINES = 40
local lines_said = {}

fs.send("/Temporary/telnetd", { type = "mkdir" })

local function publish()
  local list = {}

  for _, s in ipairs(sessions) do
    list[#list + 1] = { from = dotted(s.from), cwd = s.cwd,
                        running = s.running_name }
  end

  fs.write(STATUS, { state = "running", port = port, sessions = list })
end

local function note(text)
  local clock = sys.ticks() // math.max(1, ((fs.read("/Devices/cpu") or {}).counter_hz or 1))

  lines_said[#lines_said + 1] = ("%5ds  %s"):format(clock, text)

  while #lines_said > LOG_LINES do table.remove(lines_said, 1) end

  fs.write(LOG, lines_said)
  print("telnetd: " .. text)
end

local function send(s, text)
  -- A console's lines end in a newline; a Telnet line ends in both.
  s.out = s.out .. text:gsub("\r?\n", "\r\n")
end

local function prompt(s)
  send(s, PROMPT)
end

--
-- Bytes from the connection: Telnet's commands taken out - every option
-- refused, an interrupt noted - and what is left gathered into lines.
--
local function take(s, bytes)
  local i = 1

  while i <= #bytes do
    local b = bytes:byte(i)

    if s.iac == "sub" then
      if b == IAC and bytes:byte(i + 1) == SE then
        s.iac = nil
        i = i + 1
      end
    elseif b == IAC then
      local command, option = bytes:byte(i + 1), bytes:byte(i + 2)

      if command == IAC then
        s.partial = s.partial .. "\255"
        i = i + 1
      elseif command == DO or command == DONT then
        if command == DO and option then
          s.out = s.out .. string.char(IAC, WONT, option)
        end
        i = i + 2
      elseif command == WILL or command == WONT then
        if command == WILL and option then
          s.out = s.out .. string.char(IAC, DONT, option)
        end
        i = i + 2
      elseif command == SB then
        s.iac = "sub"
        i = i + 1
      else
        if command == IP then s.interrupts = s.interrupts + 1 end
        i = i + 1
      end
    elseif b == 3 then
      s.interrupts = s.interrupts + 1
    elseif b == 10 or b == 13 then
      -- CR LF, CR NUL or LF: one end of line, however it came.
      if not (b == 10 and s.last == 13) then
        s.lines[#s.lines + 1] = s.partial
        s.partial = ""
      end
    elseif b ~= 0 then
      s.partial = s.partial .. string.char(b)
    end

    s.last = b
    i = i + 1
  end
end

--------------------------------------------------------------------------
-- A file, as base64 - `get`. The encoding is the Compression Kit's, in C,
-- since 5 October 2026; this was a loop over bytes in Lua, a second copy
-- of the one Cafesa3D had in the 3D Kit.
--------------------------------------------------------------------------

-- A window of the file: a line is 57 bytes, and three to a group, so no
-- window but the last ends in padding.
local WINDOW = 57 * 1024

-- One, kept: a region is not given back, so one a `get` would be a leak.
local window = nil

local function get(s, path)
  local attrs = fs.getattr(path)

  if not attrs or attrs.kind == "directory" or math.type(attrs.size) ~= "integer" then
    send(s, "get: " .. path .. ": not a file\n")
    return
  end

  window = window or sys.memory(WINDOW // 4096 + 1)

  local region = window

  if not region then
    send(s, "get: no room to read " .. path .. "\n")
    return
  end

  send(s, ("BEGIN %d %s\n"):format(attrs.size, path))

  local done = 0

  while done < attrs.size do
    local want = math.min(WINDOW, attrs.size - done)
    local got = fs.read_into(path, region, done, want)

    if got ~= want then
      send(s, "get: stopped after " .. done .. " bytes\n")
      return
    end

    local text = compress.base64(sys.region_read(region, 0, want))

    for at = 1, #text, 76 do
      s.out = s.out .. text:sub(at, at + 75) .. "\r\n"
    end

    done = done + want
  end

  send(s, "END\n")
end

--------------------------------------------------------------------------
-- A file sent here, as base64 - `put`, `get`'s mirror.
--
-- `put <path> <size>`, then the file in base64 a line at a time, then a line
-- saying `END`: so a Lua application written on the Mac lands in `/Home/Apps`
-- and runs (`roadmap.md`, remote; Diego: "We could also even write Lua apps
-- in the Mac and push them to the m700"). The folders on the way are made.
-- Written whole, from a region, as a file is written here - one the file's
-- size, made for it and given back after (`regions.write_string`). It was
-- one kept and grown when a larger file came, on the belief that a region
-- is not given back, and each one it grew past was never given back.
--------------------------------------------------------------------------

local function folders_for(path)
  local at = 1

  while true do
    local slash = path:find("/", at + 1, true)

    if not slash then return end

    local folder = path:sub(1, slash - 1)

    if folder ~= "" and not fs.getattr(folder) then
      fs.send(folder, { type = "mkdir" })
    end

    at = slash
  end
end

local function put_done(s)
  local p = s.receiving

  s.receiving = nil

  -- Refused rather than raised: a line that is not base64 says where.
  local decoded, bytes = pcall(compress.unbase64, table.concat(p.parts))

  if not decoded then
    send(s, "put: " .. p.path .. ": " .. tostring(bytes) .. "\n")
    return
  end

  if #bytes ~= p.size then
    send(s, ("put: %s: %d bytes arrived of %d; not written\n"):format(p.path, #bytes, p.size))
    return
  end

  folders_for(p.path)

  local wrote, why = regions.write_string(p.path, bytes)

  if wrote then
    send(s, ("put: %s, %d bytes\n"):format(p.path, #bytes))
  else
    send(s, "put: " .. p.path .. ": " .. tostring(why) .. "\n")
  end
end

--------------------------------------------------------------------------
-- A line typed: a word this process answers, or a program.
--------------------------------------------------------------------------

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

local function resolve(s, p)
  if not p or p == "" then return s.cwd end
  if p:sub(1, 1) == "/" then return tidy(p) end

  return tidy(s.cwd .. "/" .. p)
end

local function launch(s, text)
  local name, rest = text:match("^%s*(%S+)%s*(.-)%s*$")

  if not name then return prompt(s) end

  if name == "exit" or name == "logout" then
    send(s, "bye\n")
    s.leaving = true
    return
  end

  if name == "cd" then
    local target = resolve(s, rest)

    if fs.list(target) then
      s.cwd = fs.canonical and fs.canonical(target) or target
    else
      send(s, "cd: " .. target .. ": not a folder\n")
    end

    return prompt(s)
  end

  if name == "pwd" then
    send(s, s.cwd .. "\n")
    return prompt(s)
  end

  if name == "help" then
    local out = {}

    for _, dir in ipairs({ "/Kosmos/Programs", "/Kosmos/Apps" }) do
      for _, f in ipairs(fs.list(dir) or {}) do
        out[#out + 1] = f:gsub("%.lua$", "")
      end
    end

    send(s, "this session's own: cd pwd get put help exit\n"
            .. "programs, and applications to `open`:\n  "
            .. table.concat(out, "  ") .. "\n")
    return prompt(s)
  end

  if name == "get" then
    get(s, resolve(s, rest))
    return prompt(s)
  end

  if name == "put" then
    local where, size = rest:match("^(%S+)%s+(%d+)$")

    if not where or tonumber(size) > 64 * 1024 * 1024 then
      send(s, "put: put <path> <size>, then the file in base64, then END\n")
      return prompt(s)
    end

    -- The lines that follow are the file's, until END.
    s.receiving = { path = resolve(s, where), size = tonumber(size), parts = {} }
    return
  end

  local path

  if name:match("%.lua$") or name:find("/", 1, true) then
    path = resolve(s, name)
  else
    path = fs.program(name)
  end

  if not path or not fs.getattr(path) then
    send(s, name .. ": no such program\n")
    return prompt(s)
  end

  -- Handed this session as its console, detached, as the Terminal does;
  -- its output arrives as `write`s, and its end through `sys.wait`.
  local ok, err, id = run(path, rest, true,
                          { ["/Devices/console"] = { cap = s.ep, proto = "console" } },
                          s.cwd)

  if ok then
    s.child = id
    s.running_name = name
    note(dotted(s.from) .. "  " .. text:match("^%s*(.-)%s*$"))
    publish()
  else
    send(s, name .. ": " .. tostring(err) .. "\n")
    prompt(s)
  end
end

--------------------------------------------------------------------------
-- The console protocol, answered for each session's programs.
--------------------------------------------------------------------------

local function serve_console(s)
  while true do
    local bytes, who = sys.receive_raw(s.ep, true)

    if not bytes then return end

    local req = con.decode_request(bytes)
    local reply

    if not req then
      reply = { error = con.ERR_BAD_OP }
    elseif req.op == con.WRITE then
      send(s, req.text)
      reply = {}
    elseif req.op == con.READ then
      -- A line typed while the program runs is its; none yet, and the reply
      -- waits for one rather than saying there is nothing.
      if #s.lines > 0 then
        reply = { line = table.remove(s.lines, 1) }
      else
        s.reader = who
      end
    elseif req.op == con.POLL then
      reply = { seen = s.interrupts > 0 and 1 or 0 }
      s.interrupts = 0
    elseif req.op == con.KEYS then
      reply = {}
    else
      reply = { error = con.ERR_BAD_OP }
    end

    if reply then
      pcall(sys.reply_raw, who, con.encode_reply(reply))
    end
  end
end

local function open(conn, from)
  local s = { conn = conn, from = from, ep = sys.endpoint(), cwd = "/Home",
              out = "", partial = "", lines = {}, interrupts = 0 }

  if not s.ep then
    conn:write("telnetd: no endpoint for a console\r\n")
    conn:close()
    return
  end

  sessions[#sessions + 1] = s
  note(dotted(from) .. "  connected")
  publish()

  -- What machine this is, as a Terminal starts; the prompt comes when it ends.
  launch(s, "neofetch")
end

local function close(at)
  local s = sessions[at]

  s.conn:close()
  sys.destroy(s.ep)
  table.remove(sessions, at)
  note(dotted(s.from) .. "  gone")
  publish()
end

--------------------------------------------------------------------------
-- The loop: connections, consoles and children, in one process.
--------------------------------------------------------------------------

while true do
  local reading, writing, running = {}, {}, false

  for _, s in ipairs(sessions) do
    reading[#reading + 1] = s.conn

    if #s.out > 0 then writing[#writing + 1] = s.conn end
    if s.child then running = true end
  end

  -- A child's output arrives on its console, which the network cannot wake
  -- this for: a tick while one runs, a tenth of a second otherwise.
  local ready, arrived = fs.poll("/Network", reading, writing, listener,
                                 running and 1 or 25)

  if not ready then
    print("telnetd: poll: " .. tostring(arrived))
    break
  end

  if arrived then
    local conn, from = fs.accept("/Network", listener, 1)

    if conn and not neighbour(from) then
      note(dotted(from) .. "  refused, not on this network")
      conn:close()
    elseif conn then
      open(conn, from)
    end
  end

  for at = #sessions, 1, -1 do
    local s = sessions[at]
    local piece = s.conn:read()

    while piece do
      take(s, piece)
      piece = s.conn:read()
    end

    serve_console(s)

    -- A file coming in takes every line until its END.
    while s.receiving and #s.lines > 0 do
      local line = table.remove(s.lines, 1)

      if line == "END" then
        put_done(s)
        prompt(s)
      else
        s.receiving.parts[#s.receiving.parts + 1] = line
      end
    end

    -- A line for a program that asked, or for this session to run.
    if s.receiving then
      -- still arriving
    elseif s.reader and #s.lines > 0 then
      pcall(sys.reply_raw, s.reader,
            con.encode_reply({ line = table.remove(s.lines, 1) }))
      s.reader = nil
    elseif not s.child and not s.leaving and #s.lines > 0 then
      launch(s, table.remove(s.lines, 1))
    end

    while #s.out > 0 do
      local wrote = s.conn:write(s.out)

      if not wrote or wrote == 0 then break end

      s.out = s.out:sub(wrote + 1)
    end

    if s.conn:closed() or (s.leaving and #s.out == 0) then
      close(at)
    end
  end

  -- A request from the Servers window: end an address's sessions.
  while control do
    local req, who = sys.receive(control, true)

    if not req then break end

    local ended = 0

    if type(req) == "table" and req.type == "disconnect" then
      for at = #sessions, 1, -1 do
        if dotted(sessions[at].from) == tostring(req.from) then
          sessions[at].out = sessions[at].out .. "\r\ndisconnected from this machine\r\n"
          sessions[at].leaving = true
          ended = ended + 1
        end
      end
    end

    pcall(sys.reply, who, { ok = ended > 0, ended = ended })
  end

  -- Whichever child ended, the session it was running in has a prompt.
  local id, code = sys.wait(true)

  while id do
    for _, s in ipairs(sessions) do
      if s.child == id then
        serve_console(s)
        s.child = nil
        s.running_name = nil
        publish()

        if s.reader then
          pcall(sys.reply_raw, s.reader, con.encode_reply({ error = con.ERR_BAD_OP }))
          s.reader = nil
        end

        if code and code ~= 0 then send(s, ("(exit code %d)\n"):format(code)) end

        prompt(s)
      end
    end

    id, code = sys.wait(true)
  end

end

for at = #sessions, 1, -1 do close(at) end
