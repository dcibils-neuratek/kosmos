-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- Every syscall's arguments: what the kernel reads, against what userland
-- passes.
--
-- A syscall wrapper names how many arguments it loads - `sys1`, `sys2` - and
-- the kernel's case reads `sc->arg[0]` to `sc->arg[4]`. Both architectures
-- copy every argument register into `sc->arg` whether the wrapper loaded it
-- or not (`arch/aarch64/trap.c`, `arch/x86_64/user.S`), so **a wrapper that
-- passes fewer than its case reads hands the kernel whatever that register
-- last held**. Nothing fails to compile and nothing fails at once.
--
-- It happened: `kosmos_mem_create` was `sys1`, and `SYS_MEM_CREATE` reads
-- `arg[1]` as flags, where bit 0 asks for one physical run - so a register
-- left over from the caller decided whether an ordinary region could be
-- refused on a fragmented machine. Found by reading, in September 2026, and
-- the only one of fifty-one.
--
-- Usage: lua tools/test_syscall_args.lua kernel/syscall.c source...
--
-- `make test` passes the userland sources by wildcard, so a new file's calls
-- are checked without this one being edited.

local path = arg and arg[1]

if not path or #arg < 2 then
  print("usage: lua tools/test_syscall_args.lua kernel/syscall.c source...")
  os.exit(2)
end

local function slurp(name)
  local file = assert(io.open(name, "rb"))
  local text = file:read("a")
  file:close()
  return text
end

local passed = 0
local failures = {}

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failures[#failures + 1] = what
  end
end

-- What the kernel reads ------------------------------------------------------

local source = slurp(path)
local labels = {}

for at, name, after in source:gmatch("()%f[%w]case (SYS_[%u%d_]+):()") do
  labels[#labels + 1] = { at = at, name = name, after = after }
end

local reads = {}

for i, label in ipairs(labels) do
  local finish = labels[i + 1] and labels[i + 1].at - 1 or #source
  local body = source:sub(label.after, finish)
  local most = 0

  for n in body:gmatch("sc%->arg%[(%d)%]") do
    most = math.max(most, tonumber(n) + 1)
  end

  reads[label.name] = most
end

-- Labels with nothing between them share the next one's body.
for i = #labels - 1, 1, -1 do
  local between = source:sub(labels[i].after, labels[i + 1].at - 1)

  if between:match("^%s*$") then
    reads[labels[i].name] = reads[labels[i + 1].name]
  end
end

local cases = 0

for _ in pairs(reads) do
  cases = cases + 1
end

-- A scan that found nothing would pass everything below.
check(cases >= 40, ("the kernel's switch was read: %d cases"):format(cases))

-- What userland passes -------------------------------------------------------

local calls = 0
local seen = {}

for i = 2, #arg do
  -- Read once, however many of the patterns it was given name it.
  if not seen[arg[i]] then
    local text = slurp(arg[i])

    seen[arg[i]] = true

    for count, name in text:gmatch("%f[%w]sys([0-5])%((SYS_[%u%d_]+)") do
      local kernel = reads[name]

      calls = calls + 1
      check(kernel ~= nil,
            ("%s: %s is not a case in %s"):format(arg[i], name, path))

      if kernel ~= nil then
        check(tonumber(count) >= kernel,
              ("%s: sys%s(%s) passes %s, and the kernel reads %d"):format(
              arg[i], count, name, count, kernel))
      end
    end
  end
end

check(calls >= 40, ("userland's calls were read: %d"):format(calls))

if #failures == 0 then
  print(("PASS: %d checks on what each syscall reads and what userland " ..
         "passes it (%d cases, %d calls, none short)."):format(
         passed, cases, calls))
else
  print(("FAIL: %d of %d checks on syscall arguments:"):format(
        #failures, passed + #failures))

  for _, what in ipairs(failures) do
    print("  " .. what)
  end

  os.exit(1)
end
