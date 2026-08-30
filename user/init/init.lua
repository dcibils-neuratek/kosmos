-- init: the first Lua that runs outside the kernel.
--
-- Everything here executes at EL0, in its own address space, on a lua_State
-- whose heap is two megabytes the kernel mapped and nothing more. If any of
-- it goes wrong, this process dies and the system does not.

local function line(s) sys.write(s .. "\n") end

line("init: Lua " .. _VERSION .. " at EL0")
line("init: 2+2 = " .. (2 + 2) .. ", 1/2 = " .. (1 / 2))
line("init: sqrt(2) = " .. math.sqrt(2))

-- Coroutines, which design.md 4.5 rests the whole server model on.
local co = coroutine.create(function(a)
  local b = coroutine.yield(a * 2)
  return b + 1
end)
local _, x = coroutine.resume(co, 21)
local _, y = coroutine.resume(co, 100)
line("init: coroutine gave " .. x .. " then " .. y)

-- An error raised and caught, which is setjmp/longjmp working at EL0.
local ok, err = pcall(function() error("deliberate") end)
line("init: pcall caught " .. tostring(ok) .. " " .. err)

-- The libraries that must not be here.
line("init: io=" .. tostring(io) .. " os=" .. tostring(os)
     .. " debug=" .. tostring(debug))

-- And the garbage collector, on a heap it cannot grow.
collectgarbage()
local before = collectgarbage("count")
local t = {}
for i = 1, 2000 do t[i] = { i } end
local peak = collectgarbage("count")
t = nil
collectgarbage()
line(string.format("init: gc %.0fK -> %.0fK -> %.0fK",
                   before, peak, collectgarbage("count")))

line("init: done")
