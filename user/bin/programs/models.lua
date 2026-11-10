-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: needs network
-- **Models at the prompt** (`docs/xavier.md`, *Models*): what is in the
-- models folder, what Hugging Face has, and a model brought from there.
--
--   models                          what is here, how large, which is loaded
--   models search phi               repositories on Hugging Face with GGUF files
--   models files TheBloke/phi-2-GGUF    a repository's files, sizes and kinds
--   models get TheBloke/phi-2-GGUF phi-2.Q4_0.gguf
--                                   brought here, resumed if it was cut, and
--                                   held to Hugging Face's SHA-256
--   models load phi-2.Q4_0.gguf     into the LLM server
--   models rm phi-2.Q8_0.gguf
--
-- `--report FILE` is for a window following what it started (Xavier's
-- Models page), so the window never waits on the network itself. With
-- `get`, the file holds `got total` as the bytes come, and ends `done ok`,
-- `done paused` or `done failed <why>`; while it runs, a file `FILE.stop`
-- stops it - the `.part` kept, so `get` again goes on from there. With
-- `search`, a line `repo <owner/name> <downloads>` for each of six, then
-- for each `best <repo> <file> <bytes> <kind>` or `none <repo> <why>`,
-- and `done ok`.
--
-- `--source URL` asks another place that answers as Hugging Face does, and
-- `--name NAME` holds its certificate to that name - a mirror, or a test.
-- `--quiet SECONDS` gives a get up after that long with nothing heard
-- (30 when not given), keeping what came, so `get` again goes on.

local models = use("/Kosmos/Libraries/models.lua")
local llm = use("/Kosmos/Libraries/llm.lua")
local files = use("/Kosmos/Libraries/files.lua")

local function usage()
  print("usage: models")
  print("       models search <words>")
  print("       models files <owner/name>")
  print("       models get <owner/name> <file>")
  print("       models load <file>")
  print("       models rm <file>")
  print("       (any of them with --source <url> [--name <name>])")
end

-- Sizes as everything else in the system says them (`files.size`), so
-- the prompt and the Models page give the same number.
local size_words = files.size

local words = {}
local i, list = 1, {}
local report = nil

for w in tostring(args or ""):gmatch("%S+") do list[#list + 1] = w end

while i <= #list do
  if list[i] == "--source" then
    models.source = (list[i + 1] or ""):gsub("/$", "")
    i = i + 2
  elseif list[i] == "--name" then
    models.name = list[i + 1]
    i = i + 2
  elseif list[i] == "--quiet" then
    models.quiet = tonumber(list[i + 1])
    i = i + 2
  elseif list[i] == "--report" then
    report = list[i + 1]
    i = i + 2
  else
    words[#words + 1] = list[i]
    i = i + 1
  end
end

local verb = words[1]

if verb == nil then
  local here = models.list()

  if #here == 0 then
    print("no models in " .. llm.MODELS .. " - `models search phi`, then `models get`")
    return
  end

  for _, m in ipairs(here) do
    print(("%-34s %9s  %-6s %s"):format(m.name, size_words(m.size), m.quant,
          m.loaded and "loaded" or m.part and "cut short: `models get` again goes on" or ""))
  end

  return
end

if verb == "search" then
  local found, why = models.search(table.concat(words, " ", 2), report and 6 or nil)

  -- **For a window** (Xavier's Models page): each repository, and then
  -- the file the kit would take from it, or why there is none - looked up
  -- one by one, the report written whole after each so the page fills in.
  if report then
    if not found then fs.write(report, "done failed " .. tostring(why) .. "\n") return end

    local lines = {}
    for _, m in ipairs(found) do lines[#lines + 1] = ("repo %s %d"):format(m.repo, m.downloads) end
    fs.write(report, table.concat(lines, "\n") .. "\n")

    for _, m in ipairs(found) do
      local list, oops, best

      -- A model the kit has no forward pass for is said so, and its files
      -- are not asked for.
      if m.runs then
        list, oops = models.files(m.repo)
        best = list and models.best(list)
      else
        oops = m.why_not
      end

      lines[#lines + 1] = best and ("best %s %s %d %s"):format(m.repo, best.file, best.size, best.quant)
                          or ("none %s %s"):format(m.repo, list and models.why_none(list) or tostring(oops))
      fs.write(report, table.concat(lines, "\n") .. "\n")
    end

    lines[#lines + 1] = "done ok"
    fs.write(report, table.concat(lines, "\n") .. "\n")
    return
  end

  if not found then print("models: " .. why) return end
  if #found == 0 then print("nothing on " .. models.source .. " with GGUF files matches") return end

  for _, m in ipairs(found) do
    print(("%-48s %10d downloads  %-8s %s"):format(m.repo, m.downloads, m.arch or "?",
          m.why_not or ""))
  end

  return
end

if verb == "files" and words[2] then
  local files, why = models.files(words[2])

  if not files then print("models: " .. why) return end

  for _, f in ipairs(files) do
    print(("%-40s %9s  %-7s %s"):format(f.file, size_words(f.size), f.quant,
          f.reads and "" or f.why_not))
  end

  return
end

if verb == "get" and words[2] and words[3] then
  local shown = -1
  local STOPPED = {}
  local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
  local told_at = 0

  local function tell(line)
    if report then fs.write(report, line .. "\n") end
  end

  local ok, size, sum, path = pcall(models.get, words[2], words[3], function(got, total)
    local pct = total and total > 0 and (got * 100 // total) or -1

    -- For a window: where it is, and whether it was asked to stop - eight
    -- times a second at most, since this is told for every piece the
    -- network hands over.
    if report and (sys.ticks() - told_at > hz // 8 or got == total) then
      told_at = sys.ticks()
      tell(("%d %d"):format(got, total or 0))
      if fs.getattr(report .. ".stop") then error(STOPPED) end
    end

    -- A line every tenth, so a slow download says it is going and the
    -- prompt is not a wall of numbers.
    if pct >= 0 and pct // 10 > shown // 10 then
      shown = pct
      print(("models: %d%%, %s of %s"):format(pct, size_words(got), size_words(total)))
    end
  end)

  if not ok and size == STOPPED then
    print("models: paused - `models get` again goes on from here")
    tell("done paused")
    return
  end

  if not ok then size, sum = nil, tostring(size) end

  if not size then
    print("models: " .. sum)
    tell("done failed " .. sum)
    return
  end

  print(("models: %s, %s, SHA-256 %s checked"):format(path, size_words(size), sum))
  tell("done ok")
  return
end

if verb == "load" and words[2] then
  local r, why = llm.load(words[2])

  if not r then print("models: " .. why) return end

  print(("models: %s is loaded"):format(r.model))
  return
end

if verb == "rm" and words[2] then
  local ok, why = models.remove(words[2])

  if not ok then print("models: " .. why) return end

  print("models: " .. words[2] .. " removed")
  return
end

usage()
