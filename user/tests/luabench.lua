-- The benchmarks that measure Lua, at EL0.
--
-- `serialize` and `gc_pause_max` used to run inside the kernel, against a
-- `lua_State` the kernel carried. There is no Lua in the kernel any more,
-- and a benchmark of an interpreter in a privilege level it does not run at
-- is a number about nothing.
--
-- Two things changed with the move, and both are visible in the numbers:
--
--   - `serialize` now allocates. The C version packed into a `struct
--     message` and never touched the heap; `sys.pack` returns a Lua string,
--     so every round costs an allocation and its share of a collection.
--     That is what a caller out here actually pays.
--   - interrupts are live. A process cannot mask them, so the timer lands
--     inside the measured window. For `gc_pause_max` that is an improvement
--     rather than noise: `testing.md` §18.5 cares about the longest the
--     system stops for, and a pause the user feels includes the tick that
--     landed in it.
--
-- Both are deterministic under `-icount`, which is the only property these
-- numbers need.

local role, BASE = ...

local R_SERIALIZE     = 0
local R_SERIALIZE_ONE = 1
local R_GC            = 2

local ROUNDS   = 20000
local STATES   = 5
local GC_STEPS = 2000

-- What a message between servers actually looks like: a tag, a couple of
-- strings, and a small nested table of options.
local function typical()
  return { tag = 3, op = "read", path = "/dev/temp",
           opts = { follow = true, limit = 64 } }
end

-- Hands one measurement to the kernel's harness, which is blocked in a
-- receive on capability 0 while this runs.
--
-- The numbers travel in the message *tag*, which is a raw 64-bit field the
-- kernel copies without looking at. The body would be a serialised Lua
-- value, and unpacking one needs a lua_State - the thing that is no longer
-- in there. Two calls rather than two fields for the same reason.
--
-- Total first, then iterations, so the two are one exchange and neither side
-- has to hold a count the other might drift from.
local function report(total, iterations)
  sys.call(0, { tag = total })
  sys.call(0, { tag = iterations })
end

if role == R_SERIALIZE_ONE then
  -- One state's worth. The parent runs several of these and keeps the best,
  -- because Lua randomises string hashing per state and the collision
  -- patterns underneath a table cost what they cost.
  local value = typical()
  local start = sys.ticks()

  for _ = 1, ROUNDS do
    sys.unpack(sys.pack(value))
  end

  -- The measurement leaves as an exit code. It is an int the kernel already
  -- carries, the parent is already waiting on it, and twenty million ticks
  -- fits in one with room to spare.
  sys.exit(sys.ticks() - start)
end

-- Returns total, iterations. Raising is allowed; the caller turns it into a
-- report of zero rather than letting it end the process, because a process
-- that dies without reporting leaves the harness blocked in a receive.
local function measure(r)
  if r == R_SERIALIZE then
    local best

    for _ = 1, STATES do
      sys.spawn(BASE + R_SERIALIZE_ONE, {})

      -- One at a time, so the one being measured is the only thing runnable.
      local id, code = sys.wait()
      if not id then error("a serialize run vanished") end
      if code <= 0 then error("a serialize run reported " .. tostring(code)) end

      if best == nil or code < best then best = code end
    end

    return best, ROUNDS
  end

  if r == R_GC then
    -- The longest the collector stops for. `design.md` §5.2 calls the GC
    -- this project's recurring problem, and `testing.md` §18.5 is emphatic
    -- that the maximum is what matters and not the average: a system
    -- averaging 8 ms a frame with a 40 ms spike every two seconds feels
    -- worse than one holding a steady 14 ms.
    local garbage = {}
    for i = 1, 3000 do garbage[i] = { i, tostring(i), { i } } end
    collectgarbage("setpause", 100)

    local worst = 0

    for _ = 1, GC_STEPS do
      local start = sys.ticks()
      collectgarbage("step", 1)
      local took = sys.ticks() - start
      if took > worst then worst = took end
    end

    -- One operation, because the number is a maximum and not a rate.
    return worst, 1
  end

  error("no such role: " .. tostring(r))
end

local ok, total, iterations = pcall(measure, role)

if not ok then
  -- `total` holds the error. Say what it was - this process was given the
  -- console for exactly this - and then report zeros anyway, so the harness
  -- prints bench-fail instead of waiting for ever on a message that is never
  -- coming.
  sys.write("luabench: " .. tostring(total) .. "\n")
  report(0, 0)
  sys.exit(1)
end

report(total, iterations)
sys.exit(0)
