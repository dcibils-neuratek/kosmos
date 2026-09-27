-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Lua as the editor colours it, on this machine: which bytes of a line are
-- a keyword, a number, a string, a comment, a call or a name a program is
-- born with, and what a long string or comment carries to the next line.
--
--   build/host/lua tools/test_lualex.lua

local lex = assert(loadfile(arg and arg[1] or "user/lib/lualex.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- A line's spans as "kind:text" words, which read as what they mean.
local function spans(text, state)
  local got, out = lex.line(text, state)
  local words = {}

  for _, s in ipairs(got) do
    words[#words + 1] = s[3] .. ":" .. text:sub(s[1], s[2])
  end

  return table.concat(words, " "), out
end

local function is(text, want, what, state)
  local got = spans(text, state)

  check(got == want, ("%s: %q became %q, not %q"):format(what, text, got, want))
end

-- The drawing's own lines (`docs/kosmos-ide.html`), as it colours them.
is('local ui = use("/Kosmos/Libraries/ui.lua")',
   'keyword:local library:use string:"/Kosmos/Libraries/ui.lua"',
   "a use of a library")
is('local win = ui.window{ title = "Converter", w = 420, h = 260 }',
   'keyword:local call:window string:"Converter" number:420 number:260',
   "a window made, with a table")
is('local c = tonumber(text)',
   'keyword:local library:tonumber',
   "a library function called is the library's colour")
is('  label.text = ("%.1f F"):format(c * 9 / 5 + 32)',
   'string:"%.1f F" call:format number:9 number:5 number:32',
   "a method call on a string")
is("function M.to_f(c) return c * 1.8 + 32 end -- a comment",
   "keyword:function call:to_f keyword:return number:1.8 number:32 keyword:end "
   .. "comment:-- a comment",
   "a function defined, and a comment to the line's end")
is("win:run()", "call:run", "a method called")
is("x = t.print", "", "a field named like a library is not the library")
is("local s = a .. print", "keyword:local library:print",
   "a library name after .. is still the library")

-- Every keyword, and the words that only look like one.
is("if a and not b or nil then elseif true else goto x end",
   "keyword:if keyword:and keyword:not keyword:or keyword:nil keyword:then "
   .. "keyword:elseif keyword:true keyword:else keyword:goto keyword:end",
   "keywords")
is("ends = iffy", "", "a name that begins with a keyword is a name")

-- Numbers, every shape Lua has.
is("x = 0x1F + 0xA.8p1 + 3.14e-2 + .5 + 7",
   "number:0x1F number:0xA.8p1 number:3.14e-2 number:.5 number:7",
   "numbers")

-- Strings: both quotes, escapes, and a long one on one line.
is([===[s = "a \"quoted\" word" .. 'it\'s' .. [==[long ]] still]==] ]===],
   [===[string:"a \"quoted\" word" string:'it\'s' string:[==[long ]] still]==]]===],
   "strings")
is('t = f"x" .. g{1} .. h[[y]]',
   'call:f string:"x" call:g number:1 call:h string:[[y]]',
   "a call without brackets")
is('s = "never closed', 'string:"never closed',
   "an unterminated string ends with its line")

--
-- Across lines.
--
do
  local text, state = spans("x = [[first line", nil)

  check(text == "string:[[first line" and state == "s0",
        "a long string opened: " .. text .. " state " .. tostring(state))

  text, state = spans("second line", state)
  check(text == "string:second line" and state == "s0",
        "a long string carried on: " .. text .. " state " .. tostring(state))

  text, state = spans("end]] .. print", state)
  check(text == "string:end]] library:print" and state == nil,
        "a long string closed: " .. text .. " state " .. tostring(state))

  text, state = spans("--[==[ a note", nil)
  check(text == "comment:--[==[ a note" and state == "c2",
        "a long comment of level two opened: " .. text .. " " .. tostring(state))

  text, state = spans("]] is not its end", state)
  check(text == "comment:]] is not its end" and state == "c2",
        "a closer of the wrong level ended it: " .. text .. " " .. tostring(state))

  text, state = spans("]==] x = 1", state)
  check(text == "comment:]==] number:1" and state == nil,
        "its own closer ended it: " .. text .. " " .. tostring(state))

  text, state = spans('s = "one \\', nil)
  check(state == 'q"', "a string ended with a backslash carries on: "
        .. tostring(state))

  text, state = spans('two" .. 1', state)
  check(text == 'string:two" number:1' and state == nil,
        "and ends on the next line: " .. text .. " " .. tostring(state))

  local _, empty = lex.line("", "s1")
  check(empty == "s1", "an empty line in a long string keeps its state")
end

-- Spans in order, inside the line, and never overlapping.
do
  local ok = true
  local lines = {
    'local a, b = f(1, "x") -- note',
    "t = { [1] = [[a]], ['k'] = 0x10, n = -.5e3 }",
    "for i = 1, #list do print(i, list[i]:upper()) end",
  }

  for _, text in ipairs(lines) do
    local last = 0

    for _, s in ipairs(lex.line(text, nil)) do
      if s[1] <= last or s[2] < s[1] or s[2] > #text then ok = false end
      last = s[2]
    end
  end

  check(ok, "spans out of order, overlapping or past the line's end")
end

if failed > 0 then
  print(("FAIL: %d of %d checks on Lua's colours"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on Lua's colours (the drawing's own lines, every "
       .. "keyword, every shape of number and string, long strings and "
       .. "comments carried across lines at their level, and spans in "
       .. "order)"):format(passed))
