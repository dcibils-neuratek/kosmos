-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- What Lite XL's host decides on its own, checked on this machine.
--
-- `user/lib/litexl_host.lua` is the part of the editor's launcher that asks
-- nobody anything: paths, the installed tree worked out from the image's
-- keys, files as `io.open` hands them over, the queue events wait in, the key
-- names, the rectangle a frame changed. All of it is plain Lua, so it is
-- checked here with the build machine's interpreter rather than in a window -
-- which is where the queue's fault was found, the long way.
--
-- Usage: lua tools/test_litexl_host.lua [path to the library]
--
-- The path is there for the negative control: the same checks, run against a
-- copy with the queue's old `items[head] = nil` put back, have to fail.

local host = dofile(arg and arg[1] or "user/lib/litexl_host.lua")

local passed = 0
local failures = {}

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failures[#failures + 1] = what
  end
end

-- Paths ------------------------------------------------------------------

check(host.absolute("/home", "notes.txt") == "/home/notes.txt",
      "a relative path is taken from the working directory")
check(host.absolute("/home", "../lib/./x.lua") == "/lib/x.lua",
      ". and .. resolve")
check(host.absolute("/home", "/a//b/") == "/a/b",
      "doubled and trailing slashes are dropped")
check(host.absolute("/", "../..") == "/",
      ".. above the root stays at the root")
check(host.absolute("/home", nil) == "/home",
      "no path is the working directory")

-- The queue --------------------------------------------------------------
--
-- Driven the way the launcher drives it: `pending` before each `pop`, and
-- pushes arriving between pops and after a drain.
--
-- **The last check in the first block is the one that found the fault.** The
-- queue consumed an event by clearing its slot, so once everything had been
-- taken `#` answered nought while `head` had moved on: `pending` went below
-- zero, the launcher's `pending() == 0` never saw it again, and the next
-- event was pushed into a slot `head` had already passed and was never read.
-- With that consumption put back, it fails here.

do
  local q = host.queue()

  for i = 1, 6 do q.push("event", i) end

  local first = q.pop()

  check(first and first[2] == 1, "the first event comes out first")
  check(q.pending() == 5, "after one pop, five are still waiting")

  q.push("event", 7)
  check(q.pending() == 6, "a push after a pop is counted")

  local order = {}

  while q.pending() > 0 do
    local e = q.pop()
    if not e then break end
    order[#order + 1] = e[2]
  end

  check(table.concat(order, ",") == "2,3,4,5,6,7",
        "every waiting event comes out, in order, and none is overwritten")
  check(q.pop() == nil and q.pending() == 0, "a drained queue is empty")

  q.push("keyreleased", "left ctrl")

  local after = q.pending() == 1 and q.pop()

  check(after and after[2] == "left ctrl",
        "an event pushed after the queue drained is the next one out")
end

do
  local q = host.queue()

  q.push("keypressed", "left ctrl")
  q.push("keypressed", "n")
  q.push("keyreleased", "n")
  q.push("keyreleased", "left ctrl")
  q.push("textinput", "o")

  local first = q.pop()
  local seen = { first and (first[1] .. " " .. first[2]) }

  while q.pending() > 0 do
    local e = q.pop()
    if not e then break end
    seen[#seen + 1] = e[1] .. " " .. e[2]
  end

  check(#seen == 5 and seen[2] == "keypressed n"
        and seen[4] == "keyreleased left ctrl" and seen[5] == "textinput o",
        "a batch is not lost after its first event: Control+N, its release, "
        .. "and the text after it all arrive")
end

do
  local told = 0
  local q = host.queue(function() told = told + 1 end)

  q.push("mousepressed", "left", 10, 20, 2)

  local e = q.pop()

  check(e and e.n == 5 and e[5] == 2, "an event keeps all of its values")
  check(told == 1, "a push is reported to whoever asked")
end

-- Reading and writing ----------------------------------------------------

do
  local lines = {}

  for line in host.reading("one\ntwo\r\nthree"):lines() do
    lines[#lines + 1] = line
  end

  check(#lines == 3 and lines[1] == "one" and lines[2] == "two\r"
        and lines[3] == "three",
        "lines() splits on newlines and leaves a carriage return for Lite XL")

  local f = host.reading("ab\ncd\n")

  check(f:read("L") == "ab\n" and f:read("*l") == "cd" and f:read("l") == nil,
        "read keeps the newline for L, drops it for l, and ends with nil")

  local g = host.reading("xyz")

  check(g:read(2) == "xy" and g:read("a") == "z" and g:read("a") == "",
        "read takes a count, and a at the end is empty rather than nil")
end

do
  local stored
  local w = host.writing(function(text) stored = text return true end, "0")

  w:write("a", 1):write("c")
  check(w:close() == true and stored == "0a1c",
        "a write collects, and is stored once on close after what it appends to")

  local bad = host.writing(function() return nil, "no room" end)
  local ok, err = bad:close()

  check(ok == nil and tostring(err):find("no room") ~= nil,
        "a store that fails makes close fail, and says why")
end

-- The installed tree -----------------------------------------------------

do
  local t = host.tree({
    "about.lua",
    "litexl/core/init.lua",
    "litexl/core/doc/init.lua",
    "litexl/plugins/treeview.lua",
    "litexl/colors/default.lua",
    "litexlnot/stray.lua",
  }, "litexl/")

  check(t.info("").type == "dir", "the top of the tree is a directory")
  check((t.info("core") or {}).type == "dir",
        "a name with keys under it is a directory")
  check((t.info("core/init.lua") or {}).type == "file", "a key is a file")
  check(t.info("missing") == nil, "a name nothing starts with is not there")
  check(table.concat(t.list("") or {}, ",") == "colors,core,plugins",
        "the top lists each child once, sorted, and nothing from outside the prefix")
  check(table.concat(t.list("core") or {}, ",") == "doc,init.lua",
        "a directory lists its files and its directories")
  check(t.list("missing") == nil, "an empty directory is not a directory")
end

-- Keys -------------------------------------------------------------------

do
  local k = host.KEY_NAMES

  check(k[29] == "left ctrl" and k[97] == "right ctrl" and k[42] == "left shift",
        "the modifiers carry SDL's names")
  check(k[30] == "a" and k[16] == "q" and k[44] == "z" and k[49] == "n"
        and k[2] == "1" and k[11] == "0",
        "the letter and number rows are laid out as keymap_plain has them")
  check(k[103] == "up" and k[104] == "pageup" and k[111] == "delete"
        and k[28] == "return" and k[59] == "f1" and k[68] == "f10",
        "the extended keys and the function keys are named")
  check(host.is_text(104, {}) and not host.is_text(104, { [29] = true })
        and not host.is_text(14, {}) and not host.is_text(-1, {})
        and not host.is_text(nil, {}),
        "text is a printable code with neither Control nor Alt held")
end

-- Damage -----------------------------------------------------------------

do
  local x0, y0, x1, y1 = host.damage_bounds({ 10, 20, 5, 5, 100, 0, 10, 10 },
                                            944, 648)

  check(x0 == 10 and y0 == 0 and x1 == 110 and y1 == 25,
        "the bounds cover every rectangle")
  check(host.damage_bounds({}, 944, 648) == nil, "no rectangles is no damage")

  local a, b, c, d = host.damage_bounds(true, 944, 648)

  check(a == 0 and b == 0 and c == 944 and d == 648, "true is the whole window")

  local e, f, g, h = host.damage_bounds({ -5, -5, 20, 20 }, 10, 10)

  check(e == 0 and f == 0 and g == 10 and h == 10,
        "the bounds are clipped to the window")
end

if #failures > 0 then
  for _, what in ipairs(failures) do print("FAIL: " .. what) end
  os.exit(1)
end

print(("PASS: %d checks on Lite XL's host on Kosmos, on this machine.")
      :format(passed))
