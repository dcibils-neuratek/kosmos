-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **No local declared twice in one scope** (`testing.md` 18.960), by the
-- vendored luacheck's warning 411, over every Lua file Kosmos writes.
--
-- On 21 November Flight's time of day (F6a) declared `local TIMES` four
-- hundred lines above the graphics panel's own `local TIMES = 120`; the
-- sheet's row read the second, a number, and `ipairs` on it ended Flight
-- with "attempt to index a number value" and no line, from inside C.
-- Every other case in the tree was harmless at the time and is the same
-- trap, so they are named apart and this keeps them so.
--
--     build/host/lua tools/luaredefined.lua FILE...
package.path = "runtime/upstream/luacheck/src/?.lua;runtime/upstream/luacheck/src/?/init.lua;"
               .. package.path
local luacheck = require("luacheck")

local found, files = 0, 0
for _, path in ipairs(arg) do
  local f = assert(io.open(path))
  local src = f:read("a")
  f:close()
  files = files + 1
  local report = luacheck.check_strings({ src }, { std = "max", only = { "411" } })
  for _, e in ipairs(report[1]) do
    found = found + 1
    print(("%s:%d: %s"):format(path, e.line, luacheck.get_message(e)))
  end
end

if found > 0 then
  print(("FAIL: %d local%s declared twice in one scope - name the second apart"):format(found,
        found == 1 and "" or "s"))
  os.exit(1)
end
print(("redefined: %d Lua files, no local declared twice in one scope"):format(files))
