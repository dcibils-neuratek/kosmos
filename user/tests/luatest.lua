-- The Lua tests, at EL0.
--
-- Every test here used to live in `tests/tests.c`, driving a `lua_State`
-- that the kernel carried inside itself. The kernel has no Lua in it any
-- more, and testing an interpreter at a privilege level it does not run at
-- was testing a copy of it.
--
-- The shape is one role per test. `tests/tests.c` starts a process in the
-- role, waits for it, and turns its exit code into a TAP line, so the names,
-- the count and the numbering all stay on the C side where they were. A
-- failure is an `error()`: it unwinds to `user/init/main.c`, which prints it
-- and returns non-zero.
--
-- This chunk is built into the test image only. It is not part of what
-- ships, and `user/init/init.lua` does not know it exists.

local role, BASE = ...

-- Roles are numbered from zero here and offset by BASE on the wire, which is
-- how `main.c` tells a test process from an init one. A child spawned from
-- here is this same image in another role, so the offset goes back on.
local R_ARITH        = 0
local R_FLOATS       = 1
local R_STRINGS      = 2
local R_CLOSURES     = 3
local R_COROUTINES   = 4
local R_PCALL        = 5
local R_PCALL_NEST   = 6
local R_MATH         = 7
local R_GC           = 8
local R_NO_LIBS      = 9
local R_NO_BYTECODE  = 10

local R_IPC_CLIENT   = 11
local R_IPC_DOUBLE   = 12
local R_TABLE_CLIENT = 13
local R_TABLE_ECHO   = 14
local R_REFUSES      = 15
local R_CAP_MAIN     = 16
local R_CAP_BROKER   = 17
local R_CAP_SECRET   = 18
local R_CAP_CLIENT   = 19
local R_NOCAP_CLIENT = 20
local R_NOCAP_SERVER = 21
local R_IPC_ERRORS   = 22
local R_BLOCKED      = 23
local R_GFX          = 24
local R_GFX_BLEND    = 25
local R_NO_SCREEN    = 26

-- The tag that asks a server to stop. Every other tag in here is positive,
-- so there is nothing for it to collide with.
local STOP = 0

local function check(c, what)
  if not c then error(what, 2) end
end

local function spawn(r, caps)
  local id, err = sys.spawn(BASE + r, caps)
  check(id, "spawn: " .. tostring(err))
  return id
end

-- Waits for n children and fails if any of them did.
--
-- A child has no console, so whatever it printed went nowhere; its exit code
-- is all that comes back. That is enough to say which one broke, and the way
-- to see why is to run that role as the top-level process instead.
local function wait_all(n)
  for _ = 1, n do
    local id, code = sys.wait()
    check(id, "a child vanished before it could be waited for")
    check(code == 0, "child " .. tostring(id) .. " exited " .. tostring(code))
  end
end

-- ------------------------------------------------------------------
-- The language.
-- ------------------------------------------------------------------

if role == R_ARITH then
  -- M2's definition of done, as an assertion rather than a printed line.
  check(2 + 2 == 4, "2 + 2")
  check(7 // 2 == 3 and 7 % 2 == 1, "integer division")
  check(math.type(1) == "integer", "1 is an integer")
  check(math.type(1.0) == "float", "1.0 is a float")
  sys.exit(0)
end

if role == R_FLOATS then
  -- The whole number path at once: the parser calls strtod on the literal,
  -- the arithmetic runs in FP registers that only exist because CPACR was
  -- opened, and tostring goes back out through our snprintf.
  check(1 / 2 == 0.5, "division")
  check(tostring(1.5) == "1.5", "tostring(1.5)")
  check(tostring(0.25) == "0.25", "tostring(0.25)")
  check(tonumber("3.5") == 3.5, "tonumber('3.5')")
  check(tonumber("1e3") == 1000.0, "tonumber('1e3')")
  check(2.0 ^ 10 == 1024.0, "exponentiation")
  sys.exit(0)
end

if role == R_STRINGS then
  check(("a" .. "b" .. "c") == "abc", "concatenation")
  check(#"hello" == 5, "length")
  check(("hello"):upper() == "HELLO", "string methods")
  check(string.format("%d-%s", 7, "x") == "7-x", "string.format")
  local t = {}
  for i = 1, 100 do t[i] = i * i end
  check(t[10] == 100 and #t == 100, "a table of a hundred")
  local s = 0
  for _, v in ipairs { 1, 2, 3 } do s = s + v end
  check(s == 6, "ipairs")
  sys.exit(0)
end

if role == R_CLOSURES then
  local function counter()
    local n = 0
    return function() n = n + 1 return n end
  end
  local c = counter()
  c() c()
  check(c() == 3, "an upvalue did not survive")
  sys.exit(0)
end

if role == R_COROUTINES then
  -- The single most important Lua feature for this design. `design.md` §4.5
  -- rests the entire server model on coroutines: they are what makes
  -- synchronous IPC writable as sequential code instead of a state machine.
  -- If they did not work, the architecture would not.
  local co = coroutine.create(function(a)
    local b = coroutine.yield(a + 1)
    return b * 2
  end)
  local _, x = coroutine.resume(co, 1)
  local _, y = coroutine.resume(co, 10)
  check(x == 2, "the first yield")
  check(y == 20, "the value sent back in")
  check(coroutine.status(co) == "dead", "it did not finish")
  sys.exit(0)
end

if role == R_PCALL then
  -- The setjmp/longjmp test that matters. The unit tests for them jump
  -- within one function; here Lua raises from inside its own VM, across
  -- frames it built, and unwinds to a pcall. `setup.md` warns that a wrong
  -- longjmp only shows up the first time something raises.
  local ok, e = pcall(function() error("boom") end)
  check(ok == false and e:find("boom") ~= nil, "a string error")

  check(pcall(function() return nil + 1 end) == false, "a runtime error")

  local ok2, e2 = pcall(function() error({ code = 42 }) end)
  check(ok2 == false and e2.code == 42, "a table error")

  -- And that the state is still usable afterwards, which is the part a
  -- botched stack restore breaks.
  check(1 + 1 == 2, "the state after unwinding")
  sys.exit(0)
end

if role == R_PCALL_NEST then
  -- A pcall inside a pcall, with the inner one rethrowing. Nested jmp_bufs
  -- are where an incorrect saved sp shows up as a corrupted outer frame.
  local ok, e = pcall(function()
    local ok2 = pcall(function() error("inner") end)
    error("outer:" .. tostring(ok2))
  end)
  check(ok == false and e:find("outer:false") ~= nil, "nested unwinding")
  sys.exit(0)
end

if role == R_MATH then
  -- Ours and newlib's, reached through Lua rather than called directly.
  check(math.floor(2.7) == 2 and math.ceil(2.1) == 3, "floor and ceil")
  check(math.abs(-3) == 3, "abs")
  check(math.sqrt(16.0) == 4.0, "sqrt")
  check(math.max(1, 5, 3) == 5, "max")
  check(math.fmod(7, 3) == 1.0, "fmod")
  check(type(math.random()) == "number", "random")
  sys.exit(0)
end

if role == R_GC then
  -- The GC is the design's known risk (`design.md` §5.2) and the first thing
  -- to check is simply that it gets memory back at all. A collector running
  -- against an allocator that does not coalesce would show up here as memory
  -- that never drops.
  --
  -- Four thousand tables, not twenty. Twenty thousand runs the 2 MB heap
  -- out, which is itself worth knowing: a Lua table is around 56 bytes and
  -- this allocator adds 32 more per block.
  collectgarbage()
  local before = collectgarbage("count")
  local t = {}
  for i = 1, 4000 do t[i] = { i } end
  local peak = collectgarbage("count")
  t = nil
  collectgarbage()
  local after = collectgarbage("count")
  check(peak > before * 2, "the heap did not grow")
  check(after < peak / 2, "the collector got nothing back")
  sys.exit(0)
end

if role == R_NO_LIBS then
  -- `design.md` §5.3: the list of libraries inside a state is part of the
  -- security model, not a configuration detail. Asserting the absences keeps
  -- a later "just open everything" from passing quietly.
  check(io == nil, "io is open")               -- no global tree to open
  check(os == nil, "os is open")               -- no wall clock, no exec
  check(debug == nil, "debug is open")         -- breaks every abstraction
  check(package == nil, "package is open")     -- wants dlopen
  check(dofile == nil and loadfile == nil, "the file loaders are present")
  sys.exit(0)
end

if role == R_NO_BYTECODE then
  -- `design.md` §5.3 forbids precompiled bytecode: the undump loader
  -- validates almost nothing, so a crafted chunk is arbitrary execution
  -- inside the state.
  --
  -- What this checks is that "t" is honoured, not that it is the default -
  -- it passes the mode explicitly. Every load in this system says "t", and
  -- the day one of them stops, this test will not notice. Making the default
  -- itself safe means wrapping the `load` builtin, which is written down in
  -- `docs/state.md` as open rather than done.
  local dumped = string.dump(function() return 1 end)
  local f, err = load(dumped, "evil", "t")
  check(f == nil and err ~= nil, "text mode accepted bytecode")
  -- and that the same chunk as source loads fine, so the test is really
  -- about the mode and not about load being broken
  check(load("return 1", "ok", "t")() == 1, "text mode rejected text")
  sys.exit(0)
end

-- ------------------------------------------------------------------
-- The kernel, reached from a process.
--
-- These were `sys.*` against the kernel's own Lua, where a "server" was
-- another kernel thread sharing an address space. Out here each one is a
-- real process behind a real page table, and the messages cross EL0 to EL1
-- and back. Same assertions, a boundary further out.
-- ------------------------------------------------------------------

if role == R_IPC_DOUBLE then
  while true do
    local m, who = sys.receive(0)
    if not m then break end
    if m.tag == STOP then
      sys.reply(who, {})
      break
    end
    sys.reply(who, { m[1] * 2, tag = m.tag + 1 })
  end
  sys.exit(0)
end

if role == R_IPC_CLIENT then
  local ep = sys.endpoint()
  check(ep, "no endpoint")
  spawn(R_IPC_DOUBLE, { ep })

  for i = 1, 5 do
    local r = sys.call(ep, { i, tag = i })
    check(r, "no reply for " .. i)
    check(r[1] == i * 2, "wrong answer for " .. i)
    check(r.tag == i + 1, "wrong tag for " .. i)
  end

  sys.call(ep, { tag = STOP })
  wait_all(1)
  sys.exit(0)
end

if role == R_TABLE_ECHO then
  while true do
    local m, who = sys.receive(0)
    if not m then break end
    if m.tag == STOP then
      sys.reply(who, {})
      break
    end
    sys.reply(who, m)
  end
  sys.exit(0)
end

if role == R_TABLE_CLIENT then
  -- The serialiser, through the only interface anyone uses it by.
  --
  -- `design.md` §1's thesis is that the protocol between servers is the data
  -- model of the language: the server writes no marshalling, the client
  -- writes no marshalling, and what arrives is what was sent - including the
  -- integer/float distinction Lua 5.4 makes and a naive encoder loses.
  local ep = sys.endpoint()
  spawn(R_TABLE_ECHO, { ep })

  local got = sys.call(ep, {
    tag = 3, s = "text", i = 7, f = 0.5, b = true,
    n = { deep = { "a", "b" } }, [10] = "sparse",
  })

  check(got, "nothing came back")
  check(got.s == "text", "a string")
  check(got.i == 7 and math.type(got.i) == "integer", "an integer stayed one")
  check(got.f == 0.5 and math.type(got.f) == "float", "a float stayed one")
  check(got.b == true, "a boolean")
  check(got.n.deep[2] == "b", "a nested table")
  check(got[10] == "sparse", "a sparse key")
  check(got.tag == 3, "the tag")

  sys.call(ep, { tag = STOP })
  wait_all(1)
  sys.exit(0)
end

if role == R_REFUSES then
  -- A function means nothing in a state that did not create it, and a cycle
  -- has no end. Both are refused rather than mangled, and the refusal is an
  -- error the caller sees rather than a truncated value it does not.
  --
  -- Nothing is listening on this endpoint, which is the point: if any of
  -- these serialised instead of raising, the call would block for ever and
  -- the process would never exit. The C side bounds its wait, so that shows
  -- up as a failure rather than as a hang.
  local ep = sys.endpoint()

  check(pcall(function() return sys.call(ep, { f = print }) end) == false,
        "a function crossed")

  local c = {}
  c.self = c
  check(pcall(function() return sys.call(ep, c) end) == false,
        "a cycle crossed")

  local big = {}
  for i = 1, 200 do big[i] = ("x"):rep(40) end
  check(pcall(function() return sys.call(ep, big) end) == false,
        "a value larger than a message crossed")

  sys.exit(0)
end

if role == R_CAP_BROKER then
  -- Hands out `private` to whoever asks on `public`.
  local public, private = 0, 1
  local m, who = sys.receive(public)
  check(m, "the broker was never asked")
  sys.reply(who, { granted = true }, private)
  sys.exit(0)
end

if role == R_CAP_SECRET then
  -- On its own endpoint, which the client was never told the number of.
  local m, who = sys.receive(0)
  check(m, "the private service was never reached")
  sys.reply(who, { answer = m.ask .. " answered" })
  sys.exit(0)
end

if role == R_CAP_CLIENT then
  -- It holds `public` and nothing else. Its capability table has one entry.
  local reply, got = sys.call(0, { please = true })
  check(reply and reply.granted, "the broker did not answer")
  check(got ~= nil and got >= 0, "no capability arrived with the reply")

  -- And now it can talk to a service it could not previously name.
  local answer = sys.call(got, { ask = "question" })
  check(answer and answer.answer == "question answered",
        "the granted capability did not reach the service")
  sys.exit(0)
end

if role == R_CAP_MAIN then
  -- The primitive M5 rests on, and the one that moves mounting out of the
  -- kernel. Before it, only the kernel could hand out a capability, so only
  -- the kernel could decide what a process may reach; `design.md` §4.4 puts
  -- that decision in the namespace server.
  --
  -- Three processes rather than two, so the client genuinely cannot name the
  -- private endpoint: whoever creates it holds it, so the creator has to be
  -- someone other than the client.
  local public = sys.endpoint()
  local private = sys.endpoint()

  spawn(R_CAP_BROKER, { public, private })
  spawn(R_CAP_SECRET, { private })
  spawn(R_CAP_CLIENT, { public })

  wait_all(3)
  sys.exit(0)
end

if role == R_NOCAP_SERVER then
  local m, who = sys.receive(0)
  check(m, "the server was never reached")
  sys.reply(who, { saw = m.n or 0 })
  sys.exit(0)
end

if role == R_NOCAP_CLIENT then
  -- A sender cannot pass what it does not hold, and cannot guess. The index
  -- is resolved against the sender's own table, so a number naming nothing
  -- arrives as nothing rather than as somebody else's endpoint.
  --
  -- The failure is silent by design: the message still arrives and the
  -- receiver finds no capability came with it. A send that failed outright
  -- would let a sender learn which numbers are valid by watching which sends
  -- succeed.
  local ep = sys.endpoint()
  spawn(R_NOCAP_SERVER, { ep })

  local reply, got = sys.call(ep, { n = 1 }, 12)   -- 12 names nothing here
  check(reply, "the message did not arrive")
  check(got == nil or got < 0, "a capability arrived that was never held")

  wait_all(1)
  sys.exit(0)
end

if role == R_IPC_ERRORS then
  local r, e = sys.call(99, { 1 })
  check(r == nil, "a call on a capability that names nothing succeeded")
  check(e == "no such capability", "the wrong error: " .. tostring(e))

  local m, e2 = sys.receive(99)
  check(m == nil, "a receive on a capability that names nothing succeeded")
  check(e2 == "no such capability", "the wrong error: " .. tostring(e2))
  sys.exit(0)
end

if role == R_BLOCKED then
  -- The milestone's trap, seen from a process. A server blocked in receive
  -- has to come back with an error when its endpoint goes away, or it waits
  -- for ever and can never be restarted.
  --
  -- Nothing here destroys the endpoint: the C side does, from the kernel,
  -- once this process is blocked. What is being checked is that the wake-up
  -- crosses back out to EL0 as an error a Lua program can read.
  local m, e = sys.receive(0)
  check(m == nil, "receive returned a message after the endpoint went away")
  check(e == "the endpoint was destroyed", "the wrong error: " .. tostring(e))
  sys.exit(0)
end

-- ------------------------------------------------------------------
-- M6: surfaces.
--
-- `gfx.md` §19.1's rule is that Lua tables carry intent and never pixels, so
-- what is checked here is the userdata's behaviour rather than its contents:
-- that the primitives clip instead of overrunning, that a freed handle
-- raises instead of faulting, and above all that the pitch is honoured.
-- ------------------------------------------------------------------

if role == R_GFX then
  local s = gfx.surface { w = 10, h = 4 }

  local w, h = s:size()
  check(w == 10 and h == 4, "size came back wrong")

  -- The rule this whole module exists for. 10 * 4 is 40; the pitch is padded
  -- to a cache line, so it is 64. Code that assumed width * 4 would read the
  -- wrong row from the second row on.
  check(s:pitch() > w * 4, "the pitch was not padded: " .. s:pitch())

  s:fill(0, 0, 10, 4, 0xff112233)
  check(s:get(0, 0) == 0xff112233, "fill did not write the first pixel")
  check(s:get(9, 3) == 0xff112233, "fill did not reach the last pixel")

  -- Which is the real test of row_of: the last pixel of the last row is
  -- pitch * 3 + 9 * 4 bytes in, and only correct arithmetic puts it there.
  s:set(9, 3, 0xff445566)
  check(s:get(9, 3) == 0xff445566, "set did not land on the last pixel")
  check(s:get(0, 0) == 0xff112233, "set disturbed another row")

  -- Out of bounds reads nil and writes nothing, rather than raising or
  -- touching memory that is not ours.
  check(s:get(10, 0) == nil, "a read past the right edge returned something")
  check(s:get(0, 4) == nil, "a read past the bottom returned something")
  check(s:get(-1, 0) == nil, "a read before the left edge returned something")
  s:set(10, 0, 0xffffffff)
  s:set(0, 4, 0xffffffff)

  -- Clipping, from every direction at once.
  local c = gfx.surface { w = 8, h = 8 }
  c:fill(-100, -100, 1000, 1000, 0xff00ff00)
  check(c:get(0, 0) == 0xff00ff00, "a clipped fill missed the top left")
  check(c:get(7, 7) == 0xff00ff00, "a clipped fill missed the bottom right")

  -- A span is one row and only one row.
  local sp = gfx.surface { w = 8, h = 8 }
  sp:span(0, 3, 8, 0xffabcdef)
  check(sp:get(0, 3) == 0xffabcdef, "span did not draw")
  check(sp:get(0, 2) == 0, "span wrote the row above")
  check(sp:get(0, 4) == 0, "span wrote the row below")

  -- A blit that lands partly off the destination copies the part that fits
  -- and reads nothing it should not.
  local src = gfx.surface { w = 4, h = 4 }
  local dst = gfx.surface { w = 8, h = 8 }
  src:fill(0, 0, 4, 4, 0xff0000ff)
  dst:blit(src, 0, 0, 4, 4, 6, 6)
  check(dst:get(6, 6) == 0xff0000ff, "the blit did not land")
  check(dst:get(7, 7) == 0xff0000ff, "the blit did not fill its corner")
  check(dst:get(5, 5) == 0, "the blit wrote outside its rectangle")

  -- A freed surface raises rather than faulting, and freeing twice is fine.
  local f = gfx.surface { w = 4, h = 4 }
  f:free()
  f:free()
  check(pcall(function() return f:get(0, 0) end) == false,
        "a freed surface was still usable")

  -- A surface bigger than the process heap fails cleanly.
  check(pcall(gfx.surface, { w = 4096, h = 4096 }) == false,
        "a surface larger than the heap was allocated")
  check(pcall(gfx.surface, { w = 0, h = 4 }) == false,
        "a zero-width surface was allowed")

  s:free() c:free() sp:free() src:free() dst:free()
  sys.exit(0)
end

if role == R_GFX_BLEND then
  -- The alpha maths, exhaustively.
  --
  -- `gfx.c` claims its multiply is exact for every pair in 0..255 rather
  -- than merely close. That claim is the kind that gets repeated from
  -- memory, so it is checked against the rounded quotient for all 65,536
  -- pairs - reached through blend, since the function itself is static.
  --
  -- White at alpha `a` over black gives exactly mul255(255, a) in each
  -- channel, which is a for every a. Any rounding error shows up as an
  -- off-by-one somewhere in the range.
  local src = gfx.surface { w = 1, h = 1 }
  local dst = gfx.surface { w = 1, h = 1 }

  for a = 0, 255 do
    src:fill(0, 0, 1, 1, 0xffffffff)
    dst:fill(0, 0, 1, 1, 0xff000000)
    dst:blend(src, 0, 0, 1, 1, 0, 0, a)
    local got = dst:get(0, 0) & 0xff
    check(got == a, "white over black at alpha " .. a .. " gave " .. got)
  end

  -- And the other half: a source alpha of `a` over white, which exercises
  -- the destination term rather than the source one.
  for a = 0, 255 do
    src:fill(0, 0, 1, 1, (a << 24) | 0x000000)
    dst:fill(0, 0, 1, 1, 0xffffffff)
    dst:blend(src, 0, 0, 1, 1, 0, 0, 255)
    local got = dst:get(0, 0) & 0xff
    local want = (255 * (255 - a) + 127) // 255
    check(math.abs(got - want) <= 1,
          "black over white at alpha " .. a .. " gave " .. got .. " not " .. want)
  end

  -- Fully transparent leaves the destination alone; fully opaque replaces it.
  src:fill(0, 0, 1, 1, 0x00ff0000)
  dst:fill(0, 0, 1, 1, 0xff123456)
  dst:blend(src, 0, 0, 1, 1, 0, 0, 255)
  check(dst:get(0, 0) == 0xff123456, "a transparent source changed the destination")

  src:fill(0, 0, 1, 1, 0xffff0000)
  dst:blend(src, 0, 0, 1, 1, 0, 0, 255)
  check(dst:get(0, 0) == 0xffff0000, "an opaque source did not replace the destination")

  check(pcall(function() dst:blend(src, 0, 0, 1, 1, 0, 0, 300) end) == false,
        "an alpha outside 0..255 was accepted")

  src:free() dst:free()
  sys.exit(0)
end

if role == R_NO_SCREEN then
  -- A process that was not handed the screen cannot reach it, and is told
  -- so rather than being given one. Every process but the shell is this one.
  local s, err = gfx.screen()
  check(s == nil, "a process without the screen was given one")
  check(err ~= nil, "no reason was given for refusing the screen")
  sys.exit(0)
end

-- Every role above ends in sys.exit, so reaching here means the C side asked
-- for a role this chunk does not have. Exiting zero would report that as a
-- test that passed.
error("luatest: no such role: " .. tostring(role))
