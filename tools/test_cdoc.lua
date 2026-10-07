-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A C header's names as the IDE suggests them (`user/lib/cdoc.lua`), read
-- from the headers a C app is given - the Window Kit's, Lua's, the libc's -
-- and from a template's own C, on this machine.
--
--   build/host/lua tools/test_cdoc.lua

local cdoc = assert(loadfile(arg and arg[1] or "user/lib/cdoc.lua"))()

local passed, failed = 0, 0

local function check(condition, what)
  if condition then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function read(path)
  local f = assert(io.open(path, "rb"))
  local s = f:read("a")

  f:close()
  return cdoc.read(s)
end

local function names(list)
  local out = {}

  for _, e in ipairs(list or {}) do out[#out + 1] = e.name end

  return table.concat(out, " ")
end

-- The Window Kit: its five calls with how each is written and the comment
-- above it, its constants, and its structs' fields.
local kw = read("user/kits/window/kosmos_window.h")
local open = kw.names.kw_open

check(open and open.kind == "function"
      and open.signature == "struct kw_window *kw_open(const char *title, unsigned width, unsigned height, unsigned flags);",
      "kw_open as it is written: " .. tostring(open and open.signature))
check(open and open.doc:match("^A window that draws its own pixels"),
      "kw_open's comment: " .. tostring(open and open.doc))
check(kw.names.kw_surface and kw.names.kw_surface.kind == "function",
      "kw_surface is the function, beside struct kw_surface")
check(names(kw.fields.kw_surface and kw.fields.kw_surface.list) == "height pitch pixels width",
      "struct kw_surface's fields: " .. names(kw.fields.kw_surface and kw.fields.kw_surface.list))
check(kw.fields.kw_event and kw.fields.kw_event.names.key and kw.fields.kw_event.names.x,
      "struct kw_event's fields")
check(kw.names.KW_KEY and kw.names.KW_KEY.kind == "value"
      and kw.names.KW_KEY.doc == "What happened: kw_event's type.",
      "a constant and the comment above its group")
for _, n in ipairs({ "kw_open", "kw_surface", "kw_commit", "kw_poll", "kw_close", "kw_why" }) do
  check(kw.names[n], "the Window Kit offers " .. n)
end

-- Lua's own, whose names are written in parentheses, and its macros.
local lua = read("lua/upstream/lua.h")

check(lua.names.lua_gettop and lua.names.lua_gettop.signature == "int lua_gettop(lua_State *L);",
      "lua_gettop from `LUA_API int (lua_gettop) (lua_State *L);`: "
      .. tostring(lua.names.lua_gettop and lua.names.lua_gettop.signature))
check(lua.names.lua_pop and lua.names.lua_pop.kind == "macro", "lua_pop, a macro")
check(lua.names.lua_State and lua.names.lua_State.kind == "type", "lua_State, a type")
check(lua.names.LUA_OK and lua.names.LUA_OK.kind == "value", "LUA_OK, a value")

local aux = read("lua/upstream/lauxlib.h")

check(aux.names.luaL_checkinteger and aux.names.luaL_checkinteger.kind == "function",
      "luaL_checkinteger")

-- The libc's.
local stdio = read("runtime/include/stdio.h")

check(stdio.names.snprintf and stdio.names.snprintf.kind == "function"
      and not stdio.names.__attribute__, "snprintf, without its attribute as a name")

-- A template's own C: its functions and what it keeps at the top.
local plasma = read("user/templates/Plasma/plasma.c")

check(plasma.names.make_palette and plasma.names.make_palette.kind == "function"
      and plasma.names.l_main and plasma.names.wave and plasma.names.palette,
      "plasma.c's own: " .. names(plasma.list))
check(plasma.names.make_palette.doc == "", "a function with nothing above it has no words")
check(plasma.names.draw and plasma.names.draw.kind == "function" and not plasma.fields.kw_surface,
      "a function taking a struct defines no struct: draw(struct kw_surface s, ...)")

-- What it includes.
local f = assert(io.open("user/templates/Plasma/plasma.c"))
local lines = {}
for line in f:lines() do lines[#lines + 1] = line end
f:close()
check(table.concat(cdoc.includes(lines), " ") == "math.h stdio.h kosmos_kit.h kosmos_window.h",
      "plasma.c's includes: " .. table.concat(cdoc.includes(lines), " "))

if failed > 0 then
  print(("FAIL: %d of %d checks on C headers read for suggestions"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on C headers read for suggestions (the Window Kit's calls, "
       .. "comments and struct fields, Lua's parenthesised names and macros, the "
       .. "libc's, a template's own C, and what a file includes)"):format(passed))
