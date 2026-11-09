-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
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
-- `--source URL` asks another place that answers as Hugging Face does, and
-- `--name NAME` holds its certificate to that name - a mirror, or a test.

local models = use("/Kosmos/Libraries/models.lua")
local llm = use("/Kosmos/Libraries/llm.lua")

local function usage()
  print("usage: models")
  print("       models search <words>")
  print("       models files <owner/name>")
  print("       models get <owner/name> <file>")
  print("       models load <file>")
  print("       models rm <file>")
  print("       (any of them with --source <url> [--name <name>])")
end

local function size_words(bytes)
  if bytes >= 1e9 then return ("%.2f GB"):format(bytes / 1e9) end
  if bytes >= 1e8 then return ("%.0f MB"):format(bytes / 1e6) end
  if bytes >= 1e6 then return ("%.1f MB"):format(bytes / 1e6) end

  return ("%.0f KB"):format(bytes / 1e3)
end

local words = {}
local i, list = 1, {}

for w in tostring(args or ""):gmatch("%S+") do list[#list + 1] = w end

while i <= #list do
  if list[i] == "--source" then
    models.source = (list[i + 1] or ""):gsub("/$", "")
    i = i + 2
  elseif list[i] == "--name" then
    models.name = list[i + 1]
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
  local found, why = models.search(table.concat(words, " ", 2))

  if not found then print("models: " .. why) return end
  if #found == 0 then print("nothing on " .. models.source .. " with GGUF files matches") return end

  for _, m in ipairs(found) do
    print(("%-48s %10d downloads"):format(m.repo, m.downloads))
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

  local size, sum, path = models.get(words[2], words[3], function(got, total)
    local pct = total and total > 0 and (got * 100 // total) or -1

    -- A line every tenth, so a slow download says it is going and the
    -- prompt is not a wall of numbers.
    if pct >= 0 and pct // 10 > shown // 10 then
      shown = pct
      print(("models: %d%%, %s of %s"):format(pct, size_words(got), size_words(total)))
    end
  end)

  if not size then print("models: " .. sum) return end

  print(("models: %s, %s, SHA-256 %s checked"):format(path, size_words(size), sum))
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
