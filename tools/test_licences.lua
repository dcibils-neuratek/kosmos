-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
-- LICENSE, read the way the About window reads it, and held to the tree.
--
-- `user/lib/licences.lua` turns LICENSE into the list About shows. This
-- checks that it reads every line - the project's own terms, every entry and
-- every detail, nothing stray - and then that every vendored tree it is
-- given is named in some entry, which is what keeps the list complete. A tree
-- vendored without an entry fails here, by name.
--
-- Usage: lua tools/test_licences.lua LICENSE [tree/]...
--
-- `make test` passes the trees, from the directories that exist under
-- `runtime/upstream/` and `lua/upstream/`, so a new one is checked without
-- this file being edited. The LICENSE path is there for the negative control:
-- a copy with one entry taken out has to fail, and name its tree.

local licences = dofile("user/lib/licences.lua")

local path = arg and arg[1]

if not path then
  print("usage: lua tools/test_licences.lua LICENSE [tree/]...")
  os.exit(2)
end

local file = assert(io.open(path, "rb"))
local text = file:read("a")
file:close()

local found = licences.parse(text)

local passed = 0
local failures = {}

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failures[#failures + 1] = what
  end
end

-- The project's own terms, from above the rule ------------------------------

check(found.licence == "MIT License",
      "the project's own licence is read from the top: " ..
      tostring(found.licence))
check(found.holder ~= nil and found.holder:match("Diego Cibils") ~= nil,
      "and whose it is: " .. tostring(found.holder))

-- Every line below the rule is read ------------------------------------------

check(#found.stray == 0,
      "no line below the rule is stray: " .. table.concat(found.stray, "; "))

local entry_lines, detail_lines = 0, 0
local below = false

for line in (text .. "\n"):gmatch("([^\n]*)\n") do
  if line == "---" then
    below = true
  elseif below and line:match("^      %S") then
    detail_lines = detail_lines + 1
  elseif below and line:match("^  %S") then
    entry_lines = entry_lines + 1
  end
end

local details = 0

for _, entry in ipairs(found.entries) do
  details = details + #entry.details
  check(#entry.details > 0, "an entry says where its licence is: " ..
        entry.title)
end

check(entry_lines > 0 and #found.entries == entry_lines,
      ("every entry is read: %d of %d"):format(#found.entries, entry_lines))
check(details == detail_lines,
      ("every detail line is read: %d of %d"):format(details, detail_lines))

-- And the list is complete ---------------------------------------------------

local trees = 0

for i = 2, #arg do
  local tree = (arg[i]:gsub("/*$", "")) .. "/"
  local named = false

  for _, entry in ipairs(found.entries) do
    if table.concat(entry.details, " "):find(tree, 1, true) then
      named = true
      break
    end
  end

  trees = trees + 1
  check(named, tree .. " is vendored and LICENSE does not name it")
end

if #failures == 0 then
  print(("PASS: %d checks on LICENSE as the About window reads it " ..
         "(%d entries, and all %d vendored trees named)."):format(
         passed, #found.entries, trees))
else
  print(("FAIL: %d of %d checks on LICENSE:"):format(
        #failures, passed + #failures))

  for _, what in ipairs(failures) do
    print("  " .. what)
  end

  os.exit(1)
end
