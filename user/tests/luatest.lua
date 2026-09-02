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
local R_TEXT         = 27
local R_SHARE_MAIN   = 28
local R_SHARE_PEER   = 29
local R_TRIANGLE     = 30
local R_G3D          = 31
local R_KILL_MAIN    = 32
local R_KILL_SIB     = 33
local R_KILL_VICTIM  = 34
local R_CAP_RELEASE  = 35
local R_INFLATE      = 36
local R_PDF_SCAN     = 37

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

  --------------------------------------------------------------------------
  -- The bytes, and not only the round trip.
  --
  -- A round trip proves the encoder and the decoder agree with each other.
  -- It cannot tell whether they agree on anything a *second machine* could
  -- reproduce - two matching native-endian halves round-trip perfectly and
  -- produce a format that only reads back on the architecture that wrote
  -- it.
  --
  -- Which is what this was, invisibly, because every target so far is
  -- little-endian. It matters for the disk before it matters for anything
  -- else: attribute blocks are `sys.pack` output written into a block, and
  -- a disk outlives a boot and can be carried to another machine. `hal.md`
  -- has a big-endian target on the list on purpose.
  --
  -- So these check the actual bytes. Tags from serialize.c: 3 is an
  -- integer, 5 is a string.
  --------------------------------------------------------------------------
  local packed = sys.pack(258)

  check(#packed == 9, "an integer packs to a tag and eight bytes")
  check(packed:byte(1) == 3, "the integer tag")

  -- 258 is 0x0102, so little-endian is 02 01 and then six zeroes. On a
  -- big-endian machine writing native bytes this would be the other way
  -- round, and this check is the only thing that would notice.
  check(packed:byte(2) == 2 and packed:byte(3) == 1,
        "an integer is little-endian on the wire")

  for i = 4, 9 do
    check(packed:byte(i) == 0, "the high bytes of a small integer are zero")
  end

  local text = sys.pack("hi")

  check(text:byte(1) == 5, "the string tag")
  check(text:byte(2) == 2 and text:byte(3) == 0 and text:byte(4) == 0
        and text:byte(5) == 0, "a string length is little-endian")
  check(text:sub(6) == "hi", "the string's bytes follow its length")

  -- And a float still survives, which is the part that goes through a
  -- uint64 rather than being copied.
  check(sys.unpack(sys.pack(0.5)) == 0.5, "a float round trips")
  check(sys.unpack(sys.pack(-2.25)) == -2.25, "a negative float round trips")
  check(math.type(sys.unpack(sys.pack(7))) == "integer",
        "an integer is still an integer after the change")

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

if role == R_TEXT then
  -- The font, checked pixel by pixel against patterns written out by hand.
  --
  -- Written out rather than read back from the array on purpose. Comparing
  -- what was drawn against the same bytes that drew it would pass just as
  -- happily with the bits reversed, the rows upside down, or the range off
  -- by one. These come from the generated file's own comments, read by a
  -- person, which makes them an independent statement about the same thing.
  local s = gfx.surface { w = 32, h = 32 }

  local FG, BG = 0xffffffff, 0xff000000

  -- Draws `ch` at the origin and checks one row against a pattern.
  local function row_is(ch, row, pattern, what)
    s:fill(0, 0, 32, 32, BG)
    s:text(0, 0, ch, FG)

    for i = 1, #pattern do
      local want = (pattern:sub(i, i) == "#") and FG or BG
      local got = s:get(i - 1, row)
      check(got == want,
            what .. ": row " .. row .. " pixel " .. (i - 1) ..
            " is " .. string.format("%08x", got) ..
            ", expected " .. string.format("%08x", want) ..
            " (" .. pattern .. ")")
    end
  end

  -- 'A' from assets/fonts/spleen-8x16.bdf, rows 2 and 6.
  row_is("A", 2, ".#####..", "A")
  row_is("A", 3, "##...##.", "A")
  row_is("A", 6, "#######.", "A")
  row_is("A", 0, "........", "A")

  -- '!' is narrow and centred, which catches a bit-order mistake that a
  -- symmetric glyph would hide.
  row_is("!", 2, "...##...", "!")

  -- Anything outside 0x20..0x7e draws the box the font does not have,
  -- rather than a space. A missing character should look missing.
  row_is("\1", 2, ".######.", "the unknown glyph")
  row_is("\1", 3, ".#....#.", "the unknown glyph")

  -- The advance is the only number about the font a caller should need.
  local x = s:text(0, 0, "hello", FG)
  check(x == 5 * gfx.font.w, "text returned " .. x .. " for five characters")
  check(gfx.font.w == 8 and gfx.font.h == 16,
        "the font is " .. gfx.font.w .. "x" .. gfx.font.h)

  -- Without a background, only set pixels are written: the rest shows
  -- through. With one, the whole cell is written.
  s:fill(0, 0, 32, 32, 0xff123456)
  s:text(0, 0, "A", FG)
  check(s:get(0, 2) == 0xff123456, "a transparent glyph filled its background")
  s:text(0, 0, "A", FG, BG)
  check(s:get(0, 2) == BG, "an opaque glyph did not fill its background")

  -- Clipping, from every side. None of these may write outside the surface,
  -- and the only way to see that from here is that nothing faults and the
  -- pixels that should be untouched are.
  s:fill(0, 0, 32, 32, BG)
  s:text(-4, 0, "AAAA", FG)
  s:text(28, 0, "AAAA", FG)
  s:text(0, -8, "AAAA", FG)
  s:text(0, 28, "AAAA", FG)
  s:text(-1000, -1000, "far away", FG)
  s:text(10000, 10000, "far away", FG)
  check(s:get(31, 31) ~= nil, "the surface survived drawing off its edges")

  -- A long string that starts off the left edge still lands correctly where
  -- it becomes visible, which is the case the per-glyph skip could get wrong.
  s:fill(0, 0, 32, 32, BG)
  s:text(-16, 0, "xxA", FG)          -- the A is the third cell, so at x = 0
  for i = 1, 8 do
    local want = ((".#####.."):sub(i, i) == "#") and FG or BG
    check(s:get(i - 1, 2) == want, "a string clipped on the left drew wrong")
  end

  s:free()
  sys.exit(0)
end

-- ------------------------------------------------------------------
-- Shared memory.
--
-- Two processes, one set of pages. What is being checked is both halves of
-- that: that a write on one side is visible on the other - which is the
-- feature - and that the pages go back exactly once when both sides are
-- gone, which is the invariant that broke.
--
-- It broke because a shared region used to be mapped in the same address
-- window `sys.map` hands out, and a process exiting frees everything still
-- mapped in that window on the grounds that it allocated it. Two processes
-- mapping one region therefore freed its pages twice, and the machine
-- panicked on the second. The quiet version of the same bug was worse: the
-- first process to exit handed the pages back to the allocator while the
-- second was still drawing into them.
--
-- The C side of this test is what watches the page count, because the count
-- can only be right once both processes have been reaped and neither of
-- them is around to look. See `test_shared_memory_is_freed_once`.

if role == R_SHARE_MAIN then
  local PAGES = 4
  local W, H = 16, 16               -- 1 KB of the region; the rest is spare

  local cap, err = sys.memory(PAGES)
  check(cap, "memory: " .. tostring(err))
  check(sys.memory_size(cap) == PAGES, "the region is not the size asked for")

  local at, why = sys.memory_map(cap)
  check(at, "memory_map: " .. tostring(why))

  local mine = gfx.wrap{ at = at, w = W, h = H }

  -- Zeroed on creation, and it matters: this region is about to be handed to
  -- another process, so whatever the last owner left in it must be gone.
  check(mine:get(0, 0) == 0, "a fresh region was not zeroed")

  mine:fill(0, 0, W, H, 0xff112233)

  local ep = sys.endpoint()
  local peer = spawn(R_SHARE_PEER, { ep })

  -- The peer needs the region as well as the endpoint, and a capability
  -- travels in a message rather than in the spawn.
  local reply = sys.call(ep, { tag = 1 }, cap)
  check(reply and reply.saw == 0xff112233,
        "the peer did not see what this process wrote")

  -- And back the other way, into the same pages.
  check(mine:get(1, 1) == 0xff445566,
        "this process did not see what the peer wrote")

  sys.call(ep, { tag = STOP })
  wait_all(1)
  check(peer, "the peer never started")

  sys.exit(0)
end

if role == R_SHARE_PEER then
  local ep = 0                      -- the one capability it was spawned with

  while true do
    local msg, who, cap = sys.receive(ep)

    if msg.tag == STOP then
      sys.reply(who, {})
      break
    end

    local at = sys.memory_map(cap)
    local theirs = gfx.wrap{ at = at, w = 16, h = 16 }
    local saw = theirs:get(0, 0)

    theirs:fill(1, 1, 1, 1, 0xff445566)
    sys.reply(who, { saw = saw })
  end

  -- Exits still holding the region mapped, on purpose. A peer that tidied up
  -- first would not exercise the path that broke.
  sys.exit(0)
end

-- ------------------------------------------------------------------
-- The triangle rasteriser.
--
-- The primitive the 3D engine rests on, and the one whose mistakes are
-- invisible in a screenshot: a seam between two faces is one column of
-- background pixels, and at 30 frames a second it reads as flicker rather
-- than as a gap.

if role == R_TRIANGLE then
  local BG, FG = 0xff000000, 0xffffffff
  local s = gfx.surface { w = 16, h = 16 }

  -- Inside and outside. A right triangle with the square corner at the
  -- origin: near that corner is in, the far corner is not.
  s:fill(0, 0, 16, 16, BG)
  s:triangle(0, 0, 16, 0, 0, 16, FG)
  check(s:get(1, 1) == FG, "a point inside the triangle was not filled")
  check(s:get(14, 14) == BG, "a point outside the triangle was filled")

  -- Two triangles making a quad cover it exactly: no seam down the
  -- diagonal, and no row or column missed at the edges. This is the check
  -- that says the fill rule is pixel centres and not something that happens
  -- to look right for one triangle.
  s:fill(0, 0, 16, 16, BG)
  s:triangle(0, 0, 16, 0, 16, 16, FG)
  s:triangle(0, 0, 16, 16, 0, 16, FG)

  for y = 0, 15 do
    for x = 0, 15 do
      check(s:get(x, y) == FG,
            ("the quad has a hole at %d,%d"):format(x, y))
    end
  end

  -- Degenerate: three collinear points have no area and must draw nothing
  -- rather than divide by a zero height.
  s:fill(0, 0, 16, 16, BG)
  s:triangle(0, 8, 8, 8, 15, 8, FG)
  check(s:get(8, 8) == BG, "a triangle with no height drew something")

  -- Entirely off each edge, and far larger than the surface. None of these
  -- may write outside it; the only way to see that from here is that
  -- nothing faults and the surface survives.
  s:triangle(-100, -100, -50, -100, -100, -50, FG)
  s:triangle(100, 100, 150, 100, 100, 150, FG)
  s:triangle(-1000, -1000, 1000, -1000, 0, 1000, FG)
  check(s:get(15, 15) ~= nil, "the surface did not survive a huge triangle")

  -- A vertex left of the screen: the span has to start at column 0, not at
  -- whatever truncation toward zero produces for a negative edge.
  s:fill(0, 0, 16, 16, BG)
  s:triangle(-8, 0, 8, 0, 8, 16, FG)
  check(s:get(0, 1) == FG, "a triangle crossing the left edge left a gap")

  s:free()
  sys.exit(0)
end

-- ------------------------------------------------------------------
-- The 3D engine's orientation.
--
-- Which faces of a solid you can see is the one thing about a renderer that
-- looks right while being exactly wrong. Cull with the sign the wrong way
-- round and you draw the *far* faces: still a cube, still rotating, still
-- shaded, and inside out. Nothing in a screenshot says so.
--
-- So this pins it down with a known pose and a known answer. The camera is
-- on the -z axis, the cube is not rotated, and the face at z = -h is
-- therefore the one filling the middle of the picture. Turn it half a turn
-- and the opposite face must be there instead.
--
-- The face-count check catches the other half of the same problem: windings
-- that disagree with each other rather than all being backwards. A cube
-- shows three faces at most, and the first version of `g3d.cube` had four
-- of the six wound the wrong way and showed four.

if role == R_G3D then
  -- `use` belongs to the program runner and this chunk is not a program, so
  -- the library is loaded from the same table `/lib` serves. It needs
  -- nothing but `math`, which every Lua state has.
  -- `sys.libraries` hands back the *source of a chunk* that returns the
  -- table, which is the same shape `sys.programs` has and the same reason:
  -- what the image carries is Lua text, not a built table.
  local libs = assert(load(sys.libraries(), "libraries"))()
  local source = libs["g3d.lua"]
  check(source, "the image does not carry g3d.lua")

  local g3d = assert(load(source, "g3d.lua"))()

  local NEAR = 0xff3a5f8f       -- g3d.cube's z = -h face
  local FAR  = 0xff5a7fbf       -- and z = +h
  local BG   = 0xff000000
  local N    = 64

  local s = gfx.surface { w = N, h = N }
  local mesh = g3d.cube(1.6)
  local scratch = g3d.scratch()

  local view_proj = g3d.multiply(
    g3d.look_at({ 0, 0, -4.5 }, { 0, 0, 0 }, { 0, 1, 0 }),
    g3d.perspective(math.pi / 4, 1, 0.1, 100))

  local function draw(model)
    s:fill(0, 0, N, N, BG)
    g3d.render(s, mesh, g3d.multiply(model, view_proj), N, N, scratch)
  end

  local function faces_shown()
    local seen = {}
    local n = 0

    for y = 0, N - 1, 2 do
      for x = 0, N - 1, 2 do
        local c = s:get(x, y)

        if c ~= BG and not seen[c] then
          seen[c] = true
          n = n + 1
        end
      end
    end

    return n
  end

  draw(g3d.identity())
  check(s:get(N // 2, N // 2) == NEAR,
        ("the face toward the camera is not the one drawn: got %08x, want %08x")
        :format(s:get(N // 2, N // 2), NEAR))
  check(faces_shown() == 1,
        "head on, a cube shows one face and this showed " .. faces_shown())

  -- Half a turn: the far face is now the near one.
  draw(g3d.rotation_y(math.pi))
  check(s:get(N // 2, N // 2) == FAR,
        "after half a turn the opposite face is not the one facing the camera")

  -- And at an angle, three at most - never four.
  draw(g3d.multiply(g3d.rotation_x(0.6), g3d.rotation_y(0.7)))
  local n = faces_shown()
  check(n >= 2 and n <= 3,
        ("a cube turned to a corner shows two or three faces, not %d - the "
         .. "windings disagree with each other"):format(n))

  s:free()
  sys.exit(0)
end

-- ------------------------------------------------------------------
-- Who may end whom.
--
-- `SYS_KILL` lets a parent end a child and nothing else. That rule now has
-- an exception - a process granted SPAWN_PROCCTL may end anything, which is
-- what a task manager needs - and an exception to a safety rule is exactly
-- the thing to have a test for.
--
-- Three processes, and the third is not decoration. The first version had
-- two, and the child tried to kill every id it could think of - which all
-- failed *for the wrong reason*: a role is created by the C driver and has
-- no parent, and `process_kill_any` refuses those regardless. Relaxing the
-- rule on purpose left that test passing, which is the definition of a
-- test that agrees with you.
--
-- So there is a victim, whose parent is the main role, and the sibling
-- tries to end it. That one can only fail because of the rule.

if role == R_KILL_MAIN then
  local ep = sys.endpoint()

  -- Both block in `call` until answered, so both are alive and one of them
  -- has a parent for the whole of the attempt.
  spawn(R_KILL_VICTIM, { ep })
  spawn(R_KILL_SIB, { ep })

  local report, waiting = nil, {}

  for _ = 1, 2 do
    local msg, who = sys.receive(ep)
    check(msg, "a child said nothing")

    waiting[#waiting + 1] = who
    if msg.killed then report = msg end
  end

  check(report, "the sibling never reported")
  check(report.killed == 0,
        ("a process with no grant ended %d process(es) it did not start")
        :format(report.killed))

  for _, who in ipairs(waiting) do sys.reply(who, {}) end

  wait_all(2)
  sys.exit(0)
end

if role == R_KILL_VICTIM then
  -- Alive, with a parent, and doing nothing until told.
  sys.call(0, { victim = true })
  sys.exit(0)
end

if role == R_KILL_SIB then
  local killed = 0

  -- Every process that actually exists, asked for by name rather than
  -- guessed. The first version counted from 1 to 24, and process ids climb
  -- for the life of the machine - by the ninety-fifth test they are in the
  -- hundreds, so it was trying to end ids nothing had ever had. It passed
  -- with the rule deliberately relaxed, which is how that was found.
  for _, proc in ipairs(sys.processes() or {}) do
    if proc.id ~= sys.id and sys.kill(proc.id) then
      killed = killed + 1
    end
  end

  sys.call(0, { killed = killed })
  sys.exit(0)
end

if role == R_CAP_RELEASE then
  -- Forty regions, made and given back one at a time.
  --
  -- A thread gets sixteen capability slots and, until `sys.release` existed,
  -- nothing ever freed one: `ipc_caps_release` ran when a thread died and
  -- that was all. So a server handed a buffer per request filled its table
  -- and refused every request after the sixteenth, for the life of the
  -- machine. A PDF read in 256-byte windows found it on the fifteenth read.
  --
  -- Forty rather than seventeen so the margin is not the thing being tested.
  --
  -- Each region is also mapped, written through, and read back, because the
  -- first fix for this had a second bug behind it: the userland mapping
  -- cache in `sys_user.c` is keyed by capability *index*, which was safe
  -- only while an index was never reused. Releasing made them reusable, so
  -- a stale entry handed back the address of a previous region - and the
  -- write landed in somebody else's pages and read back perfectly. Only the
  -- process that owned the buffer could see anything wrong with it.
  for i = 1, 40 do
    local cap = sys.memory(1)

    if not cap then
      sys.write(("release: no region on round %d\n"):format(i))
      sys.exit(1)
    end

    -- A marker unique to this round: a stale mapping shows up as the
    -- previous round's number rather than as an error.
    local marker = ("round-%03d"):format(i)

    if not sys.region_write(cap, 0, marker) then
      sys.write(("release: cannot write on round %d\n"):format(i))
      sys.exit(1)
    end

    local seen = sys.region_read(cap, 0, #marker)

    if seen ~= marker then
      sys.write(("release: round %d read %q, wanted %q\n")
                :format(i, tostring(seen), marker))
      sys.exit(1)
    end

    if not sys.release(cap) then
      sys.write(("release: cannot release on round %d\n"):format(i))
      sys.exit(1)
    end
  end

  sys.exit(0)
end

if role == R_INFLATE then
  -- Flate, both ways round, against a stream produced elsewhere.
  --
  -- The bytes are a zlib stream of "kosmos" repeated, made on the host. A
  -- round trip through our own compressor would prove only that the two
  -- halves agree with each other; there is no compressor here, so this is
  -- data this system did not write and has to understand.
  local stream = "\x78\xda\xcb\xce\x2f\xce\xcd\x2f\xce\x26\x8b"
                 .. "\x04\x00\x1b\x9b\x1a\x19"

  -- Straight from `sys.kit`, because a test role is a bare process with no
  -- namespace and therefore no `use`. What it is testing is the kit, not the
  -- path a program takes to reach it.
  local compress = sys.kit("compress")

  local ok, plain = pcall(compress.inflate, stream)

  if not ok then
    sys.write("inflate: " .. tostring(plain) .. "\n")
    sys.exit(1)
  end

  if plain ~= string.rep("kosmos", 10) then
    sys.write(("inflate: got %d bytes, %q\n"):format(#plain, plain:sub(1, 20)))
    sys.exit(1)
  end

  -- And the region form, which is the one a document actually uses. Same
  -- bytes in, same bytes out, with nothing on the heap in between.
  local src = sys.memory(1)
  local dst = sys.memory(1)

  sys.region_write(src, 0, stream)

  local n = compress.inflate_into(sys.memory_map(src), #stream,
                                  sys.memory_map(dst), 4096)

  if n ~= #plain or sys.region_read(dst, 0, n) ~= plain then
    sys.write(("inflate_into: %d bytes, expected %d\n"):format(n, #plain))
    sys.exit(1)
  end

  sys.exit(0)
end

if role == R_PDF_SCAN then
  -- The scanner, against a content stream with one of everything in it.
  --
  -- It exists because the C scanner replaced a Lua one that was 110 times
  -- slower, and a replacement that is faster and subtly different is worse
  -- than the thing it replaced. What is checked is the shape of what comes
  -- out: how many tokens, what each one is, and the values that are easy to
  -- get wrong - a negative number, a fraction with no leading zero, a hex
  -- string, an escaped bracket.
  local text = "q 1 0 0 -1 .5 -2.25 cm /F7 20 Tf <0003> Tj [(a\\)b) -12] TJ Q"

  local region = sys.memory(1)
  sys.region_write(region, 0, text)

  local pdfkit = sys.kit("pdf")

  local at = sys.memory_map(region)
  local kinds, values = pdfkit.scan(at, #text, 0, 64)

  local want = {
    { pdfkit.OPERATOR, "q" },
    { pdfkit.NUMBER, 1 }, { pdfkit.NUMBER, 0 }, { pdfkit.NUMBER, 0 },
    { pdfkit.NUMBER, -1 }, { pdfkit.NUMBER, 0.5 }, { pdfkit.NUMBER, -2.25 },
    { pdfkit.OPERATOR, "cm" },
    { pdfkit.NAME, "F7" },
    { pdfkit.NUMBER, 20 },
    { pdfkit.OPERATOR, "Tf" },
    { pdfkit.STRING, "\0\3" },
    { pdfkit.OPERATOR, "Tj" },
    { pdfkit.ARRAY_OPEN, true },
    { pdfkit.STRING, "a)b" },
    { pdfkit.NUMBER, -12 },
    { pdfkit.ARRAY_CLOSE, true },
    { pdfkit.OPERATOR, "TJ" },
    { pdfkit.OPERATOR, "Q" },
  }

  if #kinds ~= #want then
    sys.write(("pdf_scan: %d tokens, expected %d\n"):format(#kinds, #want))
    sys.exit(1)
  end

  for i = 1, #want do
    if kinds[i] ~= want[i][1] then
      sys.write(("pdf_scan: token %d is kind %s, expected %s\n")
                :format(i, tostring(kinds[i]), tostring(want[i][1])))
      sys.exit(1)
    end

    if want[i][2] ~= true and values[i] ~= want[i][2] then
      sys.write(("pdf_scan: token %d is %q, expected %q\n")
                :format(i, tostring(values[i]), tostring(want[i][2])))
      sys.exit(1)
    end
  end

  sys.exit(0)
end

-- Every role above ends in sys.exit, so reaching here means the C side asked
-- for a role this chunk does not have. Exiting zero would report that as a
-- test that passed.
error("luatest: no such role: " .. tostring(role))
