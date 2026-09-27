-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- What is wrong with a Lua file, before it runs.
--
-- The IDE's checking (`roadmap.md` 6n, step 4), in the drawing's two parts:
--
-- **What would stop it running** is Lua's own parser - `lint.parse` - the
-- same parser that will run it, so a line it refuses is exactly the line Lua
-- would, and nothing is guessed. It compiles and never runs: a chunk loaded
-- is a function nobody calls.
--
-- **What would go wrong once it runs** is luacheck - `lint.check` - vendored
-- unmodified in `runtime/upstream/luacheck/` and carried as `/lib/luacheck/`:
-- a name used and never set, a local set and never used, one that hides
-- another. It is told what a Kosmos program is given - the names
-- `tools/luaglobals.py` holds the tree to - so `use` and `fs` are known, and
-- told what it is *not* given, so `io` is "a Kosmos program has no io;
-- files are fs" rather than nothing.
--
-- A problem is `{ line, column, last, kind = "error" | "warning", text, by }`
-- - `column` and `last` the bytes to underline, when there are any.
--
-- Pure but for how luacheck's files are read, which is handed in: the IDE
-- hands `fs.read`, and `tools/test_lint.lua` a reader of the tree's copy.

local lint = {}

-- What a Kosmos program has beyond Lua's own: `luaglobals.py`'s list.
lint.KOSMOS = { "sys", "gfx", "fs", "args", "cwd", "run", "interrupted",
                "use", "write" }

-- What upstream Lua has and a Kosmos program does not, and what to use.
lint.ABSENT = {
  io = "files are fs",
  os = "the date is use(\"/lib/clock.lua\"), a duration sys.ticks, and a "
       .. "program ends with sys.exit",
  debug = "and no debugger yet",
  package = "a library is use(\"/lib/...\")",
  require = "a library is use(\"/lib/...\")",
  dofile = "a file is run with run",
  loadfile = "a file is read with fs.read and loaded with load",
}

--------------------------------------------------------------------------
-- Lua's own parser.
--------------------------------------------------------------------------

-- The bytes of `line` that `near 'x'` names, for the underline. Lua names
-- the token and not where it is; the last place it appears is the likelier,
-- since the parser has read past everything before the one it refused.
local function near(line, token)
  if not token or token == "<eof>" or not line then return nil end

  local s, e, at = nil, nil, 1

  while true do
    local a, b = line:find(token, at, true)

    if not a then return s, e end

    s, e, at = a, b, a + 1
  end
end

--
-- Nil when Lua would take it, or the one problem it refuses it for. `name`
-- is how the file is called in the message, and is taken out of it again.
--
function lint.parse(source, name, lines)
  local fn, err = load(source, "=" .. (name or "?"), "t", {})

  if fn then return nil end

  local line, text = tostring(err):match(":(%d+): (.*)$")

  line = tonumber(line) or 1
  text = text or tostring(err)

  local token = text:match("near '(.-)'$") or text:match("near (<eof>)$")
  local from, last = near(lines and lines[line], token)

  return { line = line, column = from, last = last, kind = "error",
           text = text, by = "Lua 5.4" }
end

--------------------------------------------------------------------------
-- luacheck.
--------------------------------------------------------------------------

--
-- **Its own environment**, because it is a library written for a Lua that
-- has `require`, `package`, `io` and `os`, and it must not be changed to run
-- here. It asks three questions of them as it loads - the path separator,
-- whether colour is wanted, whether a thing is a file - and each is answered
-- here, harmlessly; its modules are found by `require` in the tree it was
-- vendored as. Nothing it is given reaches the program being checked.
--
local function load_luacheck(read, root)
  local env = {}

  for k, v in pairs(_G) do env[k] = v end

  env._G = env
  env.package = { config = "/\n;\n?\n!\n-\n", loaded = {} }
  env.os = { getenv = function() return nil end }
  env.io = { type = function() return nil end }

  local loaded = {}

  function env.require(module)
    if loaded[module] ~= nil then return loaded[module] end

    local path = root .. module:gsub("%.", "/")
    local source = read(path .. ".lua")

    if type(source) ~= "string" then source = read(path .. "/init.lua") end

    if type(source) ~= "string" then
      error("luacheck has no module " .. module, 2)
    end

    local chunk = assert(load(source, "=" .. module, "t", env))
    local value = chunk(module)

    if value == nil then value = true end

    loaded[module] = value
    return value
  end

  return env.require("luacheck")
end

local luacheck = nil

--
-- What luacheck finds in `source`, as problems in line order - or nil and
-- why, when it could not be loaded. `read` reads a file, `root` is where
-- luacheck's modules are: `/lib/` on the machine, where `luacheck/` is.
--
function lint.check(source, read, root)
  if not luacheck then
    local ok, got = pcall(load_luacheck, read, root or "/lib/")

    if not ok then return nil, tostring(got) end

    luacheck = got
  end

  local absent = {}

  for name in pairs(lint.ABSENT) do absent[#absent + 1] = name end

  local report = luacheck.check_strings({ source }, {
    std = "lua54",
    not_globals = absent,
    read_globals = lint.KOSMOS,
  })

  local problems = {}

  for _, file in ipairs(report) do
    if file.fatal then
      problems[#problems + 1] = { line = 1, kind = "error", by = "luacheck",
                                  text = "luacheck could not read it: "
                                         .. tostring(file.msg or file.fatal) }
    end

    for _, e in ipairs(file) do
      local text = luacheck.get_message(e)
      local why = (e.code == "113" or e.code == "111" or e.code == "112")
                  and lint.ABSENT[e.name or ""]

      if why then
        text = ("a Kosmos program has no %s; %s"):format(e.name, why)
      end

      problems[#problems + 1] = {
        line = e.line, column = e.column, last = e.end_column,
        kind = (e.code:sub(1, 1) == "0" or why) and "error" or "warning",
        text = text, by = "luacheck", code = e.code,
      }
    end
  end

  table.sort(problems, function(a, b)
    if a.line ~= b.line then return a.line < b.line end
    return (a.column or 0) < (b.column or 0)
  end)

  return problems
end

return lint
