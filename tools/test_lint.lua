-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The IDE's checking, on this machine: Lua's own parser naming the line it
-- refuses, and luacheck - the vendored copy, loaded as the machine loads it
-- - told what a Kosmos program has and has not.
--
--   build/host/lua tools/test_lint.lua [lint.lua] [luacheck's root]

local lint = assert(loadfile(arg[1] or "user/lib/lint.lua"))()
local ROOT = arg[2] or "runtime/upstream/luacheck/src/"

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

-- luacheck's files, read from the tree as `fs.read` reads them on the machine.
local function read(path)
  local f = io.open(path, "rb")

  if not f then return nil end

  local body = f:read("a")
  f:close()
  return body
end

local function lines_of(text)
  local out = {}

  for line in (text .. "\n"):gmatch("([^\n]*)\n") do out[#out + 1] = line end

  return out
end

--
-- Lua's own parser.
--
do
  check(lint.parse("local x = 1\nreturn x\n", "fine.lua") == nil,
        "Lua's parser refused a file it takes")

  local src = "local function f()\n  if x then\n    return 1\nend\n"
  local p = lint.parse(src, "f.lua", lines_of(src))

  check(p and p.line == 5 and p.kind == "error" and p.by == "Lua 5.4",
        "an 'end' missing was not the error at the file's end: "
        .. tostring(p and p.line) .. " " .. tostring(p and p.text))
  check(p and p.text:find("'end' expected", 1, true) and not p.text:find("f.lua", 1, true),
        "the parser's message was not its own, or kept the file's name: "
        .. tostring(p and p.text))

  src = "local a = 1\nlocal b = = 2\n"
  p = lint.parse(src, "g.lua", lines_of(src))
  check(p and p.line == 2 and p.column and src:sub(1, 1) and p.column == 11,
        "a stray '=' was not placed on its line and byte: "
        .. tostring(p and p.line) .. "," .. tostring(p and p.column))
end

--
-- luacheck, as Kosmos has it.
--
do
  local src = table.concat({
    'local ui = use("/lib/ui.lua")',                -- 1: known
    'local win = ui.window{ title = "x" }',         -- 2
    'local unused = 1',                             -- 3: unused
    'function f(a, b) return a end',                -- 4: global set, b unused
    'print(fs.read("/Home/x"), sys.ticks())',       -- 5: Kosmos's, known
    'io.write("x")',                                -- 6: no io here
    'print(undefined_thing)',                       -- 7: never set
    'win:run()',                                    -- 8
  }, "\n") .. "\n"

  local problems, why = lint.check(src, read, ROOT)

  check(problems ~= nil, "luacheck did not load: " .. tostring(why))

  local at = {}

  for _, p in ipairs(problems or {}) do
    at[p.line] = at[p.line] or {}
    at[p.line][#at[p.line] + 1] = p
  end

  check(not at[1] and not at[2] and not at[5] and not at[8],
        "a Kosmos name - use, fs, sys - was reported as unknown")
  check(at[3] and at[3][1].kind == "warning" and at[3][1].text:find("unused", 1, true),
        "an unused local was not a warning on line 3")
  check(at[4] and #at[4] == 2, "line 4 did not have two problems, a global set "
        .. "and an unused argument")
  check(at[6] and at[6][1].kind == "error"
        and at[6][1].text == "a Kosmos program has no io; files are fs",
        "io was not said to be absent: " .. tostring(at[6] and at[6][1].text))
  check(at[7] and at[7][1].text:find("undefined_thing", 1, true)
        and at[7][1].column == 7 and at[7][1].last == 21,
        "a name never set was not placed on its bytes")

  local ordered = true

  for i = 2, #(problems or {}) do
    if problems[i].line < problems[i - 1].line then ordered = false end
  end

  check(ordered, "the problems are not in line order")

  -- A clean file has none, and a second check reuses what was loaded.
  local clean = lint.check('local ui = use("/lib/ui.lua")\nui.run()\n', read, ROOT)

  check(clean and #clean == 0, "a clean file had problems: "
        .. tostring(clean and clean[1] and clean[1].text))
end

if failed > 0 then
  print(("FAIL: %d of %d checks on the IDE's checking"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on the IDE's checking (Lua's own parser naming the "
       .. "line and bytes it refuses; luacheck, vendored and loaded as the "
       .. "machine loads it, knowing Kosmos's names, saying io is not one, "
       .. "and placing each problem on its bytes)"):format(passed))
