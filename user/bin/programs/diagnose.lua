-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- diagnose: what a diagnosis of this machine needs, in one file.
--
--   diagnose             to /home/diagnose.txt
--   diagnose name        to /home/name, or to a path that begins with /
--
-- **Instead of photographs of a screen.** Diego, on the ThinkPad, 14
-- September 2026: "a script or something I can run in the think pad like a
-- log file of things you need so I can send it to you for a full diagnosis
-- ... instead of photos of logs". A photograph holds forty lines of one
-- command. This holds what `devices`, `diskinfo`, `ls /home`, `sticks` and the
-- process list would each have shown, and the whole log after them; on the
-- Mac, `make stick-log` brings the file back off the stick.
--
-- **It asks, as those programs do, and runs none of them.** A program is a
-- process the shell starts with the capabilities it names, and one program
-- cannot start another. So each section reads what its program reads -
-- `sys.info()`, the nodes in `/dev`, `/home/.super`, the USB driver through
-- `/lib/blocks.lua`, `sys.processes()` - and writes the answer whole, as a
-- table rather than a sentence, because a diagnosis wants the field nobody
-- thought to print.
--
-- **The log is last**, so the file ends with the command that wrote it, and a
-- file that does not end there was cut short.

local lines = {}

local function say(text)
  lines[#lines + 1] = text
end

local function section(name)
  say("")
  say("== " .. name)
end

-- Numbers before names, each in order: a table's keys as somebody reads them.
local function sorted_keys(t)
  local keys = {}

  for k in pairs(t) do keys[#keys + 1] = k end

  table.sort(keys, function(a, b)
    if type(a) == type(b) and (type(a) == "number" or type(a) == "string") then
      return a < b
    end

    return type(a) == "number"
  end)

  return keys
end

local function shown(v)
  if type(v) == "string" then
    return (v:gsub("\n", "\\n"))
  end

  return tostring(v)
end

-- A value whole: its fields one a line, a table inside another beneath it.
local function dump(value, indent)
  if type(value) ~= "table" then
    say(indent .. shown(value))
    return
  end

  for _, k in ipairs(sorted_keys(value)) do
    local v = value[k]

    if type(v) == "table" then
      say(indent .. tostring(k) .. ":")
      dump(v, indent .. "  ")
    else
      say(indent .. tostring(k) .. " = " .. shown(v))
    end
  end
end

-- A table on one line, for a list of many: a process each.
local function one_line(t)
  local parts = {}

  for _, k in ipairs(sorted_keys(t)) do
    parts[#parts + 1] = tostring(k) .. "=" .. shown(t[k])
  end

  return table.concat(parts, " ")
end

-- A node read and written whole, or why it could not be read.
local function node(path)
  local value, why = fs.read(path)

  if value == nil then
    say("  " .. path .. ": " .. tostring(why))
    return
  end

  say("  " .. path)
  dump(value, "    ")
end

local info = sys.info() or {}
local build = sys.build and sys.build() or {}
local hz, origin = info.counter_hz or 0, info.log_origin or 0

say("Kosmos diagnosis")
say(("%s %s, build %s, written %s"):format(
    build.name or "Kosmos", build.version or "an unknown version",
    build.build or "unknown",
    hz > 0 and ("%.2f s into the log"):format((sys.ticks() - origin) / hz)
      or "at an unknown time"))

section("build")
dump(build, "  ")

section("machine")
dump(info, "  ")

section("devices")

--
-- **Not every name in `/dev` is the device server's.** `fs.list` gives what
-- is mounted below a directory as well as what its server holds, and three
-- things mounted there answer a read in their own way. `/dev/console` answers
-- with a line somebody types, so the first version of this program sat at
-- the prompt waiting for one and wrote nothing at all.
--
local NOT_READ = {
  console = "a read of it is a line of typed input",
  audio   = "the audio server speaks its own protocol, not a read",
  blocks  = "the USB driver speaks its own protocol; the sticks are below",
}

for _, entry in ipairs(fs.list("/dev") or {}) do
  local name = type(entry) == "table" and entry.name or entry

  if NOT_READ[name] then
    say("  /dev/" .. name .. ": not read - " .. NOT_READ[name])
  else
    node("/dev/" .. name)
  end
end

section("disk")
node("/home/.super")
node("/home/.device")

section("/home")

do
  local names, why = fs.list("/home")

  if not names then
    say("  " .. tostring(why))
  end

  for _, entry in ipairs(names or {}) do
    local name = type(entry) == "table" and entry.name or entry
    local attrs = fs.getattr("/home/" .. name) or {}

    say(("  %s  %s%s"):format(name, attrs.kind or "",
        attrs.size and (", " .. attrs.size .. " bytes") or ""))
  end
end

section("sticks")

do
  local blocks = use("/lib/blocks.lua")
  local named, why = blocks.units()

  if not named then
    say("  " .. tostring(why))
  else
    say(("  %d unit(s) named"):format(named))

    for unit = 0, named - 1 do
      local i = blocks.info(unit)

      if i then
        say(("  unit %d: %d blocks of %d bytes, \"%s\" \"%s\"")
            :format(unit, i.blocks, i.block_size, i.vendor, i.product))
      else
        say(("  unit %d: not ready, or gone"):format(unit))
      end
    end
  end
end

section("processes")

do
  local list, why = sys.processes()

  if not list then
    say("  " .. tostring(why))
  end

  for _, p in ipairs(list or {}) do
    say("  " .. one_line(p))
  end
end

section("log")
say(((sys.log(262144) or ""):gsub("\r", "")))

--
-- Through pages rather than a message, as `log save` does: the log alone is
-- a quarter of a megabyte, and a message holds two kilobytes.
--
local name = args:match("^%s*(%S+)") or "diagnose.txt"
local path = name:sub(1, 1) == "/" and name or ("/home/" .. name)
local body = table.concat(lines, "\n")
local buf = sys.memory((#body + 4095) // 4096)

if not buf then
  print(("diagnose: no memory for %d bytes"):format(#body))
  return
end

sys.region_write(buf, 0, body)

local wrote, err = fs.write_from(path, buf, #body)

sys.release(buf)

if not wrote then
  print("diagnose: " .. path .. ": " .. tostring(err))
  return
end

print(("diagnose: %d KB saved to %s - on the Mac, `make stick-log` brings it back")
      :format((#body + 1023) // 1024, path))
