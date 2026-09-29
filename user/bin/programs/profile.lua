-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs profile
-- profile: where every processor spends its time, for the Mac to read.
--
--   profile              ten seconds, to /Home/profiles/<date>.kprof
--   profile 30           thirty
--   profile 30 name      to /Home/profiles/name.kprof, or to a path that
--                        begins with /
--
-- **The first step of the App Inspector** (`roadmap.md`; Diego, 29
-- September: "lets profile in the m700 the Lua VS C"). The kernel notes, at
-- every tick on every processor, which process was running and the address
-- it was interrupted at (`kernel/profile.c`); this drains that into a region
-- and writes it to a file, with what the addresses need in order to mean
-- something: which process was which program, and which image it ran.
--
-- **What an address means is decided on the Mac**, where the build's
-- symbols are: `make profile-report` names each one - the Lua VM, the
-- collector, a kit, a server, the kernel - and draws the page. This prints
-- only what the kernel itself knows, a share for each process, so a profile
-- says something where it is taken.
--
-- It stops at its time, at Control-C, or when its region is full, and it is
-- asleep between drains: it is in its own profile, and should be small there.

local clock = use("/Kosmos/Libraries/clock.lua")

local words = {}

for w in tostring(args or ""):gmatch("%S+") do words[#words + 1] = w end

local seconds = tonumber(words[1]) or 10

if seconds <= 0 or seconds > 600 then
  print("profile: a profile is between one second and ten minutes")
  return
end

local SAMPLE = 16                       -- `struct profile_sample`
local SAMPLE_FORMAT = "<I8I4I2BB"       -- pc, pid, thread, cpu, where
local HEAD_ROOM = 64 * 1024             -- the header is written in front
local WHERE = { [1] = "user", [2] = "kernel", [3] = "idle" }

local cpu = fs.read("/Devices/cpu") or {}
local timer = fs.read("/Devices/timer") or {}
local cores = cpu.cores or 1
local tick_hz = timer.hz or 250
local counter_hz = cpu.counter_hz or timer.counter_hz or 1

-- Every sample the whole profile can make, and a second of slack.
local capacity = (seconds + 1) * cores * tick_hz
local pages = (HEAD_ROOM + capacity * SAMPLE + 4095) // 4096
local region = sys.memory(pages)

if not region then
  print(("profile: no room for %d KB of samples"):format(pages * 4))
  return
end

local limit = pages * 4096

-- Where it goes: named, or dated, and always a `.kprof` the report knows.
local function destination()
  local name = words[2]

  if name and name:sub(1, 1) == "/" then return name end

  if not name then
    local t = clock.now()

    name = t and ("%04d-%02d-%02d-%02d%02d%02d"):format(t.year, t.month, t.day,
                                                      t.hour, t.min, t.sec)
               or ("profile-%d"):format(sys.ticks())
  end

  if not name:find("%.kprof$") then name = name .. ".kprof" end

  fs.send("/Home/profiles", { type = "mkdir" })
  return "/Home/profiles/" .. name
end

local path = destination()

-- **Every process seen**, merged across the profile: one that ended before
-- it did still has samples, and still needs a name beside them.
local seen, order = {}, {}

local function note_processes()
  for _, p in ipairs(sys.processes() or {}) do
    if not seen[p.id] then
      seen[p.id] = { id = p.id, parent = p.parent or 0, name = p.name or "",
                     from = p.from or "" }
      order[#order + 1] = p.id
    end
  end
end

note_processes()

local ok, why = sys.profile("start")

if not ok then
  print("profile: " .. tostring(why))
  return
end

local at = HEAD_ROOM
local began = sys.ticks()
local ends = began + seconds * counter_hz
local full, stopped = false, false

local function drain()
  local n = sys.profile("read", region, at) or 0

  at = at + n * SAMPLE

  if at + SAMPLE > limit then full = true end
end

print(("profile: %d s on %d processors, to %s"):format(seconds, cores, path))

while sys.ticks() < ends and not full do
  sys.sleep(math.max(1, tick_hz // 4))
  drain()
  note_processes()

  if interrupted() then
    stopped = true
    break
  end
end

drain()

local lost = sys.profile("lost") or 0
local elapsed = sys.ticks() - began

sys.profile("stop")
note_processes()

local samples = (at - HEAD_ROOM) // SAMPLE

--
-- **Which image each process ran**: this one's, unless its program names
-- another in its header (`kosmos: image quake.elf`, `docs/elf.md` step 4),
-- which is the file of that name beside it. The report reads the addresses
-- of each against the right symbols.
--
local function image_of(from)
  if from == "" then return "init.elf" end

  local source = fs.read(from)

  if type(source) ~= "string" then return "init.elf" end

  for line in (source .. "\n"):gmatch("(.-)\n") do
    if line ~= "" and not line:match("^%-%-") then break end

    local name = line:match("^%-%-%s*kosmos:%s*image%s+(%S+)")

    if name then return name end
  end

  return "init.elf"
end

local build = sys.build() or {}

-- One C function's address, as this image has it, so the report can tell
-- the symbols it was given are the ones that ran rather than a rebuild.
local anchor = tostring(string.format):match("0x%x+") or ""

local head = {
  "kprof\t1",
  "version\t" .. tostring(build.version or "?"),
  "build\t" .. tostring(build.build or "?"),
  "platform\t" .. tostring(build.platform or "?"),
  "cores\t" .. cores,
  "tick_hz\t" .. tick_hz,
  "counter_hz\t" .. counter_hz,
  "asked\t" .. seconds,
  "counter_ticks\t" .. elapsed,
  "samples\t" .. samples,
  "lost\t" .. lost,
  "anchor\tstr_format\t" .. anchor,
}

for _, id in ipairs(order) do
  local p = seen[id]

  head[#head + 1] = ("process\t%d\t%d\t%s\t%s\t%s"):format(
      p.id, p.parent, p.name, p.from, image_of(p.from))
end

head[#head + 1] = "end\n"

local text = table.concat(head, "\n")

if #text > HEAD_ROOM then
  print("profile: more processes than the header holds; not written")
  return
end

-- The header, then the samples moved down to meet it: one file, one write.
sys.region_write(region, 0, text)
sys.region_copy(region, #text, region, HEAD_ROOM, samples * SAMPLE)

local wrote, werr = fs.write_from(path, region, #text + samples * SAMPLE)

--
-- **What the kernel knows, said here**: each process's share of every
-- processor's ticks, in its own code and in the kernel on its behalf.
--
local per, idle = {}, 0
local CHUNK = 4096 * SAMPLE
local done = 0

while done < samples * SAMPLE do
  local n = math.min(CHUNK, samples * SAMPLE - done)
  local bytes = sys.region_read(region, #text + done, n)

  for i = 1, n, SAMPLE do
    local _, pid, _, _, where = string.unpack(SAMPLE_FORMAT, bytes, i)

    if WHERE[where] == "idle" then
      idle = idle + 1
    else
      local c = per[pid]

      if not c then
        c = { pid = pid, user = 0, kernel = 0 }
        per[pid] = c
      end

      if WHERE[where] == "user" then c.user = c.user + 1 else c.kernel = c.kernel + 1 end
    end
  end

  done = done + n
end

local rows = {}

for _, c in pairs(per) do rows[#rows + 1] = c end

table.sort(rows, function(a, b)
  return a.user + a.kernel > b.user + b.kernel
end)

local function pct(n) return samples > 0 and 100 * n / samples or 0 end

print(("profile: %d samples in %.1f s, %d lost%s%s"):format(
    samples, elapsed / counter_hz, lost,
    full and "; stopped with its region full" or "",
    stopped and "; stopped by Control-C" or ""))
print(("  %5.1f%%  idle"):format(pct(idle)))

for i = 1, math.min(#rows, 15) do
  local c = rows[i]
  local p = seen[c.pid]
  local name = c.pid == 0 and "the kernel" or (p and p.name) or ("#" .. c.pid)

  print(("  %5.1f%%  %-16s %5.1f%% its own code, %5.1f%% in the kernel"):format(
      pct(c.user + c.kernel), name, pct(c.user), pct(c.kernel)))
end

if wrote then
  print(("written to %s, %d KB - `make profile-report` on the Mac"):format(
      path, (#text + samples * SAMPLE + 1023) // 1024))
else
  print("profile: could not write " .. path .. ": " .. tostring(werr))
end
