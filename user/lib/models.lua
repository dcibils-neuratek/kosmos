-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **Models: what is on the disk, and what Hugging Face has** (`docs/xavier.md`,
-- *Models*). One library for `models` at the prompt and Xavier's Models page,
-- so the two cannot disagree.
--
--   models.list()                    the models folder: name, size, kind, loaded
--   models.search("phi")             Hugging Face's repositories that publish GGUF
--   models.files("TheBloke/phi-2-GGUF")   a repository's GGUF files, with their
--                                    sizes and SHA-256s
--   models.get(repo, file, progress) a file into the models folder, resumed if
--                                    it was cut, held to its SHA-256, and only
--                                    then given its name
--   models.remove(name)
--
-- **Where it asks**: Hugging Face's public API, `https://huggingface.co`, with
-- no account - every model this needs is public. `models.source` is another
-- place that answers the same way: a mirror, or a test's server, which is
-- what `name` is for - the name its certificate carries when it is reached
-- by number.
--
-- A download goes into `<file>.part` and is renamed when it is whole and its
-- sum is right, so a file without `.part` in the folder is one that checked
-- out, and a cut one is picked up where it stopped.

local http = use("/Kosmos/Libraries/http.lua")
local json = use("/Kosmos/Libraries/json.lua")
local llm = use("/Kosmos/Libraries/llm.lua")
local download = use("/Kosmos/Libraries/download.lua")

local models = {}

models.source = "https://huggingface.co"
models.name = nil                           -- the certificate's name, for a source by number
models.anchors = nil                        -- more authorities, in DER
models.quiet = nil                          -- seconds of nothing before a get gives up

-- The kit's quantisations, which a file must be in to be loaded.
models.READS = { Q4_0 = true, Q8_0 = true, F16 = true, F32 = true }

-- "phi-2.Q4_0.gguf" -> "phi-2", "Q4_0"
function models.kind(file)
  local base = file:gsub("%.gguf$", ""):gsub("%.part$", "")
  local stem, quant = base:match("^(.-)[%.%-]([QqFf]%d[%w_]*)$")

  return stem or base, quant and quant:upper() or ""
end

function models.list()
  local out = {}
  local s = llm.stats()
  local loaded = s and s.state ~= "empty" and s.model or nil

  for _, name in ipairs(fs.list(llm.MODELS) or {}) do
    local part = name:match("%.gguf%.part$") ~= nil

    if name:lower():match("%.gguf$") or part then
      local a = fs.getattr(llm.MODELS .. "/" .. name) or {}
      local stem, quant = models.kind(name)

      out[#out + 1] = { name = name, size = a.size or 0, title = stem, quant = quant,
                        loaded = name == loaded, part = part }
    end
  end

  table.sort(out, function(a, b) return a.name < b.name end)
  return out
end

-- A request to the API: its JSON, or nil and why.
local function ask(path)
  local reply, why = http.get(models.source .. path, { name = models.name, anchors = models.anchors })

  if not reply then return nil, why end

  local status, _, body = http.parse(reply)

  if status ~= 200 then
    return nil, ("%s answered %d"):format(models.source, status)
  end

  local ok, v = pcall(json.decode, body)

  if not ok or type(v) ~= "table" then return nil, models.source .. " did not answer in JSON" end

  return v
end

local function escaped(text)
  return (tostring(text):gsub("[^%w%-%._~]", function(c) return ("%%%02X"):format(c:byte()) end))
end

-- Repositories matching `term` that publish GGUF, the most downloaded first,
-- each with its architecture as Hugging Face read it from the files
-- (`expand[]=gguf`, which brings the rest of what is listed only when it
-- is asked for too) - and so whether the kit can run it, without a second
-- request a repository.
function models.search(term, limit)
  local v, why = ask(("/api/models?search=%s&filter=gguf&sort=downloads&limit=%d"
                      .. "&expand%%5B%%5D=gguf&expand%%5B%%5D=downloads&expand%%5B%%5D=likes")
                     :format(escaped(term or ""), limit or 20))

  if not v then return nil, why end

  local out = {}

  for _, m in ipairs(v) do
    if type(m) == "table" and type(m.id) == "string" then
      local arch = type(m.gguf) == "table" and type(m.gguf.architecture) == "string"
                   and m.gguf.architecture or nil

      out[#out + 1] = { repo = m.id, downloads = tonumber(m.downloads) or 0,
                        likes = tonumber(m.likes) or 0, arch = arch,
                        runs = arch == nil or llm.RUNS[arch] == true,
                        why_not = arch and not llm.RUNS[arch]
                                  and models.why_arch(arch) or nil }
    end
  end

  return out
end

-- A repository's GGUF files: name, size, SHA-256, and whether the kit reads
-- its quantisation - and when not, why.
function models.files(repo)
  if not tostring(repo):match("^[%w%-%._]+/[%w%-%._]+$") then
    return nil, "a repository is owner/name: " .. tostring(repo)
  end

  local v, why = ask("/api/models/" .. repo .. "/tree/main")

  if not v then return nil, why end

  local out = {}

  for _, f in ipairs(v) do
    if type(f) == "table" and f.type == "file" and tostring(f.path):lower():match("%.gguf$") then
      local lfs = type(f.lfs) == "table" and f.lfs or {}
      local _, quant = models.kind(f.path)

      out[#out + 1] = { file = f.path, size = tonumber(lfs.size or f.size) or 0,
                        sha256 = type(lfs.oid) == "string" and lfs.oid or nil, quant = quant,
                        reads = models.READS[quant] == true,
                        why_not = not models.READS[quant]
                                  and ("the LLM Kit reads Q4_0, Q8_0, F16 and F32, not "
                                       .. (quant ~= "" and quant or "this one")) or nil }
    end
  end

  table.sort(out, function(a, b) return a.file < b.file end)
  return out
end

-- The file the kit would take from a repository's list: Q4_0 first - the
-- smallest it reads and the fastest - then Q8_0, F16, F32; nil when it
-- reads none of them.
local PREFERRED = { "Q4_0", "Q8_0", "F16", "F32" }

function models.best(list)
  for _, quant in ipairs(PREFERRED) do
    for _, f in ipairs(list or {}) do
      if f.quant == quant and f.reads and not f.file:find("/") then return f end
    end
  end

  return nil
end

-- Why a model of another architecture is not offered.
function models.why_arch(arch)
  local runs = {}

  for a in pairs(llm.RUNS) do runs[#runs + 1] = a end
  table.sort(runs)

  return ("a %s model: the LLM Kit has a forward pass for %s only"):format(arch, table.concat(runs, ", "))
end

-- Why a repository's list has nothing for the kit: its kinds, said.
function models.why_none(list)
  if #(list or {}) == 0 then return "no GGUF file at the top of the repository" end

  local kinds, seen = {}, {}

  for _, f in ipairs(list) do
    if f.reads then return "what the kit reads is in a folder of the repository, not taken yet" end
  end

  for _, f in ipairs(list) do
    local k = f.quant ~= "" and f.quant or "unnamed"

    if not seen[k] then seen[k] = true kinds[#kinds + 1] = k end
  end

  return ("its files are %s; the LLM Kit reads Q4_0, Q8_0, F16 and F32"):format(table.concat(kinds, ", "))
end

-- How much room the models folder's disk has, in bytes, or nil when the
-- mount does not say (a folder in memory, or a disk that could not count).
function models.free()
  local sb = fs.read(llm.MODELS .. "/.super")

  if type(sb) == "table" and sb.formatted and sb.free_blocks and sb.block_size then
    return sb.free_blocks * sb.block_size
  end

  return nil
end

-- A file into the models folder. `progress(got, total)` is told as it
-- comes. The size and the sum, or nil and why.
function models.get(repo, file, progress)
  local list, why = models.files(repo)

  if not list then return nil, why end

  local f

  for _, one in ipairs(list) do
    if one.file == file then f = one end
  end

  if not f then return nil, ("%s has no file %s"):format(repo, tostring(file)) end
  if not f.sha256 then return nil, ("%s lists no SHA-256 for %s"):format(repo, file) end
  if file:find("/") then return nil, "a file in a folder of the repository is not taken yet" end

  local final = llm.MODELS .. "/" .. file
  local part = final .. ".part"

  if fs.getattr(final) then return nil, final .. " is there already" end

  local size, sum, wrong = download.file(("%s/%s/resolve/main/%s"):format(models.source, repo, escaped(file)),
                                  part, { sha256 = f.sha256, size = f.size, progress = progress,
                                          name = models.name, anchors = models.anchors,
                                          quiet = models.quiet })

  -- Whole and wrong: another try would resume nothing and fail the same
  -- way, and a `.part` left would read as cut short. So it goes.
  if not size and wrong then
    fs.send(part, { type = "delete" })
    return nil, sum .. " - removed"
  end

  if not size then return nil, sum end

  local moved, oops = fs.send(part, { type = "rename", to = final })

  if not moved then
    return nil, ("%s is whole and checked, but could not be named %s: %s")
                :format(part, final, tostring(oops))
  end

  return size, sum, final
end

function models.remove(name)
  if tostring(name):find("/") then return nil, "a model is named by its file" end

  local path = llm.MODELS .. "/" .. name

  if not fs.getattr(path) then return nil, path .. ": no such model" end

  local s = llm.stats()

  if s and s.model == name and s.state ~= "empty" then
    return nil, name .. " is loaded; load another first"
  end

  local gone, why = fs.send(path, { type = "delete" })

  if not gone then return nil, why end

  return true
end

return models
