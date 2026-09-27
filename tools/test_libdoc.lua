-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- What a library offers, read from its source on this machine: the IDE's
-- suggestions and its check of a name a library does not have.
--
--   build/host/lua tools/test_libdoc.lua [libdoc.lua]

local libdoc = assert(loadfile(arg[1] or "user/lib/libdoc.lua"))()

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
  local body = f:read("a")

  f:close()
  return body
end

local function names(list)
  local out = {}

  for _, e in ipairs(list) do out[#out + 1] = e.name end

  return table.concat(out, " ")
end

--
-- A small library, every way a name is defined and a comment is placed.
--
do
  local d = libdoc.read(table.concat({
    "local M = {}",
    "local thing = {}",
    "",
    "--------------------------------------------------------------------------",
    "-- Things.",
    "--",
    "-- **A thing**: made of parts, and said here in two sentences. The second.",
    "--",
    "--   M.make{ size = 3 }",
    "--",
    "local SIZE = 3",
    "",
    "function M.make(spec)",
    "  return setmetatable({}, thing)",
    "end",
    "",
    "-- How big one is.",
    "M.SIZE = SIZE",
    "",
    "function M.quiet(x)",
    "  local y = x",
    "  -- Said inside, as some are.",
    "  return y",
    "end",
    "",
    "-- Its size, doubled.",
    "function thing:twice()",
    "  return 2",
    "end",
    "",
    "local function hidden() end",
    "",
    "return M",
  }, "\n"))

  check(d.owner == "M", "the returned table was not M: " .. tostring(d.owner))
  check(names(d.list) == "make SIZE quiet",
        "the names were not make, SIZE and quiet in order: " .. names(d.list))
  local make, size, quiet = d.names.make or {}, d.names.SIZE or {}, d.names.quiet or {}

  check(make.signature == "M.make{ size = 3 }",
        "the example was not taken as the signature: " .. tostring(make.signature))
  check(make.summary == "A thing: made of parts, and said here in two sentences.",
        "the summary skipped the banner and title badly: " .. tostring(make.summary))
  check(size.kind == "value" and size.summary == "How big one is.",
        "a value and the line above it")
  check(quiet.summary == "Said inside, as some are.",
        "a comment inside the body was not found: " .. tostring(quiet.summary))
  check(d.tables.thing and d.tables.thing.names.twice
        and d.tables.thing.names.twice.kind == "method"
        and d.tables.thing.names.twice.summary == "Its size, doubled.",
        "a method on a local table was not read")
  check(not d.names.hidden, "a local function was offered")
end

--
-- The kit itself, which is what the IDE reads most.
--
do
  local d = libdoc.read(read("user/lib/ui.lua"))

  check(d.owner == "ui" and #d.list > 30, "ui.lua offered " .. #d.list .. " names")
  check(d.names.slider and d.names.slider.summary:sub(1, 8) == "A slider",
        "ui.slider has no summary from its comment")
  check(d.tables.window and d.tables.window.names.run and d.tables.window.names.add,
        "a window's methods are not among ui.lua's tables")
  check(names(libdoc.matching(d, "sl")) == "slider",
        "sl did not narrow to slider: " .. names(libdoc.matching(d, "sl")))
  check(libdoc.nearest(d, "slidr") == "slider" and libdoc.nearest(d, "wndow") == "window",
        "the nearest name to a slip was not the name meant")
  check(libdoc.nearest(d, "frobnicate") == nil, "a name far from all was given a nearest")
end

if failed > 0 then
  print(("FAIL: %d of %d checks on reading a library"):format(failed, passed + failed))
  os.exit(1)
end

print(("PASS: %d checks on reading a library (its names in order, an example "
       .. "as the signature, the comment above - or inside - as what it is, a "
       .. "section's banner and title left out, an object's methods, ui.lua's "
       .. "own, narrowing and the nearest name to a slip)"):format(passed))
